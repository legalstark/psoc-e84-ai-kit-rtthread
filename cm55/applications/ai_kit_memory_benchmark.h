#ifndef AI_KIT_MEMORY_BENCHMARK_H
#define AI_KIT_MEMORY_BENCHMARK_H

#include <stdint.h>

/**
 * @file    ai_kit_memory_benchmark.h
 * @brief   AI Kit 内存流式基准测试对外接口
 *
 * @details 该头文件声明 CM55 侧三类内存基准测试：
 *          - SRAM-DMA 加速版（直接 memset/memcpy，编译器可优化为 DMA 友好指令）
 *          - SRAM 标量版（调用 ai_kit_portable_stream 标量实现）
 *          - PSRAM 标量版（在片外 PSRAM 上跑标量实现，含原数据备份/恢复）
 *          每次运行返回完整 ai_kit_memory_metrics_t，由 ai_kit_benchmark.c
 *          汇聚到基准目录。实现细节参见 ai_kit_memory_benchmark.c。
 */

/**
 * @brief   内存基准测试指标集合
 */
typedef struct
{
    uint32_t buffer_bytes;       /**< @brief 单次操作的缓冲字节数 */
    uint32_t passes;            /**< @brief 测试循环次数 */
    uint32_t elapsed_us;         /**< @brief 读+写+复制三段总耗时（us） */
    uint32_t read_mib_s_x1000;   /**< @brief 读带宽（MiB/s × 1000） */
    uint32_t write_mib_s_x1000;  /**< @brief 写带宽（MiB/s × 1000） */
    uint32_t copy_mib_s_x1000;   /**< @brief 复制带宽（MiB/s × 1000） */
    uint32_t source_address;     /**< @brief 源缓冲物理地址 */
    uint32_t destination_address; /**< @brief 目标缓冲物理地址 */
} ai_kit_memory_metrics_t;

/**
 * @brief   运行 SRAM 流式内存基准（DMA 加速版）
 *
 * @details 在 SRAM 中分配 256 KiB 源 / 目标缓冲（32 字节对齐），
 *          使用 memset/memcpy 跑 512 次 read/write/copy，每次跑前
 *          SCB_CleanInvalidateDCache_by_Addr 保证 cache 一致，
 *          跑后通过 DWT CYCCNT 计算带宽，并校验 checksum 与模式值。
 *
 * @param   metrics  输出参数，填充指标
 *
 * @retval  RT_EOK       成功
 * @retval  -RT_EINVAL   metrics 为空
 * @retval  -RT_ENOMEM   rt_malloc_align 失败
 * @retval  -RT_ERROR    缓冲不在合法 SRAM 范围 / 校验失败
 * @retval  -RT_ETIMEOUT DWT CYCCNT 未启动
 */
int ai_kit_memory_benchmark_run(ai_kit_memory_metrics_t *metrics);

/**
 * @brief   运行 SRAM 标量内存基准（无 DMA）
 *
 * @details 调用 ai_kit_portable_stream 标量 C 实现进行读 / 写 / 复制，
 *          缓冲规模由 AI_KIT_PORTABLE_STREAM_* 宏决定，用于与 DMA 加速
 *          版对比纯 CPU 拷贝带宽。
 *
 * @param   metrics  输出参数，填充指标
 *
 * @retval  RT_EOK       成功
 * @retval  -RT_EINVAL   metrics 为空
 * @retval  -RT_ENOMEM   rt_malloc_align 失败
 * @retval  -RT_ERROR    缓冲不合法 / 校验失败
 * @retval  -RT_ETIMEOUT DWT CYCCNT 未启动
 */
int ai_kit_memory_portable_benchmark_run(ai_kit_memory_metrics_t *metrics);

/**
 * @brief   运行 PSRAM 标量内存基准
 *
 * @details 在片外 PSRAM 中映射私有区域作为源 / 目标缓冲，跑标量
 *          读 / 写 / 复制；测试前备份原数据，测试后恢复并回读校验，
 *          保证 PSRAM 内容不被破坏。
 *
 * @param   metrics  输出参数，填充指标
 *
 * @retval  RT_EOK       成功
 * @retval  -RT_EINVAL   metrics 为空
 * @retval  -RT_ERROR    PSRAM 未就绪 / 映射失败 / 校验失败 / 恢复失败
 * @retval  -RT_ENOMEM   backup 分配失败
 * @retval  -RT_ETIMEOUT DWT CYCCNT 未启动
 */
int ai_kit_psram_portable_benchmark_run(ai_kit_memory_metrics_t *metrics);

#endif
