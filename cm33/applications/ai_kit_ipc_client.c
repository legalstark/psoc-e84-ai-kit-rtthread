/**
 * @file    ai_kit_ipc_client.c
 * @brief   AI Kit CM33 侧 IPC 客户端实现与基准命令集合
 *
 * @details 该文件实现 CM33 通过 IPC 管道与 CM55 通信的客户端逻辑，
 *          并导出一组 msh 命令用于触发跨核基准测试：
 *
 *          核心事务层
 *          - ai_kit_ipc_client_init：单次初始化 IPC 设备与信号量
 *          - ai_kit_ipc_transact   ：发送 request 帧并阻塞等待 response
 *          - ai_kit_ipc_drain_rx    ：丢弃陈旧残留帧，避免污染下一轮测量
 *          - ai_kit_ipc_timer_*    ：基于 DWT->CYCCNT 的高精度计时与
 *                                     计时器空载开销校准
 *
 *          公开 API
 *          - ai_kit_ipc_publish_benchmark
 *          - ai_kit_ipc_start_m55_benchmark
 *
 *          msh 命令
 *          - m55_coremark / m55_sram_bench / m55_sram_scalar / m55_psram_scalar
 *          - npu_bench / npu_mnist_bench / npu_resnet_peak / npu_mnist_peak
 *          - npu_person_bench / npu_person_peak
 *          - ipc_ping  / ipc_bench
 *
 *          所有跨核基准结果统一通过 ai_kit_benchmark_result_t 描述，
 *          并附带二级 checksum（帧 checksum + 结果 checksum）保证完整性。
 *
 * @note    本文件仅添加注释，未改动任何可执行代码。
 */

#include <stdlib.h>

#include <rtdevice.h>
#include <rtthread.h>

#include "board.h"
#include "drv_ipc.h"
#include "ai_kit_multicore.h"
#include "ai_kit_ipc_client.h"

#define AI_KIT_IPC_PING_TIMEOUT_MS       (1000U)   /**< PING / 基准启动事务超时（ms） */
#define AI_KIT_IPC_BENCH_TIMEOUT_MS      (100U)    /**< IPC 延迟测量单次事务超时（ms） */
#define AI_KIT_IPC_PING_PAYLOAD          (0xA5845533UL) /**< PING 携带的回声载荷 */
#define AI_KIT_IPC_BENCH_DEFAULT_COUNT   (10000U)  /**< IPC 延迟测量默认迭代次数 */
#define AI_KIT_IPC_BENCH_MIN_COUNT       (100U)    /**< IPC 延迟测量最小迭代次数 */
#define AI_KIT_IPC_BENCH_MAX_COUNT       (20000U)  /**< IPC 延迟测量最大迭代次数 */
#define AI_KIT_IPC_BENCH_WARMUP_COUNT    (32U)     /**< 正式测量前的预热迭代次数 */
#define AI_KIT_IPC_TIMER_CALIBRATIONS     (32U)     /**< 计时器空载开销采样次数 */

/**
 * @brief   IPC 基准槽：结果体 + 保留字，凑齐 64 字节
 *
 * @details 帧本身为 32 字节 cache line，槽对齐到 32 字节便于共享内存
 *          DCache 维护。reserved 保证结构体大小严格为 64 字节。
 */
typedef struct
{
    ai_kit_benchmark_result_t result;
    uint32_t reserved[3];
} ai_kit_ipc_benchmark_slot_t;

/** @brief  编译期断言：基准槽必须严格 64 字节 */
typedef char ai_kit_ipc_benchmark_slot_must_be_64_bytes[
    (sizeof(ai_kit_ipc_benchmark_slot_t) == 64U) ? 1 : -1];

static rt_device_t g_ipc_device;                /**< IPC 设备句柄 */
static struct rt_semaphore g_rx_event;           /**< 接收中断到事务的同步信号量 */
static rt_bool_t g_client_initialized;          /**< 客户端是否已完成初始化 */
static rt_uint32_t g_ipc_sequence;              /**< IPC 帧自增序列号 */
static rt_uint32_t g_benchmark_sequence;        /**< 基准结果自增序列号 */

/** @brief  共享内存中的基准结果槽（CM33 与 CM55 共享，32 字节对齐） */
CY_SECTION_SHAREDMEM
__attribute__((aligned(32)))
static ai_kit_ipc_benchmark_slot_t g_benchmark_slot;

