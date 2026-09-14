/**
 * @file    ai_kit_memory_benchmark.c
 * @brief   AI Kit CM55 侧内存流式基准测试实现
 *
 * @details 该文件实现三类内存基准：
 *          - ai_kit_memory_benchmark_run         ：SRAM-DMA 加速版（memset/memcpy）
 *          - ai_kit_memory_portable_benchmark_run：SRAM 标量版（portable_stream）
 *          - ai_kit_psram_portable_benchmark_run ：PSRAM 标量版（含数据备份/恢复）
 *
 *          所有测试均使用 DWT CYCCNT 周期计数器测量耗时，结合
 *          SCB_CleanInvalidateDCache_by_Addr 保证 cache 一致性，最终
 *          输出 ai_kit_memory_metrics_t 给 ai_kit_benchmark.c 汇聚到目录。
 */
#include <stdint.h>
#include <string.h>

#include <rtthread.h>

#include "board.h"
#include "ai_kit_memory_benchmark.h"
#include "ai_kit_portable_stream.h"
#include "ai_kit_psram.h"

/** @brief 可用 SRAM 起始地址（CM55 侧私有 SRAM 区） */
#define AI_KIT_SRAM_START             (0x26040000UL)
/** @brief 可用 SRAM 结束地址（exclusive） */
#define AI_KIT_SRAM_END               (0x262FC000UL)
/** @brief 单次操作的缓冲字节数（256 KiB） */
#define AI_KIT_MEMORY_BUFFER_BYTES    (256UL * 1024UL)
/** @brief 缓冲对齐字节数（32B，匹配 cache line + DMA 要求） */
#define AI_KIT_MEMORY_ALIGNMENT       (32UL)
/** @brief 测试循环次数（每个操作跑 512 遍） */
#define AI_KIT_MEMORY_PASSES          (512UL)
/** @brief 缓冲中的 32 位字数 */
#define AI_KIT_MEMORY_WORDS           (AI_KIT_MEMORY_BUFFER_BYTES / 4UL)
/** @brief 编译器内存屏障，防止编译器把循环合并/省略 */
#define AI_KIT_COMPILER_MEMORY_BARRIER() __asm volatile ("" ::: "memory")
/** @brief PSRAM 测试所需字节数（源+目标两份 portable_stream 缓冲） */
#define AI_KIT_PSRAM_BENCHMARK_BYTES   \
    (2UL * AI_KIT_PORTABLE_STREAM_BUFFER_BYTES)

/** @brief 读循环计算的 checksum（volatile 防止编译器消除读循环） */
static volatile uint32_t s_read_checksum;

/**
 * @brief   启用 DWT CYCCNT 周期计数器
 *
 * @details 使能 TRCENA + 复位 CYCCNT + 使能 CYCCNTENA，跑 128 NOP
 *          验证计数器自增。失败表示调试器件未使能或被锁。
 *
 * @retval  1  计数器正常自增
 * @retval  0  计数器未启动
 */
static int ai_kit_memory_timer_enable(void)
{
    uint32_t start;
    uint32_t spin;

    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0U;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    __DSB();
    __ISB();

    start = DWT->CYCCNT;
    for (spin = 0U; spin < 128U; spin++)
    {
        __NOP();
    }
    return DWT->CYCCNT != start;
}

/**
 * @brief   由字节数与周期数计算 MiB/s 带宽（×1000）
 *
 * @details bandwidth_x1000 = bytes_KiB * SystemCoreClock * 1000 /
 *          (cycles * 1024)，四舍五入到最近整数。结果×1000 即
 *          MiB/s。
 *
 * @param   bytes   传输字节数
 * @param   cycles  DWT CYCCNT 差值
 *
 * @return  带宽（MiB/s × 1000）；cycles=0 或时钟=0 返回 0
 */
static uint32_t ai_kit_memory_mib_s_x1000(uint64_t bytes,
                                          uint32_t cycles)
{
    uint64_t kib;
    uint64_t numerator;
    uint64_t denominator;

    if ((cycles == 0U) || (SystemCoreClock == 0U))
    {
        return 0U;
    }

    kib = bytes / 1024ULL;
    numerator = kib * (uint64_t)SystemCoreClock * 1000ULL;
    denominator = (uint64_t)cycles * 1024ULL;
    return (uint32_t)((numerator + (denominator / 2ULL)) / denominator);
}

