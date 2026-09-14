/******************************************************************************
 * Copyright 2020-2026 The RT-Thread Development Team. All Rights Reserved.
 *
 * SPDX-License-Identifier: Apache-2.0
 *****************************************************************************/

/**
 * @file    drv_ipc.c
 * @brief   AI Kit 双核 IPC RT-Thread 设备驱动实现
 *
 * @details 该文件把 Infineon Cy_IPC_Pipe（PSoC Edge 84 的硬件 IPC 通道）
 *          包装为 RT-Thread 的 rt_device，统一服务于 CM33 与 CM55：
 *          - 通过 COMPONENT_CM33 / COMPONENT_CM55 在编译期选择本核与
 *            对端端点、客户端 ID、IPC 通道号、中断号
 *          - 收方向：Cy_IPC_Pipe 在 ISR 中调用 ai_kit_ipc_rx_callback，
 *            校验帧头后入队 rx_ringbuffer，并触发 rx_indicate 通知上层
 *          - 发方向：write() 在 tx_lock 保护下取 tx_available 信号量
 *            （初值 1，每次发送后由 tx_release_callback 释放），
 *            拷贝帧到 g_tx_frame、写 checksum、CleanDCache 后调用
 *            Cy_IPC_Pipe_SendMessage；SEND_BUSY 时按固定间隔重试
 *          - 共享内存 g_tx_frame 跨核可见，必须 32 字节对齐 + cache 维护
 *
 *          对外通过 ai_kit_ipc_device_find 暴露 "ipc0" 设备，供
 *          ai_kit_ipc_service（CM55）与 ai_kit_ipc_client（CM33）使用。
 */
#include <rtdevice.h>
#include <rtthread.h>

#include "board.h"
#include "cy_ipc_pipe.h"
#include "drv_ipc.h"

/* CM33 / CM55 编译期端点配置：本核与对端的端点/客户端/通道/中断/核 ID。 */
#if defined(COMPONENT_CM33) || ((__CORTEX_M) == 33U)
    #define AI_KIT_IPC_LOCAL_ENDPOINT   AI_KIT_IPC_CM33_ENDPOINT
    #define AI_KIT_IPC_PEER_ENDPOINT    AI_KIT_IPC_CM55_ENDPOINT
    #define AI_KIT_IPC_LOCAL_CLIENT     AI_KIT_IPC_CM33_CLIENT_ID
    #define AI_KIT_IPC_PEER_CLIENT      AI_KIT_IPC_CM55_CLIENT_ID
    #define AI_KIT_IPC_LOCAL_CHANNEL    AI_KIT_RTTHREAD_IPC_CM33_CHANNEL
    #define AI_KIT_IPC_PEER_CHANNEL     AI_KIT_RTTHREAD_IPC_CM55_CHANNEL
    #define AI_KIT_IPC_LOCAL_INTERRUPT  AI_KIT_RTTHREAD_IPC_CM33_INTERRUPT
    #define AI_KIT_IPC_PEER_INTERRUPT   AI_KIT_RTTHREAD_IPC_CM55_INTERRUPT
    #define AI_KIT_IPC_LOCAL_CORE       AI_KIT_IPC_CORE_M33
    #define AI_KIT_IPC_PEER_CORE        AI_KIT_IPC_CORE_M55
#elif defined(COMPONENT_CM55) || ((__CORTEX_M) == 55U)
    #define AI_KIT_IPC_LOCAL_ENDPOINT   AI_KIT_IPC_CM55_ENDPOINT
    #define AI_KIT_IPC_PEER_ENDPOINT    AI_KIT_IPC_CM33_ENDPOINT
    #define AI_KIT_IPC_LOCAL_CLIENT     AI_KIT_IPC_CM55_CLIENT_ID
    #define AI_KIT_IPC_PEER_CLIENT      AI_KIT_IPC_CM33_CLIENT_ID
    #define AI_KIT_IPC_LOCAL_CHANNEL    AI_KIT_RTTHREAD_IPC_CM55_CHANNEL
    #define AI_KIT_IPC_PEER_CHANNEL     AI_KIT_RTTHREAD_IPC_CM33_CHANNEL
    #define AI_KIT_IPC_LOCAL_INTERRUPT  AI_KIT_RTTHREAD_IPC_CM55_INTERRUPT
    #define AI_KIT_IPC_PEER_INTERRUPT   AI_KIT_RTTHREAD_IPC_CM33_INTERRUPT
    #define AI_KIT_IPC_LOCAL_CORE       AI_KIT_IPC_CORE_M55
    #define AI_KIT_IPC_PEER_CORE        AI_KIT_IPC_CORE_M33