/**
 * @brief   IPC 设备接收回调
 *
 * @details 每当 IPC 设备在中断上下文中收到数据时被调用，仅释放一次
 *          信号量，通知事务线程有 response 可读。
 *
 * @param   device  触发回调的设备（未使用）
 * @param   size    本次到达的字节数（未使用）
 *
 * @return  rt_err_t  rt_sem_release 的返回值
 */
static rt_err_t ai_kit_ipc_rx_indicate(rt_device_t device, rt_size_t size)
{
    (void)device;
    (void)size;
    return rt_sem_release(&g_rx_event);
}

/**
 * @brief   初始化 IPC 客户端（单次幂等）
 *
 * @details 完成：
 *          - 查找并打开 ipc0 设备（RDWR + INT_RX）
 *          - 初始化接收信号量 ipc_rx
 *          - 注册接收回调
 *
 *          已初始化时直接返回 RT_EOK。
 *
 * @retval  RT_EOK       初始化成功或已初始化
 * @retval  -RT_ENOSYS   ipc0 设备未注册
 * @retval  其他         rt_device_open / rt_sem_init 失败码
 */
static rt_err_t ai_kit_ipc_client_init(void)
{
    rt_err_t result;

    if (g_client_initialized)
    {
        return RT_EOK;
    }

    g_ipc_device = ai_kit_ipc_device_find();
    if (g_ipc_device == RT_NULL)
    {
        return -RT_ENOSYS;
    }

    result = rt_sem_init(&g_rx_event, "ipc_rx", 0U, RT_IPC_FLAG_PRIO);
    if (result != RT_EOK)
    {
        return result;
    }

    result = rt_device_open(g_ipc_device,
                            RT_DEVICE_OFLAG_RDWR |
                            RT_DEVICE_FLAG_INT_RX);
    if (result != RT_EOK)
    {
        rt_sem_detach(&g_rx_event);
        return result;
    }

    rt_device_set_rx_indicate(g_ipc_device, ai_kit_ipc_rx_indicate);
    g_client_initialized = RT_TRUE;
    return RT_EOK;
}

/**
 * @brief   排空 IPC 接收队列与信号量的陈旧数据
 *
 * @details 调用 ai_kit_ipc_transact 之前调用，避免上一次失败或中断
 *          残留的 response 干扰下一轮事务。同时把信号量计数清零。
 */
static void ai_kit_ipc_drain_rx(void)
{
    ai_kit_ipc_frame_t stale;

    while (rt_sem_take(&g_rx_event, 0) == RT_EOK)
    {
    }
    while (rt_device_read(g_ipc_device, 0, &stale, 1U) == 1)
    {
    }
}

/**
 * @brief   发起一次 IPC 请求/响应事务
 *
 * @details 流程：
 *          1. 自增序列号并填充 request 帧（kind=REQUEST, src=M33, dst=M55）
 *          2. 若需要计时，先 __DSB / __ISB，再读取 DWT->CYCCNT 作为起点
 *          3. 写出 request 帧（一次写一帧）；失败返回 -RT_EIO
 *          4. 阻塞等待接收信号量，超时返回 -RT_ETIMEOUT
 *          5. 读取 response 帧；失败返回 -RT_ERROR
 *          6. 若需要计时，计算 elapsed = CYCCNT - start
 *          7. 校验 response 的 kind/command/sequence 与 request 一致，
 *             否则返回 -RT_ERROR
 *
 * @param   command         IPC 命令（PING / BENCHMARK_PUBLISH / BENCHMARK_START）
 * @param   argument0       命令参数 0（载荷或 test_id）
 * @param   argument1       命令参数 1（长度或 0）
 * @param   timeout_ms     事务超时（毫秒），内部转为 RT-Thread tick
 * @param   response       [out] 接收到的响应帧
 * @param   elapsed_cycles [out] 可选，事务消耗的 M33 周期数；NULL 表示不计时
 *
 * @retval  RT_EOK       事务成功且 response 校验通过
 * @retval  -RT_EIO      发送失败
 * @retval  -RT_ETIMEOUT 等待响应超时
 * @retval  -RT_ERROR    读取响应失败或校验失败
 */
