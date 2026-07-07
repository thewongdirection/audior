/*
 * capture.h - WASAPI capture for one endpoint (microphone or loopback).
 *
 * Each CaptureStream runs its own thread that polls the WASAPI capture
 * client, converts incoming audio to the canonical PCM format, and writes
 * it into a ring buffer.  A shared stop event ends every stream together.
 *
 * Loopback silence injection: a render endpoint delivers NO packets while it
 * is idle (nothing is playing), whereas a microphone always emits (silent)
 * packets.  Left alone, that makes the loopback stream shorter than the mic
 * stream - and in --separate mode an all-silent loopback would yield an empty
 * (0-byte, invalid) system file.  So a loopback stream tracks how many frames
 * it *should* have produced by now (wall clock) and pads the gap with silence,
 * keeping it continuous and aligned with the microphone timeline.
 */
#ifndef AUDIOR_CAPTURE_H
#define AUDIOR_CAPTURE_H

#include "common.h"
#include "ring_buffer.h"
#include "audio_convert.h"

typedef struct {
    IAudioClient        *client;
    IAudioCaptureClient *capture;
    WAVEFORMATEX        *mixFormat;     /* device format, freed on close      */
    HANDLE               thread;
    HANDLE               stopEvent;     /* shared, not owned                  */
    RingBuffer          *ring;          /* destination, not owned             */
    Resampler            resampler;
    INT16               *scratch;       /* reusable conversion output buffer  */
    size_t               scratchFrames;
    BOOL                 loopback;
    float                gain;          /* linear amplitude multiplier        */
    /* Silence-injection bookkeeping (loopback only). */
    LARGE_INTEGER        startQpc;      /* perf counter at capture start      */
    LARGE_INTEGER        qpcFreq;       /* perf counter ticks per second      */
    UINT64               producedFrames;/* canonical frames pushed to ring    */
    HRESULT              threadHr;      /* result reported by the thread      */
} CaptureStream;

/* Initialise (but do not start) capture on `device`.  When `loopback` is
 * TRUE the device is a render endpoint captured via WASAPI loopback.
 * `gain` is a linear amplitude multiplier applied to this stream (1.0 = unity). */
HRESULT Capture_Init(CaptureStream *cs, IMMDevice *device, BOOL loopback,
                     float gain, HANDLE stopEvent, RingBuffer *ring);

/* Start the capture stream and its worker thread. */
HRESULT Capture_Start(CaptureStream *cs);

/* Signal-independent stop: waits for the worker thread to drain and exit.
 * (The caller is expected to have set the shared stop event first.) */
void    Capture_Stop(CaptureStream *cs);

/* Release all resources.  Safe on a zeroed / partially-initialised struct. */
void    Capture_Free(CaptureStream *cs);

#endif /* AUDIOR_CAPTURE_H */