#else
    #error "AI Kit IPC supports only CM33 and CM55"
#endif

/**
 * @brief   IPC 设备私有结构
 *
 * @details 把 rt_device 与 Cy_IPC_Pipe 所需的全部状态绑定在一起：
 *          - callbacks        ：注册到 Cy_IPC_Pipe 的客户端回调表
 *          - rx_ringbuffer    ：接收帧环形缓冲（容量 RX_QUEUE_DEPTH 帧）
 *          - tx_lock          ：保护 g_tx_frame 与 tx_available 的串行化
 *          - tx_available     ：初值 1，每发送一帧后由对端 release 回调释放
 *          - stats            ：rx/tx 各种错误统计（volatile，可被 control 读出）
 *          - initialized      ：幂等 init 标志
 */
struct ai_kit_ipc_device
{
    struct rt_device parent;
    cy_ipc_pipe_callback_ptr_t callbacks[AI_KIT_IPC_CLIENT_COUNT];
    struct rt_ringbuffer rx_ringbuffer;
    rt_uint8_t rx_buffer[
        AI_KIT_IPC_RX_QUEUE_DEPTH * sizeof(ai_kit_ipc_frame_t)];
    struct rt_mutex tx_lock;
    struct rt_semaphore tx_available;
    volatile ai_kit_ipc_device_stats_t stats;
    rt_bool_t initialized;
};

/** @brief 全局 IPC 设备实例（单例） */
static struct ai_kit_ipc_device g_ipc;
/** @brief Cy_IPC_Pipe 端点表（每个端点一份） */
static cy_stc_ipc_pipe_ep_t g_pipe_endpoints[AI_KIT_IPC_MAX_ENDPOINTS];

/**
 * @brief   发送帧共享缓冲（跨核可见）
 *
 * @details 放在共享内存段（CY_SECTION_SHAREDMEM）并 32 字节对齐。
 *          每次发送前拷贝帧到这里、写 checksum、CleanDCache 后再交给
 *          Cy_IPC_Pipe_SendMessage；对端读完后由 release 回调释放
 *          tx_available 信号量，本核才能复用此缓冲。
 */
CY_SECTION_SHAREDMEM
__attribute__((aligned(32)))
static ai_kit_ipc_frame_t g_tx_frame;

static void ai_kit_ipc_pipe_isr(void);

/**
 * @brief   Cy_IPC_Pipe 配置（本核 + 对端）
 *
 * @details 配置本核与对端的 IPC 中断号/优先级/MUX、端点地址、通道与
 *          中断屏蔽，并把回调表与自定义 ISR 注册给 Cy_IPC_Pipe。
 */
static cy_stc_ipc_pipe_config_t g_pipe_config =
{
    {
        .ipcNotifierNumber = AI_KIT_IPC_LOCAL_INTERRUPT,
        .ipcNotifierPriority = AI_KIT_IPC_INTERRUPT_PRIORITY,
        .ipcNotifierMuxNumber =
            CY_IPC1_INTR_MUX(AI_KIT_IPC_LOCAL_INTERRUPT),
        .epAddress = AI_KIT_IPC_LOCAL_ENDPOINT,
        {
            .epChannel = AI_KIT_IPC_LOCAL_CHANNEL,
            .epIntr = AI_KIT_IPC_LOCAL_INTERRUPT,
            .epIntrmask = AI_KIT_IPC_PIPE_CHANNEL_MASK
        }
    },
    {
        .ipcNotifierNumber = AI_KIT_IPC_PEER_INTERRUPT,
        .ipcNotifierPriority = AI_KIT_IPC_INTERRUPT_PRIORITY,
        .ipcNotifierMuxNumber =
            CY_IPC1_INTR_MUX(AI_KIT_IPC_PEER_INTERRUPT),
        .epAddress = AI_KIT_IPC_PEER_ENDPOINT,
        {
            .epChannel = AI_KIT_IPC_PEER_CHANNEL,
            .epIntr = AI_KIT_IPC_PEER_INTERRUPT,
            .epIntrmask = AI_KIT_IPC_PIPE_CHANNEL_MASK
        }
    },
    .endpointClientsCount = AI_KIT_IPC_CLIENT_COUNT,
    .endpointsCallbacksArray = g_ipc.callbacks,
    .userPipeIsrHandler = ai_kit_ipc_pipe_isr
};

