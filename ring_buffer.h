/*
 * ring_buffer.h - Thread-safe circular byte buffer.
 *
 * A capture thread writes canonical PCM bytes; the recorder's drain loop
 * reads them and flushes to disk.  This decouples real-time audio capture
 * from (potentially slow) file I/O.
 */
#ifndef AUDIOR_RING_BUFFER_H
#define AUDIOR_RING_BUFFER_H

#include "common.h"

typedef struct {
    BYTE             *data;
    size_t            capacity;
    size_t            head;        /* write offset */
    size_t            tail;        /* read offset  */
    size_t            used;
    CRITICAL_SECTION  lock;
    BOOL              initialised;
} RingBuffer;

/* Allocate a buffer of `capacity` bytes.  Returns FALSE on allocation failure. */
BOOL   RingBuffer_Init(RingBuffer *rb, size_t capacity);

/* Release all resources.  Safe to call on a zeroed / partially-initialised rb. */
void   RingBuffer_Free(RingBuffer *rb);

/* Copy up to `count` bytes in.  Returns the number actually stored; a value
 * smaller than `count` means the buffer was full and data was dropped. */
size_t RingBuffer_Write(RingBuffer *rb, const BYTE *src, size_t count);

/* Copy up to `count` bytes out.  Returns the number actually read. */
size_t RingBuffer_Read(RingBuffer *rb, BYTE *dst, size_t count);

/* Current number of buffered bytes. */
size_t RingBuffer_Used(RingBuffer *rb);

#endif /* AUDIOR_RING_BUFFER_H */