static rt_err_t ai_kit_ipc_transact(ai_kit_ipc_command_t command,
                                    uint32_t argument0,
                                    uint32_t argument1,
                                    uint32_t timeout_ms,
                                    ai_kit_ipc_frame_t *response,
                                    uint32_t *elapsed_cycles)
{
    ai_kit_ipc_frame_t request;
    uint32_t start_cycles = 0U;

    g_ipc_sequence++;
    ai_kit_ipc_frame_prepare(&request,
                             AI_KIT_IPC_KIND_REQUEST,
                             AI_KIT_IPC_CORE_M33,
                             AI_KIT_IPC_CORE_M55,
                             g_ipc_sequence,
                             command,
                             argument0,
                             argument1);

    if (elapsed_cycles != RT_NULL)
    {
        __DSB();
        __ISB();
        start_cycles = DWT->CYCCNT;
    }

    if (rt_device_write(g_ipc_device, 0, &request, 1U) != 1)
    {
        return -RT_EIO;
    }

    if (rt_sem_take(&g_rx_event,
                    rt_tick_from_millisecond(timeout_ms)) != RT_EOK)
    {
        return -RT_ETIMEOUT;
    }

    if (rt_device_read(g_ipc_device, 0, response, 1U) != 1)
    {
        return -RT_ERROR;
    }

    if (elapsed_cycles != RT_NULL)
    {
        *elapsed_cycles = DWT->CYCCNT - start_cycles;
    }

    if ((response->kind != AI_KIT_IPC_KIND_RESPONSE) ||
        (response->command != (uint32_t)command) ||
        (response->sequence != request.sequence))
    {
        return -RT_ERROR;
    }

    return RT_EOK;
}

/**
 * @brief   打印本地 IPC 设备统计计数
 *
 * @details 通过 AI_KIT_IPC_CTRL_GET_STATS 控制码获取驱动内部统计，
 *          包括 tx_ok/rx_ok/tx_error/rx_error/drop/busy/retry/timeout/release。
 */
static void ai_kit_ipc_print_stats(void)
{
    ai_kit_ipc_device_stats_t stats;

    if (rt_device_control(g_ipc_device,
                          AI_KIT_IPC_CTRL_GET_STATS,
                          &stats) == RT_EOK)
    {
        rt_kprintf("IPC local stats: tx=%lu rx=%lu "
                   "tx_error=%lu rx_error=%lu drop=%lu "
                   "busy=%lu retry=%lu timeout=%lu release=%lu\n",
                   (unsigned long)stats.tx_ok,
                   (unsigned long)stats.rx_ok,
                   (unsigned long)stats.tx_error,
                   (unsigned long)stats.rx_error,
                   (unsigned long)stats.rx_drop,
                   (unsigned long)stats.tx_busy,
                   (unsigned long)stats.tx_retry,
                   (unsigned long)stats.tx_timeout,
                   (unsigned long)stats.tx_release);
    }
}

/**
 * @brief   使能 M33 DWT 周期计数器（CYCCNT）
 *
 * @details 顺序：置位 DEMCR.TRCENA → 复位 CYCCNT → 置位
 *          DWT_CTRL.CYCCNTENA → DSB/ISB。然后空跑 128 个 NOP，
 *          若 CYCCNT 发生变化则视为可用。
 *
 * @retval  RT_TRUE   CYCCNT 已正常自增
 * @retval  RT_FALSE  计数器未使能或 debugger 未连接
 */
static rt_bool_t ai_kit_ipc_timer_enable(void)
{
    volatile uint32_t spin;
    uint32_t start;

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

    return (DWT->CYCCNT != start);
}

/**
 * @brief   测量 CYCCNT 读取代价（空载开销）
 *
 * @details 连续 AI_KIT_IPC_TIMER_CALIBRATIONS 次读取"start = CYCCNT;
 *          delta = CYCCNT - start"，取最小值作为单次计时基准开销，
 *          在最终测量中扣除。
 *
 * @return  uint32_t  最小空载周期数
 */
static uint32_t ai_kit_ipc_timer_overhead(void)
{
    uint32_t minimum = UINT32_MAX;
    uint32_t index;

    for (index = 0U; index < AI_KIT_IPC_TIMER_CALIBRATIONS; index++)
    {
        uint32_t start = DWT->CYCCNT;
        uint32_t delta = DWT->CYCCNT - start;

        if (delta < minimum)
        {
            minimum = delta;
        }
    }

    return minimum;
}