/**
 * @brief   清 DCache（把本地写入回写共享内存，供对端可见）
 *
 * @details 仅在 __DCACHE_PRESENT 时调用 SCB_CleanDCache_by_Addr + DSB；
 *          否则 no-op。所有跨核写入前必须调用。
 *
 * @param   address  共享缓冲起始地址
 * @param   size     字节数
 */
static void ai_kit_ipc_cache_clean(void *address, rt_size_t size)
{
#if defined(__DCACHE_PRESENT) && (__DCACHE_PRESENT == 1U)
    SCB_CleanDCache_by_Addr((uint32_t *)address, (int32_t)size);
    __DSB();
#else
    (void)address;
    (void)size;
#endif
}

/**
 * @brief   失效 DCache（强制从共享内存重新加载，读到对端最新写入）
 *
 * @details 仅在 __DCACHE_PRESENT 时调用 SCB_InvalidateDCache_by_Addr + DSB；
 *          否则 no-op。所有跨核读取前必须调用。
 *
 * @param   address  共享缓冲起始地址
 * @param   size     字节数
 */
static void ai_kit_ipc_cache_invalidate(void *address, rt_size_t size)
{
#if defined(__DCACHE_PRESENT) && (__DCACHE_PRESENT == 1U)
    SCB_InvalidateDCache_by_Addr((uint32_t *)address, (int32_t)size);
    __DSB();
#else
    (void)address;
    (void)size;
#endif
}

/**
 * @brief   IPC 接收回调（在 ISR 上下文中由 Cy_IPC_Pipe 调用）
 *
 * @details 流程：
 *          - 失效 frame 缓存的 DCache 以读到对端最新内容
 *          - 校验 client_id / magic / protocol_version / source /
 *            destination / checksum 全部一致，否则累加 rx_error 丢弃
 *          - 把整帧压入 rx_ringbuffer；空间不足则累加 rx_drop 丢弃
 *          - 成功则累加 rx_ok，并调用上层 rx_indicate 通知数据到达
 *
 * @param   message  指向 32 位对齐的 IPC 消息缓冲（实际是 ai_kit_ipc_frame_t）
 */
static void ai_kit_ipc_rx_callback(uint32_t *message)
{
    ai_kit_ipc_frame_t *frame = (ai_kit_ipc_frame_t *)message;

    if (frame == RT_NULL)
    {
        g_ipc.stats.rx_error++;
        return;
    }

    ai_kit_ipc_cache_invalidate(frame, sizeof(*frame));

    if ((frame->client_id != AI_KIT_IPC_LOCAL_CLIENT) ||
        (frame->magic != AI_KIT_IPC_FRAME_MAGIC) ||
        (frame->protocol_version != AI_KIT_IPC_PROTOCOL_VERSION) ||
        (frame->source != AI_KIT_IPC_PEER_CORE) ||
        (frame->destination != AI_KIT_IPC_LOCAL_CORE) ||
        (frame->checksum != ai_kit_ipc_frame_checksum(frame)))
    {
        g_ipc.stats.rx_error++;
        return;
    }

    if ((rt_ringbuffer_space_len(&g_ipc.rx_ringbuffer) <
         sizeof(ai_kit_ipc_frame_t)) ||
        (rt_ringbuffer_put(&g_ipc.rx_ringbuffer,
                           (const rt_uint8_t *)frame,
                           sizeof(ai_kit_ipc_frame_t)) !=
         sizeof(ai_kit_ipc_frame_t)))
    {
        g_ipc.stats.rx_drop++;
        return;
    }

    g_ipc.stats.rx_ok++;
    if (g_ipc.parent.rx_indicate != RT_NULL)
    {
        g_ipc.parent.rx_indicate(&g_ipc.parent, 1U);
    }
}

