/**
 * @file    cm55_status.c
 * @brief   CM55 RT-Thread 状态区只读检视与基准报告导出
 *
 * @details 该文件提供三组 msh 命令：
 *          1. cm55_status   ：从 CM55 共享状态区（位于 0x262FC000）
 *                              打印 CM55 RT-Thread 心跳、显示 / 触摸 /
 *                              GPU / PSRAM 状态以及基准目录摘要
 *          2. bench_report  ：将基准目录以 CSV 或 JSON 导出，
 *                              schema=ai-kit-benchmark/v1
 *          3. 内部辅助函数  ：benchmark_name / executor_name /
 *                              state_name / unit_name / snapshot /
 *                              print_benchmark 等
 *
 *          所有读取均带 magic / abi / checksum 校验，校验失败时
 *          明确报错而非打印脏数据。
 *
 * @note    本文件仅添加注释，未改动任何可执行代码。
 */

#include <string.h>

#include <rtthread.h>
#include "ai_kit_multicore.h"

#define AI_KIT_BENCHMARK_REPORT_SCHEMA_VERSION (1U)  /**< 导出报告 schema 版本 */

/**
 * @brief   test_id 到可读名称的映射表
 *
 * @details 索引与 ai_kit_benchmark_test_t 枚举值一致；"none" 对应 0。
 */
static const char *const s_benchmark_names[] = {
    "none",
    "coremark",
    "memory_streaming",
    "ipc_latency",
    "npu_resnet_active_throughput",
    "memory_scalar_sram",
    "memory_scalar_psram",
    "npu_resnet_active_hot_wall",
    "npu_resnet_active_u55_cycles",
    "npu_resnet_active_model_init",
    "npu_mnist_active_throughput",
    "npu_mnist_active_hot_wall",
    "npu_mnist_active_u55_cycles",
    "npu_mnist_active_model_init",
    "npu_resnet_active_first",
    "npu_mnist_active_first",
    "npu_resnet_peak_throughput",
    "npu_resnet_peak_hot_wall",
    "npu_resnet_peak_u55_cycles",
    "npu_resnet_peak_model_init",
    "npu_resnet_peak_first",
    "npu_mnist_peak_throughput",
    "npu_mnist_peak_hot_wall",
    "npu_mnist_peak_u55_cycles",
    "npu_mnist_peak_model_init",
    "npu_mnist_peak_first",
    "npu_person_active_throughput",
    "npu_person_active_hot_wall",
    "npu_person_active_u55_cycles",
    "npu_person_active_model_init",
    "npu_person_active_first",
    "npu_person_peak_throughput",
    "npu_person_peak_hot_wall",
    "npu_person_peak_u55_cycles",
    "npu_person_peak_model_init",
    "npu_person_peak_first"
};

/**
 * @brief   根据 test_id 查询可读名称
 *
 * @param   test_id  测试 ID（见 ai_kit_benchmark_test_t）
 *
 * @return  const char*  对应名称；越界时返回 "unknown"
 */
static const char *benchmark_name(uint32_t test_id)
{
    return (test_id < (sizeof(s_benchmark_names) /
                       sizeof(s_benchmark_names[0]))) ?
           s_benchmark_names[test_id] : "unknown";
}

/**
 * @brief   根据 executor 返回可读执行者名称
 *
 * @param   executor  执行者 ID（M33 / M55 / NPU=85）
 *
 * @return  const char*  "m33" / "m55" / "u55" / "none"
 */
static const char *benchmark_executor_name(uint32_t executor)
{
    switch (executor)
    {
    case AI_KIT_BENCHMARK_EXECUTOR_M33: return "m33";
    case AI_KIT_BENCHMARK_EXECUTOR_M55: return "m55";
    case AI_KIT_BENCHMARK_EXECUTOR_NPU: return "u55";
    default: return "none";
    }
}

