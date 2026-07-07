/*
 * recorder.c - Orchestrates a recording session.
 *
 * Threading model (three roles around one manual-reset "stop" event):
 *   1. Capture thread(s)  - own by capture.c; push canonical PCM into rings.
 *   2. This (main) thread - the drain loop: pulls PCM from the rings and writes
 *      it to the file(s), pacing itself off the stop event.
 *   3. Signaller threads  - the [Enter] watcher and the named-event watcher;
 *      each sets the stop event, which unblocks everyone.
 *
 * Decoupling capture (roles 1) from disk I/O (role 2) via the ring buffers is
 * what keeps capture real-time safe: slow writes never stall the microphone.
 */
#include "recorder.h"
#include "device.h"
#include "capture.h"
#include "writer.h"
#include "ring_buffer.h"

#include <wchar.h>

/* Per-stream ring buffer: ~8 seconds of canonical audio.  Large enough to ride
 * out a disk stall without dropping samples, small enough to be cheap (~1.5 MB). */
#define RING_CAPACITY  (CANON_SAMPLE_RATE * CANON_BYTES_PER_FRAME * 8u)

/* Bytes moved per pump iteration (32 KB ~= 170 ms of canonical audio). */
#define PUMP_CHUNK     32768u

/* Canonical PCM byte rate (bytes per second of audio timeline); used to turn a
 * byte count into a duration in seconds for the progress line. */
#define CANON_BYTE_RATE  (CANON_SAMPLE_RATE * CANON_BYTES_PER_FRAME)

/* ------------------------------------------------------------------ */
/* Output path helpers                                                 */
/* ------------------------------------------------------------------ */

/* Build "<base><suffix><ext>" e.g. rec.wav + "_mic" -> rec_mic.wav. */
static void MakeSuffixedPath(const wchar_t *path, const wchar_t *suffix,
                             wchar_t *out, size_t cch)
{
    const wchar_t *dot = wcsrchr(path, L'.');
    if (dot && !wcschr(dot, L'\\') && !wcschr(dot, L'/')) {
        size_t stem = (size_t)(dot - path);
        _snwprintf_s(out, cch, _TRUNCATE, L"%.*ls%ls%ls",
                     (int)stem, path, suffix, dot);
    } else {
        _snwprintf_s(out, cch, _TRUNCATE, L"%ls%ls", path, suffix);
    }
}

/* ------------------------------------------------------------------ */
/* Mixing and pumping                                                  */
/* ------------------------------------------------------------------ */

/* Mix microphone + system audio with a caller-supplied weighting, then clamp.
 * micW + loopW always equals 1.0 (see the drain loop), so a full-scale sum can
 * never overflow - that invariant is what keeps merged output from clipping,
 * whatever split the user chose. */
static void MixInto(INT16 *dst, const INT16 *mic, const INT16 *loop,
                    size_t samples, float micW, float loopW)
{
    size_t i;
    for (i = 0; i < samples; ++i) {
        float mixed = micW * (float)mic[i] + loopW * (float)loop[i];
        if (mixed >  32767.0f) mixed =  32767.0f;
        if (mixed < -32768.0f) mixed = -32768.0f;
        dst[i] = (INT16)(mixed >= 0.0f ? mixed + 0.5f : mixed - 0.5f);
    }
}

/* Move all currently-available data from one ring to its writer.
 * Returns the number of bytes flushed. */
static size_t PumpStream(RingBuffer *ring, AudioWriter *writer, BYTE *tmp)
{
    size_t total = 0;
    size_t n;
    while ((n = RingBuffer_Read(ring, tmp, PUMP_CHUNK)) > 0) {
        if (FAILED(Writer_Write(writer, tmp, n)))
            break;
        total += n;
    }
    return total;
}

/* Mix the overlapping portion of both rings and write it out.
 * Returns the number of bytes flushed (merged output). */
static size_t PumpMerged(RingBuffer *micRing, RingBuffer *loopRing,
                         AudioWriter *writer, INT16 *bufMic, INT16 *bufLoop,
                         float micW, float loopW)
{
    size_t total = 0;
    for (;;) {
        /* Only mix the portion both streams have produced, so the two stay
         * frame-aligned; the surplus of the faster stream waits in its ring. */
        size_t avail = MIN(RingBuffer_Used(micRing), RingBuffer_Used(loopRing));
        size_t n;

        avail -= avail % CANON_BYTES_PER_FRAME;   /* never split a stereo frame */
        if (avail == 0)
            break;

        n = MIN(avail, PUMP_CHUNK);
        RingBuffer_Read(micRing,  (BYTE *)bufMic,  n);
        RingBuffer_Read(loopRing, (BYTE *)bufLoop, n);
        MixInto(bufMic, bufMic, bufLoop, n / sizeof(INT16), micW, loopW);

        if (FAILED(Writer_Write(writer, (BYTE *)bufMic, n)))
            break;
        total += n;
    }
    return total;
}

