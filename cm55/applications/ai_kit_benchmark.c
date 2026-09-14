/**
 * @file    ai_kit_benchmark.c
 * @brief   AI Kit CM55 侧基准测试目录管理实现
 *
 * @details 该文件实现整机基准测试的统一目录 s_benchmark_catalog，集中管理
 *          CM55 / CM33 / NPU 三方执行者的所有测试项结果：
 *          - CoreMark            （CM55 / CM33 各一条）
 *          - 内存流式            （SRAM-DMA / SRAM-标量 / PSRAM-标量，CM55+CM33）
 *          - NPU 推理套件        （ResNet / MNIST / Person 各含 6 项指标）
 *          - IPC 往返延迟        （CM33）
 *
 *          每个 NPU 套件包含 throughput / wall_latency / hardware_cycles /
 *          model_init / first_inference 五项指标，由 ai_kit_npu_thread 统一
 *          填充。目录所有写操作均在 s_benchmark_mutex 保护下进行，写后调用
 *          ai_kit_benchmark_catalog_changed 更新 revision 与 checksum，
 *          供 CM33 通过共享内存读取最新整机状态。
 */
#include <rtthread.h>

#include "coremark.h"
#include "ai_kit_benchmark.h"
#include "ai_kit_memory_benchmark.h"
#include "ai_kit_npu.h"
#include "mtb_ml.h"

/** @brief 基准线程栈大小（16 KiB，足够 NPU 推理栈） */
#define AI_KIT_BENCHMARK_THREAD_STACK_SIZE (16U * 1024U)
/** @brief 常规基准线程优先级（约 3/4 MAX_PRIO，比 UI 低比空闲高） */
#define AI_KIT_BENCHMARK_THREAD_PRIORITY   (RT_THREAD_PRIORITY_MAX * 3U / 4U)
/** @brief NPU 峰值线程优先级（更高优先级以减少系统干扰） */
#define AI_KIT_NPU_PEAK_THREAD_PRIORITY    (8U)
/** @brief 基准线程时间片 */
#define AI_KIT_BENCHMARK_THREAD_TIMESLICE  (10U)
/** @brief CoreMark 最小有效运行秒数，低于此值判 TOO_SHORT */
#define AI_KIT_COREMARK_MINIMUM_SECONDS    (10U)

/**
 * @brief   CoreMark 便携层回调填入的报告结构
 *
 * @details coremark_port_report 由 CoreMark 便携层在测试结束时调用，
 *          把 iterations / ticks / errors 写入本结构；valid 标记用于
 *          区分"回调未触发"与"回调已触发但值为 0"。
 */
typedef struct
{
    uint32_t valid;       /**< @brief 回调是否已触发（0=未触发，1=已触发） */
    uint32_t iterations;  /**< @brief CoreMark 迭代次数 */
    uint32_t ticks;       /**< @brief 总耗时（单位 RT tick） */
    int32_t errors;       /**< @brief 校验错误数（0=通过） */
} ai_kit_coremark_report_t;

/** @brief 保护 s_benchmark_catalog 与 s_benchmark_thread 的互斥量 */
static struct rt_mutex s_benchmark_mutex;
/** @brief 目录是否已初始化（保证 init 幂等） */
static rt_bool_t s_benchmark_initialized;
/** @brief 当前运行中的基准线程句柄（同一时刻只允许一个） */
static rt_thread_t s_benchmark_thread;
/** @brief 整机基准目录，所有执行者结果汇聚于此 */
static ai_kit_benchmark_catalog_t s_benchmark_catalog;
/** @brief CoreMark 便携层报告数据 */
static ai_kit_coremark_report_t s_coremark_report;

extern int core_mark(int argc, char *argv[]);

/**
 * @brief   在目录中按 (test, executor) 查找已登记条目
 *
 * @details 线性扫描 s_benchmark_catalog.entries，匹配 test_id 与 executor。
 *          调用者需持锁。
 *
 * @param   test      测试项 ID
 * @param   executor  执行者
 *
 * @return  指向匹配条目的指针；未找到返回 RT_NULL
 */
static ai_kit_benchmark_result_t *ai_kit_benchmark_find(
    ai_kit_benchmark_test_t test,
    ai_kit_benchmark_executor_t executor)
{
    uint32_t index;

    for (index = 0U; index < AI_KIT_BENCHMARK_CATALOG_CAPACITY; index++)
    {
        ai_kit_benchmark_result_t *entry =
            &s_benchmark_catalog.entries[index];

        if ((entry->test_id == (uint32_t)test) &&
            (entry->executor == (uint32_t)executor))
        {
            return entry;
        }
    }
    return RT_NULL;
}

/**
 * @brief   查找或创建目录条目（idempotent add）
 *
 * @details 先调用 ai_kit_benchmark_find 查找已存在条目；若不存在则扫描
 *          空槽（test_id==NONE）创建新条目，写入协议版本、test/executor、
 *          状态置 IDLE，更新 checksum 与目录 count。调用者需持锁。
 *
 * @param   test      测试项 ID
 * @param   executor  执行者
 *
 * @return  指向条目的指针；目录已满返回 RT_NULL
 */
static ai_kit_benchmark_result_t *ai_kit_benchmark_add(
    ai_kit_benchmark_test_t test,
    ai_kit_benchmark_executor_t executor)
{
    uint32_t index;
    ai_kit_benchmark_result_t *entry =
        ai_kit_benchmark_find(test, executor);

    if (entry != RT_NULL)
    {
        return entry;
    }

    for (index = 0U; index < AI_KIT_BENCHMARK_CATALOG_CAPACITY; index++)
    {
        entry = &s_benchmark_catalog.entries[index];
        if (entry->test_id == AI_KIT_BENCHMARK_TEST_NONE)
        {
            rt_memset(entry, 0, sizeof(*entry));
            entry->protocol_version = AI_KIT_BENCHMARK_PROTOCOL_VERSION;
            entry->test_id = (uint32_t)test;
            entry->executor = (uint32_t)executor;
            entry->state = AI_KIT_BENCHMARK_STATE_IDLE;
            entry->checksum = ai_kit_benchmark_result_checksum(entry);
            s_benchmark_catalog.count++;
            return entry;
        }
    }
    return RT_NULL;
}

/**
 * @brief   标记目录已变更（递增 revision 并重算 checksum）
 *
 * @details 任何对 entries 的写入操作完成后必须调用本函数，使 CM33 侧
 *          通过共享内存读取时能感知目录已被更新。
 */
static void ai_kit_benchmark_catalog_changed(void)
{
    s_benchmark_catalog.revision++;
    s_benchmark_catalog.checksum =
        ai_kit_benchmark_catalog_checksum(&s_benchmark_catalog);
}

