#ifndef AI_KIT_PSRAM_H
#define AI_KIT_PSRAM_H

#include <rtthread.h>
#include <stdint.h>

#define AI_KIT_PSRAM_M33_PRIVATE_BASE  (0x64000000UL)
#define AI_KIT_PSRAM_M33_PRIVATE_SIZE  (0x00200000UL)
#define AI_KIT_PSRAM_M55_PRIVATE_BASE  (0x64200000UL)
#define AI_KIT_PSRAM_M55_PRIVATE_SIZE  (0x00200000UL)
#define AI_KIT_PSRAM_SHARED_BASE       (0x64400000UL)
#define AI_KIT_PSRAM_SHARED_SIZE       (0x00C00000UL)

typedef enum
{
    AI_KIT_PSRAM_REGION_M33_PRIVATE = 0,
    AI_KIT_PSRAM_REGION_M55_PRIVATE,
    AI_KIT_PSRAM_REGION_SHARED,
    AI_KIT_PSRAM_REGION_COUNT
} ai_kit_psram_region_id_t;

typedef struct
{
    const char *name;
    uintptr_t base;
    rt_size_t size;
} ai_kit_psram_region_t;

rt_bool_t ai_kit_psram_is_ready(void);
const ai_kit_psram_region_t *ai_kit_psram_region(
    ai_kit_psram_region_id_t id);
const ai_kit_psram_region_t *ai_kit_psram_local_region(void);
rt_err_t ai_kit_psram_read_private(
    rt_size_t offset, void *buffer, rt_size_t size);
rt_err_t ai_kit_psram_write_private(
    rt_size_t offset, const void *buffer, rt_size_t size);
rt_err_t ai_kit_psram_map_private(
    rt_size_t offset, rt_size_t size, volatile void **address);
void ai_kit_psram_unmap_private(rt_bool_t modified);
rt_err_t ai_kit_psram_self_test_private(void);

#endif