/**
 * @brief   发送完成释放回调（由对端在读完帧后触发）
 *
 * @details Cy_IPC_Pipe 在对端读取帧并回执 release 时调用本函数：
 *          累加 tx_release 统计，释放 tx_available 信号量以允许下一次
 *          发送复用 g_tx_frame，并通知上层 tx_complete。
 */
static void ai_kit_ipc_tx_release_callback(void)
{
    g_ipc.stats.tx_release++;
    rt_sem_release(&g_ipc.tx_available);

    if (g_ipc.parent.tx_complete != RT_NULL)
    {
        g_ipc.parent.tx_complete(&g_ipc.parent, RT_NULL);
    }
}

/**
 * @brief   Cy_IPC_Pipe 自定义 ISR（覆盖默认 ISR）
 *
 * @details 进入 RT-Thread 中断上下文，调用 Cy_IPC_Pipe_ExecuteCallback
 *          派发到注册的 rx / release 回调，再退出中断上下文。
 */
static void ai_kit_ipc_pipe_isr(void)
{
    rt_interrupt_enter();
    Cy_IPC_Pipe_ExecuteCallback(AI_KIT_IPC_LOCAL_ENDPOINT);
    rt_interrupt_leave();
}

/**
 * @brief   带重试的 IPC 发送
 *
 * @details Cy_IPC_Pipe_SendMessage 在对端未及时 release 时返回
 *          CY_IPC_PIPE_ERROR_SEND_BUSY。本函数按固定间隔重试，最多
 *          AI_KIT_IPC_SEND_RETRY_COUNT 次，期间累加 tx_busy / tx_retry。
 *
 * @param   frame  指向 g_tx_frame 的待发送帧
 *
 * @return  Cy_IPC_PIPE_SUCCESS 或最终失败状态
 */
static cy_en_ipc_pipe_status_t ai_kit_ipc_send_with_retry(
    ai_kit_ipc_frame_t *frame)
{
    cy_en_ipc_pipe_status_t status;
    uint32_t retry = 0U;

    status = Cy_IPC_Pipe_SendMessage(AI_KIT_IPC_PEER_ENDPOINT,
                                     AI_KIT_IPC_LOCAL_ENDPOINT,
                                     frame,
                                     ai_kit_ipc_tx_release_callback);
    if (status == CY_IPC_PIPE_ERROR_SEND_BUSY)
    {
        g_ipc.stats.tx_busy++;
    }

    while ((status == CY_IPC_PIPE_ERROR_SEND_BUSY) &&
           (retry < AI_KIT_IPC_SEND_RETRY_COUNT))
    {
        retry++;
        g_ipc.stats.tx_retry++;
        rt_thread_mdelay(AI_KIT_IPC_SEND_RETRY_DELAY_MS);
        status = Cy_IPC_Pipe_SendMessage(AI_KIT_IPC_PEER_ENDPOINT,
                                         AI_KIT_IPC_LOCAL_ENDPOINT,
                                         frame,
                                         ai_kit_ipc_tx_release_callback);
    }

    return status;
}

/**
 * @brief   rt_device init 回调：初始化 Cy_IPC_Pipe 并注册本核接收回调
 *
 * @details 幂等：g_ipc.initialized 为真则直接返回 RT_EOK。
 *          否则 Cy_IPC_Pipe_Config + Cy_IPC_Pipe_Init + 注册
 *          ai_kit_ipc_rx_callback 给本核 client。
 *
 * @retval  RT_EOK     成功或已初始化
 * @retval  -RT_ERROR  RegisterCallback 失败
 */
static rt_err_t ai_kit_ipc_init(rt_device_t device)
{
    cy_en_ipc_pipe_status_t status;

    (void)device;

    if (g_ipc.initialized)
    {
        return RT_EOK;
    }

    Cy_IPC_Pipe_Config(g_pipe_endpoints);
    Cy_IPC_Pipe_Init(&g_pipe_config);

    status = Cy_IPC_Pipe_RegisterCallback(AI_KIT_IPC_LOCAL_ENDPOINT,
                                          ai_kit_ipc_rx_callback,
                                          AI_KIT_IPC_LOCAL_CLIENT);
    if (status != CY_IPC_PIPE_SUCCESS)
    {
        return -RT_ERROR;
    }

    g_ipc.initialized = RT_TRUE;
    return RT_EOK;
}

