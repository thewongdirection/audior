/*
 * audio_convert.h - Convert arbitrary WASAPI capture formats to the
 *                   canonical 48 kHz / 16-bit / stereo PCM used internally.
 *
 * Handles:
 *   - sample formats: 8/16/24/32-bit PCM and 32-bit IEEE float,
 *     including WAVE_FORMAT_EXTENSIBLE,
 *   - channel folding (mono is duplicated; >2 channels keep the first two),
 *   - sample-rate conversion via linear interpolation.
 *
 * A Resampler holds the per-stream interpolation state so that consecutive
 * capture packets join seamlessly across buffer boundaries.
 */
#ifndef AUDIOR_AUDIO_CONVERT_H
#define AUDIOR_AUDIO_CONVERT_H

#include "common.h"

typedef struct {
    double step;     /* source frames advanced per output frame (src/dst) */
    double pos;      /* fractional source position, current-packet coords  */
    float  lastL;    /* carry: last source frame of the previous packet    */
    float  lastR;
} Resampler;

/* Initialise for the given source and destination sample rates. */
void Resampler_Init(Resampler *r, UINT32 srcRate, UINT32 dstRate);

/* Upper bound on output frames produced from `srcFrames` input frames,
 * used to size the destination scratch buffer. */
size_t Resampler_MaxOut(const Resampler *r, UINT32 srcFrames);

/*
 * Convert one packet of `srcFrames` source frames into canonical interleaved
 * int16 stereo frames written to `out`.  If `silent` is TRUE the source data
 * is treated as silence (the WASAPI silent-buffer flag).  `gain` is a linear
 * amplitude multiplier applied before clamping (1.0 = unity).  Returns the
 * number of output frames written; never exceeds `outCapFrames`.
 */
size_t AudioConvert_Process(Resampler *r, const WAVEFORMATEX *fmt, BOOL silent,
                            float gain, const BYTE *src, UINT32 srcFrames,
                            INT16 *out, size_t outCapFrames);

#endif /* AUDIOR_AUDIO_CONVERT_H */