/**
 * @brief   CoreMark 便携层结果回调
 *
 * @details CoreMark 标准便携层在测试结束时调用本函数。回调把 iterations
 *          / ticks / errors 写入 s_coremark_report，置 valid=1；随后
 *          ai_kit_coremark_thread 会读取该结构计算 CoreMark 分数。
 *
 * @param   iterations  迭代次数
 * @param   ticks       总耗时（RT tick 单位）
 * @param   errors      校验错误数
 */
void coremark_port_report(ee_u32 iterations, CORE_TICKS ticks, ee_s16 errors)
{
    s_coremark_report.iterations = (uint32_t)iterations;
    s_coremark_report.ticks = (uint32_t)ticks;
    s_coremark_report.errors = (int32_t)errors;
    s_coremark_report.valid = 1U;
}

/**
 * @brief   CoreMark 测试线程入口
 *
 * @details 流程：
 *          - 清零 s_coremark_report
 *          - 调用 core_mark(0, NULL) 运行标准 CoreMark（默认至少 10 秒）
 *          - 在持锁状态下读取 s_coremark_report 计算：
 *              elapsed_us = ticks * 1e6 / RT_TICK_PER_SECOND
 *              metric_x1000 = iterations * RT_TICK_PER_SECOND * 1000 / ticks
 *          - 校验：回调未触发→REPORT_MISSING；errors!=0→VALIDATION；
 *            ticks<10s→TOO_SHORT；均通过则 state=COMPLETE
 *          - 写回 result，更新 checksum，置 s_benchmark_thread=NULL 释放槽位
 *
 * @param   parameter  RT-Thread 标准入参（未使用）
 */
static void ai_kit_coremark_thread(void *parameter)
{
    ai_kit_benchmark_result_t *result;
    uint64_t elapsed_us;
    uint64_t metric_x1000;

    RT_UNUSED(parameter);
    rt_memset(&s_coremark_report, 0, sizeof(s_coremark_report));
    (void)core_mark(0, RT_NULL);

    rt_mutex_take(&s_benchmark_mutex, RT_WAITING_FOREVER);
    result = ai_kit_benchmark_find(AI_KIT_BENCHMARK_TEST_COREMARK,
                                   AI_KIT_BENCHMARK_EXECUTOR_M55);
    RT_ASSERT(result != RT_NULL);

    if (s_coremark_report.valid == 0U)
    {
        result->state = AI_KIT_BENCHMARK_STATE_ERROR;
        result->error = AI_KIT_BENCHMARK_ERROR_REPORT_MISSING;
    }
    else
    {
        elapsed_us = ((uint64_t)s_coremark_report.ticks * 1000000ULL) /
                     (uint64_t)RT_TICK_PER_SECOND;
        metric_x1000 = ((uint64_t)s_coremark_report.iterations *
                        (uint64_t)RT_TICK_PER_SECOND * 1000ULL) /
                       (uint64_t)s_coremark_report.ticks;
        result->iterations = s_coremark_report.iterations;
        result->elapsed_us = (uint32_t)elapsed_us;
        result->metric_x1000 = (uint32_t)metric_x1000;

        if (s_coremark_report.errors != 0)
        {
            result->state = AI_KIT_BENCHMARK_STATE_ERROR;
            result->error = AI_KIT_BENCHMARK_ERROR_VALIDATION;
        }
        else if (s_coremark_report.ticks <
                 (AI_KIT_COREMARK_MINIMUM_SECONDS * RT_TICK_PER_SECOND))
        {
            result->state = AI_KIT_BENCHMARK_STATE_ERROR;
            result->error = AI_KIT_BENCHMARK_ERROR_TOO_SHORT;
        }
        else
        {
            result->state = AI_KIT_BENCHMARK_STATE_COMPLETE;
            result->error = AI_KIT_BENCHMARK_ERROR_NONE;
        }
    }

    result->checksum = ai_kit_benchmark_result_checksum(result);
    ai_kit_benchmark_catalog_changed();
    s_benchmark_thread = RT_NULL;
    rt_mutex_release(&s_benchmark_mutex);
}

/**
 * @brief   SRAM 内存流式基准线程（DMA 加速版）
 *
 * @details 调用 ai_kit_memory_benchmark_run 进行读 / 写 / 复制测试，
 *          将 metrics 填入 (TEST_MEMORY, EXECUTOR_M55) 条目：
 *          - metric_x1000  = 读带宽 (MiB/s * 1000)
 *          - minimum_x1000 = 写带宽
 *          - maximum_x1000 = 复制带宽
 *          失败时根据返回码映射 NO_MEMORY / TIMER_UNAVAILABLE / VALIDATION。
 *
 * @param   parameter  RT-Thread 标准入参（未使用）
 */
static void ai_kit_memory_thread(void *parameter)
{
    ai_kit_memory_metrics_t metrics;
    ai_kit_benchmark_result_t *result;
    int run_result;

    RT_UNUSED(parameter);
    run_result = ai_kit_memory_benchmark_run(&metrics);

    rt_mutex_take(&s_benchmark_mutex, RT_WAITING_FOREVER);
    result = ai_kit_benchmark_find(AI_KIT_BENCHMARK_TEST_MEMORY,
                                   AI_KIT_BENCHMARK_EXECUTOR_M55);
    RT_ASSERT(result != RT_NULL);

    if (run_result == RT_EOK)
    {
        result->iterations = metrics.buffer_bytes * metrics.passes;
        result->elapsed_us = metrics.elapsed_us;
        result->metric_x1000 = metrics.read_mib_s_x1000;
        result->minimum_x1000 = metrics.write_mib_s_x1000;
        result->maximum_x1000 = metrics.copy_mib_s_x1000;
        result->state = AI_KIT_BENCHMARK_STATE_COMPLETE;
        result->error = AI_KIT_BENCHMARK_ERROR_NONE;
        rt_kprintf("M55 SRAM streaming: buffer=%lu KiB passes=%lu "
                   "src=0x%08lx dst=0x%08lx\n",
                   (unsigned long)(metrics.buffer_bytes / 1024U),
                   (unsigned long)metrics.passes,
                   (unsigned long)metrics.source_address,
                   (unsigned long)metrics.destination_address);
        rt_kprintf("  read=%lu.%03lu write=%lu.%03lu copy=%lu.%03lu "
                   "MiB/s elapsed=%lu us\n",
                   (unsigned long)(metrics.read_mib_s_x1000 / 1000U),
                   (unsigned long)(metrics.read_mib_s_x1000 % 1000U),
                   (unsigned long)(metrics.write_mib_s_x1000 / 1000U),
                   (unsigned long)(metrics.write_mib_s_x1000 % 1000U),
                   (unsigned long)(metrics.copy_mib_s_x1000 / 1000U),
                   (unsigned long)(metrics.copy_mib_s_x1000 % 1000U),
                   (unsigned long)metrics.elapsed_us);
    }
    else
    {
        result->state = AI_KIT_BENCHMARK_STATE_ERROR;
        result->error = (run_result == -RT_ENOMEM) ?
                        AI_KIT_BENCHMARK_ERROR_NO_MEMORY :
                        ((run_result == -RT_ETIMEOUT) ?
                         AI_KIT_BENCHMARK_ERROR_TIMER_UNAVAILABLE :
                         AI_KIT_BENCHMARK_ERROR_VALIDATION);
    }

    result->checksum = ai_kit_benchmark_result_checksum(result);
    ai_kit_benchmark_catalog_changed();
    s_benchmark_thread = RT_NULL;
    rt_mutex_release(&s_benchmark_mutex);
}

