#ifndef AI_KIT_PORTABLE_STREAM_H
#define AI_KIT_PORTABLE_STREAM_H

#include <stdint.h>

#define AI_KIT_PORTABLE_STREAM_BUFFER_BYTES (64UL * 1024UL)
#define AI_KIT_PORTABLE_STREAM_WORDS        \
    (AI_KIT_PORTABLE_STREAM_BUFFER_BYTES / sizeof(uint32_t))
#define AI_KIT_PORTABLE_STREAM_PASSES       (2048UL)
#define AI_KIT_PORTABLE_STREAM_BYTES_PER_OP \
    ((uint64_t)AI_KIT_PORTABLE_STREAM_BUFFER_BYTES * \
     AI_KIT_PORTABLE_STREAM_PASSES)

/*
 * Volatile 32-bit accesses intentionally define the benchmark kernel.
 * They prevent either compiler from replacing the loops with libc calls or
 * architecture-specific vector code, while keeping the exact same C source
 * on CM33 and CM55.
 */
static inline uint32_t ai_kit_portable_stream_read(
    const volatile uint32_t *source)
{
    uint32_t pass;
    uint32_t index;
    uint32_t checksum = 0U;

    for (pass = 0U; pass < AI_KIT_PORTABLE_STREAM_PASSES; pass++)
    {
        for (index = 0U; index < AI_KIT_PORTABLE_STREAM_WORDS; index++)
        {
            checksum += source[index];
        }
    }
    return checksum;
}

static inline void ai_kit_portable_stream_write(
    volatile uint32_t *destination)
{
    uint32_t pass;
    uint32_t index;

    for (pass = 0U; pass < AI_KIT_PORTABLE_STREAM_PASSES; pass++)
    {
        uint32_t pattern = 0xA5A50000UL ^ pass;

        for (index = 0U; index < AI_KIT_PORTABLE_STREAM_WORDS; index++)
        {
            destination[index] = pattern;
        }
    }
}

static inline void ai_kit_portable_stream_copy(
    volatile uint32_t *destination,
    const volatile uint32_t *source)
{
    uint32_t pass;
    uint32_t index;

    for (pass = 0U; pass < AI_KIT_PORTABLE_STREAM_PASSES; pass++)
    {
        for (index = 0U; index < AI_KIT_PORTABLE_STREAM_WORDS; index++)
        {
            destination[index] = source[index];
        }
    }
}

#endif
