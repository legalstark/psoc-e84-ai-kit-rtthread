/**
 * @file    ai_kit_npu.c
 * @brief   AI Kit NPU 推理基准测试实现
 *
 * @details 该文件在 CM55 上通过 mtb_ml（Ethos-U55 NPU 驱动栈）运行三类
 *          公开版本模型：TFLM Person Detection。
 *          每次运行流程：
 *          - 启用 DWT CYCCNT 与 NPU cycle 计数器
 *          - 加载模型到 s_tensor_arena，记录 model_init 耗时
 *          - 跑首次推理 + 跑所有非基准校验用例，验证模型可信
 *          - WARMUP_ITERATIONS 预热 + BENCHMARK_ITERATIONS 正式测量
 *          - 统计 avg/min/max（CPU 周期 + NPU 周期）→ ai_kit_npu_metrics_t
 *          - 反初始化模型，arena 复用
 *
 *          结果由 ai_kit_benchmark.c 的 ai_kit_npu_thread 汇聚到基准目录。
 */
#include <limits.h>
#include <stdint.h>

#include <rtthread.h>

#include "board.h"
#include "ai_kit_npu.h"
#include "mtb_ml.h"

/** @brief tensor arena 最大字节数（按最大模型 Person 80896 字节设定） */
#define AI_KIT_NPU_MAX_ARENA_BYTES      (80896U)
/** @brief 正式测量前的预热迭代次数（稳定 cache 与 NPU 状态） */
#define AI_KIT_NPU_WARMUP_ITERATIONS    (5U)
/** @brief 正式测量的迭代次数（用于统计 avg/min/max） */
#define AI_KIT_NPU_BENCHMARK_ITERATIONS (100U)
/** @brief NPU 中断优先级 */
#define AI_KIT_NPU_IRQ_PRIORITY         (3U)

/**
 * @brief   单个推理校验用例
 *
 * @details 描述一个输入样本及其期望输出，用于在正式基准前验证模型可信：
 *          - 若 expected_output 不为空：要求 argmax 命中 expected_class
 *            且每个输出元素严格相等
 *          - 若 expected_output 为空：仅要求 argmax 命中 expected_class
 */
typedef struct
{
    const uint8_t *sample_start;   /**< @brief 样本起始地址 */
    const uint8_t *sample_end;     /**< @brief 样本结束地址（exclusive） */
    const int8_t *expected_output; /**< @brief 期望输出（可为 NULL） */
    uint32_t expected_output_elements; /**< @brief expected_output 元素数 */
    uint32_t expected_class;        /**< @brief argmax 应命中的分类索引 */
} ai_kit_npu_validation_case_t;

/**
 * @brief   工作负载描述符
 *
 * @details 把模型二进制、arena 大小、输入输出规模、所有校验用例整合为
 *          单一描述符。benchmark_case 指定哪个 validation_case 作为
 *          正式基准测量的输入（其他用例仅做正确性校验）。
 */
typedef struct
{
    const char *name;                       /**< @brief 工作负载可读名称 */
    const mtb_ml_model_bin_t *model_bin;    /**< @brief mtb_ml 模型描述符 */
    const uint8_t *model_start;             /**< @brief 模型权重起始地址 */
    const uint8_t *model_end;               /**< @brief 模型权重结束地址 */
    const ai_kit_npu_validation_case_t *validation_cases; /**< @brief 校验用例数组 */
    uint32_t validation_case_count;         /**< @brief 校验用例数量 */
    uint32_t benchmark_case;                /**< @brief 作为基准输入的用例索引 */
    uint32_t model_bytes;                   /**< @brief 模型权重字节数 */
    uint32_t arena_bytes;                   /**< @brief 所需 tensor arena 字节数 */
    uint32_t sample_bytes;                  /**< @brief 单样本字节数 */
    uint32_t output_elements;               /**< @brief 输出元素数 */
} ai_kit_npu_workload_descriptor_t;

/* 模型权重与样本数据由链接脚本在 .rodata 段中定义。
 * 每个工作负载提供 model_start/end（权重）与 sample_start/end（输入样本）。 */