/**
 * @brief   SRAM 内存标量基准线程（无 DMA，纯 CPU 循环）
 *
 * @details 与 ai_kit_memory_thread 类似，但调用
 *          ai_kit_memory_portable_benchmark_run 走标量 C 实现，
 *          结果写入 (TEST_MEMORY_PORTABLE, EXECUTOR_M55) 条目，
 *          用以对比 DMA 加速与纯 CPU 拷贝的带宽差距。
 *
 * @param   parameter  RT-Thread 标准入参（未使用）
 */
static void ai_kit_memory_portable_thread(void *parameter)
{
    ai_kit_memory_metrics_t metrics;
    ai_kit_benchmark_result_t *result;
    int run_result;

    RT_UNUSED(parameter);
    run_result = ai_kit_memory_portable_benchmark_run(&metrics);

    rt_mutex_take(&s_benchmark_mutex, RT_WAITING_FOREVER);
    result = ai_kit_benchmark_find(AI_KIT_BENCHMARK_TEST_MEMORY_PORTABLE,
                                   AI_KIT_BENCHMARK_EXECUTOR_M55);
    RT_ASSERT(result != RT_NULL);

    if (run_result == RT_EOK)
    {
        result->iterations = metrics.buffer_bytes * metrics.passes;
        result->elapsed_us = metrics.elapsed_us;
        result->metric_x1000 = metrics.read_mib_s_x1000;
        result->minimum_x1000 = metrics.write_mib_s_x1000;
        result->maximum_x1000 = metrics.copy_mib_s_x1000;
        result->state = AI_KIT_BENCHMARK_STATE_COMPLETE;
        result->error = AI_KIT_BENCHMARK_ERROR_NONE;
        rt_kprintf("M55 portable scalar SRAM: buffer=%lu KiB passes=%lu "
                   "src=0x%08lx dst=0x%08lx\n",
                   (unsigned long)(metrics.buffer_bytes / 1024U),
                   (unsigned long)metrics.passes,
                   (unsigned long)metrics.source_address,
                   (unsigned long)metrics.destination_address);
        rt_kprintf("  read=%lu.%03lu write=%lu.%03lu copy=%lu.%03lu "
                   "MiB/s elapsed=%lu us\n",
                   (unsigned long)(metrics.read_mib_s_x1000 / 1000U),
                   (unsigned long)(metrics.read_mib_s_x1000 % 1000U),
                   (unsigned long)(metrics.write_mib_s_x1000 / 1000U),
                   (unsigned long)(metrics.write_mib_s_x1000 % 1000U),
                   (unsigned long)(metrics.copy_mib_s_x1000 / 1000U),
                   (unsigned long)(metrics.copy_mib_s_x1000 % 1000U),
                   (unsigned long)metrics.elapsed_us);
    }
    else
    {
        result->state = AI_KIT_BENCHMARK_STATE_ERROR;
        result->error = (run_result == -RT_ENOMEM) ?
                        AI_KIT_BENCHMARK_ERROR_NO_MEMORY :
                        ((run_result == -RT_ETIMEOUT) ?
                         AI_KIT_BENCHMARK_ERROR_TIMER_UNAVAILABLE :
                         AI_KIT_BENCHMARK_ERROR_VALIDATION);
    }

    result->checksum = ai_kit_benchmark_result_checksum(result);
    ai_kit_benchmark_catalog_changed();
    s_benchmark_thread = RT_NULL;
    rt_mutex_release(&s_benchmark_mutex);
}

/**
 * @brief   PSRAM 标量内存基准线程
 *
 * @details 调用 ai_kit_psram_portable_benchmark_run 在外部 PSRAM 上跑
 *          标量读 / 写 / 复制，结果写入 (TEST_PSRAM_PORTABLE, EXECUTOR_M55)，
 *          用以评估片外 PSRAM 的可用带宽。运行后会自动恢复原数据。
 *
 * @param   parameter  RT-Thread 标准入参（未使用）
 */
static void ai_kit_psram_portable_thread(void *parameter)
{
    ai_kit_memory_metrics_t metrics;
    ai_kit_benchmark_result_t *result;
    int run_result;

    RT_UNUSED(parameter);
    run_result = ai_kit_psram_portable_benchmark_run(&metrics);

    rt_mutex_take(&s_benchmark_mutex, RT_WAITING_FOREVER);
    result = ai_kit_benchmark_find(AI_KIT_BENCHMARK_TEST_PSRAM_PORTABLE,
                                   AI_KIT_BENCHMARK_EXECUTOR_M55);
    RT_ASSERT(result != RT_NULL);

    if (run_result == RT_EOK)
    {
        result->iterations = metrics.buffer_bytes * metrics.passes;
        result->elapsed_us = metrics.elapsed_us;
        result->metric_x1000 = metrics.read_mib_s_x1000;
        result->minimum_x1000 = metrics.write_mib_s_x1000;
        result->maximum_x1000 = metrics.copy_mib_s_x1000;
        result->state = AI_KIT_BENCHMARK_STATE_COMPLETE;
        result->error = AI_KIT_BENCHMARK_ERROR_NONE;
        rt_kprintf("M55 portable scalar PSRAM: buffer=%lu KiB passes=%lu "
                   "src=0x%08lx dst=0x%08lx\n",
                   (unsigned long)(metrics.buffer_bytes / 1024U),
                   (unsigned long)metrics.passes,
                   (unsigned long)metrics.source_address,
                   (unsigned long)metrics.destination_address);
        rt_kprintf("  read=%lu.%03lu write=%lu.%03lu copy=%lu.%03lu "
                   "MiB/s elapsed=%lu us / original data restored\n",
                   (unsigned long)(metrics.read_mib_s_x1000 / 1000U),
                   (unsigned long)(metrics.read_mib_s_x1000 % 1000U),
                   (unsigned long)(metrics.write_mib_s_x1000 / 1000U),
                   (unsigned long)(metrics.write_mib_s_x1000 % 1000U),
                   (unsigned long)(metrics.copy_mib_s_x1000 / 1000U),
                   (unsigned long)(metrics.copy_mib_s_x1000 % 1000U),
                   (unsigned long)metrics.elapsed_us);
    }
    else
    {
        result->state = AI_KIT_BENCHMARK_STATE_ERROR;
        result->error = (run_result == -RT_ENOMEM) ?
                        AI_KIT_BENCHMARK_ERROR_NO_MEMORY :
                        ((run_result == -RT_ETIMEOUT) ?
                         AI_KIT_BENCHMARK_ERROR_TIMER_UNAVAILABLE :
                         AI_KIT_BENCHMARK_ERROR_VALIDATION);
    }

    result->checksum = ai_kit_benchmark_result_checksum(result);
    ai_kit_benchmark_catalog_changed();
    s_benchmark_thread = RT_NULL;
    rt_mutex_release(&s_benchmark_mutex);
}