/** @brief rt_device open 回调（no-op） */
static rt_err_t ai_kit_ipc_open(rt_device_t device, rt_uint16_t oflag)
{
    (void)device;
    (void)oflag;
    return RT_EOK;
}

/** @brief rt_device close 回调（no-op） */
static rt_err_t ai_kit_ipc_close(rt_device_t device)
{
    (void)device;
    return RT_EOK;
}

/**
 * @brief   rt_device read 回调：从 rx_ringbuffer 取整帧
 *
 * @param   frame_count  期望读取的帧数
 * @param   buffer       输出缓冲（每帧 sizeof(ai_kit_ipc_frame_t) 字节）
 *
 * @return  实际读出的帧数；buffer 为空或 frame_count==0 返回 0
 */
static rt_ssize_t ai_kit_ipc_read(rt_device_t device,
                                  rt_off_t position,
                                  void *buffer,
                                  rt_size_t frame_count)
{
    ai_kit_ipc_frame_t *frames = (ai_kit_ipc_frame_t *)buffer;
    rt_size_t read_count = 0U;

    (void)device;
    (void)position;

    if ((buffer == RT_NULL) || (frame_count == 0U))
    {
        return 0;
    }

    while (read_count < frame_count)
    {
        if (rt_ringbuffer_get(&g_ipc.rx_ringbuffer,
                              (rt_uint8_t *)&frames[read_count],
                              sizeof(ai_kit_ipc_frame_t)) !=
            sizeof(ai_kit_ipc_frame_t))
        {
            break;
        }
        read_count++;
    }

    return (rt_ssize_t)read_count;
}

/**
 * @brief   rt_device write 回调：发送一帧或多帧
 *
 * @details 持锁串行化发送：
 *          - rt_mutex_take tx_lock 永久等待
 *          - 每帧前 rt_sem_take tx_available（带 TX_TIMEOUT_MS 超时），
 *            超时则累加 tx_timeout 并退出
 *          - 拷贝帧到 g_tx_frame、改写 client_id 为对端、清 release_mask、
 *            重算 checksum、CleanDCache
 *          - ai_kit_ipc_send_with_retry 发送；失败则累加 tx_error 并释放
 *            tx_available（避免下次永远拿不到信号量）
 *          - 成功则累加 tx_ok
 *          - 注意：信号量由对端 release 回调释放，故成功路径不在
 *            本地释放 tx_available
 *
 * @param   frame_count  要发送的帧数
 * @param   buffer       帧数组
 *
 * @return  实际成功发送的帧数
 */
static rt_ssize_t ai_kit_ipc_write(rt_device_t device,
                                   rt_off_t position,
                                   const void *buffer,
                                   rt_size_t frame_count)
{
    const ai_kit_ipc_frame_t *frames =
        (const ai_kit_ipc_frame_t *)buffer;
    rt_size_t written = 0U;
    cy_en_ipc_pipe_status_t status;

    (void)device;
    (void)position;

    if ((buffer == RT_NULL) || (frame_count == 0U))
    {
        return 0;
    }

    if (rt_mutex_take(&g_ipc.tx_lock, RT_WAITING_FOREVER) != RT_EOK)
    {
        return 0;
    }

    while (written < frame_count)
    {
        if (rt_sem_take(&g_ipc.tx_available,
                        rt_tick_from_millisecond(
                            AI_KIT_IPC_TX_TIMEOUT_MS)) != RT_EOK)
        {
            g_ipc.stats.tx_timeout++;
            break;
        }

        g_tx_frame = frames[written];
        g_tx_frame.client_id = AI_KIT_IPC_PEER_CLIENT;
        g_tx_frame.release_mask = 0U;
        g_tx_frame.checksum = ai_kit_ipc_frame_checksum(&g_tx_frame);
        ai_kit_ipc_cache_clean(&g_tx_frame, sizeof(g_tx_frame));

        status = ai_kit_ipc_send_with_retry(&g_tx_frame);
        if (status != CY_IPC_PIPE_SUCCESS)
        {
            g_ipc.stats.tx_error++;
            rt_sem_release(&g_ipc.tx_available);
            break;
        }

        g_ipc.stats.tx_ok++;
        written++;
    }

    rt_mutex_release(&g_ipc.tx_lock);
    return (rt_ssize_t)written;
}

