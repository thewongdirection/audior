/*
 * audio_convert.c - Normalise arbitrary WASAPI capture formats to the single
 *                   canonical PCM format (48 kHz / 16-bit / stereo).
 *
 * Why normalise at all?  A device's mix format can be mono or multichannel,
 * 44.1/48/96 kHz, and 16/24/32-bit int or 32-bit float.  Converting every
 * stream to ONE format up front means the ring buffer, the mixer, and the file
 * writers are all trivially simple and identical for mic and loopback - the
 * complexity is paid once, here.
 *
 * Sample-rate conversion uses linear interpolation.  It is cheap, streaming-
 * friendly, and adequate for speech/screen capture.  The tricky part is that a
 * capture packet does not know the previous packet's last frame, yet linear
 * interpolation across the packet boundary needs it - see AudioConvert_Process
 * for how the "carry" frame and fractional position are threaded between calls.
 */
#include "audio_convert.h"

#include <mmreg.h>
#include <math.h>

/* ------------------------------------------------------------------ */
/* Source format inspection                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    UINT32 channels;
    UINT32 containerBytes;   /* bytes per sample slot = nBlockAlign / channels */
    BOOL   isFloat;
} SrcLayout;

static SrcLayout DescribeFormat(const WAVEFORMATEX *fmt)
{
    SrcLayout s;
    s.channels       = fmt->nChannels ? fmt->nChannels : 1;
    s.containerBytes = fmt->nBlockAlign / s.channels;
    s.isFloat        = FALSE;

    /* Shared-mode mix formats are almost always WAVE_FORMAT_EXTENSIBLE, whose
     * real sample type lives in a sub-format GUID rather than wFormatTag.  We
     * avoid depending on the KSDATAFORMAT_SUBTYPE_* symbols (which aren't in a
     * C-linkable lib) by reading Data1: those GUIDs are of the form
     * {tag,0000,0010,8000,00aa00389b71}, so Data1 IS the classic format tag. */
    if (fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        s.isFloat = TRUE;
    } else if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
               fmt->cbSize >= 22) {
        const WAVEFORMATEXTENSIBLE *ext = (const WAVEFORMATEXTENSIBLE *)fmt;
        if (ext->SubFormat.Data1 == WAVE_FORMAT_IEEE_FLOAT)
            s.isFloat = TRUE;
    }
    return s;
}

/* Decode one sample slot to a normalised float in roughly [-1, 1]. */
static float DecodeSample(const BYTE *p, UINT32 containerBytes, BOOL isFloat)
{
    if (isFloat)
        return *(const float *)p;

    switch (containerBytes) {
    case 1: /* 8-bit PCM is unsigned */
        return ((float)p[0] - 128.0f) / 128.0f;
    case 2:
        return (float)(*(const INT16 *)p) / 32768.0f;
    case 3: {
        INT32 v = (p[0]) | (p[1] << 8) | (p[2] << 16);
        if (v & 0x00800000)
            v |= ~0x00FFFFFF;          /* sign-extend 24 -> 32 */
        return (float)v / 8388608.0f;
    }
    case 4:
        return (float)(*(const INT32 *)p) / 2147483648.0f;
    default:
        return 0.0f;
    }
}

/* Fetch source frame `idx` as a stereo float pair.  idx == -1 returns the
 * carry frame held in the resampler.  `silent` forces a zero result. */
static void FetchFrame(const Resampler *r, const SrcLayout *s, BOOL silent,
                       const BYTE *src, INT32 idx, float *L, float *R)
{
    if (silent) {
        *L = *R = 0.0f;
        return;
    }
    if (idx < 0) {
        *L = r->lastL;
        *R = r->lastR;
        return;
    }

    {
        const BYTE *frame = src + (size_t)idx * s->channels * s->containerBytes;
        *L = DecodeSample(frame, s->containerBytes, s->isFloat);
        *R = (s->channels >= 2)
                 ? DecodeSample(frame + s->containerBytes, s->containerBytes, s->isFloat)
                 : *L;
    }
}

static INT16 ClampToInt16(float v)
{
    float scaled = v * 32767.0f;
    if (scaled >  32767.0f) scaled =  32767.0f;
    if (scaled < -32768.0f) scaled = -32768.0f;
    return (INT16)(scaled >= 0.0f ? scaled + 0.5f : scaled - 0.5f);
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

void Resampler_Init(Resampler *r, UINT32 srcRate, UINT32 dstRate)
{
    r->step  = (double)srcRate / (double)dstRate;
    r->pos   = 0.0;
    r->lastL = 0.0f;
    r->lastR = 0.0f;
}

size_t Resampler_MaxOut(const Resampler *r, UINT32 srcFrames)
{
    /* Worst case (upsampling) produces srcFrames/step output frames; the carry
     * frame extends the usable span by one and +2 absorbs rounding.  Callers
     * size their scratch buffer from this so the loop below can never overflow. */
    return (size_t)(((double)srcFrames + 1.0) / r->step) + 2;
}

/*
 * Streaming linear resampler + format/gain conversion for one packet.
 *
 * Coordinate system: `pos` is a fractional position measured in SOURCE frames,
 * relative to the start of THIS packet.  Index -1 refers to the previous
 * packet's last frame (the "carry", held in r->lastL/lastR), and indices
 * 0..srcFrames-1 are this packet's frames.  Output frame N samples the source
 * at `pos`, then pos advances by `step` (= srcRate/dstRate).
 *
 * We can only interpolate when both floor(pos) and floor(pos)+1 exist, i.e.
 * within [-1, srcFrames-1], which holds while pos < srcFrames-1.  When we run
 * out, we (a) remember frame srcFrames-1 as the next call's carry and (b)
 * rebase pos by subtracting srcFrames, so the leftover fractional offset flows
 * seamlessly into the next packet.  This is what makes packet boundaries
 * inaudible without buffering whole seconds of audio.
 */
size_t AudioConvert_Process(Resampler *r, const WAVEFORMATEX *fmt, BOOL silent,
                            float gain, const BYTE *src, UINT32 srcFrames,
                            INT16 *out, size_t outCapFrames)
{
    SrcLayout layout = DescribeFormat(fmt);
    size_t    outCount = 0;
    double    limit;

    if (srcFrames == 0)
        return 0;

    limit = (double)srcFrames - 1.0;

    /* `outCount < outCapFrames` is a belt-and-suspenders bound; MaxOut already
     * guarantees the scratch is big enough. */
    while (r->pos < limit && outCount < outCapFrames) {
        INT32  i0   = (INT32)floor(r->pos);
        double frac = r->pos - (double)i0;
        float  l0, r0, l1, r1;

        FetchFrame(r, &layout, silent, src, i0,     &l0, &r0);
        FetchFrame(r, &layout, silent, src, i0 + 1, &l1, &r1);

        /* Gain is applied here, in the float domain, BEFORE the int16 clamp so
         * that amplification and clipping happen at full precision. */
        out[outCount * 2 + 0] = ClampToInt16((float)(l0 + frac * (l1 - l0)) * gain);
        out[outCount * 2 + 1] = ClampToInt16((float)(r0 + frac * (r1 - r0)) * gain);
        outCount++;

        r->pos += r->step;
    }

    /* Carry the final source frame and rebase position for the next packet. */
    FetchFrame(r, &layout, silent, src, (INT32)srcFrames - 1, &r->lastL, &r->lastR);
    r->pos -= (double)srcFrames;

    return outCount;
}