/* Accumulate flush totals and print a progress line.
 *
 * Two counters are tracked because they differ in separate mode: `diskBytes` is
 * every byte handed to the writer(s) this flush (both files in separate mode),
 * while `timelineBytes` is just the microphone stream's bytes, which represent
 * the recording's DURATION.  So "seconds" comes from timeline, "bytes" from
 * disk - reporting total-disk-bytes as seconds would double-count in separate
 * mode. */
static void ReportFlush(size_t diskBytes, size_t timelineBytes,
                        unsigned long long *totalDisk,
                        unsigned long long *totalTimeline)
{
    if (diskBytes == 0)
        return;

    *totalDisk     += diskBytes;
    *totalTimeline += timelineBytes;

    /* Refresh in place: leading '\r' rewinds, trailing spaces clear leftovers. */
    printf("\r  written to disk: %.2f s, %llu bytes (last flush %zu bytes)        ",
           (double)(*totalTimeline) / (double)CANON_BYTE_RATE,
           *totalDisk,
           diskBytes);
    fflush(stdout);
}

/* ------------------------------------------------------------------ */
/* Enter-to-stop input thread                                          */
/* ------------------------------------------------------------------ */

static DWORD WINAPI WaitForEnterThread(LPVOID param)
{
    HANDLE stopEvent = (HANDLE)param;
    int    c;
    do {
        c = fgetc(stdin);
    } while (c != '\n' && c != EOF);
    SetEvent(stopEvent);
    return 0;
}

/* Bridges the cross-process named stop event to the in-process stop event.
 *
 * It waits on BOTH so it always terminates: if the user pressed [Enter] the
 * LOCAL event fires and this thread must still wake up and exit (otherwise it
 * would leak, blocked forever on the named event).  If a second 'audior --stop'
 * process fired the NAMED event, we set the local event so the capture threads
 * and drain loop - which only know about the local event - stop too. */
typedef struct { HANDLE named; HANDLE local; } STOP_WATCH;