/**
 * @brief   把 CPU 周期数换算为微秒
 *
 * @param   cycles  DWT CYCCNT 差值
 *
 * @return  对应的微秒数；时钟为 0 时返回 0
 */
static uint32_t ai_kit_memory_elapsed_us(uint32_t cycles)
{
    return (uint32_t)(((uint64_t)cycles * 1000000ULL +
                       ((uint64_t)SystemCoreClock / 2ULL)) /
                      (uint64_t)SystemCoreClock);
}

/**
 * @brief   读循环：累加 source 所有字以测量读带宽
 *
 * @details 跑 passes 遍，每遍累加整个缓冲的 32 位字到 checksum，
 *          写入 s_read_checksum 防止编译器消除循环。返回 checksum
 *          用于校验数据完整性。
 *
 * @param   source  源缓冲（uint32_t 视角）
 * @param   passes  循环次数
 *
 * @return  所有字的累加和
 */
static uint32_t ai_kit_memory_read(uint32_t *source, uint32_t passes)
{
    uint32_t pass;
    uint32_t index;
    uint32_t checksum = 0U;

    for (pass = 0U; pass < passes; pass++)
    {
        for (index = 0U; index < AI_KIT_MEMORY_WORDS; index++)
        {
            checksum += source[index];
        }
    }
    s_read_checksum = checksum;
    return checksum;
}

/**
 * @brief   校验缓冲是否落在合法 SRAM 范围内
 *
 * @param   buffer  缓冲起始地址
 * @param   size    缓冲字节数
 *
 * @retval  1  在 [AI_KIT_SRAM_START, AI_KIT_SRAM_END) 内
 * @retval  0  越界或 size 溢出
 */
static int ai_kit_memory_pointer_valid(const void *buffer, uint32_t size)
{
    uintptr_t begin = (uintptr_t)buffer;
    uintptr_t end = begin + size;

    return (begin >= AI_KIT_SRAM_START) &&
           (end > begin) &&
           (end <= AI_KIT_SRAM_END);
}

/**
 * @brief   运行 SRAM 流式内存基准（DMA 加速版，对外接口）
 *
 * @details 流程：
 *          - rt_malloc_align 分配 source/destination（256 KiB，32B 对齐）
 *          - 校验缓冲在合法 SRAM 范围
 *          - 启用 DWT CYCCNT
 *          - 写入已知模式 0x9E3779B9^index 并预算 expected_sum
 *          - 跑读循环，校验 checksum 一致
 *          - 跑 memset(0xA5^pass) 写循环，校验首尾模式值
 *          - 跑 memcpy 复制循环，校验内容一致
 *          - 填充 metrics
 *          - cleanup 释放缓冲
 *
 * @param   metrics  输出参数
 *
 * @retval  RT_EOK       成功
 * @retval  -RT_EINVAL   metrics 为空
 * @retval  -RT_ENOMEM   rt_malloc_align 失败
 * @retval  -RT_ERROR    越界 / 校验失败
 * @retval  -RT_ETIMEOUT DWT 未启动
 */
