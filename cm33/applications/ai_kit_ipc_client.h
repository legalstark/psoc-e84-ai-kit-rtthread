/**
 * @file    ai_kit_ipc_client.h
 * @brief   AI Kit CM33 侧 IPC 客户端对外接口
 *
 * @details 该头文件声明 M33 通过 IPC 管道与 CM55 通信的两个公开 API：
 *          - ai_kit_ipc_publish_benchmark：发布本核基准结果到 CM55 状态区
 *          - ai_kit_ipc_start_m55_benchmark：触发 CM55 / U55 执行指定基准
 *
 *          实现、协议帧格式及共享内存布局参见 ai_kit_ipc_client.c 与
 *          ipc_common.h。
 *
 * @note    本文件仅添加注释，未改动任何可执行代码。
 */

#ifndef AI_KIT_IPC_CLIENT_H
#define AI_KIT_IPC_CLIENT_H

#include <rtthread.h>

#include "ai_kit_multicore.h"

/**
 * @brief   通过 IPC 将 M33 侧基准结果发布到 CM55 状态区
 *
 * @details 调用前，调用方需先填充 result 的 protocol_version / sequence /
 *          test_id / executor / state / error / unit / metric_x1000 /
 *          minimum_x1000 / maximum_x1000 等字段，并按
 *          ai_kit_benchmark_result_checksum 计算 checksum。
 *
 *          客户端会把 result 复制到 64 字节的共享内存槽，执行 DCache 清
 *          洁后，通过 BENCHMARK_PUBLISH 命令通知 CM55 读取。CM55 返回
 *          远端结果码与回送的 checksum，二者一致才视为成功。
 *
 * @param   result  指向已填充并校验过 checksum 的基准结果结构体
 *
 * @retval  RT_EOK       发布成功，CM55 已确认结果
 * @retval  -RT_EINVAL   result 为空、协议版本不匹配或 checksum 校验失败
 * @retval  -RT_ENOSYS   IPC 设备未注册
 * @retval  -RT_ERROR    IPC 事务失败或 CM55 回送校验不匹配
 * @retval  其他         rt_device_open / rt_sem_init 失败码
 */
rt_err_t ai_kit_ipc_publish_benchmark(
    const ai_kit_benchmark_result_t *result);

/**
 * @brief   通过 IPC 触发 CM55 / U55 执行指定的基准测试
 *
 * @details 当前支持触发以下测试：
 *          - COREMARK：CM55 CoreMark
 *          - MEMORY：CM55 SRAM 流式读写拷贝
 *          - MEMORY_PORTABLE：CM55 便携标量 SRAM
 *          - PSRAM_PORTABLE：CM55 私有 PSRAM
 *          - NPU_RESNET_INFERENCE：CM55+U55 ResNet 活跃态吞吐
 *          - NPU_MNIST_INFERENCE：CM55+U55 MNIST 活跃态吞吐
 *          - NPU_RESNET_PEAK_INFERENCE：ResNet 隔离峰值
 *          - NPU_MNIST_PEAK_INFERENCE：MNIST 隔离峰值
 *          - NPU_PERSON_INFERENCE：Person Detection 活跃态吞吐
 *          - NPU_PERSON_PEAK_INFERENCE：Person Detection 隔离峰值
 *
 *          其余 test_id 视为不支持并返回 -RT_EINVAL。
 *
 * @param   test  待启动的基准测试 ID（取值见 ai_kit_benchmark_test_t）
 *
 * @retval  RT_EOK       CM55 接收请求并开始执行
 * @retval  -RT_EINVAL   test 不在白名单内
 * @retval  -RT_ENOSYS   IPC 设备未注册
 * @retval  其他         IPC 事务失败或 CM55 返回的远端错误码
 */
rt_err_t ai_kit_ipc_start_m55_benchmark(ai_kit_benchmark_test_t test);

#endif
