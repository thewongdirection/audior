/*
 * common.h - Shared types, constants and includes for audior.
 *
 * audior is a Windows 10/11 command-line audio recorder built on WASAPI
 * (capture) and Media Foundation (MP3 encoding).  It depends only on the
 * Windows SDK and the C runtime - no third-party libraries.
 *
 * All audio is normalised to a single canonical PCM format internally so
 * that the microphone and (optional) system-loopback streams can be mixed
 * or written independently with uniform code paths.
 */
#ifndef AUDIOR_COMMON_H
#define AUDIOR_COMMON_H

/* COBJMACROS lets us call COM methods in C as IFoo_Method(p, ...) instead of
 * the verbose p->lpVtbl->Method(p, ...).  There is no C++ here, so this is how
 * we talk to WASAPI and Media Foundation interfaces. */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* Canonical internal audio format                                     */
/* ------------------------------------------------------------------ */
/* Every captured stream is converted to this format before buffering, which
 * lets the ring buffer, mixer and writers stay format-agnostic.
 *
 * 48 kHz / 16-bit / stereo is chosen deliberately:
 *   - 48 kHz is the default shared-mode mix rate on modern Windows, so in the
 *     common case no resampling is needed at all.
 *   - Both WAV and the MP3 encoder accept it directly.
 *   - 16-bit stereo is universally playable and keeps the files reasonable. */
#define CANON_SAMPLE_RATE   48000u
#define CANON_CHANNELS      2u
#define CANON_BITS          16u
#define CANON_BYTES_PER_FRAME  (CANON_CHANNELS * (CANON_BITS / 8u))   /* 4 */

/* Named event used so a second 'audior --stop' instance can end a recording. */
#define STOP_EVENT_NAME     L"Local\\AudiorStopEvent"

/* ------------------------------------------------------------------ */
/* Configuration produced by command-line parsing                      */
/* ------------------------------------------------------------------ */

typedef enum {
    FORMAT_WAV = 0,
    FORMAT_MP3 = 1
} AudioFormat;

typedef enum {
    MIX_PROMPT   = -1,   /* ask the user (loopback enabled, no choice given) */
    MIX_MERGE    =  0,   /* mic + system mixed into one file                */
    MIX_SEPARATE =  1    /* mic and system written to two files             */
} MixMode;

/* Amplification and merge-weighting limits.
 *   amplifyPercent maps to gain = 1 + percent/100, so 0 = unity (1x),
 *   100 = 2x, and the max 300 = 4x.
 *   mergeMicPercent is the microphone's share of a merged mix (0..100); the
 *   system audio gets the remaining (100 - mergeMicPercent).  65 keeps the
 *   original voice-favoured default. */
#define MAX_AMPLIFY_PERCENT        300
#define DEFAULT_MERGE_MIC_PERCENT   65

typedef struct {
    wchar_t     outputPath[MAX_PATH];
    AudioFormat format;
    BOOL        includeOutput;              /* also capture system loopback     */
    MixMode     mixMode;
    wchar_t     micSelector[256];           /* empty => system default          */
    wchar_t     renderSelector[256];        /* empty => system default          */
    int         amplifyPercent;             /* mic gain: 0 = none .. 300 = 4x   */
    int         mergeMicPercent;            /* mic share of merge: 0..100 (=65) */
} RecordConfig;

/* ------------------------------------------------------------------ */
/* Small shared helpers                                                */
/* ------------------------------------------------------------------ */

#define SAFE_RELEASE(p)                                              \
    do {                                                             \
        if (p) {                                                     \
            ((IUnknown *)(p))->lpVtbl->Release((IUnknown *)(p));     \
            (p) = NULL;                                              \
        }                                                            \
    } while (0)

#define CHECK_HR(hr, msg)                                            \
    do {                                                             \
        if (FAILED(hr)) {                                            \
            fprintf(stderr, "%s (hr=0x%08lX)\n",                     \
                    (msg), (unsigned long)(hr));                     \
            goto cleanup;                                            \
        }                                                            \
    } while (0)

#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif

#endif /* AUDIOR_COMMON_H */