/**
 * @brief   由单次推理延迟（us*1000）换算吞吐率（inferences/s*1000）
 *
 * @details throughput_x1000 = 1e12 / latency_us_x1000。延迟为 0 时返回 0
 *          以避免除零。
 *
 * @param   latency_us_x1000  单次推理平均延迟（单位 us*1000）
 *
 * @return  吞吐率（单位 inferences/s*1000）
 */
static uint32_t ai_kit_npu_throughput_x1000(uint32_t latency_us_x1000)
{
    return (latency_us_x1000 == 0U) ? 0U :
           (uint32_t)(1000000000000ULL / latency_us_x1000);
}

/**
 * @brief   NPU 基准套件描述符
 *
 * @details 一个套件绑定一个 workload 与 5 个目录条目 ID：throughput /
 *          wall_latency / hardware_cycles / model_init / first_inference，
 *          由 ai_kit_npu_thread 一次性填充。isolated=true 表示峰值模式
 *          （更高线程优先级，减少系统干扰）。
 */
typedef struct
{
    ai_kit_npu_workload_t workload;        /**< @brief 工作负载类型 */
    ai_kit_benchmark_test_t throughput;    /**< @brief 吞吐率条目 ID */
    ai_kit_benchmark_test_t wall_latency;  /**< @brief 端到端延迟条目 ID */
    ai_kit_benchmark_test_t hardware_cycles; /**< @brief NPU 硬件周期条目 ID */
    ai_kit_benchmark_test_t model_init;     /**< @brief 模型初始化耗时条目 ID */
    ai_kit_benchmark_test_t first_inference; /**< @brief 首次推理耗时条目 ID */
    rt_bool_t isolated;   /**< @brief 是否峰值模式（独立高优先级线程） */
    const char *thread_name; /**< @brief 线程名（用于 rt_thread_create） */
} ai_kit_npu_suite_t;

/**
 * @brief   NPU 套件静态表
 *
 * @details 公开版本只提供 Person Detection 的"系统活跃"与
 *          "isolated-peak"两套指标。
 */
static const ai_kit_npu_suite_t s_npu_suites[] = {
    {
        AI_KIT_NPU_WORKLOAD_PERSON,
        AI_KIT_BENCHMARK_TEST_NPU_PERSON_INFERENCE,
        AI_KIT_BENCHMARK_TEST_NPU_PERSON_WALL_LATENCY,
        AI_KIT_BENCHMARK_TEST_NPU_PERSON_HARDWARE_CYCLES,
        AI_KIT_BENCHMARK_TEST_NPU_PERSON_MODEL_INIT,
        AI_KIT_BENCHMARK_TEST_NPU_PERSON_FIRST_INFERENCE,
        RT_FALSE,
        "npu_person"
    },
    {
        AI_KIT_NPU_WORKLOAD_PERSON,
        AI_KIT_BENCHMARK_TEST_NPU_PERSON_PEAK_INFERENCE,
        AI_KIT_BENCHMARK_TEST_NPU_PERSON_PEAK_WALL_LATENCY,
        AI_KIT_BENCHMARK_TEST_NPU_PERSON_PEAK_HARDWARE_CYCLES,
        AI_KIT_BENCHMARK_TEST_NPU_PERSON_PEAK_MODEL_INIT,
        AI_KIT_BENCHMARK_TEST_NPU_PERSON_PEAK_FIRST_INFERENCE,
        RT_TRUE,
        "npu_p_peak"
    }
};

/**
 * @brief   按 throughput 条目 ID 反查套件描述符
 *
 * @param   throughput  套件中的 throughput 测试 ID
 *
 * @return  指向匹配套件的指针；未匹配返回 RT_NULL
 */
static const ai_kit_npu_suite_t *ai_kit_npu_suite_find(
    ai_kit_benchmark_test_t throughput)
{
    uint32_t index;

    for (index = 0U; index < (sizeof(s_npu_suites) / sizeof(s_npu_suites[0]));
         index++)
    {
        if (s_npu_suites[index].throughput == throughput)
        {
            return &s_npu_suites[index];
        }
    }
    return RT_NULL;
}

/**
 * @brief   重置套件 4 个辅助指标条目（wall_latency / hardware_cycles /
 *          model_init / first_inference）到指定状态
 *
 * @details 由 ai_kit_benchmark_start 在启动 NPU 测试或创建线程失败时调用，
 *          把辅助条目同步设为 RUNNING 或 ERROR，统一 sequence 与 error，
 *          清零 metric，并按测试项写 unit（hardware_cycles 用 kilocycles，
 *          其他用 microseconds）。调用者需持锁。
 *
 * @param   suite     套件指针
 * @param   sequence  当前主条目 sequence
 * @param   state     要设置的状态
 * @param   error     要设置的 error
 */
static void ai_kit_npu_aux_reset(const ai_kit_npu_suite_t *suite,
                                 uint32_t sequence,
                                 ai_kit_benchmark_state_t state,
                                 ai_kit_benchmark_error_t error)
{
    const ai_kit_benchmark_test_t aux_tests[] = {
        suite->wall_latency,
        suite->hardware_cycles,
        suite->model_init,
        suite->first_inference
    };
    uint32_t index;

    for (index = 0U; index < (sizeof(aux_tests) / sizeof(aux_tests[0]));
         index++)
    {
        ai_kit_benchmark_result_t *aux = ai_kit_benchmark_find(
            aux_tests[index], AI_KIT_BENCHMARK_EXECUTOR_NPU);

        RT_ASSERT(aux != RT_NULL);
        aux->sequence = sequence;
        aux->state = state;
        aux->error = error;
        aux->iterations = 0U;
        aux->elapsed_us = 0U;
        aux->unit = (aux_tests[index] == suite->hardware_cycles) ?
                    AI_KIT_BENCHMARK_UNIT_KILOCYCLES :
                    AI_KIT_BENCHMARK_UNIT_MICROSECONDS;
        aux->metric_x1000 = 0U;
        aux->minimum_x1000 = 0U;
        aux->maximum_x1000 = 0U;
        aux->checksum = ai_kit_benchmark_result_checksum(aux);
    }
}

