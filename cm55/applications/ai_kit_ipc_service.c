/**
 * @file    ai_kit_ipc_service.c
 * @brief   AI Kit CM55 侧 IPC 服务实现
 *
 * @details 该文件在 CM55 上启动一个独立线程 "ipc_svc"，处理来自
 *          CM33 的 IPC 请求帧。三种命令的处理方式：
 *
 *          - PING              ：回声 argument0 + 把当前 rt_tick_get 写入
 *                               argument1，CM33 据此测量往返时延
 *          - BENCHMARK_PUBLISH  ：把 CM33 共享内存槽中的结果导入本地
 *                                  基准目录（ai_kit_benchmark_publish_external），
 *                                  并把导入返回码与回送的 checksum 作为
 *                                  响应的 argument0/argument1
 *          - BENCHMARK_START    ：触发 ai_kit_benchmark_start，启动
 *                                  CoreMark / 内存 / NPU 等基准线程，
 *                                  返回值作为 argument0，test_id 作为
 *                                  argument1 供 CM33 校验
 *
 *          共享内存访问使用 GET_ALIAS_ADDRESS 解决非直写窗口的地址
 *          别名问题；接收侧先 InvalidateDCache 再读取，确保看到
 *          CM33 写入的最新内容。
 *
 * @note    本文件仅添加注释，未改动任何可执行代码。
 */

#include <rtdevice.h>
#include <rtthread.h>

#include "drv_ipc.h"
#include "ai_kit_benchmark.h"
#include "ai_kit_ipc_service.h"

#define AI_KIT_IPC_SERVICE_STACK_SIZE  (2048U)                          /**< IPC 服务线程栈大小（字节） */
#define AI_KIT_IPC_SERVICE_PRIORITY    (RT_THREAD_PRIORITY_MAX / 2U)   /**< IPC 服务线程优先级（中等） */
#define AI_KIT_IPC_SERVICE_TIMESLICE   (10U)                            /**< IPC 服务线程时间片 */

static rt_device_t g_ipc_device;                /**< IPC 设备句柄 */
static struct rt_semaphore g_rx_event;           /**< 接收中断到服务线程的同步信号量 */
static struct rt_thread g_service_thread;        /**< IPC 服务线程对象 */
static rt_uint8_t g_service_stack[AI_KIT_IPC_SERVICE_STACK_SIZE];  /**< 服务线程栈 */
static rt_bool_t g_service_started;              /**< 服务是否已启动 */

/**
 * @brief   IPC 设备接收回调
 *
 * @details 中断上下文中被调用，仅释放一次信号量，通知服务线程
 *          有可读的 request 帧。
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
 * @brief   从 CM33 共享内存槽导入基准结果到本地目录
 *
 * @details 安全地访问 CM33 共享内存：
 *          1. 校验 argument1 与 ai_kit_benchmark_result_t 大小一致
 *          2. 校验 address 32 字节对齐
 *          3. 校验 address 落在 [AI_KIT_IPC_M33_SHARED_START, start+size)
 *             的合法共享内存窗口内
 *          4. 通过 GET_ALIAS_ADDRESS 解决非直写窗口地址别名
 *          5. 若启用 DCache，先 InvalidateDCache_by_Addr 再读取，
 *             保证看到 CM33 写入的最新内容
 *          6. 调用 ai_kit_benchmark_publish_external 落库
 *
 * @param   request          收到的请求帧，argument0/1 携带指针与长度
 * @param   result_checksum  [out] 用于把回送的 checksum 写回响应帧
 *
 * @retval  RT_EOK       导入并落库成功
 * @retval  -RT_EINVAL   地址 / 长度 / 对齐校验失败
 * @retval  其他          ai_kit_benchmark_publish_external 的返回码
 */
static int ai_kit_ipc_import_benchmark(
    const ai_kit_ipc_frame_t *request,
    uint32_t *result_checksum)
{
    const ai_kit_benchmark_result_t *shared_result;
    ai_kit_benchmark_result_t result;
    uintptr_t address = (uintptr_t)request->argument0;
    uintptr_t limit =
        AI_KIT_IPC_M33_SHARED_START + AI_KIT_IPC_M33_SHARED_SIZE;

    if ((request->argument1 != sizeof(ai_kit_benchmark_result_t)) ||
        ((address & 0x1FU) != 0U) ||
        (address < AI_KIT_IPC_M33_SHARED_START) ||
        (address > (limit - sizeof(ai_kit_benchmark_result_t))))
    {
        return -RT_EINVAL;
    }

    shared_result = (const ai_kit_benchmark_result_t *)(uintptr_t)
                    GET_ALIAS_ADDRESS(address);
#if defined(__DCACHE_PRESENT) && (__DCACHE_PRESENT == 1U)
    SCB_InvalidateDCache_by_Addr(
        (uint32_t *)(uintptr_t)shared_result,
        (int32_t)sizeof(ai_kit_benchmark_result_t));
    __DSB();
#endif
    result = *shared_result;
    *result_checksum = result.checksum;
    return ai_kit_benchmark_publish_external(&result);
}