/**
 * @brief   根据 state 返回可读状态名称
 *
 * @param   state  状态 ID（IDLE / RUNNING / COMPLETE / ERROR）
 *
 * @return  const char*  状态字符串；未知返回 "unknown"
 */
static const char *benchmark_state_name(uint32_t state)
{
    switch (state)
    {
    case AI_KIT_BENCHMARK_STATE_IDLE: return "idle";
    case AI_KIT_BENCHMARK_STATE_RUNNING: return "running";
    case AI_KIT_BENCHMARK_STATE_COMPLETE: return "complete";
    case AI_KIT_BENCHMARK_STATE_ERROR: return "error";
    default: return "unknown";
    }
}

/**
 * @brief   根据 unit 返回可读单位名称
 *
 * @param   unit  单位 ID（COREMARK_PER_SECOND / MEGABYTES_PER_SECOND 等）
 *
 * @return  const char*  单位字符串；未知返回 "none"
 */
static const char *benchmark_unit_name(uint32_t unit)
{
    switch (unit)
    {
    case AI_KIT_BENCHMARK_UNIT_COREMARK_PER_SECOND: return "coremark_per_s";
    case AI_KIT_BENCHMARK_UNIT_MEGABYTES_PER_SECOND: return "mib_per_s";
    case AI_KIT_BENCHMARK_UNIT_MICROSECONDS: return "microseconds";
    case AI_KIT_BENCHMARK_UNIT_INFERENCES_PER_SECOND: return "infer_per_s";
    case AI_KIT_BENCHMARK_UNIT_KILOCYCLES: return "kilocycles";
    default: return "none";
    }
}

/**
 * @brief   从 CM55 共享状态区取出一份基准目录快照并校验
 *
 * @details 校验项：
 *          1. status->magic == AI_KIT_CM55_STATUS_MAGIC
 *          2. status->abi_version == AI_KIT_CM55_STATUS_ABI_VERSION
 *          3. catalog->protocol_version 与 ai_kit_benchmark_catalog_checksum
 *             全部匹配
 *          4. 每个 entry 的 protocol_version 与 result_checksum 匹配
 *          5. catalog->count 与非 NONE 项数一致
 *
 *          校验通过后将目录复制到调用方提供的 catalog 中。
 *
 * @param   status   CM55 共享状态区指针
 * @param   catalog  [out] 用于接收校验通过的目录
 *
 * @retval  RT_EOK     快照有效
 * @retval  -RT_ERROR  校验失败
 */
static rt_err_t benchmark_snapshot(
    const volatile ai_kit_cm55_status_t *status,
    ai_kit_benchmark_catalog_t *catalog)
{
    uint32_t index;
    uint32_t count = 0U;

    if ((status->magic != AI_KIT_CM55_STATUS_MAGIC) ||
        (status->abi_version != AI_KIT_CM55_STATUS_ABI_VERSION))
    {
        return -RT_ERROR;
    }
    *catalog = status->benchmarks;
    if ((catalog->protocol_version != AI_KIT_BENCHMARK_PROTOCOL_VERSION) ||
        (catalog->checksum != ai_kit_benchmark_catalog_checksum(catalog)))
    {
        return -RT_ERROR;
    }
    for (index = 0U; index < AI_KIT_BENCHMARK_CATALOG_CAPACITY; index++)
    {
        const ai_kit_benchmark_result_t *entry = &catalog->entries[index];

        if (entry->test_id == AI_KIT_BENCHMARK_TEST_NONE)
        {
            continue;
        }
        count++;
        if ((entry->protocol_version != AI_KIT_BENCHMARK_PROTOCOL_VERSION) ||
            (entry->checksum != ai_kit_benchmark_result_checksum(entry)))
        {
            return -RT_ERROR;
        }
    }
    return (count == catalog->count) ? RT_EOK : -RT_ERROR;
}