/**
 * @brief   NPU 推理基准线程入口
 *
 * @details 流程：
 *          - 调用 ai_kit_npu_benchmark_run(workload, &metrics) 执行
 *            ResNet / MNIST / Person 模型推理
 *          - 在持锁状态下找到套件对应的 5 个条目并填充：
 *              throughput        = inferences/s*1000（由 latency 换算）
 *              wall_latency      = 平均 / 最小 / 最大端到端延迟（us*1000）
 *              hardware_cycles   = NPU 周期数（kilocycles）
 *              model_init        = 模型初始化耗时（us*1000）
 *              first_inference   = 首次推理耗时（us*1000）
 *          - 通过/失败均会写 state 与 error，并打印详细日志
 *          - 写回所有条目的 checksum，更新目录，置 s_benchmark_thread=NULL
 *
 * @param   parameter  指向 ai_kit_npu_suite_t 套件描述符
 */
static void ai_kit_npu_thread(void *parameter)
{
    const ai_kit_npu_suite_t *suite =
        (const ai_kit_npu_suite_t *)parameter;
    ai_kit_npu_metrics_t metrics;
    ai_kit_benchmark_result_t *throughput;
    ai_kit_benchmark_result_t *wall_latency;
    ai_kit_benchmark_result_t *hardware_cycles;
    ai_kit_benchmark_result_t *model_init;
    ai_kit_benchmark_result_t *first_inference;
    int run_result;

    RT_ASSERT(suite != RT_NULL);
    run_result = ai_kit_npu_benchmark_run(suite->workload, &metrics);

    rt_mutex_take(&s_benchmark_mutex, RT_WAITING_FOREVER);
    throughput = ai_kit_benchmark_find(suite->throughput,
                                       AI_KIT_BENCHMARK_EXECUTOR_NPU);
    wall_latency = ai_kit_benchmark_find(
        suite->wall_latency,
        AI_KIT_BENCHMARK_EXECUTOR_NPU);
    hardware_cycles = ai_kit_benchmark_find(
        suite->hardware_cycles,
        AI_KIT_BENCHMARK_EXECUTOR_NPU);
    model_init = ai_kit_benchmark_find(suite->model_init,
                                       AI_KIT_BENCHMARK_EXECUTOR_NPU);
    first_inference = ai_kit_benchmark_find(
        suite->first_inference, AI_KIT_BENCHMARK_EXECUTOR_NPU);
    RT_ASSERT((throughput != RT_NULL) && (wall_latency != RT_NULL) &&
              (hardware_cycles != RT_NULL) && (model_init != RT_NULL) &&
              (first_inference != RT_NULL));

    if (run_result == RT_EOK)
    {
        uint32_t total_elapsed_us =
            (uint32_t)(((uint64_t)metrics.average_us_x1000 *
                        metrics.iterations) / 1000ULL);

        throughput->iterations = metrics.iterations;
        throughput->elapsed_us = total_elapsed_us;
        throughput->unit = AI_KIT_BENCHMARK_UNIT_INFERENCES_PER_SECOND;
        throughput->metric_x1000 =
            ai_kit_npu_throughput_x1000(metrics.average_us_x1000);
        throughput->minimum_x1000 =
            ai_kit_npu_throughput_x1000(metrics.maximum_us_x1000);
        throughput->maximum_x1000 =
            ai_kit_npu_throughput_x1000(metrics.minimum_us_x1000);
        throughput->state = AI_KIT_BENCHMARK_STATE_COMPLETE;
        throughput->error = AI_KIT_BENCHMARK_ERROR_NONE;

        wall_latency->iterations = metrics.iterations;
        wall_latency->elapsed_us = total_elapsed_us;
        wall_latency->unit = AI_KIT_BENCHMARK_UNIT_MICROSECONDS;
        wall_latency->metric_x1000 = metrics.average_us_x1000;
        wall_latency->minimum_x1000 = metrics.minimum_us_x1000;
        wall_latency->maximum_x1000 = metrics.maximum_us_x1000;
        wall_latency->state = AI_KIT_BENCHMARK_STATE_COMPLETE;
        wall_latency->error = AI_KIT_BENCHMARK_ERROR_NONE;

        hardware_cycles->iterations = metrics.iterations;
        hardware_cycles->elapsed_us = total_elapsed_us;
        hardware_cycles->unit = AI_KIT_BENCHMARK_UNIT_KILOCYCLES;
        hardware_cycles->metric_x1000 = metrics.average_npu_cycles;
        hardware_cycles->minimum_x1000 = metrics.minimum_npu_cycles;
        hardware_cycles->maximum_x1000 = metrics.maximum_npu_cycles;
        hardware_cycles->state = AI_KIT_BENCHMARK_STATE_COMPLETE;
        hardware_cycles->error = AI_KIT_BENCHMARK_ERROR_NONE;

        model_init->iterations = 1U;
        model_init->elapsed_us =
            (metrics.model_init_us_x1000 + 500U) / 1000U;
        model_init->unit = AI_KIT_BENCHMARK_UNIT_MICROSECONDS;
        model_init->metric_x1000 = metrics.model_init_us_x1000;
        model_init->minimum_x1000 = metrics.model_init_us_x1000;
        model_init->maximum_x1000 = metrics.model_init_us_x1000;
        model_init->state = AI_KIT_BENCHMARK_STATE_COMPLETE;
        model_init->error = AI_KIT_BENCHMARK_ERROR_NONE;

        first_inference->iterations = 1U;
        first_inference->elapsed_us =
            (metrics.first_inference_us_x1000 + 500U) / 1000U;
        first_inference->unit = AI_KIT_BENCHMARK_UNIT_MICROSECONDS;
        first_inference->metric_x1000 = metrics.first_inference_us_x1000;
        first_inference->minimum_x1000 = metrics.first_inference_us_x1000;
        first_inference->maximum_x1000 = metrics.first_inference_us_x1000;
        first_inference->state = AI_KIT_BENCHMARK_STATE_COMPLETE;
        first_inference->error = AI_KIT_BENCHMARK_ERROR_NONE;

        rt_kprintf("NPU %s %s PASS: %lu/%lu class=%lu model=%lu arena=%lu\n",
                   ai_kit_npu_workload_name(suite->workload),
                   suite->isolated ? "isolated-peak" : "system-active",
                   (unsigned long)metrics.correct,
                   (unsigned long)metrics.iterations,
                   (unsigned long)metrics.observed_class,
                   (unsigned long)metrics.model_bytes,
                   (unsigned long)metrics.arena_bytes);
        rt_kprintf("  wall=%lu.%03lu us avg min=%lu.%03lu max=%lu.%03lu\n",
                   (unsigned long)(metrics.average_us_x1000 / 1000U),
                   (unsigned long)(metrics.average_us_x1000 % 1000U),
                   (unsigned long)(metrics.minimum_us_x1000 / 1000U),
                   (unsigned long)(metrics.minimum_us_x1000 % 1000U),
                   (unsigned long)(metrics.maximum_us_x1000 / 1000U),
                   (unsigned long)(metrics.maximum_us_x1000 % 1000U));
        rt_kprintf("  npu_cycles=%lu avg min=%lu max=%lu at %luMHz\n",
                   (unsigned long)metrics.average_npu_cycles,
                   (unsigned long)metrics.minimum_npu_cycles,
                   (unsigned long)metrics.maximum_npu_cycles,
                   (unsigned long)(mtb_ml_npu_clk_freq / 1000000U));
        rt_kprintf("  model_init=%lu.%03lu us\n",
                   (unsigned long)(metrics.model_init_us_x1000 / 1000U),
                   (unsigned long)(metrics.model_init_us_x1000 % 1000U));
        rt_kprintf("  first_inference=%lu.%03lu us cold_total=%lu.%03lu us\n",
                   (unsigned long)(metrics.first_inference_us_x1000 / 1000U),
                   (unsigned long)(metrics.first_inference_us_x1000 % 1000U),
                   (unsigned long)((metrics.model_init_us_x1000 +
                                    metrics.first_inference_us_x1000) / 1000U),
                   (unsigned long)((metrics.model_init_us_x1000 +
                                    metrics.first_inference_us_x1000) % 1000U));
    }
    else
    {
        uint32_t error = (run_result == -RT_ETIMEOUT) ?
                         AI_KIT_BENCHMARK_ERROR_TIMER_UNAVAILABLE :
                         AI_KIT_BENCHMARK_ERROR_VALIDATION;

        throughput->state = AI_KIT_BENCHMARK_STATE_ERROR;
        throughput->error = error;
        wall_latency->state = AI_KIT_BENCHMARK_STATE_ERROR;
        wall_latency->error = error;
        hardware_cycles->state = AI_KIT_BENCHMARK_STATE_ERROR;
        hardware_cycles->error = error;
        model_init->state = AI_KIT_BENCHMARK_STATE_ERROR;
        model_init->error = error;
        first_inference->state = AI_KIT_BENCHMARK_STATE_ERROR;
        first_inference->error = error;
        rt_kprintf("NPU %s FAIL: %d\n",
                   ai_kit_npu_workload_name(suite->workload), run_result);
    }

    throughput->checksum = ai_kit_benchmark_result_checksum(throughput);
    wall_latency->checksum = ai_kit_benchmark_result_checksum(wall_latency);
    hardware_cycles->checksum =
        ai_kit_benchmark_result_checksum(hardware_cycles);
    model_init->checksum = ai_kit_benchmark_result_checksum(model_init);
    first_inference->checksum =
        ai_kit_benchmark_result_checksum(first_inference);
    ai_kit_benchmark_catalog_changed();
    s_benchmark_thread = RT_NULL;
    rt_mutex_release(&s_benchmark_mutex);
}

