/*
 * writer.h - Output file writers for canonical PCM audio.
 *
 * A single opaque AudioWriter abstracts the two backends:
 *   - WAV: a RIFF/WAVE file written directly with the Win32 file API,
 *   - MP3: encoded through the built-in Media Foundation MP3 sink writer.
 *
 * Callers always feed canonical 48 kHz / 16-bit / stereo PCM via Writer_Write.
 */
#ifndef AUDIOR_WRITER_H
#define AUDIOR_WRITER_H

#include "common.h"

typedef struct AudioWriter AudioWriter;

/* Open `path` for writing in the given format.  Returns a writer in *out. */
HRESULT Writer_Open(AudioWriter **out, const wchar_t *path, AudioFormat format);

/* Append `bytes` of canonical PCM.  May be called repeatedly while recording. */
HRESULT Writer_Write(AudioWriter *w, const BYTE *pcm, size_t bytes);

/* Finalise the file (patch the WAV header / finalise the MP3 stream) and
 * release all resources.  Safe to pass NULL. */
HRESULT Writer_Close(AudioWriter *w);

#endif /* AUDIOR_WRITER_H */