/**
 * @brief   初始化 IPC 延迟基准结果结构体
 *
 * @details 清零共享内存槽、自增本地基准序列号，并预填：
 *          - protocol_version
 *          - sequence
 *          - test_id  = IPC_LATENCY
 *          - executor = M33
 *          - state    = RUNNING
 *          - error    = NONE
 *          - unit     = MICROSECONDS
 *
 * @param   result  [out] 待初始化的结果结构体
 */
static void ai_kit_ipc_result_init(ai_kit_benchmark_result_t *result)
{
    rt_memset(&g_benchmark_slot, 0, sizeof(g_benchmark_slot));
    g_benchmark_sequence++;
    result->protocol_version = AI_KIT_BENCHMARK_PROTOCOL_VERSION;
    result->sequence = g_benchmark_sequence;
    result->test_id = AI_KIT_BENCHMARK_TEST_IPC_LATENCY;
    result->executor = AI_KIT_BENCHMARK_EXECUTOR_M33;
    result->state = AI_KIT_BENCHMARK_STATE_RUNNING;
    result->error = AI_KIT_BENCHMARK_ERROR_NONE;
    result->unit = AI_KIT_BENCHMARK_UNIT_MICROSECONDS;
}

rt_err_t ai_kit_ipc_publish_benchmark(
    const ai_kit_benchmark_result_t *result)
{
    ai_kit_ipc_frame_t response;
    rt_err_t remote_result;

    if ((result == RT_NULL) ||
        (result->protocol_version != AI_KIT_BENCHMARK_PROTOCOL_VERSION) ||
        (result->executor != AI_KIT_BENCHMARK_EXECUTOR_M33) ||
        (result->checksum != ai_kit_benchmark_result_checksum(result)))
    {
        return -RT_EINVAL;
    }

    remote_result = ai_kit_ipc_client_init();
    if (remote_result != RT_EOK)
    {
        return remote_result;
    }

    g_benchmark_slot.result = *result;
#if defined(__DCACHE_PRESENT) && (__DCACHE_PRESENT == 1U)
    SCB_CleanDCache_by_Addr((uint32_t *)&g_benchmark_slot,
                            (int32_t)sizeof(g_benchmark_slot));
    __DSB();
#endif

    ai_kit_ipc_drain_rx();
    if (ai_kit_ipc_transact(
            AI_KIT_IPC_COMMAND_BENCHMARK_PUBLISH,
            (uint32_t)(uintptr_t)&g_benchmark_slot.result,
            sizeof(g_benchmark_slot.result),
            AI_KIT_IPC_PING_TIMEOUT_MS,
            &response,
            RT_NULL) != RT_EOK)
    {
        return -RT_ERROR;
    }

    remote_result = (rt_err_t)response.argument0;
    if ((remote_result != RT_EOK) ||
        (response.argument1 != g_benchmark_slot.result.checksum))
    {
        return (remote_result != RT_EOK) ? remote_result : -RT_ERROR;
    }

    return RT_EOK;
}

rt_err_t ai_kit_ipc_start_m55_benchmark(ai_kit_benchmark_test_t test)
{
    ai_kit_ipc_frame_t response;
    rt_err_t result;

    if ((test != AI_KIT_BENCHMARK_TEST_COREMARK) &&
        (test != AI_KIT_BENCHMARK_TEST_MEMORY) &&
        (test != AI_KIT_BENCHMARK_TEST_MEMORY_PORTABLE) &&
        (test != AI_KIT_BENCHMARK_TEST_PSRAM_PORTABLE) &&
        (test != AI_KIT_BENCHMARK_TEST_NPU_RESNET_INFERENCE) &&
        (test != AI_KIT_BENCHMARK_TEST_NPU_MNIST_INFERENCE) &&
        (test != AI_KIT_BENCHMARK_TEST_NPU_RESNET_PEAK_INFERENCE) &&
        (test != AI_KIT_BENCHMARK_TEST_NPU_MNIST_PEAK_INFERENCE) &&
        (test != AI_KIT_BENCHMARK_TEST_NPU_PERSON_INFERENCE) &&
        (test != AI_KIT_BENCHMARK_TEST_NPU_PERSON_PEAK_INFERENCE))
    {
        return -RT_EINVAL;
    }
    result = ai_kit_ipc_client_init();
    if (result != RT_EOK)
    {
        return result;
    }

    ai_kit_ipc_drain_rx();
    result = ai_kit_ipc_transact(AI_KIT_IPC_COMMAND_BENCHMARK_START,
                                 (uint32_t)test,
                                 0U,
                                 AI_KIT_IPC_PING_TIMEOUT_MS,
                                 &response,
                                 RT_NULL);
    if (result != RT_EOK)
    {
        return result;
    }
    if (response.argument1 != (uint32_t)test)
    {
        return -RT_ERROR;
    }
    return (rt_err_t)response.argument0;
}