/**
 * @brief   初始化基准测试目录（对外接口，幂等）
 *
 * @details 完成：
 *          - rt_mutex_init "bench"
 *          - 清零目录并写协议版本
 *          - 预登记所有受支持条目（CoreMark / 内存 / NPU 全部 5 项 / IPC），
 *            每个条目初始 state=IDLE
 *          - 标记 s_benchmark_initialized
 *
 * @retval  RT_EOK       成功或已初始化
 * @retval  -RT_ERROR    rt_mutex_init 失败
 * @retval  -RT_ENOMEM   目录容量不足
 */
int ai_kit_benchmark_init(void)
{
    if (s_benchmark_initialized)
    {
        return RT_EOK;
    }
    if (rt_mutex_init(&s_benchmark_mutex, "bench", RT_IPC_FLAG_PRIO) != RT_EOK)
    {
        return -RT_ERROR;
    }

    rt_memset(&s_benchmark_catalog, 0, sizeof(s_benchmark_catalog));
    s_benchmark_catalog.protocol_version = AI_KIT_BENCHMARK_PROTOCOL_VERSION;
    if ((ai_kit_benchmark_add(AI_KIT_BENCHMARK_TEST_COREMARK,
                              AI_KIT_BENCHMARK_EXECUTOR_M55) == RT_NULL) ||
        (ai_kit_benchmark_add(AI_KIT_BENCHMARK_TEST_COREMARK,
                              AI_KIT_BENCHMARK_EXECUTOR_M33) == RT_NULL) ||
        (ai_kit_benchmark_add(AI_KIT_BENCHMARK_TEST_MEMORY,
                              AI_KIT_BENCHMARK_EXECUTOR_M55) == RT_NULL) ||
        (ai_kit_benchmark_add(AI_KIT_BENCHMARK_TEST_MEMORY,
                              AI_KIT_BENCHMARK_EXECUTOR_M33) == RT_NULL) ||
        (ai_kit_benchmark_add(AI_KIT_BENCHMARK_TEST_MEMORY_PORTABLE,
                              AI_KIT_BENCHMARK_EXECUTOR_M55) == RT_NULL) ||
        (ai_kit_benchmark_add(AI_KIT_BENCHMARK_TEST_MEMORY_PORTABLE,
                              AI_KIT_BENCHMARK_EXECUTOR_M33) == RT_NULL) ||
        (ai_kit_benchmark_add(AI_KIT_BENCHMARK_TEST_PSRAM_PORTABLE,
                              AI_KIT_BENCHMARK_EXECUTOR_M55) == RT_NULL) ||
        (ai_kit_benchmark_add(AI_KIT_BENCHMARK_TEST_PSRAM_PORTABLE,
                              AI_KIT_BENCHMARK_EXECUTOR_M33) == RT_NULL) ||
        (ai_kit_benchmark_add(AI_KIT_BENCHMARK_TEST_NPU_PERSON_INFERENCE,
                              AI_KIT_BENCHMARK_EXECUTOR_NPU) == RT_NULL) ||
        (ai_kit_benchmark_add(AI_KIT_BENCHMARK_TEST_NPU_PERSON_WALL_LATENCY,
                              AI_KIT_BENCHMARK_EXECUTOR_NPU) == RT_NULL) ||
        (ai_kit_benchmark_add(
                              AI_KIT_BENCHMARK_TEST_NPU_PERSON_HARDWARE_CYCLES,
                              AI_KIT_BENCHMARK_EXECUTOR_NPU) == RT_NULL) ||
        (ai_kit_benchmark_add(AI_KIT_BENCHMARK_TEST_NPU_PERSON_MODEL_INIT,
                              AI_KIT_BENCHMARK_EXECUTOR_NPU) == RT_NULL) ||
        (ai_kit_benchmark_add(
                              AI_KIT_BENCHMARK_TEST_NPU_PERSON_FIRST_INFERENCE,
                              AI_KIT_BENCHMARK_EXECUTOR_NPU) == RT_NULL) ||
        (ai_kit_benchmark_add(
                              AI_KIT_BENCHMARK_TEST_NPU_PERSON_PEAK_INFERENCE,
                              AI_KIT_BENCHMARK_EXECUTOR_NPU) == RT_NULL) ||
        (ai_kit_benchmark_add(
                           AI_KIT_BENCHMARK_TEST_NPU_PERSON_PEAK_WALL_LATENCY,
                           AI_KIT_BENCHMARK_EXECUTOR_NPU) == RT_NULL) ||
        (ai_kit_benchmark_add(
                        AI_KIT_BENCHMARK_TEST_NPU_PERSON_PEAK_HARDWARE_CYCLES,
                        AI_KIT_BENCHMARK_EXECUTOR_NPU) == RT_NULL) ||
        (ai_kit_benchmark_add(
                               AI_KIT_BENCHMARK_TEST_NPU_PERSON_PEAK_MODEL_INIT,
                               AI_KIT_BENCHMARK_EXECUTOR_NPU) == RT_NULL) ||
        (ai_kit_benchmark_add(
                         AI_KIT_BENCHMARK_TEST_NPU_PERSON_PEAK_FIRST_INFERENCE,
                         AI_KIT_BENCHMARK_EXECUTOR_NPU) == RT_NULL) ||
        (ai_kit_benchmark_add(AI_KIT_BENCHMARK_TEST_IPC_LATENCY,
                              AI_KIT_BENCHMARK_EXECUTOR_M33) == RT_NULL))
    {
        return -RT_ENOMEM;
    }
    ai_kit_benchmark_catalog_changed();
    s_benchmark_initialized = RT_TRUE;
    return RT_EOK;
}

