/**
 * @file    ai_kit_ipc_service.h
 * @brief   AI Kit CM55 侧 IPC 服务对外接口
 *
 * @details 该头文件声明 CM55 接收并处理 CM33 IPC 请求的初始化入口。
 *          实现细节参见 ai_kit_ipc_service.c。协议帧与共享内存布局
 *          参见 ipc_common.h。
 *
 * @note    本文件仅添加注释，未改动任何可执行代码。
 */

#ifndef AI_KIT_IPC_SERVICE_H
#define AI_KIT_IPC_SERVICE_H

/**
 * @brief   初始化 CM55 IPC 服务（单次幂等）
 *
 * @details 完成：
 *          - 查找并打开 ipc0 设备（RDWR + INT_RX）
 *          - 初始化接收信号量 ipc_rx
 *          - 注册接收回调
 *          - 创建并启动 "ipc_svc" 服务线程，循环阻塞在信号量上
 *
 *          收到请求帧后服务线程根据 command 分发：
 *          - PING               ：回声载荷 + 当前 tick
 *          - BENCHMARK_PUBLISH  ：从 CM33 共享内存导入基准结果并落库
 *          - BENCHMARK_START    ：触发本地基准线程（CoreMark / 内存 / NPU）
 *
 * @retval  RT_EOK       初始化成功或已初始化
 * @retval  -RT_ENOSYS   ipc0 设备未注册
 * @retval  其他         rt_sem_init / rt_device_open / rt_thread_init
 *                       / rt_thread_startup 失败码
 */
int ai_kit_ipc_service_init(void);

#endif