/**
 * @brief   msh 命令：通过 IPC 触发 CM55 CoreMark
 *
 * @details 调用 ai_kit_ipc_start_m55_benchmark，并打印 M55 是否接收请求。
 *          实际基准结果由 CM55 写回状态区，通过 cm55_status 命令查询。
 */
static void ai_kit_m55_coremark_command(void)
{
    rt_err_t result = ai_kit_ipc_start_m55_benchmark(
        AI_KIT_BENCHMARK_TEST_COREMARK);

    rt_kprintf("M55 CoreMark request: %s (%d)\n",
               (result == RT_EOK) ? "started" : "failed", result);
}
MSH_CMD_EXPORT_ALIAS(ai_kit_m55_coremark_command, m55_coremark,
                     start M55 system-active CoreMark through IPC);

/**
 * @brief   msh 命令：通过 IPC 触发 CM55 SRAM 流式基准
 */
static void ai_kit_m55_memory_benchmark_command(void)
{
    rt_err_t result = ai_kit_ipc_start_m55_benchmark(
        AI_KIT_BENCHMARK_TEST_MEMORY);

    rt_kprintf("M55 SRAM streaming request: %s (%d)\n",
               (result == RT_EOK) ? "started" : "failed", result);
}
MSH_CMD_EXPORT_ALIAS(ai_kit_m55_memory_benchmark_command, m55_sram_bench,
                     start M55 cache-on SRAM streaming through IPC);

/**
 * @brief   msh 命令：通过 IPC 触发 CM55 便携标量 SRAM 基准
 */
static void ai_kit_m55_portable_memory_command(void)
{
    rt_err_t result = ai_kit_ipc_start_m55_benchmark(
        AI_KIT_BENCHMARK_TEST_MEMORY_PORTABLE);

    rt_kprintf("M55 portable SRAM request: %s (%d)\n",
               (result == RT_EOK) ? "started" : "failed", result);
}
MSH_CMD_EXPORT_ALIAS(ai_kit_m55_portable_memory_command, m55_sram_scalar,
                     start M55 portable scalar SRAM benchmark through IPC);

/**
 * @brief   msh 命令：通过 IPC 触发 CM55 私有 PSRAM 基准
 */
static void ai_kit_m55_psram_benchmark_command(void)
{
    rt_err_t result = ai_kit_ipc_start_m55_benchmark(
        AI_KIT_BENCHMARK_TEST_PSRAM_PORTABLE);

    rt_kprintf("M55 PSRAM benchmark request: %s (%d)\n",
               (result == RT_EOK) ? "started" : "failed", result);
}
MSH_CMD_EXPORT_ALIAS(ai_kit_m55_psram_benchmark_command, m55_psram_scalar,
                     start M55 private PSRAM benchmark through IPC);

/**
 * @brief   msh 命令：通过 IPC 触发 CM55+U55 ResNet 活跃态吞吐基准
 */
static void ai_kit_npu_benchmark_command(void)
{
    rt_err_t result = ai_kit_ipc_start_m55_benchmark(
        AI_KIT_BENCHMARK_TEST_NPU_RESNET_INFERENCE);

    rt_kprintf("CM55/U55 ResNet benchmark request: %s (%d)\n",
               (result == RT_EOK) ? "started" : "failed", result);
}
MSH_CMD_EXPORT_ALIAS(ai_kit_npu_benchmark_command, npu_bench,
                     start CM55 Ethos-U55 ResNet benchmark through IPC);

/**
 * @brief   msh 命令：通过 IPC 触发 CM55+U55 MNIST 活跃态吞吐基准
 */
