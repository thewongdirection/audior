/*
 * writer.c - Output file writers for canonical PCM: WAV and MP3.
 *
 * WAV is written by hand with the Win32 file API - a RIFF/WAVE container is
 * trivial and needs no library.  Its two size fields depend on the total data
 * length, which is unknown until recording stops, so we write a placeholder
 * header up front and patch it in on close (see WavClose).
 *
 * MP3 is produced by Media Foundation's IMFSinkWriter driving the MP3 file
 * sink, which internally loads Windows' built-in MP3 encoder MFT.  This keeps
 * the "no external libraries" promise: the encoder ships with Windows.  We feed
 * it canonical PCM and it emits an ID3-tagged .mp3.
 */
#include "writer.h"

/* MP3 target bitrate: 128 kbps => 16000 bytes/sec (MF wants bytes/sec here). */
#define MP3_BYTES_PER_SEC 16000u

/* ------------------------------------------------------------------ */
/* WAV header (RIFF / WAVE, PCM)                                       */
/* ------------------------------------------------------------------ */

#pragma pack(push, 1)
typedef struct {
    char     riff[4];        /* "RIFF" */
    uint32_t riffSize;       /* 36 + dataSize */
    char     wave[4];        /* "WAVE" */
    char     fmt[4];         /* "fmt " */
    uint32_t fmtSize;        /* 16 */
    uint16_t audioFormat;    /* 1 = PCM */
    uint16_t channels;
    uint32_t sampleRate;
    uint32_t byteRate;
    uint16_t blockAlign;
    uint16_t bitsPerSample;
    char     data[4];        /* "data" */
    uint32_t dataSize;
} WavHeader;
#pragma pack(pop)

/* ------------------------------------------------------------------ */
/* Writer object                                                       */
/* ------------------------------------------------------------------ */

struct AudioWriter {
    AudioFormat format;

    /* WAV backend */
    HANDLE   file;
    uint32_t dataBytes;

    /* MP3 backend */
    IMFSinkWriter *sink;
    DWORD          streamIndex;
    LONGLONG       timeHns;       /* running presentation time, 100-ns units */
};

/* ------------------------------------------------------------------ */
/* WAV implementation                                                  */
/* ------------------------------------------------------------------ */

static void WavFillHeader(WavHeader *h, uint32_t dataSize)
{
    const uint16_t channels   = (uint16_t)CANON_CHANNELS;
    const uint32_t sampleRate = CANON_SAMPLE_RATE;
    const uint16_t bits       = (uint16_t)CANON_BITS;
    const uint16_t blockAlign = (uint16_t)CANON_BYTES_PER_FRAME;

    memcpy(h->riff, "RIFF", 4);
    h->riffSize = 36 + dataSize;
    memcpy(h->wave, "WAVE", 4);
    memcpy(h->fmt, "fmt ", 4);
    h->fmtSize       = 16;
    h->audioFormat   = 1;
    h->channels      = channels;
    h->sampleRate    = sampleRate;
    h->byteRate      = sampleRate * blockAlign;
    h->blockAlign    = blockAlign;
    h->bitsPerSample = bits;
    memcpy(h->data, "data", 4);
    h->dataSize = dataSize;
}

static HRESULT WavOpen(AudioWriter *w, const wchar_t *path)
{
    WavHeader header;
    DWORD     written;

    w->file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                          FILE_ATTRIBUTE_NORMAL, NULL);
    if (w->file == INVALID_HANDLE_VALUE) {
        w->file = NULL;
        return HRESULT_FROM_WIN32(GetLastError());
    }

    /* Reserve space for the header; sizes are patched on close. */
    WavFillHeader(&header, 0);
    if (!WriteFile(w->file, &header, sizeof(header), &written, NULL))
        return HRESULT_FROM_WIN32(GetLastError());

    return S_OK;
}

static HRESULT WavWrite(AudioWriter *w, const BYTE *pcm, size_t bytes)
{
    DWORD written;
    if (!WriteFile(w->file, pcm, (DWORD)bytes, &written, NULL))
        return HRESULT_FROM_WIN32(GetLastError());
    w->dataBytes += written;
    return S_OK;
}

static HRESULT WavClose(AudioWriter *w)
{
    WavHeader header;
    DWORD     written;

    if (!w->file)
        return S_OK;

    WavFillHeader(&header, w->dataBytes);
    SetFilePointer(w->file, 0, NULL, FILE_BEGIN);
    WriteFile(w->file, &header, sizeof(header), &written, NULL);

    CloseHandle(w->file);
    w->file = NULL;
    return S_OK;
}

/* ------------------------------------------------------------------ */
/* MP3 implementation (Media Foundation sink writer)                   */
/* ------------------------------------------------------------------ */

static HRESULT Mp3CreateOutputType(IMFMediaType **out)
{
    IMFMediaType *t = NULL;
    HRESULT hr = MFCreateMediaType(&t);
    if (FAILED(hr))
        return hr;

    IMFMediaType_SetGUID(t, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio);
    IMFMediaType_SetGUID(t, &MF_MT_SUBTYPE, &MFAudioFormat_MP3);
    IMFMediaType_SetUINT32(t, &MF_MT_AUDIO_NUM_CHANNELS, CANON_CHANNELS);
    IMFMediaType_SetUINT32(t, &MF_MT_AUDIO_SAMPLES_PER_SECOND, CANON_SAMPLE_RATE);
    IMFMediaType_SetUINT32(t, &MF_MT_AUDIO_AVG_BYTES_PER_SECOND, MP3_BYTES_PER_SEC);
    IMFMediaType_SetUINT32(t, &MF_MT_AUDIO_BLOCK_ALIGNMENT, 1);

    *out = t;
    return S_OK;
}

