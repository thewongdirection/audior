/*
 * ring_buffer.c - Thread-safe circular byte buffer.
 *
 * Why it exists: audio capture is real-time (a stalled capture thread drops
 * samples), but disk/encoder writes can block unpredictably.  The capture
 * thread must therefore never call the writer directly.  Instead it writes
 * canonical PCM into this buffer and returns immediately; a separate drain
 * loop reads from the buffer and does the slow file I/O.  The buffer is sized
 * for several seconds, so a transient disk hiccup is absorbed rather than lost.
 *
 * On overflow, Write stores only what fits and reports the shortfall (i.e. it
 * drops the newest data).  With a multi-second buffer and continuous draining
 * this effectively never happens, but dropping is the correct real-time
 * behaviour: never block the capture thread.
 */
#include "ring_buffer.h"

#include <string.h>

BOOL RingBuffer_Init(RingBuffer *rb, size_t capacity)
{
    memset(rb, 0, sizeof(*rb));
    rb->data = (BYTE *)malloc(capacity);
    if (!rb->data)
        return FALSE;
    rb->capacity = capacity;
    InitializeCriticalSection(&rb->lock);
    rb->initialised = TRUE;
    return TRUE;
}

void RingBuffer_Free(RingBuffer *rb)
{
    if (!rb)
        return;
    if (rb->initialised)
        DeleteCriticalSection(&rb->lock);
    free(rb->data);
    memset(rb, 0, sizeof(*rb));
}

size_t RingBuffer_Write(RingBuffer *rb, const BYTE *src, size_t count)
{
    size_t stored = 0;

    EnterCriticalSection(&rb->lock);
    {
        size_t space = rb->capacity - rb->used;
        size_t n     = MIN(count, space);
        size_t first = MIN(n, rb->capacity - rb->head);

        memcpy(rb->data + rb->head, src, first);
        if (n > first)
            memcpy(rb->data, src + first, n - first);

        rb->head  = (rb->head + n) % rb->capacity;
        rb->used += n;
        stored    = n;
    }
    LeaveCriticalSection(&rb->lock);

    return stored;
}

size_t RingBuffer_Read(RingBuffer *rb, BYTE *dst, size_t count)
{
    size_t fetched = 0;

    EnterCriticalSection(&rb->lock);
    {
        size_t n     = MIN(count, rb->used);
        size_t first = MIN(n, rb->capacity - rb->tail);

        memcpy(dst, rb->data + rb->tail, first);
        if (n > first)
            memcpy(dst + first, rb->data, n - first);

        rb->tail  = (rb->tail + n) % rb->capacity;
        rb->used -= n;
        fetched   = n;
    }
    LeaveCriticalSection(&rb->lock);

    return fetched;
}

size_t RingBuffer_Used(RingBuffer *rb)
{
    size_t used;
    EnterCriticalSection(&rb->lock);
    used = rb->used;
    LeaveCriticalSection(&rb->lock);
    return used;
}