static void ai_kit_npu_mnist_benchmark_command(void)
{
    rt_err_t result = ai_kit_ipc_start_m55_benchmark(
        AI_KIT_BENCHMARK_TEST_NPU_MNIST_INFERENCE);

    rt_kprintf("CM55/U55 MNIST benchmark request: %s (%d)\n",
               (result == RT_EOK) ? "started" : "failed", result);
}
MSH_CMD_EXPORT_ALIAS(ai_kit_npu_mnist_benchmark_command, npu_mnist_bench,
                     start CM55 Ethos-U55 MNIST benchmark through IPC);

/**
 * @brief   msh 命令：通过 IPC 触发 ResNet 隔离峰值基准
 */
static void ai_kit_npu_resnet_peak_command(void)
{
    rt_err_t result = ai_kit_ipc_start_m55_benchmark(
        AI_KIT_BENCHMARK_TEST_NPU_RESNET_PEAK_INFERENCE);

    rt_kprintf("CM55/U55 ResNet isolated peak request: %s (%d)\n",
               (result == RT_EOK) ? "started" : "failed", result);
}
MSH_CMD_EXPORT_ALIAS(ai_kit_npu_resnet_peak_command, npu_resnet_peak,
                     run CM55 Ethos-U55 ResNet isolated peak benchmark);

/**
 * @brief   msh 命令：通过 IPC 触发 MNIST 隔离峰值基准
 */
static void ai_kit_npu_mnist_peak_command(void)
{
    rt_err_t result = ai_kit_ipc_start_m55_benchmark(
        AI_KIT_BENCHMARK_TEST_NPU_MNIST_PEAK_INFERENCE);

    rt_kprintf("CM55/U55 MNIST isolated peak request: %s (%d)\n",
               (result == RT_EOK) ? "started" : "failed", result);
}
MSH_CMD_EXPORT_ALIAS(ai_kit_npu_mnist_peak_command, npu_mnist_peak,
                     run CM55 Ethos-U55 MNIST isolated peak benchmark);

/**
 * @brief   msh 命令：通过 IPC 触发 Person Detection 活跃态吞吐基准
 */
static void ai_kit_npu_person_benchmark_command(void)
{
    rt_err_t result = ai_kit_ipc_start_m55_benchmark(
        AI_KIT_BENCHMARK_TEST_NPU_PERSON_INFERENCE);

    rt_kprintf("CM55/U55 Person Detection benchmark request: %s (%d)\n",
               (result == RT_EOK) ? "started" : "failed", result);
}
MSH_CMD_EXPORT_ALIAS(ai_kit_npu_person_benchmark_command, npu_person_bench,
                     start CM55 Ethos-U55 Person Detection benchmark);

/**
 * @brief   msh 命令：通过 IPC 触发 Person detection 隔离峰值基准
 */
static void ai_kit_npu_person_peak_command(void)
{
    rt_err_t result = ai_kit_ipc_start_m55_benchmark(
        AI_KIT_BENCHMARK_TEST_NPU_PERSON_PEAK_INFERENCE);

    rt_kprintf("CM55/U55 Person Detection peak request: %s (%d)\n",
               (result == RT_EOK) ? "started" : "failed", result);
}
MSH_CMD_EXPORT_ALIAS(ai_kit_npu_person_peak_command, npu_person_peak,
                     run CM55 Ethos-U55 Person Detection isolated peak);

/**
 * @brief   msh 命令：对 CM55 发起一次 PING，测量端到端往返时延
 *
 * @details 流程：初始化客户端 → 排空接收队列 → 记录开始 tick →
 *          发起 PING 事务（载荷 AI_KIT_IPC_PING_PAYLOAD）→
 *          计算并打印 RTT（ms）、回声载荷、远端 tick，最后打印本地统计。
 */