extern const uint8_t ai_kit_npu_person_model_start[];
extern const uint8_t ai_kit_npu_person_model_end[];
extern const uint8_t ai_kit_npu_person_sample_start[];
extern const uint8_t ai_kit_npu_person_sample_end[];
extern const uint8_t ai_kit_npu_no_person_sample_start[];
extern const uint8_t ai_kit_npu_no_person_sample_end[];

/** @brief Person Detection "person" 样本的期望输出（2 元素量化值） */
static const int8_t s_person_expected_output[] = {-113, 113};
/** @brief Person Detection "no person" 样本的期望输出（2 元素量化值） */
static const int8_t s_no_person_expected_output[] = {57, -57};

/** @brief Person Detection 校验用例（含 person / no_person 两类） */
static const ai_kit_npu_validation_case_t s_person_cases[] = {
    {
        ai_kit_npu_person_sample_start,
        ai_kit_npu_person_sample_end,
        s_person_expected_output,
        2U,
        1U
    },
    {
        ai_kit_npu_no_person_sample_start,
        ai_kit_npu_no_person_sample_end,
        s_no_person_expected_output,
        2U,
        0U
    }
};

/** @brief TFLM Person Detection mtb_ml 模型描述符 */
static const mtb_ml_model_bin_t s_person_model_bin = {
    "TFLM_PERSON_DETECTION",
    ai_kit_npu_person_model_start,
    239744U,
    80896U
};

/**
 * @brief   工作负载描述符静态表
 *
 * @details 公开版本只保留 Person Detection。条目整合模型二进制、
 *          arena 大小、样本规模与校验用例。
 */
static const ai_kit_npu_workload_descriptor_t s_workloads[] = {
    {
        "TFLM Person Detection",
        &s_person_model_bin,
        ai_kit_npu_person_model_start,
        ai_kit_npu_person_model_end,
        s_person_cases,
        2U,
        0U,
        239744U,
        80896U,
        9216U,
        2U
    }
};

/**
 * @brief   NPU tensor arena 缓冲区
 *
 * @details 放置在 .cy_socmem_data 段（SRAM 中可被 NPU 直接访问的区域），
 *          16 字节对齐。所有模型共享同一 arena：每次 ai_kit_npu_model_init
 *          时按 workload 的 arena_bytes 大小使用前部空间，多余空间未用。
 */
__attribute__((section(".cy_socmem_data"), aligned(16)))
static uint8_t s_tensor_arena[AI_KIT_NPU_MAX_ARENA_BYTES];

/** @brief 当前已加载的 mtb_ml 模型句柄（NULL=未加载） */
static mtb_ml_model_t *s_model;

/**
 * @brief   启用 DWT CYCCNT 周期计数器
 *
 * @details 使能 TRCENA + CYCCNT 复位 + CYCCNTENA，跑 128 个 NOP 后回读
 *          CYCCNT 验证计数器是否真正在自增。失败通常意味着调试器件
 *          被锁死或时钟未使能。
 *
 * @retval  1  计数器已正常自增
 * @retval  0  计数器未启动（调用者应返回 -RT_ETIMEOUT）
 */
static int ai_kit_npu_timer_enable(void)
{
    uint32_t start;
    uint32_t spin;

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
    return DWT->CYCCNT != start;
}

/**
 * @brief   把 CPU 周期数换算为微秒（×1000）
 *
 * @details us_x1000 = cycles * 1e9 / SystemCoreClock，四舍五入到最近整数。
 *          SystemCoreClock==0 时返回 0 防止除零。
 *
 * @param   cycles  CPU 周期数（DWT->CYCCNT 差值）
 *
 * @return  对应的微秒数 × 1000
 */
static uint32_t ai_kit_npu_cycles_to_us_x1000(uint32_t cycles)
{
    uint64_t numerator;

    if (SystemCoreClock == 0U)
    {
        return 0U;
    }
    numerator = (uint64_t)cycles * 1000000000ULL;
    return (uint32_t)((numerator + ((uint64_t)SystemCoreClock / 2ULL)) /
                      (uint64_t)SystemCoreClock);
}