/**
 * @brief   打印单条基准结果，并标注 checksum 是否有效
 *
 * @details 根据 unit 字段选择不同的打印格式：
 *          - MICROSECONDS              ：avg/min/max us
 *          - COREMARK_PER_SECOND       ：CoreMark/s
 *          - MEGABYTES_PER_SECOND 且为 memory/psram：read/write/copy MiB/s
 *          - INFERENCES_PER_SECOND     ：avg/min/max infer/s
 *          - KILOCYCLES                ：avg/min/max kcycles
 *          - 其他                       ：通用 value_x1000 格式
 *
 * @param   benchmark  待打印的基准结果
 */
static void cm55_status_print_benchmark(
    const ai_kit_benchmark_result_t *benchmark)
{
    const char *valid =
        (benchmark->checksum == ai_kit_benchmark_result_checksum(benchmark)) ?
        "valid" : "invalid";

    rt_kprintf("Benchmark: seq=%u test=%u executor=%u state=%u error=%u "
               "iterations=%u elapsed=%u us\n",
               benchmark->sequence,
               benchmark->test_id,
               benchmark->executor,
               benchmark->state,
               benchmark->error,
               benchmark->iterations,
               benchmark->elapsed_us);
    if (benchmark->unit == AI_KIT_BENCHMARK_UNIT_MICROSECONDS)
    {
        rt_kprintf("Metric: avg=%u.%03u us min=%u.%03u us max=%u.%03u us "
                   "checksum=0x%08x (%s)\n",
                   benchmark->metric_x1000 / 1000U,
                   benchmark->metric_x1000 % 1000U,
                   benchmark->minimum_x1000 / 1000U,
                   benchmark->minimum_x1000 % 1000U,
                   benchmark->maximum_x1000 / 1000U,
                   benchmark->maximum_x1000 % 1000U,
                   benchmark->checksum,
                   valid);
    }
    else if (benchmark->unit == AI_KIT_BENCHMARK_UNIT_COREMARK_PER_SECOND)
    {
        rt_kprintf("Metric: %u.%03u CoreMark/s checksum=0x%08x (%s)\n",
                   benchmark->metric_x1000 / 1000U,
                   benchmark->metric_x1000 % 1000U,
                   benchmark->checksum,
                   valid);
    }
    else if (((benchmark->test_id == AI_KIT_BENCHMARK_TEST_MEMORY) ||
              (benchmark->test_id ==
               AI_KIT_BENCHMARK_TEST_MEMORY_PORTABLE) ||
              (benchmark->test_id ==
               AI_KIT_BENCHMARK_TEST_PSRAM_PORTABLE)) &&
             (benchmark->unit == AI_KIT_BENCHMARK_UNIT_MEGABYTES_PER_SECOND))
    {
        rt_kprintf("Metric: read=%u.%03u write=%u.%03u copy=%u.%03u MiB/s "
                   "checksum=0x%08x (%s)\n",
                   benchmark->metric_x1000 / 1000U,
                   benchmark->metric_x1000 % 1000U,
                   benchmark->minimum_x1000 / 1000U,
                   benchmark->minimum_x1000 % 1000U,
                   benchmark->maximum_x1000 / 1000U,
                   benchmark->maximum_x1000 % 1000U,
                   benchmark->checksum,
                   valid);
    }
    else if ((benchmark->unit ==
              AI_KIT_BENCHMARK_UNIT_INFERENCES_PER_SECOND))
    {
        rt_kprintf("Metric: avg=%u.%03u min=%u.%03u max=%u.%03u infer/s "
                   "checksum=0x%08x (%s)\n",
                   benchmark->metric_x1000 / 1000U,
                   benchmark->metric_x1000 % 1000U,
                   benchmark->minimum_x1000 / 1000U,
                   benchmark->minimum_x1000 % 1000U,
                   benchmark->maximum_x1000 / 1000U,
                   benchmark->maximum_x1000 % 1000U,
                   benchmark->checksum,
                   valid);
    }
    else if (benchmark->unit == AI_KIT_BENCHMARK_UNIT_KILOCYCLES)
    {
        rt_kprintf("Metric: avg=%u.%03u min=%u.%03u max=%u.%03u kcycles "
                   "checksum=0x%08x (%s)\n",
                   benchmark->metric_x1000 / 1000U,
                   benchmark->metric_x1000 % 1000U,
                   benchmark->minimum_x1000 / 1000U,
                   benchmark->minimum_x1000 % 1000U,
                   benchmark->maximum_x1000 / 1000U,
                   benchmark->maximum_x1000 % 1000U,
                   benchmark->checksum,
                   valid);
    }
    else
    {
        rt_kprintf("Metric: unit=%u value=%u.%03u checksum=0x%08x (%s)\n",
                   benchmark->unit,
                   benchmark->metric_x1000 / 1000U,
                   benchmark->metric_x1000 % 1000U,
                   benchmark->checksum,
                   valid);
    }
}

