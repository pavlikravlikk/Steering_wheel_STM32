/**
 * @file    ring_buffer.h
 * @brief   Single-producer / single-consumer byte ring (ISR <-> main loop).
 *
 * Capacity is RB_SIZE - 1 bytes. RB_SIZE must be a power of two <= 256*256.
 * The producer only writes `head`, the consumer only writes `tail`; both are
 * volatile 16-bit (atomic on Cortex-M), so no locking is needed.
 */
#ifndef RING_BUFFER_H
#define RING_BUFFER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RB_SIZE  256U

typedef struct
{
  uint8_t           buf[RB_SIZE];
  volatile uint16_t head;   /* next write index (producer) */
  volatile uint16_t tail;   /* next read index  (consumer) */
} ring_buffer_t;

static inline void rb_init(ring_buffer_t *rb)
{
  rb->head = 0U;
  rb->tail = 0U;
}

/** Producer side. Returns false (byte dropped) when full. */
static inline bool rb_put(ring_buffer_t *rb, uint8_t b)
{
  const uint16_t h = rb->head;
  const uint16_t next = (uint16_t)((h + 1U) & (RB_SIZE - 1U));
  if (next == rb->tail)
  {
    return false;
  }
  rb->buf[h] = b;
  rb->head = next;
  return true;
}

/** Consumer side. Returns false when empty. */
static inline bool rb_get(ring_buffer_t *rb, uint8_t *b)
{
  const uint16_t t = rb->tail;
  if (t == rb->head)
  {
    return false;
  }
  *b = rb->buf[t];
  rb->tail = (uint16_t)((t + 1U) & (RB_SIZE - 1U));
  return true;
}

static inline size_t rb_count(const ring_buffer_t *rb)
{
  return (size_t)((rb->head - rb->tail) & (RB_SIZE - 1U));
}

#endif /* RING_BUFFER_H */