static void ai_kit_ipc_ping_command(void)
{
    ai_kit_ipc_frame_t response;
    rt_tick_t start_tick;
    rt_tick_t elapsed_tick;
    rt_err_t result;

    result = ai_kit_ipc_client_init();
    if (result != RT_EOK)
    {
        rt_kprintf("IPC init failed: %d\n", result);
        return;
    }

    ai_kit_ipc_drain_rx();
    start_tick = rt_tick_get();
    result = ai_kit_ipc_transact(AI_KIT_IPC_COMMAND_PING,
                                 AI_KIT_IPC_PING_PAYLOAD,
                                 0U,
                                 AI_KIT_IPC_PING_TIMEOUT_MS,
                                 &response,
                                 RT_NULL);
    if (result != RT_EOK)
    {
        rt_kprintf("IPC ping failed: %d\n", result);
        return;
    }

    elapsed_tick = rt_tick_get() - start_tick;
    rt_kprintf("IPC ping OK: M33 -> M55 -> M33, seq=%lu, "
               "echo=0x%08lx, remote_tick=%lu, rtt=%lu ms\n",
               (unsigned long)response.sequence,
               (unsigned long)response.argument0,
               (unsigned long)response.argument1,
               (unsigned long)
               ((elapsed_tick * 1000U) / RT_TICK_PER_SECOND));
    ai_kit_ipc_print_stats();
}
MSH_CMD_EXPORT_ALIAS(ai_kit_ipc_ping_command, ipc_ping,
                     ping CM55 through the AI Kit IPC pipe);

/**
 * @brief   msh 命令：测量 M33→M55 应用层往返延迟
 *
 * @details 流程：
 *          1. 解析可选参数 iterations（100..20000），默认 10000
 *          2. 初始化客户端、初始化基准结果、使能 DWT、校准空载开销
 *          3. 进行 AI_KIT_IPC_BENCH_WARMUP_COUNT 次预热 PING
 *          4. 正式迭代：每轮记录 elapsed_cycles 并扣除 timer_overhead，
 *             同时维护 min/max/total
 *          5. 按 SystemCoreClock 换算为 us 与 min/max/avg us
 *          6. 通过 IPC 发布基准结果（state=COMPLETE 或 ERROR），
 *             并打印本地统计
 *
 * @param   argc  msh 参数个数
 * @param   argv  argv[1] 可选：iterations，取值范围 100..20000
 */