int ai_kit_memory_benchmark_run(ai_kit_memory_metrics_t *metrics)
{
    uint32_t *source;
    uint32_t *destination;
    uint32_t index;
    uint32_t pass;
    uint32_t expected_sum = 0U;
    uint32_t read_cycles;
    uint32_t write_cycles;
    uint32_t copy_cycles;
    uint32_t start;
    uint8_t final_pattern;
    uint64_t transferred_bytes =
        (uint64_t)AI_KIT_MEMORY_BUFFER_BYTES * AI_KIT_MEMORY_PASSES;
    int result = RT_EOK;

    if (metrics == RT_NULL)
    {
        return -RT_EINVAL;
    }
    rt_memset(metrics, 0, sizeof(*metrics));

    source = (uint32_t *)rt_malloc_align(AI_KIT_MEMORY_BUFFER_BYTES,
                                         AI_KIT_MEMORY_ALIGNMENT);
    destination = (uint32_t *)rt_malloc_align(AI_KIT_MEMORY_BUFFER_BYTES,
                                              AI_KIT_MEMORY_ALIGNMENT);
    if ((source == RT_NULL) || (destination == RT_NULL))
    {
        result = -RT_ENOMEM;
        goto cleanup;
    }
    if (!ai_kit_memory_pointer_valid(source, AI_KIT_MEMORY_BUFFER_BYTES) ||
        !ai_kit_memory_pointer_valid(destination, AI_KIT_MEMORY_BUFFER_BYTES))
    {
        result = -RT_ERROR;
        goto cleanup;
    }
    if (!ai_kit_memory_timer_enable())
    {
        result = -RT_ETIMEOUT;
        goto cleanup;
    }

    for (index = 0U; index < AI_KIT_MEMORY_WORDS; index++)
    {
        source[index] = 0x9E3779B9UL ^ index;
        expected_sum += source[index];
    }
    expected_sum *= AI_KIT_MEMORY_PASSES;

    SCB_CleanInvalidateDCache_by_Addr(
        source, (int32_t)AI_KIT_MEMORY_BUFFER_BYTES);
    __DSB();
    start = DWT->CYCCNT;
    if (ai_kit_memory_read(source, AI_KIT_MEMORY_PASSES) != expected_sum)
    {
        result = -RT_ERROR;
        goto cleanup;
    }
    read_cycles = DWT->CYCCNT - start;

    SCB_CleanInvalidateDCache_by_Addr(
        destination, (int32_t)AI_KIT_MEMORY_BUFFER_BYTES);
    __DSB();
    start = DWT->CYCCNT;
    for (pass = 0U; pass < AI_KIT_MEMORY_PASSES; pass++)
    {
        memset(destination, (int)(0xA5U ^ (uint8_t)pass),
               AI_KIT_MEMORY_BUFFER_BYTES);
        AI_KIT_COMPILER_MEMORY_BARRIER();
    }
    __DSB();
    write_cycles = DWT->CYCCNT - start;
    final_pattern = (uint8_t)(0xA5U ^
                              (uint8_t)(AI_KIT_MEMORY_PASSES - 1U));
    if ((((uint8_t *)destination)[0] != final_pattern) ||
        (((uint8_t *)destination)[AI_KIT_MEMORY_BUFFER_BYTES - 1U] !=
         final_pattern))
    {
        result = -RT_ERROR;
        goto cleanup;
    }

    SCB_CleanInvalidateDCache_by_Addr(
        source, (int32_t)AI_KIT_MEMORY_BUFFER_BYTES);
    SCB_CleanInvalidateDCache_by_Addr(
        destination, (int32_t)AI_KIT_MEMORY_BUFFER_BYTES);
    __DSB();
    start = DWT->CYCCNT;
    for (pass = 0U; pass < AI_KIT_MEMORY_PASSES; pass++)
    {
        memcpy(destination, source, AI_KIT_MEMORY_BUFFER_BYTES);
        AI_KIT_COMPILER_MEMORY_BARRIER();
    }
    __DSB();
    copy_cycles = DWT->CYCCNT - start;
    if (memcmp(destination, source, AI_KIT_MEMORY_BUFFER_BYTES) != 0)
    {
        result = -RT_ERROR;
        goto cleanup;
    }

    metrics->buffer_bytes = AI_KIT_MEMORY_BUFFER_BYTES;
    metrics->passes = AI_KIT_MEMORY_PASSES;
    metrics->elapsed_us = ai_kit_memory_elapsed_us(read_cycles) +
                          ai_kit_memory_elapsed_us(write_cycles) +
                          ai_kit_memory_elapsed_us(copy_cycles);
    metrics->read_mib_s_x1000 =
        ai_kit_memory_mib_s_x1000(transferred_bytes, read_cycles);
    metrics->write_mib_s_x1000 =
        ai_kit_memory_mib_s_x1000(transferred_bytes, write_cycles);
    metrics->copy_mib_s_x1000 =
        ai_kit_memory_mib_s_x1000(transferred_bytes, copy_cycles);
    metrics->source_address = (uint32_t)(uintptr_t)source;
    metrics->destination_address = (uint32_t)(uintptr_t)destination;

cleanup:
    if (destination != RT_NULL)
    {
        rt_free_align(destination);
    }
    if (source != RT_NULL)
    {
        rt_free_align(source);
    }
    return result;
}

/**
 * @brief   运行 SRAM 标量内存基准（对外接口）
 *
 * @details 与 ai_kit_memory_benchmark_run 类似，但调用
 *          ai_kit_portable_stream_read/write/copy 走标量 C 实现，
 *          缓冲规模由 AI_KIT_PORTABLE_STREAM_* 决定。校验：
 *          - 读循环 checksum 一致
 *          - 写循环 destination 与 expected_pattern 一致
 *          - 复制循环 destination 与 source 内容一致
 */