/**
 * @brief   求输出向量的 argmax（最大元素索引）
 *
 * @param   values  输出元素数组
 * @param   count    元素数
 *
 * @return  最大元素索引；count=0 时返回 0
 */
static uint32_t ai_kit_npu_argmax(const MTB_ML_DATA_T *values,
                                  uint32_t count)
{
    uint32_t index;
    uint32_t best = 0U;

    for (index = 1U; index < count; index++)
    {
        if (values[index] > values[best])
        {
            best = index;
        }
    }
    return best;
}

/**
 * @brief   校验推理输出是否与期望用例一致
 *
 * @details 校验规则：
 *          - argmax 必须等于 validation_case->expected_class
 *          - 若 expected_output_elements>0，要求每个输出元素严格相等
 *
 * @param   output           推理输出缓冲
 * @param   output_elements  输出元素数
 * @param   validation_case  期望用例
 *
 * @retval  1  通过
 * @retval  0  不通过
 */
static int ai_kit_npu_output_matches(
    const MTB_ML_DATA_T *output,
    uint32_t output_elements,
    const ai_kit_npu_validation_case_t *validation_case)
{
    uint32_t index;

    if (ai_kit_npu_argmax(output, output_elements) !=
        validation_case->expected_class)
    {
        return 0;
    }
    for (index = 0U; index < validation_case->expected_output_elements;
         index++)
    {
        if ((int8_t)output[index] != validation_case->expected_output[index])
        {
            return 0;
        }
    }
    return 1;
}

/**
 * @brief   反初始化当前已加载的 mtb_ml 模型与运行时
 *
 * @details 顺序：先 mtb_ml_model_deinit 释放模型；若 mtb_ml 运行时仍在
 *          初始化状态，再调用 mtb_ml_deinit 释放运行时。s_model 置 NULL。
 *          基准测试结束与 model_init 失败回滚时均会调用。
 */
static void ai_kit_npu_model_deinit(void)
{
    if (s_model != RT_NULL)
    {
        (void)mtb_ml_model_deinit(s_model);
        s_model = RT_NULL;
    }
    if (mtb_ml_get_init_state() != 0U)
    {
        (void)mtb_ml_deinit();
    }
}

/**
 * @brief   按 workload 枚举取工作负载描述符
 *
 * @param   workload  工作负载枚举
 *
 * @return  指向 s_workloads 对应条目的指针；越界返回 RT_NULL
 */
static const ai_kit_npu_workload_descriptor_t *ai_kit_npu_descriptor(
    ai_kit_npu_workload_t workload)
{
    if ((uint32_t)workload >=
        (sizeof(s_workloads) / sizeof(s_workloads[0])))
    {
        return RT_NULL;
    }
    return &s_workloads[(uint32_t)workload];
}

/**
 * @brief   获取工作负载的可读名称（对外接口）
 *
 * @param   workload  工作负载枚举
 *
 * @return  静态字符串；非法值返回 "invalid"
 */
const char *ai_kit_npu_workload_name(ai_kit_npu_workload_t workload)
{
    const ai_kit_npu_workload_descriptor_t *descriptor =
        ai_kit_npu_descriptor(workload);

    return (descriptor == RT_NULL) ? "invalid" : descriptor->name;
}

/**
 * @brief   加载并初始化模型
 *
 * @details 流程：
 *          - 先调用 ai_kit_npu_model_deinit 清理前次状态
 *          - 校验 descriptor 各字段一致性（模型大小、用例数、arena 大小等）
 *          - 启动 DWT CYCCNT 计时
 *          - mtb_ml_init + mtb_ml_model_init 加载模型到 s_tensor_arena
 *          - 校验实际输入/输出规模与 descriptor 一致
 *          - 设置 cache 管理策略为 ALL_LAYERS
 *          - 记录 model_init 耗时（CPU 周期 → us×1000）
 *
 * @param   descriptor            工作负载描述符
 * @param   model_init_us_x1000  输出参数，写入初始化耗时（us×1000）；可为 NULL
 *
 * @retval  RT_EOK       模型已加载
 * @retval  -RT_EINVAL   descriptor 字段不一致
 * @retval  -RT_ERROR    mtb_ml_init / mtb_ml_model_init 失败
 */
