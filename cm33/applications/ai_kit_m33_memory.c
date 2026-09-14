/**
 * @file    ai_kit_m33_memory.c
 * @brief   CM33 SRAM / PSRAM 流式与标量基准测试
 *
 * @details 该文件实现 CM33 侧三类内存基准：
 *          1. m33_sram_bench    ：CM33 cache-on SRAM 64 KiB
 *                                 读 / 写 / 拷贝流式吞吐（MiB/s）
 *          2. m33_sram_scalar   ：CM33 便携标量 SRAM 流式
 *                                 （使用 ai_kit_portable_stream 接口）
 *          3. m33_psram_scalar  ：CM33 私有 PSRAM 标量流式，运行结束
 *                                 后还原原始数据
 *
 *          测量基于 DWT->CYCCNT 周期计数器，结果通过 IPC 发布到
 *          CM55 状态区，metric_x1000 / minimum_x1000 / maximum_x1000
 *          分别对应读 / 写 / 拷贝吞吐，以"千分之一 MiB/s"形式存储。
 *
 * @note    本文件仅添加注释，未改动任何可执行代码。
 */

#include <stdint.h>
#include <string.h>

#include <rtthread.h>

#include "board.h"
#include "ai_kit_ipc_client.h"
#include "ai_kit_portable_stream.h"
#include "ai_kit_psram.h"

#define AI_KIT_M33_SRAM_START           (0x240BD000UL)  /**< CM33 SRAM 缓冲区起始地址 */
#define AI_KIT_M33_SRAM_END             (0x240FC000UL)  /**< CM33 SRAM 缓冲区结束地址 */
#define AI_KIT_M33_MEMORY_BUFFER_BYTES  (64UL * 1024UL)  /**< 单次测试缓冲区字节数 */
#define AI_KIT_M33_MEMORY_ALIGNMENT     (32UL)            /**< 缓冲区对齐字节数 */
#define AI_KIT_M33_MEMORY_PASSES        (2048UL)          /**< 流式测试遍历次数（64K * 2048 = 128 MiB） */
#define AI_KIT_M33_MEMORY_WORDS         (AI_KIT_M33_MEMORY_BUFFER_BYTES / 4UL)  /**< 缓冲区字数（uint32_t） */
/** @brief 编译器内存屏障，防止优化器重排或消除流式循环 */
#define AI_KIT_COMPILER_MEMORY_BARRIER() __asm volatile ("" ::: "memory")
/** @brief PSRAM 基准测试字节数（两个流式缓冲区，便于源/目的分离） */
#define AI_KIT_M33_PSRAM_BENCHMARK_BYTES \
    (2UL * AI_KIT_PORTABLE_STREAM_BUFFER_BYTES)

static volatile uint32_t s_m33_read_checksum;          /**< 防止读循环被优化掉的校验和 */
static uint32_t s_m33_memory_sequence;                  /**< SRAM 流式基准序列号 */
static uint32_t s_m33_portable_memory_sequence;         /**< 便携标量 SRAM 基准序列号 */
static uint32_t s_m33_psram_sequence;                   /**< PSRAM 基准序列号 */

/**
 * @brief   使能 M33 DWT 周期计数器
 *
 * @details 顺序：DEMCR.TRCENA → CYCCNT=0 → DWT_CTRL.CYCCNTENA → DSB/ISB。
 *          随后空跑 128 个 NOP 验证 CYCCNT 是否自增。
 *
 * @retval  1  CYCCNT 已可用
 * @retval  0  计数器未使能或调试器未连接
 */
static int ai_kit_m33_memory_timer_enable(void)
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
 * @brief   测量前重置缓冲区的 DCache 行
 *
 * @details 若启用 DCache，调用 SCB_CleanInvalidateDCache_by_Addr
 *          刷新并失效化指定区域，避免脏行影响测量。最后 DSB/ISB
 *          保证操作完成。
 *
 * @param   buffer  缓冲区起始地址
 * @param   size    缓冲区字节数
 */
static void ai_kit_m33_memory_cache_reset(void *buffer, uint32_t size)
{
#if defined(__DCACHE_PRESENT) && (__DCACHE_PRESENT == 1U)
    SCB_CleanInvalidateDCache_by_Addr((uint32_t *)buffer, (int32_t)size);
#else
    RT_UNUSED(buffer);
    RT_UNUSED(size);
#endif
    __DSB();
    __ISB();
}