int ai_kit_memory_portable_benchmark_run(ai_kit_memory_metrics_t *metrics)
{
    uint32_t *source = RT_NULL;
    uint32_t *destination = RT_NULL;
    uint32_t index;
    uint32_t expected_sum = 0U;
    uint32_t expected_pattern =
        0xA5A50000UL ^ (AI_KIT_PORTABLE_STREAM_PASSES - 1U);
    uint32_t read_cycles;
    uint32_t write_cycles;
    uint32_t copy_cycles;
    uint32_t start;
    int result = RT_EOK;

    if (metrics == RT_NULL)
    {
        return -RT_EINVAL;
    }
    rt_memset(metrics, 0, sizeof(*metrics));

    source = (uint32_t *)rt_malloc_align(
        AI_KIT_PORTABLE_STREAM_BUFFER_BYTES, AI_KIT_MEMORY_ALIGNMENT);
    destination = (uint32_t *)rt_malloc_align(
        AI_KIT_PORTABLE_STREAM_BUFFER_BYTES, AI_KIT_MEMORY_ALIGNMENT);
    if ((source == RT_NULL) || (destination == RT_NULL))
    {
        result = -RT_ENOMEM;
        goto cleanup;
    }
    if (!ai_kit_memory_pointer_valid(
            source, AI_KIT_PORTABLE_STREAM_BUFFER_BYTES) ||
        !ai_kit_memory_pointer_valid(
            destination, AI_KIT_PORTABLE_STREAM_BUFFER_BYTES))
    {
        result = -RT_ERROR;
        goto cleanup;
    }
    if (!ai_kit_memory_timer_enable())
    {
        result = -RT_ETIMEOUT;
        goto cleanup;
    }

    for (index = 0U; index < AI_KIT_PORTABLE_STREAM_WORDS; index++)
    {
        source[index] = 0x9E3779B9UL ^ index;
        expected_sum += source[index];
    }
    expected_sum *= AI_KIT_PORTABLE_STREAM_PASSES;

    SCB_CleanInvalidateDCache_by_Addr(
        source, (int32_t)AI_KIT_PORTABLE_STREAM_BUFFER_BYTES);
    __DSB();
    start = DWT->CYCCNT;
    if (ai_kit_portable_stream_read(source) != expected_sum)
    {
        result = -RT_ERROR;
        goto cleanup;
    }
    read_cycles = DWT->CYCCNT - start;

    SCB_CleanInvalidateDCache_by_Addr(
        destination, (int32_t)AI_KIT_PORTABLE_STREAM_BUFFER_BYTES);
    __DSB();
    start = DWT->CYCCNT;
    ai_kit_portable_stream_write(destination);
    __DSB();
    write_cycles = DWT->CYCCNT - start;
    for (index = 0U; index < AI_KIT_PORTABLE_STREAM_WORDS; index++)
    {
        if (destination[index] != expected_pattern)
        {
            result = -RT_ERROR;
            goto cleanup;
        }
    }

    SCB_CleanInvalidateDCache_by_Addr(
        source, (int32_t)AI_KIT_PORTABLE_STREAM_BUFFER_BYTES);
    SCB_CleanInvalidateDCache_by_Addr(
        destination, (int32_t)AI_KIT_PORTABLE_STREAM_BUFFER_BYTES);
    __DSB();
    start = DWT->CYCCNT;
    ai_kit_portable_stream_copy(destination, source);
    __DSB();
    copy_cycles = DWT->CYCCNT - start;
    if (memcmp(destination, source,
               AI_KIT_PORTABLE_STREAM_BUFFER_BYTES) != 0)
    {
        result = -RT_ERROR;
        goto cleanup;
    }

    metrics->buffer_bytes = AI_KIT_PORTABLE_STREAM_BUFFER_BYTES;
    metrics->passes = AI_KIT_PORTABLE_STREAM_PASSES;
    metrics->elapsed_us = ai_kit_memory_elapsed_us(read_cycles) +
                          ai_kit_memory_elapsed_us(write_cycles) +
                          ai_kit_memory_elapsed_us(copy_cycles);
    metrics->read_mib_s_x1000 = ai_kit_memory_mib_s_x1000(
        AI_KIT_PORTABLE_STREAM_BYTES_PER_OP, read_cycles);
    metrics->write_mib_s_x1000 = ai_kit_memory_mib_s_x1000(
        AI_KIT_PORTABLE_STREAM_BYTES_PER_OP, write_cycles);
    metrics->copy_mib_s_x1000 = ai_kit_memory_mib_s_x1000(
        AI_KIT_PORTABLE_STREAM_BYTES_PER_OP, copy_cycles);
    metrics->source_address = (uint32_t)(uintptr_t)source;
    metrics->destination_address = (uint32_t)(uintptr_t)destination;

cleanup:
    if (destination != RT_NULL)
    {
        rt_free_align(destination);
    }
    if (source != RT_NULL)
    {
        rt_free_align(source);
    }
    return result;
}