/**
 * @brief   启动一次本地基准测试（对外接口）
 *
 * @details 流程：
 *          - 校验 test 是否受支持（COREMARK / MEMORY / MEMORY_PORTABLE /
 *            PSRAM_PORTABLE 或 NPU 套件 ID）
 *          - 自动初始化目录
 *          - 持锁检查 s_benchmark_thread 是否已占用
 *          - 在条目上递增 sequence、置 RUNNING、清零 metric、写 unit
 *          - NPU 套件需调用 ai_kit_npu_aux_reset 同步辅助条目
 *          - 按 test 选择线程入口与名称；NPU 峰值套件使用更高优先级
 *          - rt_thread_create + rt_thread_startup
 *          - 失败时回滚条目状态为 ERROR / THREAD_CREATE
 *
 * @param   test  待启动的测试项 ID
 *
 * @retval  RT_EOK       线程已创建并启动
 * @retval  -RT_EINVAL   test 不受支持
 * @retval  -RT_ERROR    目录初始化失败
 * @retval  -RT_EBUSY    已有基准线程运行
 * @retval  -RT_ENOMEM   rt_thread_create 失败
 * @retval  其他         rt_thread_startup 失败码
 */
int ai_kit_benchmark_start(ai_kit_benchmark_test_t test)
{
    ai_kit_benchmark_result_t *result;
    rt_thread_t thread;
    rt_err_t start_result;
    void (*thread_entry)(void *parameter);
    void *thread_parameter = RT_NULL;
    const char *thread_name;
    rt_uint8_t thread_priority = AI_KIT_BENCHMARK_THREAD_PRIORITY;
    const ai_kit_npu_suite_t *npu_suite = ai_kit_npu_suite_find(test);

    if ((test != AI_KIT_BENCHMARK_TEST_COREMARK) &&
        (test != AI_KIT_BENCHMARK_TEST_MEMORY) &&
        (test != AI_KIT_BENCHMARK_TEST_MEMORY_PORTABLE) &&
        (test != AI_KIT_BENCHMARK_TEST_PSRAM_PORTABLE) &&
        (npu_suite == RT_NULL))
    {
        return -RT_EINVAL;
    }
    if (!s_benchmark_initialized && (ai_kit_benchmark_init() != RT_EOK))
    {
        return -RT_ERROR;
    }

    rt_mutex_take(&s_benchmark_mutex, RT_WAITING_FOREVER);
    if (s_benchmark_thread != RT_NULL)
    {
        rt_mutex_release(&s_benchmark_mutex);
        return -RT_EBUSY;
    }

    result = ai_kit_benchmark_find(
        test, (npu_suite != RT_NULL) ?
              AI_KIT_BENCHMARK_EXECUTOR_NPU :
              AI_KIT_BENCHMARK_EXECUTOR_M55);
    RT_ASSERT(result != RT_NULL);
    result->sequence++;
    result->state = AI_KIT_BENCHMARK_STATE_RUNNING;
    result->error = AI_KIT_BENCHMARK_ERROR_NONE;
    result->iterations = 0U;
    result->elapsed_us = 0U;
    result->unit = (test == AI_KIT_BENCHMARK_TEST_COREMARK) ?
                   AI_KIT_BENCHMARK_UNIT_COREMARK_PER_SECOND :
                   ((npu_suite != RT_NULL) ?
                    AI_KIT_BENCHMARK_UNIT_INFERENCES_PER_SECOND :
                    AI_KIT_BENCHMARK_UNIT_MEGABYTES_PER_SECOND);
    result->metric_x1000 = 0U;
    result->minimum_x1000 = 0U;
    result->maximum_x1000 = 0U;
    result->checksum = ai_kit_benchmark_result_checksum(result);

    if (npu_suite != RT_NULL)
    {
        ai_kit_npu_aux_reset(npu_suite, result->sequence,
                             AI_KIT_BENCHMARK_STATE_RUNNING,
                             AI_KIT_BENCHMARK_ERROR_NONE);
    }
    ai_kit_benchmark_catalog_changed();

    if (test == AI_KIT_BENCHMARK_TEST_COREMARK)
    {
        thread_entry = ai_kit_coremark_thread;
        thread_name = "coremark";
    }
    else if (test == AI_KIT_BENCHMARK_TEST_MEMORY_PORTABLE)
    {
        thread_entry = ai_kit_memory_portable_thread;
        thread_name = "sramscalar";
    }
    else if (test == AI_KIT_BENCHMARK_TEST_PSRAM_PORTABLE)
    {
        thread_entry = ai_kit_psram_portable_thread;
        thread_name = "psramscalar";
    }
    else if (npu_suite != RT_NULL)
    {
        thread_entry = ai_kit_npu_thread;
        thread_parameter = (void *)npu_suite;
        thread_name = npu_suite->thread_name;
        if (npu_suite->isolated)
        {
            thread_priority = AI_KIT_NPU_PEAK_THREAD_PRIORITY;
        }
    }
    else
    {
        thread_entry = ai_kit_memory_thread;
        thread_name = "srambench";
    }
    thread = rt_thread_create(thread_name, thread_entry, thread_parameter,
                              AI_KIT_BENCHMARK_THREAD_STACK_SIZE,
                              thread_priority,
                              AI_KIT_BENCHMARK_THREAD_TIMESLICE);
    if (thread == RT_NULL)
    {
        result->state = AI_KIT_BENCHMARK_STATE_ERROR;
        result->error = AI_KIT_BENCHMARK_ERROR_THREAD_CREATE;
        result->checksum = ai_kit_benchmark_result_checksum(result);
        if (npu_suite != RT_NULL)
        {
            ai_kit_npu_aux_reset(npu_suite, result->sequence,
                                 AI_KIT_BENCHMARK_STATE_ERROR,
                                 AI_KIT_BENCHMARK_ERROR_THREAD_CREATE);
        }
        ai_kit_benchmark_catalog_changed();
        rt_mutex_release(&s_benchmark_mutex);
        return -RT_ENOMEM;
    }

    s_benchmark_thread = thread;
    start_result = rt_thread_startup(thread);
    if (start_result != RT_EOK)
    {
        s_benchmark_thread = RT_NULL;
        result->state = AI_KIT_BENCHMARK_STATE_ERROR;
        result->error = AI_KIT_BENCHMARK_ERROR_THREAD_CREATE;
        result->checksum = ai_kit_benchmark_result_checksum(result);
        if (npu_suite != RT_NULL)
        {
            ai_kit_npu_aux_reset(npu_suite, result->sequence,
                                 AI_KIT_BENCHMARK_STATE_ERROR,
                                 AI_KIT_BENCHMARK_ERROR_THREAD_CREATE);
        }
        ai_kit_benchmark_catalog_changed();
        rt_thread_delete(thread);
        rt_mutex_release(&s_benchmark_mutex);
        return start_result;
    }
    rt_mutex_release(&s_benchmark_mutex);
    return RT_EOK;
}

