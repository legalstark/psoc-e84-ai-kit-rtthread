#include <rtthread.h>
#include <string.h>

#include "cy_pdl.h"
#include "ai_kit_psram.h"

#define LOG_TAG "ai.psram"
#include <drv_log.h>

#define AI_KIT_PSRAM_TEST_SIZE    (256U)
#define AI_KIT_PSRAM_TEST_GUARD   (0x00010000UL)
#define AI_KIT_CACHE_LINE_SIZE    (32UL)

static const ai_kit_psram_region_t g_regions[AI_KIT_PSRAM_REGION_COUNT] =
{
    { "m33-private", AI_KIT_PSRAM_M33_PRIVATE_BASE,
      AI_KIT_PSRAM_M33_PRIVATE_SIZE },
    { "m55-private", AI_KIT_PSRAM_M55_PRIVATE_BASE,
      AI_KIT_PSRAM_M55_PRIVATE_SIZE },
    { "m33-m55-shared", AI_KIT_PSRAM_SHARED_BASE,
      AI_KIT_PSRAM_SHARED_SIZE }
};

static struct rt_mutex g_lock;
static rt_bool_t g_ready;
static uintptr_t g_mapped_address;
static rt_size_t g_mapped_size;

const ai_kit_psram_region_t *ai_kit_psram_region(
    ai_kit_psram_region_id_t id)
{
    if ((unsigned int)id >= (unsigned int)AI_KIT_PSRAM_REGION_COUNT)
    {
        return RT_NULL;
    }
    return &g_regions[id];
}

const ai_kit_psram_region_t *ai_kit_psram_local_region(void)
{
#if defined(COMPONENT_CM33)
    return &g_regions[AI_KIT_PSRAM_REGION_M33_PRIVATE];
#elif defined(COMPONENT_CM55)
    return &g_regions[AI_KIT_PSRAM_REGION_M55_PRIVATE];
#else
#error ai_kit_psram requires COMPONENT_CM33 or COMPONENT_CM55
#endif
}

rt_bool_t ai_kit_psram_is_ready(void)
{
    return g_ready;
}

static rt_err_t check_private_range(rt_size_t offset, rt_size_t size)
{
    const ai_kit_psram_region_t *region = ai_kit_psram_local_region();

    if ((size > region->size) || (offset > (region->size - size)))
    {
        return -RT_EINVAL;
    }
    return RT_EOK;
}

#if defined(COMPONENT_CM55) && defined(__DCACHE_PRESENT) && (__DCACHE_PRESENT == 1U)
static void cache_invalidate(uintptr_t address, rt_size_t size)
{
    uintptr_t start;
    uintptr_t end;

    if (size == 0U)
    {
        return;
    }
    start = address & ~(AI_KIT_CACHE_LINE_SIZE - 1UL);
    end = (address + size + AI_KIT_CACHE_LINE_SIZE - 1UL) &
          ~(AI_KIT_CACHE_LINE_SIZE - 1UL);
    SCB_InvalidateDCache_by_Addr((uint32_t *)start, (int32_t)(end - start));
    __DSB();
}

static void cache_clean(uintptr_t address, rt_size_t size)
{
    uintptr_t start;
    uintptr_t end;

    if (size == 0U)
    {
        return;
    }
    start = address & ~(AI_KIT_CACHE_LINE_SIZE - 1UL);
    end = (address + size + AI_KIT_CACHE_LINE_SIZE - 1UL) &
          ~(AI_KIT_CACHE_LINE_SIZE - 1UL);
    __DSB();
    SCB_CleanDCache_by_Addr((uint32_t *)start, (int32_t)(end - start));
    __DSB();
}
#else
static void cache_invalidate(uintptr_t address, rt_size_t size)
{
    (void)address;
    (void)size;
}

static void cache_clean(uintptr_t address, rt_size_t size)
{
    (void)address;
    (void)size;
}
#endif

static void read_raw(uintptr_t address, uint8_t *buffer, rt_size_t size)
{
    volatile const uint8_t *source = (volatile const uint8_t *)address;
    rt_size_t i;

    cache_invalidate(address, size);
    for (i = 0; i < size; i++)
    {
        buffer[i] = source[i];
    }
}

static void write_raw(
    uintptr_t address, const uint8_t *buffer, rt_size_t size)
{
    volatile uint8_t *destination = (volatile uint8_t *)address;
    rt_size_t i;

    for (i = 0; i < size; i++)
    {
        destination[i] = buffer[i];
    }
    cache_clean(address, size);
}

rt_err_t ai_kit_psram_read_private(
    rt_size_t offset, void *buffer, rt_size_t size)
{
    const ai_kit_psram_region_t *region = ai_kit_psram_local_region();

    if ((!g_ready) || (buffer == RT_NULL) ||
        (check_private_range(offset, size) != RT_EOK))
    {
        return -RT_EINVAL;
    }

    rt_mutex_take(&g_lock, RT_WAITING_FOREVER);
    read_raw(region->base + offset, (uint8_t *)buffer, size);
    rt_mutex_release(&g_lock);
    return RT_EOK;
}