/**
 * @brief   从 volatile 源逐字节拷贝到普通目的（避免编译器优化为 memcpy）
 *
 * @details 用于把 PSRAM 映射区的原始数据备份到 SRAM 内的普通缓冲，
 *          volatile 修饰防止编译器把循环合并为 DMA/memcpy 调用，
 *          确保逐字节走 load/store。
 *
 * @param   destination  目的缓冲
 * @param   source       源缓冲（volatile）
 * @param   size         字节数
 */
static void ai_kit_copy_from_volatile(uint8_t *destination,
                                      const volatile uint8_t *source,
                                      uint32_t size)
{
    uint32_t index;

    for (index = 0U; index < size; index++)
    {
        destination[index] = source[index];
    }
}

/**
 * @brief   从普通源逐字节拷贝到 volatile 目的
 *
 * @details 用于把 SRAM 中的备份数据恢复到 PSRAM 映射区。volatile 修饰
 *          同样防止编译器优化为 DMA 调用。
 *
 * @param   destination  目的缓冲（volatile）
 * @param   source       源缓冲
 * @param   size         字节数
 */
static void ai_kit_copy_to_volatile(volatile uint8_t *destination,
                                    const uint8_t *source,
                                    uint32_t size)
{
    uint32_t index;

    for (index = 0U; index < size; index++)
    {
        destination[index] = source[index];
    }
}

/**
 * @brief   运行 PSRAM 标量内存基准（对外接口）
 *
 * @details 流程：
 *          - ai_kit_psram_is_ready 检查 PSRAM 可用
 *          - rt_malloc backup 缓冲（用于保存原数据）
 *          - ai_kit_psram_map_private 映射私有区域，前半为 source 后半为 destination
 *          - 备份原数据到 backup
 *          - 启用 DWT CYCCNT
 *          - 跑标量读 / 写 / 复制，校验一致
 *          - 填充 metrics
 *          - cleanup：从 backup 恢复 PSRAM 数据 → unmap →
 *            ai_kit_psram_read_private 分段回读校验 → 释放 backup
 *
 * @param   metrics  输出参数
 *
 * @retval  RT_EOK       成功
 * @retval  -RT_EINVAL   metrics 为空
 * @retval  -RT_ERROR    PSRAM 未就绪 / 映射失败 / 校验失败 / 恢复失败
 * @retval  -RT_ENOMEM   backup 分配失败
 * @retval  -RT_ETIMEOUT DWT 未启动
 */