static DWORD WINAPI WaitForStopEventThread(LPVOID param)
{
    STOP_WATCH *w = (STOP_WATCH *)param;
    HANDLE      h[2] = { w->named, w->local };
    WaitForMultipleObjects(2, h, FALSE, INFINITE);
    SetEvent(w->local);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Merge / separate prompt                                             */
/* ------------------------------------------------------------------ */

static MixMode PromptMixMode(void)
{
    char line[16];
    printf("\nSystem audio output will be included.\n");
    printf("Save as (M)erged single file or (S)eparate files? [M/s]: ");
    fflush(stdout);

    if (fgets(line, sizeof(line), stdin)) {
        if (line[0] == 's' || line[0] == 'S')
            return MIX_SEPARATE;
    }
    return MIX_MERGE;
}

/* ------------------------------------------------------------------ */
/* Session driver                                                      */
/* ------------------------------------------------------------------ */

int Recorder_Run(RecordConfig *cfg)
{
    int          rc        = 1;
    BOOL         comReady  = FALSE;
    BOOL         mfReady   = FALSE;

    IMMDevice   *micDev    = NULL;
    IMMDevice   *loopDev   = NULL;

    RingBuffer   micRing   = {0};
    RingBuffer   loopRing  = {0};
    BOOL         micRingOk = FALSE, loopRingOk = FALSE;

    CaptureStream micCap   = {0};
    CaptureStream loopCap  = {0};
    BOOL          micCapOk = FALSE, loopCapOk = FALSE;

    AudioWriter  *writerA  = NULL;   /* mic, or merged */
    AudioWriter  *writerB  = NULL;   /* system (separate mode only) */

    HANDLE        stopEvent = NULL;
    HANDLE        inputThread = NULL;
    HANDLE        namedStop = NULL;
    HANDLE        stopWatcher = NULL;
    STOP_WATCH    watch = {0};

    BYTE         *tmp     = NULL;
    INT16        *mixA    = NULL;
    INT16        *mixB    = NULL;

    wchar_t       pathMic[MAX_PATH];
    wchar_t       pathSys[MAX_PATH];
    HRESULT       hr;

    /* -------- COM / Media Foundation -------- */
    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        fprintf(stderr, "CoInitializeEx failed (hr=0x%08lX)\n", (unsigned long)hr);
        goto cleanup;
    }
    comReady = TRUE;

    /* Media Foundation is only needed for MP3 encoding, so start it lazily.
     * MFSTARTUP_LITE skips the network/socket layer we never use. */
    if (cfg->format == FORMAT_MP3) {
        hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
        CHECK_HR(hr, "MFStartup failed");
        mfReady = TRUE;
    }

    /* -------- Resolve devices -------- */
    hr = Device_Select(eCapture, cfg->micSelector, &micDev);
    if (FAILED(hr)) {
        fprintf(stderr, "Could not open microphone device.\n");
        goto cleanup;
    }
    if (cfg->includeOutput) {
        hr = Device_Select(eRender, cfg->renderSelector, &loopDev);
        if (FAILED(hr)) {
            fprintf(stderr, "Could not open playback device for loopback.\n");
            goto cleanup;
        }
    }

    /* -------- Resolve mix mode (prompt if needed) -------- */
    if (cfg->includeOutput && cfg->mixMode == MIX_PROMPT)
        cfg->mixMode = PromptMixMode();

    /* -------- Ring buffers -------- */
    micRingOk = RingBuffer_Init(&micRing, RING_CAPACITY);
    if (!micRingOk) { fprintf(stderr, "Out of memory.\n"); goto cleanup; }
    if (cfg->includeOutput) {
        loopRingOk = RingBuffer_Init(&loopRing, RING_CAPACITY);
        if (!loopRingOk) { fprintf(stderr, "Out of memory.\n"); goto cleanup; }
    }

    /* -------- Writers -------- */
    if (cfg->includeOutput && cfg->mixMode == MIX_SEPARATE) {
        MakeSuffixedPath(cfg->outputPath, L"_mic",    pathMic, MAX_PATH);
        MakeSuffixedPath(cfg->outputPath, L"_system", pathSys, MAX_PATH);
        hr = Writer_Open(&writerA, pathMic, cfg->format);
        CHECK_HR(hr, "Failed to open microphone output file");
        hr = Writer_Open(&writerB, pathSys, cfg->format);
        CHECK_HR(hr, "Failed to open system output file");
    } else {
        hr = Writer_Open(&writerA, cfg->outputPath, cfg->format);
        CHECK_HR(hr, "Failed to open output file");
    }

    /* -------- Stop event + capture -------- */
    stopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);   /* manual reset */
    if (!stopEvent) { fprintf(stderr, "CreateEvent failed.\n"); goto cleanup; }

    /* Named event allows a second 'audior --stop' instance to end this session. */
    namedStop = CreateEventW(NULL, TRUE, FALSE, STOP_EVENT_NAME);
    if (namedStop)
        ResetEvent(namedStop);   /* clear any stale signal from a prior --stop */

    {
        float micGain = 1.0f + (float)cfg->amplifyPercent / 100.0f;  /* 0%=1.0, 100%=2.0 */

        hr = Capture_Init(&micCap, micDev, FALSE, micGain, stopEvent, &micRing);
        CHECK_HR(hr, "Microphone capture init failed");
        micCapOk = TRUE;

        if (cfg->includeOutput) {
            hr = Capture_Init(&loopCap, loopDev, TRUE, 1.0f, stopEvent, &loopRing);
            CHECK_HR(hr, "Loopback capture init failed");
            loopCapOk = TRUE;
        }
    }

    /* -------- Scratch buffers for pumping -------- */
    tmp  = (BYTE  *)malloc(PUMP_CHUNK);
    mixA = (INT16 *)malloc(PUMP_CHUNK);
    mixB = (INT16 *)malloc(PUMP_CHUNK);
    if (!tmp || !mixA || !mixB) { fprintf(stderr, "Out of memory.\n"); goto cleanup; }

    /* -------- Go -------- */
    hr = Capture_Start(&micCap);
    CHECK_HR(hr, "Failed to start microphone capture");
    if (cfg->includeOutput) {
        hr = Capture_Start(&loopCap);
        CHECK_HR(hr, "Failed to start loopback capture");
    }

    wprintf(L"\nRecording to %ls", cfg->outputPath);
    if (cfg->includeOutput) {
        if (cfg->mixMode == MIX_SEPARATE)
            wprintf(L" (separate system audio)");
        else
            wprintf(L" (merged, mic %d%%)", cfg->mergeMicPercent);
    }
    wprintf(L"\nPress [Enter] to stop, or run --stop from another window.\n");
    fflush(stdout);

    inputThread = CreateThread(NULL, 0, WaitForEnterThread, stopEvent, 0, NULL);
    if (namedStop) {
        watch.named = namedStop;
        watch.local = stopEvent;
        stopWatcher = CreateThread(NULL, 0, WaitForStopEventThread, &watch, 0, NULL);
    }

    /* Drain loop: flush available audio to disk until stopped.
     *
     * We wake every 250 ms (or immediately when the stop event fires).  The
     * multi-second ring buffers mean this relaxed cadence never risks overflow,
     * while keeping wake-ups - and progress-line updates - infrequent enough to
     * be cheap and readable. */
    {
        unsigned long long totalDisk = 0, totalTimeline = 0;

        /* Merge weights derived once from the configured mic percentage; loopW
         * is the complement so they always sum to 1.0 (no clipping). */
        float micW  = (float)cfg->mergeMicPercent / 100.0f;
        float loopW = 1.0f - micW;

        for (;;) {
            DWORD  wait = WaitForSingleObject(stopEvent, 250);
            size_t disk = 0, timeline = 0;

            if (cfg->includeOutput && cfg->mixMode == MIX_MERGE) {
                disk = timeline = PumpMerged(&micRing, &loopRing, writerA, mixA, mixB, micW, loopW);
            } else {
                size_t mic = PumpStream(&micRing, writerA, tmp);
                size_t sys = cfg->includeOutput ? PumpStream(&loopRing, writerB, tmp) : 0;
                disk     = mic + sys;
                timeline = mic;            /* microphone defines the timeline */
            }

            ReportFlush(disk, timeline, &totalDisk, &totalTimeline);

            if (wait == WAIT_OBJECT_0)
                break;
        }

        /* Stop capture (threads do a final device drain), then flush remainder. */
        Capture_Stop(&micCap);
        if (loopCapOk)
            Capture_Stop(&loopCap);

        if (cfg->includeOutput && cfg->mixMode == MIX_MERGE) {
            size_t merged = PumpMerged(&micRing, &loopRing, writerA, mixA, mixB, micW, loopW);
            /* Any unmatched tail (one stream longer) is written as-is. */
            size_t tail = PumpStream(&micRing, writerA, tmp) +
                          PumpStream(&loopRing, writerA, tmp);
            ReportFlush(merged + tail, merged + tail, &totalDisk, &totalTimeline);
        } else {
            size_t mic = PumpStream(&micRing, writerA, tmp);
            size_t sys = cfg->includeOutput ? PumpStream(&loopRing, writerB, tmp) : 0;
            ReportFlush(mic + sys, mic, &totalDisk, &totalTimeline);
        }

        if (totalDisk > 0)
            printf("\n");        /* terminate the in-place status line */
        printf("Recording stopped. Finalising file(s)...\n");
    }
    rc = 0;

cleanup:
    if (stopEvent)
        SetEvent(stopEvent);   /* wake the input / stop-watcher threads */

    if (stopWatcher) {
        WaitForSingleObject(stopWatcher, 1000);
        CloseHandle(stopWatcher);
    }
    if (inputThread) {
        /* The input thread may still be blocked on stdin if we stopped via
         * --stop; do not wait indefinitely in that case. */
        WaitForSingleObject(inputThread, 1000);
        CloseHandle(inputThread);
    }
    if (namedStop) CloseHandle(namedStop);

    if (micCapOk)  Capture_Free(&micCap);
    if (loopCapOk) Capture_Free(&loopCap);

    if (writerA) Writer_Close(writerA);
    if (writerB) Writer_Close(writerB);

    if (micRingOk)  RingBuffer_Free(&micRing);
    if (loopRingOk) RingBuffer_Free(&loopRing);

    SAFE_RELEASE(micDev);
    SAFE_RELEASE(loopDev);

    free(tmp);
    free(mixA);
    free(mixB);

    if (stopEvent) CloseHandle(stopEvent);
    if (mfReady)   MFShutdown();
    if (comReady)  CoUninitialize();

    if (rc == 0)
        wprintf(L"Done.\n");
    return rc;
}