static HRESULT Mp3CreateInputType(IMFMediaType **out)
{
    IMFMediaType *t = NULL;
    HRESULT hr = MFCreateMediaType(&t);
    if (FAILED(hr))
        return hr;

    IMFMediaType_SetGUID(t, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio);
    IMFMediaType_SetGUID(t, &MF_MT_SUBTYPE, &MFAudioFormat_PCM);
    IMFMediaType_SetUINT32(t, &MF_MT_AUDIO_NUM_CHANNELS, CANON_CHANNELS);
    IMFMediaType_SetUINT32(t, &MF_MT_AUDIO_SAMPLES_PER_SECOND, CANON_SAMPLE_RATE);
    IMFMediaType_SetUINT32(t, &MF_MT_AUDIO_BITS_PER_SAMPLE, CANON_BITS);
    IMFMediaType_SetUINT32(t, &MF_MT_AUDIO_BLOCK_ALIGNMENT, CANON_BYTES_PER_FRAME);
    IMFMediaType_SetUINT32(t, &MF_MT_AUDIO_AVG_BYTES_PER_SECOND,
                           CANON_SAMPLE_RATE * CANON_BYTES_PER_FRAME);

    *out = t;
    return S_OK;
}

static HRESULT Mp3Open(AudioWriter *w, const wchar_t *path)
{
    IMFMediaType *outType = NULL;
    IMFMediaType *inType  = NULL;
    HRESULT       hr;

    hr = MFCreateSinkWriterFromURL(path, NULL, NULL, &w->sink);
    if (FAILED(hr))
        goto done;

    hr = Mp3CreateOutputType(&outType);
    if (FAILED(hr))
        goto done;

    hr = IMFSinkWriter_AddStream(w->sink, outType, &w->streamIndex);
    if (FAILED(hr))
        goto done;

    hr = Mp3CreateInputType(&inType);
    if (FAILED(hr))
        goto done;

    hr = IMFSinkWriter_SetInputMediaType(w->sink, w->streamIndex, inType, NULL);
    if (FAILED(hr))
        goto done;

    hr = IMFSinkWriter_BeginWriting(w->sink);

done:
    SAFE_RELEASE(outType);
    SAFE_RELEASE(inType);
    return hr;
}

static HRESULT Mp3Write(AudioWriter *w, const BYTE *pcm, size_t bytes)
{
    IMFSample     *sample = NULL;
    IMFMediaBuffer *buffer = NULL;
    BYTE          *dst     = NULL;
    HRESULT        hr;
    LONGLONG       durationHns;
    size_t         frames = bytes / CANON_BYTES_PER_FRAME;

    if (bytes == 0)
        return S_OK;

    hr = MFCreateMemoryBuffer((DWORD)bytes, &buffer);
    if (FAILED(hr))
        goto done;

    hr = IMFMediaBuffer_Lock(buffer, &dst, NULL, NULL);
    if (FAILED(hr))
        goto done;
    memcpy(dst, pcm, bytes);
    IMFMediaBuffer_Unlock(buffer);
    IMFMediaBuffer_SetCurrentLength(buffer, (DWORD)bytes);

    hr = MFCreateSample(&sample);
    if (FAILED(hr))
        goto done;

    IMFSample_AddBuffer(sample, buffer);

    /* The encoder needs each sample's presentation time and duration to build a
     * correct MP3 timeline; we derive them from the running frame count rather
     * than wall-clock, so the output duration exactly matches the audio data.
     * 100-ns units are Media Foundation's time base. */
    durationHns = (LONGLONG)frames * 10000000LL / CANON_SAMPLE_RATE;
    IMFSample_SetSampleTime(sample, w->timeHns);
    IMFSample_SetSampleDuration(sample, durationHns);
    w->timeHns += durationHns;

    hr = IMFSinkWriter_WriteSample(w->sink, w->streamIndex, sample);

done:
    SAFE_RELEASE(sample);
    SAFE_RELEASE(buffer);
    return hr;
}

static HRESULT Mp3Close(AudioWriter *w)
{
    HRESULT hr = S_OK;
    if (w->sink) {
        hr = IMFSinkWriter_Finalize(w->sink);
        SAFE_RELEASE(w->sink);
    }
    return hr;
}

/* ------------------------------------------------------------------ */
/* Public dispatch                                                     */
/* ------------------------------------------------------------------ */

HRESULT Writer_Open(AudioWriter **out, const wchar_t *path, AudioFormat format)
{
    AudioWriter *w;
    HRESULT      hr;

    *out = NULL;
    w = (AudioWriter *)calloc(1, sizeof(*w));
    if (!w)
        return E_OUTOFMEMORY;

    w->format = format;
    hr = (format == FORMAT_MP3) ? Mp3Open(w, path) : WavOpen(w, path);
    if (FAILED(hr)) {
        Writer_Close(w);
        return hr;
    }

    *out = w;
    return S_OK;
}

HRESULT Writer_Write(AudioWriter *w, const BYTE *pcm, size_t bytes)
{
    if (!w)
        return E_POINTER;
    return (w->format == FORMAT_MP3) ? Mp3Write(w, pcm, bytes)
                                     : WavWrite(w, pcm, bytes);
}

HRESULT Writer_Close(AudioWriter *w)
{
    HRESULT hr;
    if (!w)
        return S_OK;

    hr = (w->format == FORMAT_MP3) ? Mp3Close(w) : WavClose(w);
    free(w);
    return hr;
}
