#ifndef AI_KIT_BENCHMARK_H
#define AI_KIT_BENCHMARK_H

#include "ai_kit_multicore.h"

/**
 * @file    ai_kit_benchmark.h
 * @brief   AI Kit 基准测试管理对外接口
 *
 * @details 该头文件声明 CM55 侧基准测试目录的统一管理接口，覆盖：
 *          - 初始化基准目录（注册 CoreMark / 内存 / NPU / IPC 各项条目）
 *          - 启动本地基准测试（CoreMark、SRAM、PSRAM、NPU 推理）
 *          - 导入 CM33 通过共享内存上报的基准结果
 *          - 查询单项结果或整个目录快照
 *
 *          目录结构与条目数据结构定义参见 ai_kit_multicore.h，
 *          实现细节参见 ai_kit_benchmark.c。
 */

/**
 * @brief   初始化基准测试目录（单次幂等）
 *
 * @details 完成：
 *          - 初始化互斥量 s_benchmark_mutex
 *          - 清零目录并写入协议版本号
 *          - 预登记 CoreMark / 内存 / NPU / IPC 全部条目（state=IDLE）
 *          - 标记 s_benchmark_initialized
 *
 * @retval  RT_EOK        初始化成功或已初始化
 * @retval  -RT_ERROR     rt_mutex_init 失败
 * @retval  -RT_ENOMEM    目录容量不足，预登记失败
 */
int ai_kit_benchmark_init(void);

/**
 * @brief   启动一次本地基准测试
 *
 * @details 该函数会根据 test 选择对应的线程入口并创建独立线程执行：
 *          - COREMARK          -> ai_kit_coremark_thread
 *          - MEMORY            -> ai_kit_memory_thread（DMA 加速版）
 *          - MEMORY_PORTABLE   -> ai_kit_memory_portable_thread（标量版）
 *          - PSRAM_PORTABLE    -> ai_kit_psram_portable_thread
 *          - NPU_*             -> ai_kit_npu_thread（含辅助指标项）
 *
 *          同一时刻只允许一个基准线程运行，重复启动返回 -RT_EBUSY。
 *
 * @param   test  待启动的测试项 ID
 *
 * @retval  RT_EOK        线程已创建并启动
 * @retval  -RT_EINVAL    test 不是受支持的本地测试项
 * @retval  -RT_ERROR     目录未就绪且初始化失败
 * @retval  -RT_EBUSY     已有基准线程运行
 * @retval  -RT_ENOMEM    rt_thread_create 失败
 * @retval  其他          rt_thread_startup 失败码
 */
int ai_kit_benchmark_start(ai_kit_benchmark_test_t test);

/**
 * @brief   导入 CM33 上报的基准结果
 *
 * @details 用于接收 CM33 通过 IPC 共享内存推送过来的结果：
 *          - 校验 protocol_version / executor==M33 / test_id 合法 / checksum
 *          - 在目录中查找对应 (test, EXECUTOR_M33) 条目并覆盖
 *          - 更新目录 revision 与 checksum
 *
 * @param   result  指向 CM33 共享内存中读出的结果结构
 *
 * @retval  RT_EOK        导入成功
 * @retval  -RT_EINVAL    result 为空、字段非法或 checksum 不匹配
 * @retval  -RT_ERROR     目录未就绪且初始化失败
 */
int ai_kit_benchmark_publish_external(
    const ai_kit_benchmark_result_t *result);

/**
 * @brief   查询单项基准结果
 *
 * @param   test      测试项 ID
 * @param   executor  执行者（M55 / M33 / NPU）
 * @param   result    输出参数，拷贝对应条目内容
 *
 * @retval  RT_EOK      查询成功
 * @retval  -RT_EINVAL  result 为空
 * @retval  -RT_ERROR   目录未就绪且初始化失败，result 被清零
 * @retval  -RT_ENOSYS  对应 (test, executor) 条目不存在，result 被清零
 */
int ai_kit_benchmark_get_result(ai_kit_benchmark_test_t test,
                                ai_kit_benchmark_executor_t executor,
                                ai_kit_benchmark_result_t *result);

/**
 * @brief   获取整个基准目录快照
 *
 * @details 调用者在互斥量保护下拷贝整个目录结构，适合一次性导出全部
 *          基准结果（例如 CM33 msh 命令导出整机性能报告）。
 *
 * @param   catalog  输出参数，拷贝当前目录内容
 */
void ai_kit_benchmark_get_catalog(ai_kit_benchmark_catalog_t *catalog);

#endif