static void ai_kit_ipc_benchmark_command(int argc, char **argv)
{
    ai_kit_benchmark_result_t *benchmark = &g_benchmark_slot.result;
    ai_kit_ipc_frame_t response;
    uint64_t total_cycles = 0ULL;
    uint64_t denominator;
    uint32_t minimum_cycles = UINT32_MAX;
    uint32_t maximum_cycles = 0U;
    uint32_t timer_overhead;
    uint32_t iterations = AI_KIT_IPC_BENCH_DEFAULT_COUNT;
    uint32_t completed = 0U;
    uint32_t index;
    rt_err_t result;

    if (argc > 2)
    {
        rt_kprintf("Usage: ipc_bench [100..20000]\n");
        return;
    }

    if (argc == 2)
    {
        char *end;
        unsigned long requested = strtoul(argv[1], &end, 10);

        if ((*end != '\0') ||
            (requested < AI_KIT_IPC_BENCH_MIN_COUNT) ||
            (requested > AI_KIT_IPC_BENCH_MAX_COUNT))
        {
            rt_kprintf("Usage: ipc_bench [100..20000]\n");
            return;
        }
        iterations = (uint32_t)requested;
    }

    result = ai_kit_ipc_client_init();
    if (result != RT_EOK)
    {
        rt_kprintf("IPC init failed: %d\n", result);
        return;
    }

    ai_kit_ipc_result_init(benchmark);
    if (!ai_kit_ipc_timer_enable())
    {
        benchmark->state = AI_KIT_BENCHMARK_STATE_ERROR;
        benchmark->error = AI_KIT_BENCHMARK_ERROR_TIMER_UNAVAILABLE;
        benchmark->checksum = ai_kit_benchmark_result_checksum(benchmark);
        (void)ai_kit_ipc_publish_benchmark(benchmark);
        rt_kprintf("IPC benchmark failed: M33 DWT cycle counter unavailable\n");
        return;
    }

    timer_overhead = ai_kit_ipc_timer_overhead();
    ai_kit_ipc_drain_rx();

    for (index = 0U; index < AI_KIT_IPC_BENCH_WARMUP_COUNT; index++)
    {
        result = ai_kit_ipc_transact(AI_KIT_IPC_COMMAND_PING,
                                     AI_KIT_IPC_PING_PAYLOAD,
                                     0U,
                                     AI_KIT_IPC_BENCH_TIMEOUT_MS,
                                     &response,
                                     RT_NULL);
        if (result != RT_EOK)
        {
            break;
        }
    }

    if (result == RT_EOK)
    {
        for (index = 0U; index < iterations; index++)
        {
            uint32_t cycles;

            result = ai_kit_ipc_transact(AI_KIT_IPC_COMMAND_PING,
                                         AI_KIT_IPC_PING_PAYLOAD,
                                         0U,
                                         AI_KIT_IPC_BENCH_TIMEOUT_MS,
                                         &response,
                                         &cycles);
            if (result != RT_EOK)
            {
                break;
            }

            if (cycles > timer_overhead)
            {
                cycles -= timer_overhead;
            }
            if (cycles < minimum_cycles)
            {
                minimum_cycles = cycles;
            }
            if (cycles > maximum_cycles)
            {
                maximum_cycles = cycles;
            }
            total_cycles += cycles;
            completed++;
        }
    }

    benchmark->iterations = completed;
    if (completed == iterations)
    {
        denominator = (uint64_t)SystemCoreClock * completed;
        benchmark->elapsed_us =
            (uint32_t)((total_cycles * 1000000ULL) / SystemCoreClock);
        benchmark->metric_x1000 =
            (uint32_t)((total_cycles * 1000000000ULL +
                        (denominator / 2ULL)) / denominator);
        benchmark->minimum_x1000 =
            (uint32_t)(((uint64_t)minimum_cycles * 1000000000ULL +
                        (SystemCoreClock / 2U)) / SystemCoreClock);
        benchmark->maximum_x1000 =
            (uint32_t)(((uint64_t)maximum_cycles * 1000000000ULL +
                        (SystemCoreClock / 2U)) / SystemCoreClock);
        benchmark->state = AI_KIT_BENCHMARK_STATE_COMPLETE;
        benchmark->error = AI_KIT_BENCHMARK_ERROR_NONE;
    }
    else
    {
        benchmark->state = AI_KIT_BENCHMARK_STATE_ERROR;
        benchmark->error = (result == -RT_ETIMEOUT) ?
                           AI_KIT_BENCHMARK_ERROR_TIMEOUT :
                           AI_KIT_BENCHMARK_ERROR_PUBLISH;
    }

    benchmark->checksum = ai_kit_benchmark_result_checksum(benchmark);
    result = ai_kit_ipc_publish_benchmark(benchmark);
    if (result != RT_EOK)
    {
        benchmark->state = AI_KIT_BENCHMARK_STATE_ERROR;
        benchmark->error = AI_KIT_BENCHMARK_ERROR_PUBLISH;
        benchmark->checksum = ai_kit_benchmark_result_checksum(benchmark);
        rt_kprintf("IPC benchmark result publish failed: %d\n", result);
    }

    if (benchmark->state == AI_KIT_BENCHMARK_STATE_COMPLETE)
    {
        rt_kprintf("IPC latency (system-active, M33 cycle counter %lu Hz):\n",
                   (unsigned long)SystemCoreClock);
        rt_kprintf("  iterations=%lu warmup=%u timer_overhead=%lu cycles\n",
                   (unsigned long)benchmark->iterations,
                   AI_KIT_IPC_BENCH_WARMUP_COUNT,
                   (unsigned long)timer_overhead);
        rt_kprintf("  average=%lu.%03lu us min=%lu.%03lu us "
                   "max=%lu.%03lu us measured_total=%lu us\n",
                   (unsigned long)(benchmark->metric_x1000 / 1000U),
                   (unsigned long)(benchmark->metric_x1000 % 1000U),
                   (unsigned long)(benchmark->minimum_x1000 / 1000U),
                   (unsigned long)(benchmark->minimum_x1000 % 1000U),
                   (unsigned long)(benchmark->maximum_x1000 / 1000U),
                   (unsigned long)(benchmark->maximum_x1000 % 1000U),
                   (unsigned long)benchmark->elapsed_us);
    }
    else
    {
        rt_kprintf("IPC benchmark failed: completed=%lu/%lu error=%lu\n",
                   (unsigned long)completed,
                   (unsigned long)iterations,
                   (unsigned long)benchmark->error);
    }

    ai_kit_ipc_print_stats();
}
MSH_CMD_EXPORT_ALIAS(ai_kit_ipc_benchmark_command, ipc_bench,
                     benchmark M33 to M55 application round-trip latency);