/**
 * @brief   rt_device control 回调：仅支持 AI_KIT_IPC_CTRL_GET_STATS
 *
 * @param   command  命令字
 * @param   args     输出参数（stats 结构指针）
 *
 * @retval  RT_EOK      成功，stats 已拷贝到 args
 * @retval  -RT_ENOSYS  不支持的命令
 * @retval  -RT_EINVAL  args 为空
 */
static rt_err_t ai_kit_ipc_control(rt_device_t device, int command, void *args)
{
    ai_kit_ipc_device_stats_t *stats;

    (void)device;

    if (command != AI_KIT_IPC_CTRL_GET_STATS)
    {
        return -RT_ENOSYS;
    }

    if (args == RT_NULL)
    {
        return -RT_EINVAL;
    }

    stats = (ai_kit_ipc_device_stats_t *)args;
    *stats = g_ipc.stats;
    return RT_EOK;
}

#ifdef RT_USING_DEVICE_OPS
/** @brief rt_device 操作表（RT_USING_DEVICE_OPS 模式） */
static const struct rt_device_ops g_ai_kit_ipc_ops =
{
    ai_kit_ipc_init,
    ai_kit_ipc_open,
    ai_kit_ipc_close,
    ai_kit_ipc_read,
    ai_kit_ipc_write,
    ai_kit_ipc_control
};
#endif

/**
 * @brief   查找已注册的 IPC 设备（按 AI_KIT_IPC_DEVICE_NAME）
 *
 * @return  rt_device 句柄；未注册返回 RT_NULL
 */
rt_device_t ai_kit_ipc_device_find(void)
{
    return rt_device_find(AI_KIT_IPC_DEVICE_NAME);
}

/**
 * @brief   注册 IPC 设备（INIT_PREV_EXPORT 自动调用）
 *
 * @details 初始化 rx_ringbuffer、tx_lock、tx_available，挂载 rt_device
 *          操作表，最后 rt_device_register 注册为 "ipc0"，
 *          flags = RDWR | INT_RX。失败时回滚信号量与互斥量。
 *
 * @retval  RT_EOK  注册成功
 * @retval  其他    rt_mutex_init / rt_sem_init / rt_device_register 失败码
 */
int ai_kit_ipc_device_register(void)
{
    rt_err_t result;

    rt_memset(&g_ipc, 0, sizeof(g_ipc));
    rt_ringbuffer_init(&g_ipc.rx_ringbuffer,
                       g_ipc.rx_buffer,
                       sizeof(g_ipc.rx_buffer));

    result = rt_mutex_init(&g_ipc.tx_lock, "ipc_tx", RT_IPC_FLAG_PRIO);
    if (result != RT_EOK)
    {
        return result;
    }

    result = rt_sem_init(&g_ipc.tx_available, "ipc_dn", 1U,
                         RT_IPC_FLAG_PRIO);
    if (result != RT_EOK)
    {
        rt_mutex_detach(&g_ipc.tx_lock);
        return result;
    }

#ifdef RT_USING_DEVICE_OPS
    g_ipc.parent.ops = &g_ai_kit_ipc_ops;
#else
    g_ipc.parent.init = ai_kit_ipc_init;
    g_ipc.parent.open = ai_kit_ipc_open;
    g_ipc.parent.close = ai_kit_ipc_close;
    g_ipc.parent.read = ai_kit_ipc_read;
    g_ipc.parent.write = ai_kit_ipc_write;
    g_ipc.parent.control = ai_kit_ipc_control;
#endif

    result = rt_device_register(&g_ipc.parent,
                                AI_KIT_IPC_DEVICE_NAME,
                                RT_DEVICE_FLAG_RDWR |
                                RT_DEVICE_FLAG_INT_RX);
    if (result != RT_EOK)
    {
        rt_sem_detach(&g_ipc.tx_available);
        rt_mutex_detach(&g_ipc.tx_lock);
    }

    return result;
}
INIT_PREV_EXPORT(ai_kit_ipc_device_register);
