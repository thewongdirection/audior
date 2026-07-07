/*
 * capture.c - One WASAPI capture stream (microphone or system loopback).
 *
 * Design notes (the "why"):
 *
 *  - Timer-driven polling, not event-driven.  WASAPI supports an event-callback
 *    capture mode, but loopback capture historically does not raise the buffer
 *    event reliably, and mixing two capture modes complicates the code.  A
 *    single uniform poll (wake every 10 ms, drain whatever is ready) works
 *    identically for mic and loopback and is simpler to reason about.  10 ms is
 *    well below the 200 ms device buffer, so no data is ever dropped.
 *
 *  - The worker thread joins the process MTA.  The COM objects are created on
 *    the caller's thread but used here; WASAPI endpoint objects are agile, so
 *    cross-thread use is legal, but the thread must still have COM initialised
 *    before it makes interface calls.
 *
 *  - Convert-on-capture.  Each packet is normalised to the canonical PCM format
 *    (see audio_convert.c) before entering the ring buffer, so the drain loop
 *    and mixer downstream never have to care about device formats.
 */
#include "capture.h"

/* Input frames handed to the converter per call.  WASAPI packets are small
 * (~10 ms) so this is rarely reached, but capping the slice bounds the size of
 * the reusable conversion scratch buffer regardless of packet size. */
#define CAPTURE_SLICE_FRAMES 4096u

/* Requested shared-mode buffer duration: 200 ms, in 100-ns units.  Generous
 * enough that a slow disk flush never overruns the device buffer. */
#define CAPTURE_BUFFER_HNS   2000000

/* ------------------------------------------------------------------ */
/* Capture worker                                                      */
/* ------------------------------------------------------------------ */

/* Move every currently available packet from the device into the ring. */
static void DrainDevice(CaptureStream *cs)
{
    UINT32 packetFrames = 0;

    while (SUCCEEDED(IAudioCaptureClient_GetNextPacketSize(cs->capture, &packetFrames)) &&
           packetFrames > 0) {

        BYTE  *data   = NULL;
        UINT32 frames = 0;
        DWORD  flags  = 0;
        UINT32 offset;
        BOOL   silent;

        HRESULT hr = IAudioCaptureClient_GetBuffer(cs->capture, &data, &frames,
                                                   &flags, NULL, NULL);
        if (FAILED(hr) || frames == 0) {
            if (SUCCEEDED(hr))
                IAudioCaptureClient_ReleaseBuffer(cs->capture, frames);
            break;
        }

        /* WASAPI may hand back a "silent" packet whose data pointer is not
         * meaningful; the converter emits zeros for it rather than reading. */
        silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) ? TRUE : FALSE;

        for (offset = 0; offset < frames; offset += CAPTURE_SLICE_FRAMES) {
            UINT32 chunk = frames - offset;
            const BYTE *slice;
            size_t outFrames;

            if (chunk > CAPTURE_SLICE_FRAMES)
                chunk = CAPTURE_SLICE_FRAMES;

            slice = data ? data + (size_t)offset * cs->mixFormat->nBlockAlign : NULL;

            outFrames = AudioConvert_Process(&cs->resampler, cs->mixFormat, silent,
                                             cs->gain, slice, chunk,
                                             cs->scratch, cs->scratchFrames);

            RingBuffer_Write(cs->ring, (const BYTE *)cs->scratch,
                             outFrames * CANON_BYTES_PER_FRAME);
        }

        IAudioCaptureClient_ReleaseBuffer(cs->capture, frames);
    }
}

static DWORD WINAPI CaptureThread(LPVOID param)
{
    CaptureStream *cs = (CaptureStream *)param;

    /* Join the process MTA so COM interface calls from this thread are valid. */
    CoInitializeEx(NULL, COINIT_MULTITHREADED);

    /* Drain AFTER checking the stop event, then break: this guarantees one
     * final drain once stop is signalled, so audio buffered in the device at
     * stop time is not lost. */
    for (;;) {
        DWORD wait = WaitForSingleObject(cs->stopEvent, 10);
        DrainDevice(cs);
        if (wait == WAIT_OBJECT_0)
            break;
    }

    CoUninitialize();
    return 0;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

HRESULT Capture_Init(CaptureStream *cs, IMMDevice *device, BOOL loopback,
                     float gain, HANDLE stopEvent, RingBuffer *ring)
{
    HRESULT hr;
    DWORD   streamFlags = loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0;

    memset(cs, 0, sizeof(*cs));
    cs->loopback  = loopback;
    cs->gain      = gain;
    cs->stopEvent = stopEvent;
    cs->ring      = ring;

    hr = IMMDevice_Activate(device, &IID_IAudioClient, CLSCTX_ALL, NULL,
                            (void **)&cs->client);
    if (FAILED(hr))
        return hr;

    hr = IAudioClient_GetMixFormat(cs->client, &cs->mixFormat);
    if (FAILED(hr))
        return hr;

    hr = IAudioClient_Initialize(cs->client, AUDCLNT_SHAREMODE_SHARED, streamFlags,
                                 CAPTURE_BUFFER_HNS, 0, cs->mixFormat, NULL);
    if (FAILED(hr))
        return hr;

    hr = IAudioClient_GetService(cs->client, &IID_IAudioCaptureClient,
                                 (void **)&cs->capture);
    if (FAILED(hr))
        return hr;

    Resampler_Init(&cs->resampler, cs->mixFormat->nSamplesPerSec, CANON_SAMPLE_RATE);

    cs->scratchFrames = Resampler_MaxOut(&cs->resampler, CAPTURE_SLICE_FRAMES);
    cs->scratch       = (INT16 *)malloc(cs->scratchFrames * CANON_BYTES_PER_FRAME);
    if (!cs->scratch)
        return E_OUTOFMEMORY;

    return S_OK;
}

HRESULT Capture_Start(CaptureStream *cs)
{
    HRESULT hr = IAudioClient_Start(cs->client);
    if (FAILED(hr))
        return hr;

    cs->thread = CreateThread(NULL, 0, CaptureThread, cs, 0, NULL);
    if (!cs->thread)
        return HRESULT_FROM_WIN32(GetLastError());

    return S_OK;
}

void Capture_Stop(CaptureStream *cs)
{
    if (cs->thread) {
        WaitForSingleObject(cs->thread, INFINITE);
        CloseHandle(cs->thread);
        cs->thread = NULL;
    }
    if (cs->client)
        IAudioClient_Stop(cs->client);
}

void Capture_Free(CaptureStream *cs)
{
    if (!cs)
        return;
    if (cs->thread) {
        WaitForSingleObject(cs->thread, INFINITE);
        CloseHandle(cs->thread);
        cs->thread = NULL;
    }
    SAFE_RELEASE(cs->capture);
    SAFE_RELEASE(cs->client);
    if (cs->mixFormat) {
        CoTaskMemFree(cs->mixFormat);
        cs->mixFormat = NULL;
    }
    free(cs->scratch);
    cs->scratch = NULL;
}
