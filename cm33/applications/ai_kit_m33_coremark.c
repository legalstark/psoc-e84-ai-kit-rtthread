/**
 * @file    ai_kit_m33_coremark.c
 * @brief   Cortex-M33 系统态 CoreMark 基准测试与发布
 *
 * @details 该文件在 CM33 Non-Secure 域上运行 EEMBC CoreMark 基准，
 *          并通过 IPC 将结果发布到 CM55 状态区，供整机合并报告使用。
 *
 *          流程：
 *          1. m33_coremark 命令调用 core_mark() 进入 CoreMark
 *          2. CoreMark 完成时回调 coremark_port_report() 收集
 *             iterations / ticks / errors
 *          3. 按 RT_TICK_PER_SECOND 与 SystemCoreClock 换算
 *             CoreMark/s、运行时长 us
 *          4. 校验 ticks 是否达到最小运行时间要求
 *          5. 通过 ai_kit_ipc_publish_benchmark 发布 START / COMPLETE 两阶段
 *             基准结果
 *
 *          结果中 metric_x1000 字段以"千分之一 CoreMark/s"形式存储，
 *          便于跨核无浮点环境下保留 3 位精度。
 *
 * @note    本文件仅添加注释，未改动任何可执行代码。
 */

#include <rtthread.h>

#include "coremark.h"
#include "ai_kit_ipc_client.h"

#define AI_KIT_COREMARK_MINIMUM_SECONDS (10U)  /**< CoreMark 要求最少运行 10 秒 */

/**
 * @brief   CoreMark 运行结果本地缓存
 *
 * @details 由 coremark_port_report 回调写入。valid 字段用于判断
 *          core_mark() 是否真正回调过本函数。
 */
typedef struct
{
    uint32_t valid;       /**< 回调是否已发生（0=未回调，1=已回调） */
    uint32_t iterations;  /**< CoreMark 报告的迭代次数 */
    uint32_t ticks;       /**< CoreMark 报告的 RT-Thread tick 数 */
    int32_t errors;       /**< CoreMark 报告的错误数（非 0 表示校验失败） */
} ai_kit_m33_coremark_report_t;

static ai_kit_m33_coremark_report_t s_coremark_report;  /**< 最近一次 CoreMark 运行结果 */
static uint32_t s_coremark_sequence;                    /**< CoreMark 结果自增序列号 */

extern int core_mark(int argc, char *argv[]);  /**< CoreMark 入口，由 coremark 库提供 */

/**
 * @brief   CoreMark 运行完成的回调端口函数
 *
 * @details 由 EEMBC CoreMark 在 main_loop 结束时调用。本实现把
 *          iterations/ticks/errors 写入本地缓存，并置 valid=1。
 *
 * @param   iterations  CoreMark 报告的迭代次数
 * @param   ticks       CoreMark 报告的耗时（tick 数）
 * @param   errors      CoreMark 报告的错误数
 */
void coremark_port_report(ee_u32 iterations, CORE_TICKS ticks, ee_s16 errors)
{
    s_coremark_report.iterations = (uint32_t)iterations;
    s_coremark_report.ticks = (uint32_t)ticks;
    s_coremark_report.errors = (int32_t)errors;
    s_coremark_report.valid = 1U;
}

/**
 * @brief   初始化 M33 CoreMark 基准结果结构体
 *
 * @details 清零 result、自增本地序列号，并预填：
 *          - protocol_version
 *          - sequence
 *          - test_id  = COREMARK
 *          - executor = M33
 *          - state    = RUNNING
 *          - error    = NONE
 *          - unit     = COREMARK_PER_SECOND
 *          - checksum
 *
 * @param   result  [out] 待初始化的结果结构体
 */
static void ai_kit_m33_coremark_result_init(
    ai_kit_benchmark_result_t *result)
{
    rt_memset(result, 0, sizeof(*result));
    s_coremark_sequence++;
    result->protocol_version = AI_KIT_BENCHMARK_PROTOCOL_VERSION;
    result->sequence = s_coremark_sequence;
    result->test_id = AI_KIT_BENCHMARK_TEST_COREMARK;
    result->executor = AI_KIT_BENCHMARK_EXECUTOR_M33;
    result->state = AI_KIT_BENCHMARK_STATE_RUNNING;
    result->error = AI_KIT_BENCHMARK_ERROR_NONE;
    result->unit = AI_KIT_BENCHMARK_UNIT_COREMARK_PER_SECOND;
    result->checksum = ai_kit_benchmark_result_checksum(result);
}