/**
 * @brief   导入 CM33 上报的基准结果（对外接口）
 *
 * @details 校验 protocol_version / executor==M33 / test_id 在受支持列表
 *          （COREMARK / MEMORY / MEMORY_PORTABLE / PSRAM_PORTABLE / IPC_LATENCY）
 *          / checksum 一致；通过后持锁查找 (test, EXECUTOR_M33) 条目并
 *          整体覆盖，更新目录。
 *
 * @param   result  指向 CM33 共享内存中读出的结果结构
 *
 * @retval  RT_EOK       导入成功
 * @retval  -RT_EINVAL   result 空 / 字段非法 / checksum 不匹配
 * @retval  -RT_ERROR    目录未就绪且初始化失败
 */
int ai_kit_benchmark_publish_external(
    const ai_kit_benchmark_result_t *result)
{
    ai_kit_benchmark_result_t *entry;

    if ((result == RT_NULL) ||
        (result->protocol_version != AI_KIT_BENCHMARK_PROTOCOL_VERSION) ||
        (result->executor != AI_KIT_BENCHMARK_EXECUTOR_M33) ||
        ((result->test_id != AI_KIT_BENCHMARK_TEST_COREMARK) &&
         (result->test_id != AI_KIT_BENCHMARK_TEST_MEMORY) &&
         (result->test_id != AI_KIT_BENCHMARK_TEST_MEMORY_PORTABLE) &&
         (result->test_id != AI_KIT_BENCHMARK_TEST_PSRAM_PORTABLE) &&
         (result->test_id != AI_KIT_BENCHMARK_TEST_IPC_LATENCY)) ||
        (result->checksum != ai_kit_benchmark_result_checksum(result)))
    {
        return -RT_EINVAL;
    }
    if (!s_benchmark_initialized && (ai_kit_benchmark_init() != RT_EOK))
    {
        return -RT_ERROR;
    }

    rt_mutex_take(&s_benchmark_mutex, RT_WAITING_FOREVER);
    entry = ai_kit_benchmark_find((ai_kit_benchmark_test_t)result->test_id,
                                  AI_KIT_BENCHMARK_EXECUTOR_M33);
    RT_ASSERT(entry != RT_NULL);
    *entry = *result;
    ai_kit_benchmark_catalog_changed();
    rt_mutex_release(&s_benchmark_mutex);
    return RT_EOK;
}

/**
 * @brief   查询单项基准结果（对外接口）
 *
 * @param   test      测试项 ID
 * @param   executor  执行者（M55 / M33 / NPU）
 * @param   result    输出参数，拷贝对应条目内容；失败时清零
 *
 * @retval  RT_EOK      查询成功
 * @retval  -RT_EINVAL  result 为空
 * @retval  -RT_ERROR   目录未就绪且初始化失败
 * @retval  -RT_ENOSYS  条目不存在
 */
int ai_kit_benchmark_get_result(ai_kit_benchmark_test_t test,
                                ai_kit_benchmark_executor_t executor,
                                ai_kit_benchmark_result_t *result)
{
    ai_kit_benchmark_result_t *entry;

    if (result == RT_NULL)
    {
        return -RT_EINVAL;
    }
    if (!s_benchmark_initialized && (ai_kit_benchmark_init() != RT_EOK))
    {
        rt_memset(result, 0, sizeof(*result));
        return -RT_ERROR;
    }

    rt_mutex_take(&s_benchmark_mutex, RT_WAITING_FOREVER);
    entry = ai_kit_benchmark_find(test, executor);
    if (entry == RT_NULL)
    {
        rt_memset(result, 0, sizeof(*result));
        rt_mutex_release(&s_benchmark_mutex);
        return -RT_ENOSYS;
    }
    *result = *entry;
    rt_mutex_release(&s_benchmark_mutex);
    return RT_EOK;
}

/**
 * @brief   获取整个基准目录快照（对外接口）
 *
 * @details 持锁拷贝整个 s_benchmark_catalog 到调用者缓冲区，适合一次性
 *          导出整机性能报告（如 CM33 msh 命令）。失败时清零。
 *
 * @param   catalog  输出参数
 */
void ai_kit_benchmark_get_catalog(ai_kit_benchmark_catalog_t *catalog)
{
    if (catalog == RT_NULL)
    {
        return;
    }
    if (!s_benchmark_initialized && (ai_kit_benchmark_init() != RT_EOK))
    {
        rt_memset(catalog, 0, sizeof(*catalog));
        return;
    }

    rt_mutex_take(&s_benchmark_mutex, RT_WAITING_FOREVER);
    *catalog = s_benchmark_catalog;
    rt_mutex_release(&s_benchmark_mutex);
}