/**
 * @brief   msh 命令：打印 CM55 RT-Thread 状态区
 *
 * @details 从固定地址 AI_KIT_CM55_STATUS_ADDRESS 读取 CM55 状态，
 *          顺序打印：
 *          1. 心跳 / ABI / 显示 / GPU / 触摸 / PSRAM 状态
 *          2. 显示硬件帧缓冲地址与扫描位置
 *          3. FT5406 触摸事件序列
 *          4. PSRAM 私有窗口与大小
 *          5. 基准目录：revision / count / checksum 是否有效
 *          6. 每个非 NONE 条目调用 cm55_status_print_benchmark
 *
 *          任一前置校验失败即提前返回并打印错误码。
 *
 * @param   argc  msh 参数个数（未使用）
 * @param   argv  msh 参数数组（未使用）
 */
static void cm55_status(int argc, char **argv)
{
    volatile ai_kit_cm55_status_t *status =
        (volatile ai_kit_cm55_status_t *)AI_KIT_CM55_STATUS_ADDRESS;
    ai_kit_benchmark_catalog_t benchmarks;
    uint32_t index;

    RT_UNUSED(argc);
    RT_UNUSED(argv);

    if (status->magic != AI_KIT_CM55_STATUS_MAGIC)
    {
        rt_kprintf("CM55 status unavailable: magic=0x%08x state=%u heartbeat=%u\n",
                   status->magic, status->state, status->heartbeat);
        return;
    }

    if (status->abi_version != AI_KIT_CM55_STATUS_ABI_VERSION)
    {
        rt_kprintf("CM55 status ABI mismatch: expected=%u actual=%u\n",
                   AI_KIT_CM55_STATUS_ABI_VERSION,
                   status->abi_version);
        return;
    }

    rt_kprintf("CM55 RT-Thread running: ABI=%u state=%u heartbeat=%u "
               "display=%u error=%u gpu=%u\n",
               status->abi_version, status->state, status->heartbeat,
               status->display_state, status->display_error,
               status->gpu_state);
    rt_kprintf("Display HW: framebuffer=0x%08x scan=0x%08x\n",
               status->display_fb_address,
               status->display_scan_position);
    rt_kprintf("FT5406: state=%u error=%u pressed=%u event=%u "
               "raw=(%u,%u) sequence=%u\n",
               status->touch_state,
               status->touch_error,
               status->touch_pressed,
               status->touch_event,
               status->touch_x,
               status->touch_y,
               status->touch_sequence);
    rt_kprintf("PSRAM: state=%u error=%d private=0x%08x / %u KiB\n",
               status->psram_state,
               (int32_t)status->psram_error,
               status->psram_private_base,
               status->psram_private_size / 1024U);

    benchmarks = status->benchmarks;
    if (benchmarks.protocol_version != AI_KIT_BENCHMARK_PROTOCOL_VERSION)
    {
        rt_kprintf("Benchmark unavailable: protocol=%u\n",
                   benchmarks.protocol_version);
        return;
    }

    rt_kprintf("Benchmark catalog: revision=%u count=%u "
               "checksum=0x%08x (%s)\n",
               benchmarks.revision,
               benchmarks.count,
               benchmarks.checksum,
               (benchmarks.checksum ==
                ai_kit_benchmark_catalog_checksum(&benchmarks)) ?
               "valid" : "invalid");
    for (index = 0U; index < AI_KIT_BENCHMARK_CATALOG_CAPACITY; index++)
    {
        if (benchmarks.entries[index].test_id != AI_KIT_BENCHMARK_TEST_NONE)
        {
            cm55_status_print_benchmark(&benchmarks.entries[index]);
        }
    }
}
MSH_CMD_EXPORT(cm55_status, show CM55 RT-Thread boot status and heartbeat);