/**
 * @brief   处理单个 IPC 请求帧并回写响应帧
 *
 * @details 三种命令的响应字段：
 *          - PING               ：arg0 = 回声载荷，arg1 = 远端 tick
 *          - BENCHMARK_PUBLISH  ：arg0 = 导入返回码，arg1 = 回送的 checksum
 *          - BENCHMARK_START    ：arg0 = 启动返回码，arg1 = test_id 回送
 *
 *          非请求帧或未知命令直接丢弃，不响应。
 *
 * @param   request  收到的请求帧
 */
static void ai_kit_ipc_process_request(const ai_kit_ipc_frame_t *request)
{
    ai_kit_ipc_frame_t response;
    uint32_t response_argument0;
    uint32_t response_argument1;

    if (request->kind != AI_KIT_IPC_KIND_REQUEST)
    {
        return;
    }

    if (request->command == AI_KIT_IPC_COMMAND_PING)
    {
        response_argument0 = request->argument0;
        response_argument1 = (uint32_t)rt_tick_get();
    }
    else if (request->command == AI_KIT_IPC_COMMAND_BENCHMARK_PUBLISH)
    {
        response_argument1 = 0U;
        response_argument0 = (uint32_t)ai_kit_ipc_import_benchmark(
            request, &response_argument1);
    }
    else if (request->command == AI_KIT_IPC_COMMAND_BENCHMARK_START)
    {
        response_argument0 = (uint32_t)ai_kit_benchmark_start(
            (ai_kit_benchmark_test_t)request->argument0);
        response_argument1 = request->argument0;
    }
    else
    {
        return;
    }

    ai_kit_ipc_frame_prepare(&response,
                             AI_KIT_IPC_KIND_RESPONSE,
                             AI_KIT_IPC_CORE_M55,
                             AI_KIT_IPC_CORE_M33,
                             request->sequence,
                             (ai_kit_ipc_command_t)request->command,
                             response_argument0,
                             response_argument1);
    (void)rt_device_write(g_ipc_device, 0, &response, 1U);
}

/**
 * @brief   IPC 服务线程入口
 *
 * @details 阻塞在 g_rx_event 信号量上；每当有接收中断触发，
 *          循环读取所有 request 帧并调用 ai_kit_ipc_process_request。
 *          永不返回。
 *
 * @param   parameter  RT-Thread 传入参数（未使用）
 */
static void ai_kit_ipc_service_entry(void *parameter)
{
    ai_kit_ipc_frame_t frame;

    (void)parameter;

    while (1)
    {
        rt_sem_take(&g_rx_event, RT_WAITING_FOREVER);
        while (rt_device_read(g_ipc_device, 0, &frame, 1U) == 1)
        {
            ai_kit_ipc_process_request(&frame);
        }
    }
}

/**
 * @brief   初始化 CM55 IPC 服务（单次幂等）
 *
 * @details 流程：
 *          1. 查找并打开 ipc0 设备（RDWR + INT_RX）
 *          2. 初始化接收信号量 ipc_rx
 *          3. 注册接收回调 ai_kit_ipc_rx_indicate
 *          4. 静态初始化 g_service_thread（栈 / 优先级 / 时间片）
 *          5. 启动线程
 *
 *          任一步失败均回滚已完成的资源，保证幂等。
 *
 * @retval  RT_EOK       初始化成功或已初始化
 * @retval  -RT_ENOSYS   ipc0 设备未注册
 * @retval  其他         rt_sem_init / rt_device_open / rt_thread_init
 *                       / rt_thread_startup 失败码
 */
int ai_kit_ipc_service_init(void)
{
    rt_err_t result;

    if (g_service_started)
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

    result = rt_thread_init(&g_service_thread,
                            "ipc_svc",
                            ai_kit_ipc_service_entry,
                            RT_NULL,
                            g_service_stack,
                            sizeof(g_service_stack),
                            AI_KIT_IPC_SERVICE_PRIORITY,
                            AI_KIT_IPC_SERVICE_TIMESLICE);
    if (result != RT_EOK)
    {
        rt_device_close(g_ipc_device);
        rt_sem_detach(&g_rx_event);
        return result;
    }

    result = rt_thread_startup(&g_service_thread);
    if (result != RT_EOK)
    {
        rt_thread_detach(&g_service_thread);
        rt_device_close(g_ipc_device);
        rt_sem_detach(&g_rx_event);
        return result;
    }

    g_service_started = RT_TRUE;
    return RT_EOK;
}