/**
 * @brief   msh 命令：在 CM33 系统态运行 CoreMark 并发布结果
 *
 * @details 流程：
 *          1. 初始化基准结果并通过 IPC 发布 RUNNING 状态，告知 CM55
 *             测试开始
 *          2. 清空本地缓存，调用 core_mark(0, NULL) 同步执行 CoreMark
 *          3. 检查 valid：未回调则置 REPORT_MISSING 错误
 *          4. 计算 elapsed_us 与 metric_x1000（CoreMark/s * 1000）
 *          5. 若 errors != 0 置 VALIDATION 错误；若 ticks 不足 10 秒
 *             置 TOO_SHORT 错误；否则置 COMPLETE
 *          6. 重算 checksum 并通过 IPC 发布最终结果
 *          7. 打印 CM33 CoreMark/s 整数+小数部分
 *
 *          命令本身是阻塞的，等 CoreMark 完成后 msh 才返回。
 */
static void ai_kit_m33_coremark_command(void)
{
    ai_kit_benchmark_result_t result;
    uint64_t elapsed_us;
    uint64_t metric_x1000;
    rt_err_t publish_result;

    ai_kit_m33_coremark_result_init(&result);
    publish_result = ai_kit_ipc_publish_benchmark(&result);
    if (publish_result != RT_EOK)
    {
        rt_kprintf("M33 CoreMark start publish failed: %d\n", publish_result);
        return;
    }

    rt_memset(&s_coremark_report, 0, sizeof(s_coremark_report));
    rt_kprintf("M33 CoreMark system-active run started; msh returns after completion.\n");
    (void)core_mark(0, RT_NULL);

    if (s_coremark_report.valid == 0U)
    {
        result.state = AI_KIT_BENCHMARK_STATE_ERROR;
        result.error = AI_KIT_BENCHMARK_ERROR_REPORT_MISSING;
    }
    else
    {
        elapsed_us = ((uint64_t)s_coremark_report.ticks * 1000000ULL) /
                     (uint64_t)RT_TICK_PER_SECOND;
        metric_x1000 = ((uint64_t)s_coremark_report.iterations *
                        (uint64_t)RT_TICK_PER_SECOND * 1000ULL) /
                       (uint64_t)s_coremark_report.ticks;
        result.iterations = s_coremark_report.iterations;
        result.elapsed_us = (uint32_t)elapsed_us;
        result.metric_x1000 = (uint32_t)metric_x1000;

        if (s_coremark_report.errors != 0)
        {
            result.state = AI_KIT_BENCHMARK_STATE_ERROR;
            result.error = AI_KIT_BENCHMARK_ERROR_VALIDATION;
        }
        else if (s_coremark_report.ticks <
                 (AI_KIT_COREMARK_MINIMUM_SECONDS * RT_TICK_PER_SECOND))
        {
            result.state = AI_KIT_BENCHMARK_STATE_ERROR;
            result.error = AI_KIT_BENCHMARK_ERROR_TOO_SHORT;
        }
        else
        {
            result.state = AI_KIT_BENCHMARK_STATE_COMPLETE;
            result.error = AI_KIT_BENCHMARK_ERROR_NONE;
        }
    }

    result.checksum = ai_kit_benchmark_result_checksum(&result);
    publish_result = ai_kit_ipc_publish_benchmark(&result);
    if (publish_result != RT_EOK)
    {
        rt_kprintf("M33 CoreMark result publish failed: %d\n", publish_result);
        return;
    }

    if (result.state == AI_KIT_BENCHMARK_STATE_COMPLETE)
    {
        rt_kprintf("M33 CoreMark result: %lu.%03lu CoreMark/s, "
                   "%lu iterations, %lu ms, system-active Release -O2\n",
                   (unsigned long)(result.metric_x1000 / 1000U),
                   (unsigned long)(result.metric_x1000 % 1000U),
                   (unsigned long)result.iterations,
                   (unsigned long)(result.elapsed_us / 1000U));
    }
    else
    {
        rt_kprintf("M33 CoreMark failed: error=%lu\n",
                   (unsigned long)result.error);
    }
}
MSH_CMD_EXPORT_ALIAS(ai_kit_m33_coremark_command, m33_coremark,
                     run system-active CoreMark on Cortex-M33);