/**
 * @brief   以 CSV 格式导出已校验的基准目录
 *
 * @details 输出 BEGIN_AI_KIT_BENCHMARK_CSV / END_AI_KIT_BENCHMARK_CSV
 *          包围的一块 CSV，包含 schema_version、status_abi、protocol、
 *          revision、catalog_count、heartbeat、display、gpu、touch、psram
 *          以及每个非 NONE 条目的完整字段。
 *
 * @param   status   CM55 共享状态区指针（用于系统字段）
 * @param   catalog   已通过 benchmark_snapshot 校验的目录
 */
static void benchmark_report_csv(
    const volatile ai_kit_cm55_status_t *status,
    const ai_kit_benchmark_catalog_t *catalog)
{
    uint32_t index;

    rt_kprintf("BEGIN_AI_KIT_BENCHMARK_CSV\n");
    rt_kprintf("schema_version,status_abi,protocol,revision,catalog_count,"
               "heartbeat,display,gpu,touch,psram,test_id,test_name,executor,"
               "state,error,sequence,iterations,elapsed_us,unit,metric_x1000,"
               "minimum_x1000,maximum_x1000,result_checksum\n");
    for (index = 0U; index < AI_KIT_BENCHMARK_CATALOG_CAPACITY; index++)
    {
        const ai_kit_benchmark_result_t *entry = &catalog->entries[index];

        if (entry->test_id == AI_KIT_BENCHMARK_TEST_NONE)
        {
            continue;
        }
        rt_kprintf("%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%s,%s,%s,%u,%u,"
                   "%u,%u,%s,%u,%u,%u,%u\n",
                   AI_KIT_BENCHMARK_REPORT_SCHEMA_VERSION,
                   status->abi_version,
                   catalog->protocol_version,
                   catalog->revision,
                   catalog->count,
                   status->heartbeat,
                   status->display_state,
                   status->gpu_state,
                   status->touch_state,
                   status->psram_state,
                   entry->test_id,
                   benchmark_name(entry->test_id),
                   benchmark_executor_name(entry->executor),
                   benchmark_state_name(entry->state),
                   entry->error,
                   entry->sequence,
                   entry->iterations,
                   entry->elapsed_us,
                   benchmark_unit_name(entry->unit),
                   entry->metric_x1000,
                   entry->minimum_x1000,
                   entry->maximum_x1000,
                   entry->checksum);
    }
    rt_kprintf("END_AI_KIT_BENCHMARK_CSV\n");
}

/**
 * @brief   以 JSON 格式导出已校验的基准目录
 *
 * @details 输出 BEGIN_AI_KIT_BENCHMARK_JSON / END_AI_KIT_BENCHMARK_JSON
 *          包围的一块 JSON。结构：
 * @code
 *  {
 *    "schema":"ai-kit-benchmark/v1",
 *    "schema_version":1, "status_abi":..., "protocol":...,
 *    "revision":..., "catalog_count":..., "catalog_checksum":...,
 *    "validated":true,
 *    "system":{"heartbeat":...,"display":...,"gpu":...,
 *              "touch":...,"psram":...},
 *    "results":[ {entry...}, ... ]
 *  }
 *  @endcode
 *
 * @param   status   CM55 共享状态区指针（用于系统字段）
 * @param   catalog   已通过 benchmark_snapshot 校验的目录
 */