static int ai_kit_npu_model_init(
    const ai_kit_npu_workload_descriptor_t *descriptor,
    uint32_t *model_init_us_x1000)
{
    mtb_ml_model_buffer_t model_buffer;
    cy_rslt_t result;
    uint32_t start;
    uint32_t case_index;
    uint32_t model_bytes =
        (uint32_t)(descriptor->model_end - descriptor->model_start);

    ai_kit_npu_model_deinit();
    if ((model_bytes != descriptor->model_bytes) ||
        (descriptor->validation_cases == RT_NULL) ||
        (descriptor->validation_case_count == 0U) ||
        (descriptor->benchmark_case >= descriptor->validation_case_count) ||
        (descriptor->arena_bytes > sizeof(s_tensor_arena)))
    {
        return -RT_EINVAL;
    }
    for (case_index = 0U; case_index < descriptor->validation_case_count;
         case_index++)
    {
        const ai_kit_npu_validation_case_t *validation_case =
            &descriptor->validation_cases[case_index];

        if (((uint32_t)(validation_case->sample_end -
                        validation_case->sample_start) !=
             descriptor->sample_bytes) ||
            (validation_case->expected_class >= descriptor->output_elements) ||
            ((validation_case->expected_output_elements != 0U) &&
             ((validation_case->expected_output == RT_NULL) ||
              (validation_case->expected_output_elements !=
               descriptor->output_elements))))
        {
            return -RT_EINVAL;
        }
    }

    start = DWT->CYCCNT;
    result = mtb_ml_init(AI_KIT_NPU_IRQ_PRIORITY);
    if (result != MTB_ML_RESULT_SUCCESS)
    {
        rt_kprintf("NPU: mtb_ml_init failed 0x%08lx\n",
                   (unsigned long)result);
        ai_kit_npu_model_deinit();
        return -RT_ERROR;
    }

    model_buffer.tensor_arena = s_tensor_arena;
    model_buffer.tensor_arena_size = descriptor->arena_bytes;

    result = mtb_ml_model_init(descriptor->model_bin,
                               &model_buffer, &s_model);
    if (result != MTB_ML_RESULT_SUCCESS)
    {
        rt_kprintf("NPU: mtb_ml_model_init failed 0x%08lx\n",
                   (unsigned long)result);
        ai_kit_npu_model_deinit();
        return -RT_ERROR;
    }

    if ((s_model == RT_NULL) ||
        (mtb_ml_model_get_input_size(s_model) != descriptor->sample_bytes) ||
        (s_model->output_count != 1) ||
        (s_model->output_size != descriptor->output_elements))
    {
        rt_kprintf("NPU: unexpected tensors input=%d count=%d output=%d\n",
                   (s_model == RT_NULL) ? -1 :
                       mtb_ml_model_get_input_size(s_model),
                   (s_model == RT_NULL) ? -1 : s_model->output_count,
                   (s_model == RT_NULL) ? -1 : s_model->output_size);
        ai_kit_npu_model_deinit();
        return -RT_EINVAL;
    }

    mtb_ml_set_cache_mgmt_type(MTB_ML_ETHOSU_CACHE_MGMT_ALL_LAYERS);
    if (model_init_us_x1000 != RT_NULL)
    {
        *model_init_us_x1000 =
            ai_kit_npu_cycles_to_us_x1000(DWT->CYCCNT - start);
    }
    rt_kprintf("NPU: ready workload=%s model=%lu arena=%lu input=%d output=%d "
               "cpu=%luMHz npu=%luMHz\n",
               descriptor->name,
               (unsigned long)model_bytes,
               (unsigned long)descriptor->arena_bytes,
               mtb_ml_model_get_input_size(s_model), s_model->output_size,
               (unsigned long)(mtb_ml_cpu_clk_freq / 1000000U),
               (unsigned long)(mtb_ml_npu_clk_freq / 1000000U));
    return RT_EOK;
}