/**
 * @brief   把字节数 + 周期数换算为千分之一 MiB/s
 *
 * @details 公式：MiB/s = (bytes / 1024) * SystemCoreClock / (cycles * 1024)
 *          结果乘 1000，并以四舍五入方式处理分母，避免浮点。
 *
 * @param   bytes  本次操作传输的字节数
 * @param   cycles 本次操作消耗的周期数
 *
 * @return  uint32_t  千分之一 MiB/s；cycles 或 SystemCoreClock 为 0 时返回 0
 */
static uint32_t ai_kit_m33_memory_mib_s_x1000(uint64_t bytes,
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
 * @brief   把周期数换算为微秒
 *
 * @details us = cycles * 1e6 / SystemCoreClock，含四舍五入。
 *
 * @param   cycles 周期数
 *
 * @return  uint32_t 微秒数
 */
static uint32_t ai_kit_m33_memory_elapsed_us(uint32_t cycles)
{
    return (uint32_t)(((uint64_t)cycles * 1000000ULL +
                       ((uint64_t)SystemCoreClock / 2ULL)) /
                      (uint64_t)SystemCoreClock);
}

/**
 * @brief   对 source 缓冲区执行 passes 遍读循环
 *
 * @details 累加每个 32 位字的和，写入全局 s_m33_read_checksum
 *          以防止编译器消除读循环。
 *
 * @param   source  32 位字缓冲区
 * @param   passes  遍历次数
 *
 * @return  uint32_t  累加和
 */
static uint32_t ai_kit_m33_memory_read(uint32_t *source, uint32_t passes)
{
    uint32_t pass;
    uint32_t index;
    uint32_t checksum = 0U;

    for (pass = 0U; pass < passes; pass++)
    {
        for (index = 0U; index < AI_KIT_M33_MEMORY_WORDS; index++)
        {
            checksum += source[index];
        }
    }
    s_m33_read_checksum = checksum;
    return checksum;
}

/**
 * @brief   验证缓冲区是否落在 CM33 SRAM 范围内
 *
 * @details 用于诊断 rt_malloc_align 是否返回了非预期地址（例如
 *          PSRAM 或保留区）。范围：[AI_KIT_M33_SRAM_START, END]。
 *
 * @param   buffer  待校验的缓冲区指针
 *
 * @retval  1  落在 CM33 SRAM 内
 * @retval  0  超出范围
 */
static int ai_kit_m33_memory_pointer_valid(const void *buffer)
{
    uintptr_t begin = (uintptr_t)buffer;
    uintptr_t end = begin + AI_KIT_M33_MEMORY_BUFFER_BYTES;

    return (begin >= AI_KIT_M33_SRAM_START) &&
           (end > begin) &&
           (end <= AI_KIT_M33_SRAM_END);
}

/**
 * @brief   msh 命令：CM33 cache-on SRAM 读 / 写 / 拷贝吞吐基准
 *
 * @details 流程：
 *          1. 发布 RUNNING 基准结果
 *          2. 分配两个 64 KiB 对齐缓冲区 source / destination，
 *             校验都落在 CM33 SRAM 内
 *          3. 使能 DWT CYCCNT
 *          4. 用 0x9E3779B9 ^ index 填充 source，计算 expected_sum
 *          5. 测读：cache_reset + 读循环 + checksum 校验
 *          6. 测写：cache_reset + memset 模式填充 + 首尾字节校验
 *          7. 测拷贝：cache_reset + memcpy + memcmp 校验
 *          8. 计算 read/write/copy MiB/s（千分之一）
 *          9. 通过 IPC 发布 COMPLETE / ERROR 结果
 *
 *          每次操作传输 64 KiB * 2048 = 128 MiB，足以稳定测出吞吐。
 */
static void ai_kit_m33_memory_command(void)
{
    ai_kit_benchmark_result_t benchmark;
    uint32_t *source = RT_NULL;
    uint32_t *destination = RT_NULL;
    uint32_t index;
    uint32_t pass;
    uint32_t expected_sum = 0U;
    uint32_t read_cycles = 0U;
    uint32_t write_cycles = 0U;
    uint32_t copy_cycles = 0U;
    uint32_t start;
    uint8_t final_pattern;
    uint64_t transferred_bytes =
        (uint64_t)AI_KIT_M33_MEMORY_BUFFER_BYTES *
        AI_KIT_M33_MEMORY_PASSES;
    rt_err_t publish_result;

    rt_memset(&benchmark, 0, sizeof(benchmark));
    benchmark.protocol_version = AI_KIT_BENCHMARK_PROTOCOL_VERSION;
    benchmark.sequence = ++s_m33_memory_sequence;
    benchmark.test_id = AI_KIT_BENCHMARK_TEST_MEMORY;
    benchmark.executor = AI_KIT_BENCHMARK_EXECUTOR_M33;
    benchmark.state = AI_KIT_BENCHMARK_STATE_RUNNING;
    benchmark.unit = AI_KIT_BENCHMARK_UNIT_MEGABYTES_PER_SECOND;
    benchmark.checksum = ai_kit_benchmark_result_checksum(&benchmark);
    if (ai_kit_ipc_publish_benchmark(&benchmark) != RT_EOK)
    {
        rt_kprintf("M33 SRAM benchmark start publish failed\n");
        return;
    }

    source = (uint32_t *)rt_malloc_align(AI_KIT_M33_MEMORY_BUFFER_BYTES,
                                         AI_KIT_M33_MEMORY_ALIGNMENT);
    destination = (uint32_t *)rt_malloc_align(AI_KIT_M33_MEMORY_BUFFER_BYTES,
                                              AI_KIT_M33_MEMORY_ALIGNMENT);
    if ((source == RT_NULL) || (destination == RT_NULL))
    {
        benchmark.error = AI_KIT_BENCHMARK_ERROR_NO_MEMORY;
        goto failed;
    }
    if (!ai_kit_m33_memory_pointer_valid(source) ||
        !ai_kit_m33_memory_pointer_valid(destination))
    {
        benchmark.error = AI_KIT_BENCHMARK_ERROR_VALIDATION;
        goto failed;
    }
    if (!ai_kit_m33_memory_timer_enable())
    {
        benchmark.error = AI_KIT_BENCHMARK_ERROR_TIMER_UNAVAILABLE;
        goto failed;
    }

    for (index = 0U; index < AI_KIT_M33_MEMORY_WORDS; index++)
    {
        source[index] = 0x9E3779B9UL ^ index;
        expected_sum += source[index];
    }
    expected_sum *= AI_KIT_M33_MEMORY_PASSES;

    ai_kit_m33_memory_cache_reset(source, AI_KIT_M33_MEMORY_BUFFER_BYTES);
    start = DWT->CYCCNT;
    if (ai_kit_m33_memory_read(source, AI_KIT_M33_MEMORY_PASSES) !=
        expected_sum)
    {
        benchmark.error = AI_KIT_BENCHMARK_ERROR_VALIDATION;
        goto failed;
    }
    read_cycles = DWT->CYCCNT - start;

    ai_kit_m33_memory_cache_reset(destination,
                                  AI_KIT_M33_MEMORY_BUFFER_BYTES);
    start = DWT->CYCCNT;
    for (pass = 0U; pass < AI_KIT_M33_MEMORY_PASSES; pass++)
    {
        memset(destination, (int)(0xA5U ^ (uint8_t)pass),
               AI_KIT_M33_MEMORY_BUFFER_BYTES);
        AI_KIT_COMPILER_MEMORY_BARRIER();
    }
    __DSB();
    write_cycles = DWT->CYCCNT - start;
    final_pattern = (uint8_t)(0xA5U ^
                              (uint8_t)(AI_KIT_M33_MEMORY_PASSES - 1U));
    if ((((uint8_t *)destination)[0] != final_pattern) ||
        (((uint8_t *)destination)[AI_KIT_M33_MEMORY_BUFFER_BYTES - 1U] !=
         final_pattern))
    {
        benchmark.error = AI_KIT_BENCHMARK_ERROR_VALIDATION;
        goto failed;
    }

    ai_kit_m33_memory_cache_reset(source, AI_KIT_M33_MEMORY_BUFFER_BYTES);
    ai_kit_m33_memory_cache_reset(destination,
                                  AI_KIT_M33_MEMORY_BUFFER_BYTES);
    start = DWT->CYCCNT;
    for (pass = 0U; pass < AI_KIT_M33_MEMORY_PASSES; pass++)
    {
        memcpy(destination, source, AI_KIT_M33_MEMORY_BUFFER_BYTES);
        AI_KIT_COMPILER_MEMORY_BARRIER();
    }
    __DSB();
    copy_cycles = DWT->CYCCNT - start;
    if (memcmp(destination, source, AI_KIT_M33_MEMORY_BUFFER_BYTES) != 0)
    {
        benchmark.error = AI_KIT_BENCHMARK_ERROR_VALIDATION;
        goto failed;
    }

    benchmark.iterations = (uint32_t)transferred_bytes;
    benchmark.elapsed_us = ai_kit_m33_memory_elapsed_us(read_cycles) +
                           ai_kit_m33_memory_elapsed_us(write_cycles) +
                           ai_kit_m33_memory_elapsed_us(copy_cycles);
    benchmark.metric_x1000 =
        ai_kit_m33_memory_mib_s_x1000(transferred_bytes, read_cycles);
    benchmark.minimum_x1000 =
        ai_kit_m33_memory_mib_s_x1000(transferred_bytes, write_cycles);
    benchmark.maximum_x1000 =
        ai_kit_m33_memory_mib_s_x1000(transferred_bytes, copy_cycles);
    benchmark.state = AI_KIT_BENCHMARK_STATE_COMPLETE;
    benchmark.error = AI_KIT_BENCHMARK_ERROR_NONE;
    goto publish;

failed:
    benchmark.state = AI_KIT_BENCHMARK_STATE_ERROR;

publish:
    if (destination != RT_NULL)
    {
        rt_free_align(destination);
    }
    if (source != RT_NULL)
    {
        rt_free_align(source);
    }
    benchmark.checksum = ai_kit_benchmark_result_checksum(&benchmark);
    publish_result = ai_kit_ipc_publish_benchmark(&benchmark);
    if (publish_result != RT_EOK)
    {
        rt_kprintf("M33 SRAM result publish failed: %d\n", publish_result);
        return;
    }

    if (benchmark.state == AI_KIT_BENCHMARK_STATE_COMPLETE)
    {
        rt_kprintf("M33 SRAM streaming (cache-on, 64 KiB, 128 MiB/op):\n");
        rt_kprintf("  read=%lu.%03lu write=%lu.%03lu copy=%lu.%03lu "
                   "MiB/s elapsed=%lu us\n",
                   (unsigned long)(benchmark.metric_x1000 / 1000U),
                   (unsigned long)(benchmark.metric_x1000 % 1000U),
                   (unsigned long)(benchmark.minimum_x1000 / 1000U),
                   (unsigned long)(benchmark.minimum_x1000 % 1000U),
                   (unsigned long)(benchmark.maximum_x1000 / 1000U),
                   (unsigned long)(benchmark.maximum_x1000 % 1000U),
                   (unsigned long)benchmark.elapsed_us);
    }
    else
    {
        rt_kprintf("M33 SRAM benchmark failed: error=%lu\n",
                   (unsigned long)benchmark.error);
    }
}
MSH_CMD_EXPORT_ALIAS(ai_kit_m33_memory_command, m33_sram_bench,
                     benchmark M33 cache-on SRAM read write and copy);

/**
 * @brief   msh 命令：CM33 便携标量 SRAM 流式基准
 *
 * @details 与 m33_sram_bench 类似，但读 / 写 / 拷贝动作改为调用
 *          ai_kit_portable_stream.h 提供的标量接口，便于跨核
 *          对照同样代码路径下的内存子系统性能。每次操作传输
 *          AI_KIT_PORTABLE_STREAM_BYTES_PER_OP 字节。
 */
static void ai_kit_m33_portable_memory_command(void)
{
    ai_kit_benchmark_result_t benchmark;
    uint32_t *source = RT_NULL;
    uint32_t *destination = RT_NULL;
    uint32_t index;
    uint32_t expected_sum = 0U;
    uint32_t expected_pattern =
        0xA5A50000UL ^ (AI_KIT_PORTABLE_STREAM_PASSES - 1U);
    uint32_t read_cycles = 0U;
    uint32_t write_cycles = 0U;
    uint32_t copy_cycles = 0U;
    uint32_t start;
    rt_err_t publish_result;

    rt_memset(&benchmark, 0, sizeof(benchmark));
    benchmark.protocol_version = AI_KIT_BENCHMARK_PROTOCOL_VERSION;
    benchmark.sequence = ++s_m33_portable_memory_sequence;
    benchmark.test_id = AI_KIT_BENCHMARK_TEST_MEMORY_PORTABLE;
    benchmark.executor = AI_KIT_BENCHMARK_EXECUTOR_M33;
    benchmark.state = AI_KIT_BENCHMARK_STATE_RUNNING;
    benchmark.unit = AI_KIT_BENCHMARK_UNIT_MEGABYTES_PER_SECOND;
    benchmark.checksum = ai_kit_benchmark_result_checksum(&benchmark);
    if (ai_kit_ipc_publish_benchmark(&benchmark) != RT_EOK)
    {
        rt_kprintf("M33 portable SRAM start publish failed\n");
        return;
    }

    source = (uint32_t *)rt_malloc_align(
        AI_KIT_PORTABLE_STREAM_BUFFER_BYTES, AI_KIT_M33_MEMORY_ALIGNMENT);
    destination = (uint32_t *)rt_malloc_align(
        AI_KIT_PORTABLE_STREAM_BUFFER_BYTES, AI_KIT_M33_MEMORY_ALIGNMENT);
    if ((source == RT_NULL) || (destination == RT_NULL))
    {
        benchmark.error = AI_KIT_BENCHMARK_ERROR_NO_MEMORY;
        goto failed;
    }
    if (!ai_kit_m33_memory_pointer_valid(source) ||
        !ai_kit_m33_memory_pointer_valid(destination))
    {
        benchmark.error = AI_KIT_BENCHMARK_ERROR_VALIDATION;
        goto failed;
    }
    if (!ai_kit_m33_memory_timer_enable())
    {
        benchmark.error = AI_KIT_BENCHMARK_ERROR_TIMER_UNAVAILABLE;
        goto failed;
    }

    for (index = 0U; index < AI_KIT_PORTABLE_STREAM_WORDS; index++)
    {
        source[index] = 0x9E3779B9UL ^ index;
        expected_sum += source[index];
    }
    expected_sum *= AI_KIT_PORTABLE_STREAM_PASSES;

    ai_kit_m33_memory_cache_reset(
        source, AI_KIT_PORTABLE_STREAM_BUFFER_BYTES);
    start = DWT->CYCCNT;
    if (ai_kit_portable_stream_read(source) != expected_sum)
    {
        benchmark.error = AI_KIT_BENCHMARK_ERROR_VALIDATION;
        goto failed;
    }
    read_cycles = DWT->CYCCNT - start;

    ai_kit_m33_memory_cache_reset(
        destination, AI_KIT_PORTABLE_STREAM_BUFFER_BYTES);
    start = DWT->CYCCNT;
    ai_kit_portable_stream_write(destination);
    __DSB();
    write_cycles = DWT->CYCCNT - start;
    for (index = 0U; index < AI_KIT_PORTABLE_STREAM_WORDS; index++)
    {
        if (destination[index] != expected_pattern)
        {
            benchmark.error = AI_KIT_BENCHMARK_ERROR_VALIDATION;
            goto failed;
        }
    }

    ai_kit_m33_memory_cache_reset(
        source, AI_KIT_PORTABLE_STREAM_BUFFER_BYTES);
    ai_kit_m33_memory_cache_reset(
        destination, AI_KIT_PORTABLE_STREAM_BUFFER_BYTES);
    start = DWT->CYCCNT;
    ai_kit_portable_stream_copy(destination, source);
    __DSB();
    copy_cycles = DWT->CYCCNT - start;
    if (memcmp(destination, source,
               AI_KIT_PORTABLE_STREAM_BUFFER_BYTES) != 0)
    {
        benchmark.error = AI_KIT_BENCHMARK_ERROR_VALIDATION;
        goto failed;
    }

    benchmark.iterations = (uint32_t)AI_KIT_PORTABLE_STREAM_BYTES_PER_OP;
    benchmark.elapsed_us = ai_kit_m33_memory_elapsed_us(read_cycles) +
                           ai_kit_m33_memory_elapsed_us(write_cycles) +
                           ai_kit_m33_memory_elapsed_us(copy_cycles);
    benchmark.metric_x1000 = ai_kit_m33_memory_mib_s_x1000(
        AI_KIT_PORTABLE_STREAM_BYTES_PER_OP, read_cycles);
    benchmark.minimum_x1000 = ai_kit_m33_memory_mib_s_x1000(
        AI_KIT_PORTABLE_STREAM_BYTES_PER_OP, write_cycles);
    benchmark.maximum_x1000 = ai_kit_m33_memory_mib_s_x1000(
        AI_KIT_PORTABLE_STREAM_BYTES_PER_OP, copy_cycles);
    benchmark.state = AI_KIT_BENCHMARK_STATE_COMPLETE;
    benchmark.error = AI_KIT_BENCHMARK_ERROR_NONE;
    goto publish;

failed:
    benchmark.state = AI_KIT_BENCHMARK_STATE_ERROR;

publish:
    if (destination != RT_NULL)
    {
        rt_free_align(destination);
    }
    if (source != RT_NULL)
    {
        rt_free_align(source);
    }
    benchmark.checksum = ai_kit_benchmark_result_checksum(&benchmark);
    publish_result = ai_kit_ipc_publish_benchmark(&benchmark);
    if (publish_result != RT_EOK)
    {
        rt_kprintf("M33 portable SRAM result publish failed: %d\n",
                   publish_result);
        return;
    }

    if (benchmark.state == AI_KIT_BENCHMARK_STATE_COMPLETE)
    {
        rt_kprintf("M33 portable scalar SRAM (cache-on, 64 KiB, "
                   "128 MiB/op):\n");
        rt_kprintf("  read=%lu.%03lu write=%lu.%03lu copy=%lu.%03lu "
                   "MiB/s elapsed=%lu us\n",
                   (unsigned long)(benchmark.metric_x1000 / 1000U),
                   (unsigned long)(benchmark.metric_x1000 % 1000U),
                   (unsigned long)(benchmark.minimum_x1000 / 1000U),
                   (unsigned long)(benchmark.minimum_x1000 % 1000U),
                   (unsigned long)(benchmark.maximum_x1000 / 1000U),
                   (unsigned long)(benchmark.maximum_x1000 % 1000U),
                   (unsigned long)benchmark.elapsed_us);
    }
    else
    {
        rt_kprintf("M33 portable SRAM failed: error=%lu\n",
                   (unsigned long)benchmark.error);
    }
}
MSH_CMD_EXPORT_ALIAS(ai_kit_m33_portable_memory_command, m33_sram_scalar,
                     benchmark M33 portable scalar SRAM streaming);

/**
 * @brief   从 volatile 内存按字节拷贝到普通缓冲区
 *
 * @details 用于把 PSRAM 的内容备份到 SRAM，避免后续写入破坏原始数据。
 *          逐字节拷贝保证对 volatile 限定的访问不被编译器优化。
 *
 * @param   destination  目标缓冲区（普通内存）
 * @param   source       源地址（volatile 限定，通常是 PSRAM 映射）
 * @param   size         字节数
 */
static void ai_kit_m33_copy_from_volatile(
    uint8_t *destination, const volatile uint8_t *source, uint32_t size)
{
    uint32_t index;

    for (index = 0U; index < size; index++)
    {
        destination[index] = source[index];
    }
}

/**
 * @brief   从普通缓冲区按字节拷贝到 volatile 内存
 *
 * @details 用于基准测试后还原 PSRAM 原始数据。逐字节拷贝
 *          保证对 volatile 限定的写入不被合并或省略。
 *
 * @param   destination  目标地址（volatile 限定，通常是 PSRAM 映射）
 * @param   source       源缓冲区（普通内存）
 * @param   size         字节数
 */
static void ai_kit_m33_copy_to_volatile(
    volatile uint8_t *destination, const uint8_t *source, uint32_t size)
{
    uint32_t index;

    for (index = 0U; index < size; index++)
    {
        destination[index] = source[index];
    }
}

/**
 * @brief   msh 命令：CM33 私有 PSRAM 标量流式基准（含数据还原）
 *
 * @details 流程：
 *          1. 发布 RUNNING 基准结果
 *          2. 校验 PSRAM 已就绪，分配 backup 缓冲区
 *          3. 通过 ai_kit_psram_map_private 映射私有窗口
 *          4. 用 ai_kit_m33_copy_from_volatile 备份原始数据
 *          5. 在 PSRAM 上做 source / destination 双缓冲流式
 *             读 / 写 / 拷贝（标量接口 ai_kit_portable_stream_*）
 *          6. 用 ai_kit_m33_copy_to_volatile 还原原始数据
 *          7. 通过 ai_kit_psram_read_private 抽样校验还原正确性
 *          8. 通过 IPC 发布 COMPLETE / ERROR 结果
 *
 *          任一步失败立即跳转 failed / cleanup，保证数据被还原。
 */
static void ai_kit_m33_psram_command(void)
{
    ai_kit_benchmark_result_t benchmark;
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
    rt_bool_t mapped = RT_FALSE;
    rt_err_t publish_result;

    rt_memset(&benchmark, 0, sizeof(benchmark));
    benchmark.protocol_version = AI_KIT_BENCHMARK_PROTOCOL_VERSION;
    benchmark.sequence = ++s_m33_psram_sequence;
    benchmark.test_id = AI_KIT_BENCHMARK_TEST_PSRAM_PORTABLE;
    benchmark.executor = AI_KIT_BENCHMARK_EXECUTOR_M33;
    benchmark.state = AI_KIT_BENCHMARK_STATE_RUNNING;
    benchmark.unit = AI_KIT_BENCHMARK_UNIT_MEGABYTES_PER_SECOND;
    benchmark.checksum = ai_kit_benchmark_result_checksum(&benchmark);
    if (ai_kit_ipc_publish_benchmark(&benchmark) != RT_EOK)
    {
        rt_kprintf("M33 PSRAM benchmark start publish failed\n");
        return;
    }

    if (!ai_kit_psram_is_ready())
    {
        benchmark.error = AI_KIT_BENCHMARK_ERROR_VALIDATION;
        goto failed;
    }
    backup = (uint8_t *)rt_malloc(AI_KIT_M33_PSRAM_BENCHMARK_BYTES);
    if (backup == RT_NULL)
    {
        benchmark.error = AI_KIT_BENCHMARK_ERROR_NO_MEMORY;
        goto failed;
    }
    if (ai_kit_psram_map_private(0U, AI_KIT_M33_PSRAM_BENCHMARK_BYTES,
                                 &mapping) != RT_EOK)
    {
        benchmark.error = AI_KIT_BENCHMARK_ERROR_VALIDATION;
        goto failed;
    }
    mapped = RT_TRUE;
    ai_kit_m33_copy_from_volatile(
        backup, (const volatile uint8_t *)mapping,
        AI_KIT_M33_PSRAM_BENCHMARK_BYTES);
    source = (volatile uint32_t *)mapping;
    destination = source + AI_KIT_PORTABLE_STREAM_WORDS;

    if (!ai_kit_m33_memory_timer_enable())
    {
        benchmark.error = AI_KIT_BENCHMARK_ERROR_TIMER_UNAVAILABLE;
        goto failed;
    }
    for (index = 0U; index < AI_KIT_PORTABLE_STREAM_WORDS; index++)
    {
        source[index] = 0x9E3779B9UL ^ index;
        expected_sum += source[index];
    }
    expected_sum *= AI_KIT_PORTABLE_STREAM_PASSES;

    ai_kit_m33_memory_cache_reset(
        (void *)source, AI_KIT_PORTABLE_STREAM_BUFFER_BYTES);
    start = DWT->CYCCNT;
    if (ai_kit_portable_stream_read(source) != expected_sum)
    {
        benchmark.error = AI_KIT_BENCHMARK_ERROR_VALIDATION;
        goto failed;
    }
    read_cycles = DWT->CYCCNT - start;

    ai_kit_m33_memory_cache_reset(
        (void *)destination, AI_KIT_PORTABLE_STREAM_BUFFER_BYTES);
    start = DWT->CYCCNT;
    ai_kit_portable_stream_write(destination);
    __DSB();
    write_cycles = DWT->CYCCNT - start;
    for (index = 0U; index < AI_KIT_PORTABLE_STREAM_WORDS; index++)
    {
        if (destination[index] != expected_pattern)
        {
            benchmark.error = AI_KIT_BENCHMARK_ERROR_VALIDATION;
            goto failed;
        }
    }

    ai_kit_m33_memory_cache_reset(
        (void *)source, AI_KIT_PORTABLE_STREAM_BUFFER_BYTES);
    ai_kit_m33_memory_cache_reset(
        (void *)destination, AI_KIT_PORTABLE_STREAM_BUFFER_BYTES);
    start = DWT->CYCCNT;
    ai_kit_portable_stream_copy(destination, source);
    __DSB();
    copy_cycles = DWT->CYCCNT - start;
    for (index = 0U; index < AI_KIT_PORTABLE_STREAM_WORDS; index++)
    {
        if (destination[index] != source[index])
        {
            benchmark.error = AI_KIT_BENCHMARK_ERROR_VALIDATION;
            goto failed;
        }
    }

    benchmark.iterations = (uint32_t)AI_KIT_PORTABLE_STREAM_BYTES_PER_OP;
    benchmark.elapsed_us = ai_kit_m33_memory_elapsed_us(read_cycles) +
                           ai_kit_m33_memory_elapsed_us(write_cycles) +
                           ai_kit_m33_memory_elapsed_us(copy_cycles);
    benchmark.metric_x1000 = ai_kit_m33_memory_mib_s_x1000(
        AI_KIT_PORTABLE_STREAM_BYTES_PER_OP, read_cycles);
    benchmark.minimum_x1000 = ai_kit_m33_memory_mib_s_x1000(
        AI_KIT_PORTABLE_STREAM_BYTES_PER_OP, write_cycles);
    benchmark.maximum_x1000 = ai_kit_m33_memory_mib_s_x1000(
        AI_KIT_PORTABLE_STREAM_BYTES_PER_OP, copy_cycles);
    benchmark.state = AI_KIT_BENCHMARK_STATE_COMPLETE;
    benchmark.error = AI_KIT_BENCHMARK_ERROR_NONE;
    goto cleanup;

failed:
    benchmark.state = AI_KIT_BENCHMARK_STATE_ERROR;

cleanup:
    if (mapped)
    {
        ai_kit_m33_copy_to_volatile(
            (volatile uint8_t *)mapping, backup,
            AI_KIT_M33_PSRAM_BENCHMARK_BYTES);
        ai_kit_psram_unmap_private(RT_TRUE);
        for (offset = 0U; offset < AI_KIT_M33_PSRAM_BENCHMARK_BYTES;
             offset += sizeof(verify))
        {
            if ((ai_kit_psram_read_private(offset, verify,
                                           sizeof(verify)) != RT_EOK) ||
                (memcmp(&backup[offset], verify, sizeof(verify)) != 0))
            {
                benchmark.state = AI_KIT_BENCHMARK_STATE_ERROR;
                benchmark.error = AI_KIT_BENCHMARK_ERROR_VALIDATION;
                break;
            }
        }
    }
    if (backup != RT_NULL)
    {
        rt_free(backup);
    }
    benchmark.checksum = ai_kit_benchmark_result_checksum(&benchmark);
    publish_result = ai_kit_ipc_publish_benchmark(&benchmark);
    if (publish_result != RT_EOK)
    {
        rt_kprintf("M33 PSRAM result publish failed: %d\n", publish_result);
        return;
    }

    if (benchmark.state == AI_KIT_BENCHMARK_STATE_COMPLETE)
    {
        rt_kprintf("M33 portable scalar PSRAM (64 KiB, 128 MiB/op):\n");
        rt_kprintf("  read=%lu.%03lu write=%lu.%03lu copy=%lu.%03lu "
                   "MiB/s elapsed=%lu us / original data restored\n",
                   (unsigned long)(benchmark.metric_x1000 / 1000U),
                   (unsigned long)(benchmark.metric_x1000 % 1000U),
                   (unsigned long)(benchmark.minimum_x1000 / 1000U),
                   (unsigned long)(benchmark.minimum_x1000 % 1000U),
                   (unsigned long)(benchmark.maximum_x1000 / 1000U),
                   (unsigned long)(benchmark.maximum_x1000 % 1000U),
                   (unsigned long)benchmark.elapsed_us);
    }
    else
    {
        rt_kprintf("M33 PSRAM benchmark failed: error=%lu\n",
                   (unsigned long)benchmark.error);
    }
}
MSH_CMD_EXPORT_ALIAS(ai_kit_m33_psram_command, m33_psram_scalar,
                     benchmark M33 private PSRAM and restore original data);