rt_err_t ai_kit_psram_write_private(
    rt_size_t offset, const void *buffer, rt_size_t size)
{
    const ai_kit_psram_region_t *region = ai_kit_psram_local_region();

    if ((!g_ready) || (buffer == RT_NULL) ||
        (check_private_range(offset, size) != RT_EOK))
    {
        return -RT_EINVAL;
    }

    rt_mutex_take(&g_lock, RT_WAITING_FOREVER);
    write_raw(region->base + offset, (const uint8_t *)buffer, size);
    rt_mutex_release(&g_lock);
    return RT_EOK;
}

rt_err_t ai_kit_psram_map_private(
    rt_size_t offset, rt_size_t size, volatile void **address)
{
    const ai_kit_psram_region_t *region = ai_kit_psram_local_region();

    if ((!g_ready) || (address == RT_NULL) || (size == 0U) ||
        (check_private_range(offset, size) != RT_EOK))
    {
        return -RT_EINVAL;
    }

    rt_mutex_take(&g_lock, RT_WAITING_FOREVER);
    g_mapped_address = region->base + offset;
    g_mapped_size = size;
    cache_invalidate(g_mapped_address, g_mapped_size);
    *address = (volatile void *)g_mapped_address;
    return RT_EOK;
}

void ai_kit_psram_unmap_private(rt_bool_t modified)
{
    RT_ASSERT(g_mapped_size != 0U);
    if (modified)
    {
        cache_clean(g_mapped_address, g_mapped_size);
    }
    g_mapped_address = 0U;
    g_mapped_size = 0U;
    rt_mutex_release(&g_lock);
}

rt_err_t ai_kit_psram_self_test_private(void)
{
    const ai_kit_psram_region_t *region = ai_kit_psram_local_region();
    const rt_size_t offset = region->size - AI_KIT_PSRAM_TEST_GUARD;
    uint8_t backup[AI_KIT_PSRAM_TEST_SIZE];
    uint8_t pattern[AI_KIT_PSRAM_TEST_SIZE];
    uint8_t verify[AI_KIT_PSRAM_TEST_SIZE];
    rt_err_t result = RT_EOK;
    rt_size_t i;

    if (!g_ready)
    {
        return -RT_ERROR;
    }

    for (i = 0; i < sizeof(pattern); i++)
    {
        pattern[i] = (uint8_t)(0x5AU ^ (uint8_t)i ^
                     (uint8_t)(region->base >> 16));
    }

    rt_mutex_take(&g_lock, RT_WAITING_FOREVER);
    read_raw(region->base + offset, backup, sizeof(backup));
    write_raw(region->base + offset, pattern, sizeof(pattern));
    read_raw(region->base + offset, verify, sizeof(verify));
    if (memcmp(pattern, verify, sizeof(pattern)) != 0)
    {
        result = -RT_ERROR;
    }

    write_raw(region->base + offset, backup, sizeof(backup));
    read_raw(region->base + offset, verify, sizeof(verify));
    if (memcmp(backup, verify, sizeof(backup)) != 0)
    {
        result = -RT_ERROR;
    }
    rt_mutex_release(&g_lock);
    return result;
}

static int ai_kit_psram_init(void)
{
    const ai_kit_psram_region_t *local = ai_kit_psram_local_region();
    uint32_t slot2_ctl = SMIF_DEVICE_CTL(SMIF1_CORE_DEVICE2);

    (void)local;
    rt_mutex_init(&g_lock, "psram", RT_IPC_FLAG_PRIO);
    if ((!Cy_SMIF_IsEnabled(SMIF1_CORE)) ||
        ((SMIF_CTL(SMIF1_CORE) & SMIF_CTL_XIP_MODE_Msk) == 0U) ||
        ((slot2_ctl & SMIF_DEVICE_CTL_ENABLED_Msk) == 0U) ||
        ((slot2_ctl & SMIF_DEVICE_CTL_WR_EN_Msk) == 0U))
    {
        LOG_E("SMIF1/Slot2 is not ready for PSRAM XIP");
        return -RT_ERROR;
    }

    g_ready = RT_TRUE;
    LOG_I("%s ready: 0x%08x / %u KiB",
          local->name, (unsigned int)local->base,
          (unsigned int)(local->size / 1024U));
    return RT_EOK;
}
INIT_BOARD_EXPORT(ai_kit_psram_init);

#ifdef RT_USING_FINSH
#include <finsh.h>

static int psram_layout(int argc, char **argv)
{
    unsigned int i;

    (void)argc;
    (void)argv;
    rt_kprintf("AI Kit PSRAM ready=%u local=%s\n",
               g_ready ? 1U : 0U,
               ai_kit_psram_local_region()->name);
    for (i = 0; i < (unsigned int)AI_KIT_PSRAM_REGION_COUNT; i++)
    {
        rt_kprintf("  %-14s 0x%08x / %u KiB\n",
                   g_regions[i].name,
                   (unsigned int)g_regions[i].base,
                   (unsigned int)(g_regions[i].size / 1024U));
    }
    return 0;
}
MSH_CMD_EXPORT(psram_layout, show AI Kit PSRAM regions and local ownership);

static int psram_self_test(int argc, char **argv)
{
    rt_err_t result;

    (void)argc;
    (void)argv;
    result = ai_kit_psram_self_test_private();
    rt_kprintf("PSRAM %s private self-test: %s (%d)\n",
               ai_kit_psram_local_region()->name,
               (result == RT_EOK) ? "PASS" : "FAIL", result);
    return (int)result;
}
MSH_CMD_EXPORT(psram_self_test, test and restore the local private PSRAM region);
#endif