/**
 * @brief   运行一次 NPU 推理基准测试（对外接口）
 *
 * @details 完整流程：
 *          - 入口校验 + 取 descriptor + 清零 metrics
 *          - 启用 DWT CYCCNT 计数器
 *          - ai_kit_npu_model_init 加载模型并记录 model_init 耗时
 *          - 取输出缓冲并跑首次推理，校验输出与期望一致；记录 first_inference
 *          - 跑所有非基准校验用例，确保模型可信
 *          - 跑 WARMUP_ITERATIONS 预热
 *          - 跑 BENCHMARK_ITERATIONS 正式测量，每次记录 CPU 周期 + NPU 周期
 *            + 是否命中期望类，最后统计 avg/min/max
 *          - cleanup 反初始化模型并返回；correct==iterations 才视为成功
 *
 * @param   workload  工作负载类型
 * @param   metrics   输出参数，填充完整指标
 *
 * @retval  RT_EOK       所有迭代均命中期望分类
 * @retval  -RT_EINVAL   metrics 为空或 workload 非法
 * @retval  -RT_ETIMEOUT  DWT 计数器未启动
 * @retval  -RT_ERROR    模型初始化 / 首次推理 / 校验 / 正式迭代失败
 */
int ai_kit_npu_benchmark_run(ai_kit_npu_workload_t workload,
                             ai_kit_npu_metrics_t *metrics)
{
    const ai_kit_npu_workload_descriptor_t *descriptor;
    const ai_kit_npu_validation_case_t *benchmark_case;
    const uint8_t *benchmark_sample;
    MTB_ML_DATA_T *output = RT_NULL;
    int output_elements = 0;
    uint32_t iteration;
    uint32_t start;
    uint32_t elapsed_cycles;
    uint32_t minimum_cycles = UINT32_MAX;
    uint32_t maximum_cycles = 0U;
    uint64_t total_cycles = 0ULL;
    uint64_t total_npu_cycles = 0ULL;
    uint32_t minimum_npu_cycles = UINT32_MAX;
    uint32_t maximum_npu_cycles = 0U;
    uint32_t correct = 0U;
    uint32_t observed_class = UINT32_MAX;
    uint32_t validation_case_index;
    uint64_t npu_before;
    uint32_t npu_delta;
    cy_rslt_t result;
    int run_result = -RT_ERROR;

    if (metrics == RT_NULL)
    {
        return -RT_EINVAL;
    }
    descriptor = ai_kit_npu_descriptor(workload);
    if (descriptor == RT_NULL)
    {
        return -RT_EINVAL;
    }
    benchmark_case =
        &descriptor->validation_cases[descriptor->benchmark_case];
    benchmark_sample = benchmark_case->sample_start;
    rt_memset(metrics, 0, sizeof(*metrics));
    if (!ai_kit_npu_timer_enable())
    {
        return -RT_ETIMEOUT;
    }
    if (ai_kit_npu_model_init(descriptor,
                              &metrics->model_init_us_x1000) != RT_EOK)
    {
        return -RT_ERROR;
    }

    result = mtb_ml_model_get_output(s_model, &output, &output_elements);
    if ((result != MTB_ML_RESULT_SUCCESS) || (output == RT_NULL) ||
        (output_elements != descriptor->output_elements))
    {
        run_result = -RT_EINVAL;
        goto cleanup;
    }

    start = DWT->CYCCNT;
    result = mtb_ml_model_run(
        s_model, (MTB_ML_DATA_T *)(uintptr_t)benchmark_sample);
    elapsed_cycles = DWT->CYCCNT - start;
    if (result != MTB_ML_RESULT_SUCCESS)
    {
        rt_kprintf("NPU: first inference failed 0x%08lx\n",
                   (unsigned long)result);
        run_result = -RT_ERROR;
        goto cleanup;
    }
    observed_class = ai_kit_npu_argmax(output, output_elements);
    if (!ai_kit_npu_output_matches(output, output_elements, benchmark_case))
    {
        rt_kprintf("NPU: first inference class=%lu expected=%lu\n",
                   (unsigned long)observed_class,
                   (unsigned long)benchmark_case->expected_class);
        run_result = -RT_ERROR;
        goto cleanup;
    }
    metrics->first_inference_us_x1000 =
        ai_kit_npu_cycles_to_us_x1000(elapsed_cycles);

    for (validation_case_index = 0U;
         validation_case_index < descriptor->validation_case_count;
         validation_case_index++)
    {
        const ai_kit_npu_validation_case_t *validation_case;

        if (validation_case_index == descriptor->benchmark_case)
        {
            continue;
        }
        validation_case =
            &descriptor->validation_cases[validation_case_index];
        result = mtb_ml_model_run(
            s_model,
            (MTB_ML_DATA_T *)(uintptr_t)validation_case->sample_start);
        if ((result != MTB_ML_RESULT_SUCCESS) ||
            !ai_kit_npu_output_matches(output, output_elements,
                                       validation_case))
        {
            rt_kprintf("NPU: validation case %lu failed\n",
                       (unsigned long)validation_case_index);
            run_result = -RT_ERROR;
            goto cleanup;
        }
    }

    for (iteration = 0U; iteration < AI_KIT_NPU_WARMUP_ITERATIONS; iteration++)
    {
        result = mtb_ml_model_run(
            s_model, (MTB_ML_DATA_T *)(uintptr_t)benchmark_sample);
        if (result != MTB_ML_RESULT_SUCCESS)
        {
            run_result = -RT_ERROR;
            goto cleanup;
        }
    }

    for (iteration = 0U; iteration < AI_KIT_NPU_BENCHMARK_ITERATIONS;
         iteration++)
    {
        npu_before = mtb_ml_npu_cycles;
        start = DWT->CYCCNT;
        result = mtb_ml_model_run(
            s_model, (MTB_ML_DATA_T *)(uintptr_t)benchmark_sample);
        elapsed_cycles = DWT->CYCCNT - start;
        if (result != MTB_ML_RESULT_SUCCESS)
        {
            rt_kprintf("NPU: inference %lu failed 0x%08lx\n",
                       (unsigned long)iteration, (unsigned long)result);
            run_result = -RT_ERROR;
            goto cleanup;
        }

        npu_delta = (uint32_t)(mtb_ml_npu_cycles - npu_before);
        observed_class = ai_kit_npu_argmax(output, output_elements);
        if (ai_kit_npu_output_matches(output, output_elements,
                                      benchmark_case))
        {
            correct++;
        }
        total_cycles += elapsed_cycles;
        total_npu_cycles += npu_delta;
        if (elapsed_cycles < minimum_cycles) minimum_cycles = elapsed_cycles;
        if (elapsed_cycles > maximum_cycles) maximum_cycles = elapsed_cycles;
        if (npu_delta < minimum_npu_cycles) minimum_npu_cycles = npu_delta;
        if (npu_delta > maximum_npu_cycles) maximum_npu_cycles = npu_delta;
    }

    metrics->iterations = AI_KIT_NPU_BENCHMARK_ITERATIONS;
    metrics->correct = correct;
    metrics->average_us_x1000 = ai_kit_npu_cycles_to_us_x1000(
        (uint32_t)(total_cycles / AI_KIT_NPU_BENCHMARK_ITERATIONS));
    metrics->minimum_us_x1000 =
        ai_kit_npu_cycles_to_us_x1000(minimum_cycles);
    metrics->maximum_us_x1000 =
        ai_kit_npu_cycles_to_us_x1000(maximum_cycles);
    metrics->average_npu_cycles =
        (uint32_t)(total_npu_cycles / AI_KIT_NPU_BENCHMARK_ITERATIONS);
    metrics->minimum_npu_cycles = minimum_npu_cycles;
    metrics->maximum_npu_cycles = maximum_npu_cycles;
    metrics->model_bytes = descriptor->model_bytes;
    metrics->arena_bytes = descriptor->arena_bytes;
    metrics->input_bytes = descriptor->sample_bytes;
    metrics->output_elements = descriptor->output_elements;
    metrics->expected_class = benchmark_case->expected_class;
    metrics->observed_class = observed_class;
    run_result = (correct == AI_KIT_NPU_BENCHMARK_ITERATIONS) ?
                 RT_EOK : -RT_ERROR;

cleanup:
    ai_kit_npu_model_deinit();
    return run_result;
}