int ai_kit_psram_portable_benchmark_run(ai_kit_memory_metrics_t *metrics)
{
    volatile void *mapping = RT_NULL;
    volatile uint32_t *source;
    volatile uint32_t *destination;
    uint8_t *backup = RT_NULL;
    uint8_t verify[256];
    uint32_t index;
    uint32_t offset;
    uint32_t expected_sum = 0U;
    uint32_t expected_pattern =
        0xA5A50000UL ^ (AI_KIT_PORTABLE_STREAM_PASSES - 1U);
    uint32_t read_cycles = 0U;
    uint32_t write_cycles = 0U;
    uint32_t copy_cycles = 0U;
    uint32_t start;
    int result = RT_EOK;
    rt_bool_t mapped = RT_FALSE;

    if (metrics == RT_NULL)
    {
        return -RT_EINVAL;
    }
    rt_memset(metrics, 0, sizeof(*metrics));
    if (!ai_kit_psram_is_ready())
    {
        return -RT_ERROR;
    }
    backup = (uint8_t *)rt_malloc(AI_KIT_PSRAM_BENCHMARK_BYTES);
    if (backup == RT_NULL)
    {
        return -RT_ENOMEM;
    }
    if (ai_kit_psram_map_private(0U, AI_KIT_PSRAM_BENCHMARK_BYTES,
                                 &mapping) != RT_EOK)
    {
        result = -RT_ERROR;
        goto cleanup;
    }
    mapped = RT_TRUE;
    ai_kit_copy_from_volatile(backup, (const volatile uint8_t *)mapping,
                              AI_KIT_PSRAM_BENCHMARK_BYTES);
    source = (volatile uint32_t *)mapping;
    destination = source + AI_KIT_PORTABLE_STREAM_WORDS;

    if (!ai_kit_memory_timer_enable())
    {
        result = -RT_ETIMEOUT;
        goto cleanup;
    }
    for (index = 0U; index < AI_KIT_PORTABLE_STREAM_WORDS; index++)
    {
        source[index] = 0x9E3779B9UL ^ index;
        expected_sum += source[index];
    }
    expected_sum *= AI_KIT_PORTABLE_STREAM_PASSES;

    SCB_CleanInvalidateDCache_by_Addr(
        (uint32_t *)source, (int32_t)AI_KIT_PORTABLE_STREAM_BUFFER_BYTES);
    __DSB();
    start = DWT->CYCCNT;
    if (ai_kit_portable_stream_read(source) != expected_sum)
    {
        result = -RT_ERROR;
        goto cleanup;
    }
    read_cycles = DWT->CYCCNT - start;

    SCB_CleanInvalidateDCache_by_Addr(
        (uint32_t *)destination,
        (int32_t)AI_KIT_PORTABLE_STREAM_BUFFER_BYTES);
    __DSB();
    start = DWT->CYCCNT;
    ai_kit_portable_stream_write(destination);
    __DSB();
    write_cycles = DWT->CYCCNT - start;
    for (index = 0U; index < AI_KIT_PORTABLE_STREAM_WORDS; index++)
    {
        if (destination[index] != expected_pattern)
        {
            result = -RT_ERROR;
            goto cleanup;
        }
    }

    SCB_CleanInvalidateDCache_by_Addr(
        (uint32_t *)source, (int32_t)AI_KIT_PORTABLE_STREAM_BUFFER_BYTES);
    SCB_CleanInvalidateDCache_by_Addr(
        (uint32_t *)destination,
        (int32_t)AI_KIT_PORTABLE_STREAM_BUFFER_BYTES);
    __DSB();
    start = DWT->CYCCNT;
    ai_kit_portable_stream_copy(destination, source);
    __DSB();
    copy_cycles = DWT->CYCCNT - start;
    for (index = 0U; index < AI_KIT_PORTABLE_STREAM_WORDS; index++)
    {
        if (destination[index] != source[index])
        {
            result = -RT_ERROR;
            goto cleanup;
        }
    }

    metrics->buffer_bytes = AI_KIT_PORTABLE_STREAM_BUFFER_BYTES;
    metrics->passes = AI_KIT_PORTABLE_STREAM_PASSES;
    metrics->elapsed_us = ai_kit_memory_elapsed_us(read_cycles) +
                          ai_kit_memory_elapsed_us(write_cycles) +
                          ai_kit_memory_elapsed_us(copy_cycles);
    metrics->read_mib_s_x1000 = ai_kit_memory_mib_s_x1000(
        AI_KIT_PORTABLE_STREAM_BYTES_PER_OP, read_cycles);
    metrics->write_mib_s_x1000 = ai_kit_memory_mib_s_x1000(
        AI_KIT_PORTABLE_STREAM_BYTES_PER_OP, write_cycles);
    metrics->copy_mib_s_x1000 = ai_kit_memory_mib_s_x1000(
        AI_KIT_PORTABLE_STREAM_BYTES_PER_OP, copy_cycles);
    metrics->source_address = (uint32_t)(uintptr_t)source;
    metrics->destination_address = (uint32_t)(uintptr_t)destination;

cleanup:
    if (mapped)
    {
        ai_kit_copy_to_volatile((volatile uint8_t *)mapping, backup,
                                AI_KIT_PSRAM_BENCHMARK_BYTES);
        ai_kit_psram_unmap_private(RT_TRUE);
        for (offset = 0U; offset < AI_KIT_PSRAM_BENCHMARK_BYTES;
             offset += sizeof(verify))
        {
            if ((ai_kit_psram_read_private(offset, verify,
                                           sizeof(verify)) != RT_EOK) ||
                (memcmp(&backup[offset], verify, sizeof(verify)) != 0))
            {
                result = -RT_ERROR;
                break;
            }
        }
    }
    rt_free(backup);
    return result;
}