static void benchmark_report_json(
    const volatile ai_kit_cm55_status_t *status,
    const ai_kit_benchmark_catalog_t *catalog)
{
    uint32_t index;
    rt_bool_t first = RT_TRUE;

    rt_kprintf("BEGIN_AI_KIT_BENCHMARK_JSON\n");
    rt_kprintf("{\"schema\":\"ai-kit-benchmark/v1\","
               "\"schema_version\":%u,\"status_abi\":%u,"
               "\"protocol\":%u,\"revision\":%u,\"catalog_count\":%u,"
               "\"catalog_checksum\":%u,\"validated\":true,"
               "\"system\":{\"heartbeat\":%u,\"display\":%u,\"gpu\":%u,"
               "\"touch\":%u,\"psram\":%u},\"results\":[",
               AI_KIT_BENCHMARK_REPORT_SCHEMA_VERSION,
               status->abi_version,
               catalog->protocol_version,
               catalog->revision,
               catalog->count,
               catalog->checksum,
               status->heartbeat,
               status->display_state,
               status->gpu_state,
               status->touch_state,
               status->psram_state);
    for (index = 0U; index < AI_KIT_BENCHMARK_CATALOG_CAPACITY; index++)
    {
        const ai_kit_benchmark_result_t *entry = &catalog->entries[index];

        if (entry->test_id == AI_KIT_BENCHMARK_TEST_NONE)
        {
            continue;
        }
        rt_kprintf("%s{\"test_id\":%u,\"name\":\"%s\","
                   "\"executor\":\"%s\",\"state\":\"%s\",\"error\":%u,"
                   "\"sequence\":%u,\"iterations\":%u,\"elapsed_us\":%u,"
                   "\"unit\":\"%s\",\"metric_x1000\":%u,"
                   "\"minimum_x1000\":%u,\"maximum_x1000\":%u,"
                   "\"checksum\":%u}",
                   first ? "" : ",",
                   entry->test_id,
                   benchmark_name(entry->test_id),
                   benchmark_executor_name(entry->executor),
                   benchmark_state_name(entry->state),
                   entry->error,
                   entry->sequence,
                   entry->iterations,
                   entry->elapsed_us,
                   benchmark_unit_name(entry->unit),
                   entry->metric_x1000,
                   entry->minimum_x1000,
                   entry->maximum_x1000,
                   entry->checksum);
        first = RT_FALSE;
    }
    rt_kprintf("]}\nEND_AI_KIT_BENCHMARK_JSON\n");
}

/**
 * @brief   msh 命令：导出已校验的 CM55 基准报告
 *
 * @details 流程：
 *          1. 解析格式参数（默认 csv，可选 json）
 *          2. 调用 benchmark_snapshot 取得校验过的目录快照
 *          3. 调用 benchmark_report_csv 或 benchmark_report_json 输出
 *
 * @param   argc  msh 参数个数
 * @param   argv  argv[1] 可选：输出格式 "csv" 或 "json"，默认 "csv"
 */
static void bench_report(int argc, char **argv)
{
    volatile ai_kit_cm55_status_t *status =
        (volatile ai_kit_cm55_status_t *)AI_KIT_CM55_STATUS_ADDRESS;
    ai_kit_benchmark_catalog_t catalog;
    const char *format = (argc == 1) ? "csv" : argv[1];

    if ((argc > 2) ||
        ((strcmp(format, "csv") != 0) && (strcmp(format, "json") != 0)))
    {
        rt_kprintf("Usage: bench_report [csv|json]\n");
        return;
    }
    if (benchmark_snapshot(status, &catalog) != RT_EOK)
    {
        rt_kprintf("Benchmark report unavailable: snapshot validation failed\n");
        return;
    }
    if (strcmp(format, "json") == 0)
    {
        benchmark_report_json(status, &catalog);
    }
    else
    {
        benchmark_report_csv(status, &catalog);
    }
}
MSH_CMD_EXPORT(bench_report, export validated CM55 benchmark report as CSV or JSON);
