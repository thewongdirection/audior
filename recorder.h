/*
 * recorder.h - Recording session orchestration.
 *
 * Wires together device selection, capture streams, ring buffers and file
 * writers, then runs the foreground record loop until the user presses Enter.
 */
#ifndef AUDIOR_RECORDER_H
#define AUDIOR_RECORDER_H

#include "common.h"

/* Run a complete recording session described by `cfg`.
 * Returns 0 on success, non-zero on failure. */
int Recorder_Run(RecordConfig *cfg);

#endif /* AUDIOR_RECORDER_H */
