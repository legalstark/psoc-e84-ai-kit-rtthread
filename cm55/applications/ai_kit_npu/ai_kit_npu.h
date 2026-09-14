#ifndef AI_KIT_NPU_H
#define AI_KIT_NPU_H

#include <stdint.h>

/**
 * @file    ai_kit_npu.h
 * @brief   AI Kit NPU 推理基准测试对外接口
 *
 * @details 该头文件声明 CM55 上运行 Ethos-U55 NPU 推理基准测试的接口：
 *          公开版本只保留来源和许可清晰的 Person Detection 工作负载，每次运行
 *          返回完整 ai_kit_npu_metrics_t 指标结构（含推理时间、NPU 周期、
 *          模型大小、准确率等）。结果由 ai_kit_benchmark.c 汇聚到基准目录。
 *          实现细节参见 ai_kit_npu.c。
 */

/**
 * @brief   NPU 工作负载类型
 */
typedef enum
{
    AI_KIT_NPU_WORKLOAD_PERSON = 0  /**< @brief TFLM Person Detection 人物检测 */
} ai_kit_npu_workload_t;

/**
 * @brief   NPU 推理基准指标集合
 *
 * @details 由 ai_kit_npu_benchmark_run 填充，包含：
 *          - 运行规模：iterations / correct（正确数）
 *          - 时间维度（us*1000）：model_init / first_inference / avg / min / max
 *          - NPU 周期：avg / min / max（kilocycles 单位由调用者换算）
 *          - 模型规模：model_bytes / arena_bytes / input_bytes / output_elements
 *          - 准确率：expected_class / observed_class
 */
typedef struct
{
    uint32_t iterations;             /**< @brief 实际跑的推理迭代数 */
    uint32_t correct;                /**< @brief 与期望类一致的推理数 */
    uint32_t model_init_us_x1000;    /**< @brief 模型初始化耗时（us*1000） */
    uint32_t first_inference_us_x1000; /**< @brief 首次推理耗时（us*1000） */
    uint32_t average_us_x1000;       /**< @brief 平均端到端延迟（us*1000） */
    uint32_t minimum_us_x1000;      /**< @brief 最小端到端延迟（us*1000） */
    uint32_t maximum_us_x1000;      /**< @brief 最大端到端延迟（us*1000） */
    uint32_t average_npu_cycles;    /**< @brief 平均 NPU 周期数 */
    uint32_t minimum_npu_cycles;    /**< @brief 最小 NPU 周期数 */
    uint32_t maximum_npu_cycles;    /**< @brief 最大 NPU 周期数 */
    uint32_t model_bytes;            /**< @brief 模型权重字节数 */
    uint32_t arena_bytes;            /**< @brief tensor arena 字节数 */
    uint32_t input_bytes;            /**< @brief 单个输入样本字节数 */
    uint32_t output_elements;        /**< @brief 输出元素数 */
    uint32_t expected_class;         /**< @brief 期望分类索引 */
    uint32_t observed_class;         /**< @brief 实际观察到的分类索引 */
} ai_kit_npu_metrics_t;

/**
 * @brief   获取工作负载的可读名称
 *
 * @param   workload  工作负载类型
 *
 * @return  静态字符串；非法值返回 "invalid"
 */
const char *ai_kit_npu_workload_name(ai_kit_npu_workload_t workload);

/**
 * @brief   运行一次 NPU 推理基准测试
 *
 * @details 完整流程：
 *          - 启用 DWT 周期计数器
 *          - 初始化 mtb_ml + 加载模型（记录 model_init 耗时）
 *          - 跑首次推理并校验输出（记录 first_inference 耗时）
 *          - 跑所有非基准校验样本，确保模型可信
 *          - 跑 WARMUP_ITERATIONS 预热
 *          - 跑 BENCHMARK_ITERATIONS 正式测量，统计 avg/min/max
 *            （CPU 周期 + NPU 周期）
 *          - 反初始化模型并返回
 *
 * @param   workload  工作负载类型
 * @param   metrics   输出参数，填充基准指标
 *
 * @retval  RT_EOK        测试通过，所有迭代均命中期望分类
 * @retval  -RT_EINVAL    metrics 为空或 workload 非法
 * @retval  -RT_ETIMEOUT   DWT 周期计数器未启动
 * @retval  -RT_ERROR     模型初始化 / 推理 / 校验失败
 */
int ai_kit_npu_benchmark_run(ai_kit_npu_workload_t workload,
                             ai_kit_npu_metrics_t *metrics);

#endif
