#ifndef AI_KIT_MULTICORE_H
#define AI_KIT_MULTICORE_H

#include <stdint.h>

/**
 * @file    ai_kit_multicore.h
 * @brief   AI Kit 双核共享数据结构与协议常量
 *
 * @details 该头文件被 CM33 / CM55 / NPU 三方共同包含，定义：
 *          - CM55 整机状态区（ai_kit_cm55_status_t）的内存布局、地址、
 *            魔数与 ABI 版本，供 CM33 通过共享 SRAM 读取 CM55 整机状态
 *          - 基准测试目录（ai_kit_benchmark_catalog_t）的协议版本、
 *            容量、条目结构、状态/单位/错误枚举、checksum 算法
 *          - 显示 / 触摸 / GPU / PSRAM 等子系统的状态枚举
 *
 *          任何字段的 ABI 变化都必须同步更新 *_ABI_VERSION，否则
 *          对端会因为版本不匹配拒绝接收。
 */

/** @brief CM55 状态区魔数（ASCII "M55R"），用于校验状态区已初始化 */
#define AI_KIT_CM55_STATUS_MAGIC       (0x4D353552UL)
/** @brief CM55 状态区 ABI 版本；不兼容修改必须递增 */
#define AI_KIT_CM55_STATUS_ABI_VERSION (13UL)
/** @brief CM55 状态区在共享 SRAM 中的物理起始地址 */
#define AI_KIT_CM55_STATUS_ADDRESS     (0x262FC000UL)
/** @brief CM55 状态区字节数；下方静态断言保证结构大小与此一致 */
#define AI_KIT_CM55_STATUS_SIZE        (2176UL)

/** @brief 基准测试协议版本；不兼容修改必须递增 */
#define AI_KIT_BENCHMARK_PROTOCOL_VERSION (7UL)
/** @brief 基准目录最大条目数（覆盖全部 NPU 套件 + CoreMark + 内存 + IPC） */
#define AI_KIT_BENCHMARK_CATALOG_CAPACITY (40UL)

/**
 * @brief   基准测试项 ID
 *
 * @details 每个枚举值对应目录中一种测试条目。NPU 套件按
 *          workload × metric × peak/system-active 展开，共 6 套 30 项。
 */
typedef enum
{
    AI_KIT_BENCHMARK_TEST_NONE = 0,                              /**< @brief 占位/未使用 */
    AI_KIT_BENCHMARK_TEST_COREMARK = 1,                          /**< @brief CoreMark 分数 */
    AI_KIT_BENCHMARK_TEST_MEMORY = 2,                            /**< @brief SRAM DMA 内存带宽 */
    AI_KIT_BENCHMARK_TEST_IPC_LATENCY = 3,                       /**< @brief IPC 往返延迟 */
    AI_KIT_BENCHMARK_TEST_NPU_RESNET_INFERENCE = 4,              /**< @brief ResNet 推理吞吐 */
    AI_KIT_BENCHMARK_TEST_MEMORY_PORTABLE = 5,                   /**< @brief SRAM 标量内存带宽 */
    AI_KIT_BENCHMARK_TEST_PSRAM_PORTABLE = 6,                    /**< @brief PSRAM 标量内存带宽 */
    AI_KIT_BENCHMARK_TEST_NPU_RESNET_WALL_LATENCY = 7,          /**< @brief ResNet 端到端延迟 */
    AI_KIT_BENCHMARK_TEST_NPU_RESNET_HARDWARE_CYCLES = 8,        /**< @brief ResNet NPU 周期数 */
    AI_KIT_BENCHMARK_TEST_NPU_RESNET_MODEL_INIT = 9,             /**< @brief ResNet 模型初始化耗时 */
    AI_KIT_BENCHMARK_TEST_NPU_MNIST_INFERENCE = 10,              /**< @brief MNIST 推理吞吐 */
    AI_KIT_BENCHMARK_TEST_NPU_MNIST_WALL_LATENCY = 11,           /**< @brief MNIST 端到端延迟 */
    AI_KIT_BENCHMARK_TEST_NPU_MNIST_HARDWARE_CYCLES = 12,        /**< @brief MNIST NPU 周期数 */
    AI_KIT_BENCHMARK_TEST_NPU_MNIST_MODEL_INIT = 13,             /**< @brief MNIST 模型初始化耗时 */
    AI_KIT_BENCHMARK_TEST_NPU_RESNET_FIRST_INFERENCE = 14,       /**< @brief ResNet 首次推理耗时 */
    AI_KIT_BENCHMARK_TEST_NPU_MNIST_FIRST_INFERENCE = 15,        /**< @brief MNIST 首次推理耗时 */
    AI_KIT_BENCHMARK_TEST_NPU_RESNET_PEAK_INFERENCE = 16,        /**< @brief ResNet 峰值吞吐 */
    AI_KIT_BENCHMARK_TEST_NPU_RESNET_PEAK_WALL_LATENCY = 17,     /**< @brief ResNet 峰值延迟 */
    AI_KIT_BENCHMARK_TEST_NPU_RESNET_PEAK_HARDWARE_CYCLES = 18,  /**< @brief ResNet 峰值 NPU 周期 */
    AI_KIT_BENCHMARK_TEST_NPU_RESNET_PEAK_MODEL_INIT = 19,       /**< @brief ResNet 峰值模型初始化 */
    AI_KIT_BENCHMARK_TEST_NPU_RESNET_PEAK_FIRST_INFERENCE = 20,  /**< @brief ResNet 峰值首次推理 */
    AI_KIT_BENCHMARK_TEST_NPU_MNIST_PEAK_INFERENCE = 21,        /**< @brief MNIST 峰值吞吐 */
    AI_KIT_BENCHMARK_TEST_NPU_MNIST_PEAK_WALL_LATENCY = 22,      /**< @brief MNIST 峰值延迟 */
    AI_KIT_BENCHMARK_TEST_NPU_MNIST_PEAK_HARDWARE_CYCLES = 23,   /**< @brief MNIST 峰值 NPU 周期 */
    AI_KIT_BENCHMARK_TEST_NPU_MNIST_PEAK_MODEL_INIT = 24,        /**< @brief MNIST 峰值模型初始化 */
    AI_KIT_BENCHMARK_TEST_NPU_MNIST_PEAK_FIRST_INFERENCE = 25,   /**< @brief MNIST 峰值首次推理 */
    AI_KIT_BENCHMARK_TEST_NPU_PERSON_INFERENCE = 26,             /**< @brief Person 推理吞吐 */
    AI_KIT_BENCHMARK_TEST_NPU_PERSON_WALL_LATENCY = 27,          /**< @brief Person 端到端延迟 */
    AI_KIT_BENCHMARK_TEST_NPU_PERSON_HARDWARE_CYCLES = 28,       /**< @brief Person NPU 周期数 */
    AI_KIT_BENCHMARK_TEST_NPU_PERSON_MODEL_INIT = 29,           /**< @brief Person 模型初始化耗时 */
    AI_KIT_BENCHMARK_TEST_NPU_PERSON_FIRST_INFERENCE = 30,      /**< @brief Person 首次推理耗时 */
    AI_KIT_BENCHMARK_TEST_NPU_PERSON_PEAK_INFERENCE = 31,        /**< @brief Person 峰值吞吐 */
    AI_KIT_BENCHMARK_TEST_NPU_PERSON_PEAK_WALL_LATENCY = 32,     /**< @brief Person 峰值延迟 */
    AI_KIT_BENCHMARK_TEST_NPU_PERSON_PEAK_HARDWARE_CYCLES = 33,  /**< @brief Person 峰值 NPU 周期 */
    AI_KIT_BENCHMARK_TEST_NPU_PERSON_PEAK_MODEL_INIT = 34,       /**< @brief Person 峰值模型初始化 */
    AI_KIT_BENCHMARK_TEST_NPU_PERSON_PEAK_FIRST_INFERENCE = 35   /**< @brief Person 峰值首次推理 */
} ai_kit_benchmark_test_t;

/**
 * @brief   基准测试执行者
 *
 * @details 用 Cortex-M 核号作为枚举值便于人工识别；NPU 单独一个值。
 */
typedef enum
{
    AI_KIT_BENCHMARK_EXECUTOR_NONE = 0,  /**< @brief 未指定 */
    AI_KIT_BENCHMARK_EXECUTOR_M33 = 33,   /**< @brief CM33 主核 */
    AI_KIT_BENCHMARK_EXECUTOR_M55 = 55,   /**< @brief CM55 主核 */
    AI_KIT_BENCHMARK_EXECUTOR_NPU = 85    /**< @brief Ethos-U55 NPU */
} ai_kit_benchmark_executor_t;

/**
 * @brief   基准条目运行状态
 */
typedef enum
{
    AI_KIT_BENCHMARK_STATE_IDLE = 0,     /**< @brief 已登记未启动 */
    AI_KIT_BENCHMARK_STATE_RUNNING = 1,  /**< @brief 测试中 */
    AI_KIT_BENCHMARK_STATE_COMPLETE = 2, /**< @brief 测试完成，结果可用 */
    AI_KIT_BENCHMARK_STATE_ERROR = 3     /**< @brief 测试失败 */
} ai_kit_benchmark_state_t;

/**
 * @brief   基准条目 metric 单位
 */
typedef enum
{
    AI_KIT_BENCHMARK_UNIT_NONE = 0,                    /**< @brief 未指定 */
    AI_KIT_BENCHMARK_UNIT_COREMARK_PER_SECOND = 1,    /**< @brief CoreMark/s */
    AI_KIT_BENCHMARK_UNIT_MEGABYTES_PER_SECOND = 2,   /**< @brief MiB/s */
    AI_KIT_BENCHMARK_UNIT_MICROSECONDS = 3,            /**< @brief us */
    AI_KIT_BENCHMARK_UNIT_INFERENCES_PER_SECOND = 4,  /**< @brief inferences/s */
    AI_KIT_BENCHMARK_UNIT_KILOCYCLES = 5               /**< @brief 千周期 */
} ai_kit_benchmark_unit_t;

/**
 * @brief   基准测试失败原因
 */
typedef enum
{
    AI_KIT_BENCHMARK_ERROR_NONE = 0,                /**< @brief 无错误 */
    AI_KIT_BENCHMARK_ERROR_BUSY = 1,                /**< @brief 已有线程运行 */
    AI_KIT_BENCHMARK_ERROR_THREAD_CREATE = 2,       /**< @brief rt_thread_create 失败 */
    AI_KIT_BENCHMARK_ERROR_REPORT_MISSING = 3,      /**< @brief CoreMark 回调未触发 */
    AI_KIT_BENCHMARK_ERROR_TOO_SHORT = 4,           /**< @brief CoreMark 运行不足最小秒数 */
    AI_KIT_BENCHMARK_ERROR_VALIDATION = 5,          /**< @brief 数据校验失败 */
    AI_KIT_BENCHMARK_ERROR_UNSUPPORTED = 6,        /**< @brief 不支持的测试项 */
    AI_KIT_BENCHMARK_ERROR_TIMER_UNAVAILABLE = 7,   /**< @brief DWT 计数器未启动 */
    AI_KIT_BENCHMARK_ERROR_TIMEOUT = 8,             /**< @brief 超时 */
    AI_KIT_BENCHMARK_ERROR_PUBLISH = 9,             /**< @brief publish_external 失败 */
    AI_KIT_BENCHMARK_ERROR_NO_MEMORY = 10           /**< @brief 内存分配失败 */
} ai_kit_benchmark_error_t;

/**
 * @brief   单条基准测试结果（52 字节，跨核共享）
 *
 * @details 所有 metric 字段均为 ×1000 的定点数：
 *          - CoreMark 分数        → iterations * RT_TICK_PER_SECOND / ticks
 *          - 内存带宽            → MiB/s × 1000
 *          - 推理延迟            → us × 1000
 *          - NPU 周期            → kilocycles
 *          - 吞吐率              → inferences/s × 1000
 *
 *          minimum / maximum 用于延迟类指标的抖动范围，或内存带宽的
 *          写 / 复制副指标（由 ai_kit_benchmark.c 的 NPU/内存线程决定）。
 *          checksum 由 ai_kit_benchmark_result_checksum 计算，用于
 *          跨核传递时检测字段是否被正确写回。
 */
typedef struct
{
    uint32_t protocol_version;  /**< @brief 协议版本（== AI_KIT_BENCHMARK_PROTOCOL_VERSION） */
    uint32_t sequence;          /**< @brief 同一 (test, executor) 条目的重测序号 */
    uint32_t test_id;           /**< @brief 测试项 ID */
    uint32_t executor;          /**< @brief 执行者 ID */
    uint32_t state;             /**< @brief 运行状态 */
    uint32_t error;             /**< @brief 失败原因 */
    uint32_t iterations;        /**< @brief 测试迭代次数 */
    uint32_t elapsed_us;        /**< @brief 总耗时（us） */
    uint32_t unit;              /**< @brief metric 单位 */
    uint32_t metric_x1000;      /**< @brief 主指标 × 1000 */
    uint32_t minimum_x1000;     /**< @brief 最小值 × 1000 */
    uint32_t maximum_x1000;     /**< @brief 最大值 × 1000 */
    uint32_t checksum;          /**< @brief 二级校验，由下方函数计算 */
} ai_kit_benchmark_result_t;

/**
 * @brief   计算单条结果的 checksum
 *
 * @details 用初值 0x42454E43（"BENC"）依次异或所有字段并循环左移 5 位。
 *          写结果到共享内存前必须调用本函数更新 checksum；对端读取后
 *          也用本函数校验，避免读到半新半旧的字段。
 *
 * @param   result  指向结果条目
 *
 * @return  32 位 checksum
 */
static inline uint32_t ai_kit_benchmark_result_checksum(
    const ai_kit_benchmark_result_t *result)
{
    uint32_t checksum = 0x42454E43UL;

    checksum ^= result->protocol_version;
    checksum = (checksum << 5) | (checksum >> 27);
    checksum ^= result->sequence;
    checksum = (checksum << 5) | (checksum >> 27);
    checksum ^= result->test_id;
    checksum ^= result->executor << 8;
    checksum ^= result->state << 16;
    checksum ^= result->error << 24;
    checksum ^= result->iterations;
    checksum ^= result->elapsed_us;
    checksum ^= result->unit;
    checksum ^= result->metric_x1000;
    checksum ^= result->minimum_x1000;
    checksum ^= result->maximum_x1000;
    return checksum;
}

/**
 * @brief   整机基准目录（嵌入在 CM55 状态区内）
 *
 * @details revision 由 ai_kit_benchmark_catalog_changed 递增；
 *          checksum 由 ai_kit_benchmark_catalog_checksum 计算，覆盖
 *          头部字段与所有条目 checksum，用于 CM33 检测目录是否更新。
 */
typedef struct
{
    uint32_t protocol_version;  /**< @brief 协议版本 */
    uint32_t revision;          /**< @brief 目录变更序号（任何写入后递增） */
    uint32_t count;             /**< @brief 已登记条目数 */
    uint32_t checksum;          /**< @brief 目录级 checksum */
    ai_kit_benchmark_result_t entries[AI_KIT_BENCHMARK_CATALOG_CAPACITY]; /**< @brief 条目数组 */
} ai_kit_benchmark_catalog_t;

/**
 * @brief   计算整个目录的 checksum
 *
 * @details 初值 0x4341544C（"CATL"），异或头部字段 + 每个条目的
 *          checksum 与字段级 checksum。任何条目修改后必须重算并
 *          写回 catalog->checksum，否则对端检测不到变更。
 *
 * @param   catalog  指向目录
 *
 * @return  32 位 checksum
 */
static inline uint32_t ai_kit_benchmark_catalog_checksum(
    const ai_kit_benchmark_catalog_t *catalog)
{
    uint32_t checksum = 0x4341544CUL;
    uint32_t index;

    checksum ^= catalog->protocol_version;
    checksum = (checksum << 5) | (checksum >> 27);
    checksum ^= catalog->revision;
    checksum = (checksum << 5) | (checksum >> 27);
    checksum ^= catalog->count;

    for (index = 0U; index < AI_KIT_BENCHMARK_CATALOG_CAPACITY; index++)
    {
        checksum = (checksum << 5) | (checksum >> 27);
        checksum ^= catalog->entries[index].checksum;
        checksum ^= ai_kit_benchmark_result_checksum(
            &catalog->entries[index]);
    }

    return checksum;
}

/**
 * @brief   CM55 整机运行状态
 */
typedef enum
{
    AI_KIT_CM55_STATE_RESET = 0,   /**< @brief 复位中 */
    AI_KIT_CM55_STATE_RUNNING = 1  /**< @brief 已启动运行 */
} ai_kit_cm55_state_t;

/**
 * @brief   显示子系统状态
 */
typedef enum
{
    AI_KIT_DISPLAY_STATE_DISABLED = 0,       /**< @brief 未初始化 */
    AI_KIT_DISPLAY_STATE_INITIALIZING = 1,   /**< @brief 初始化中 */
    AI_KIT_DISPLAY_STATE_READY = 2,          /**< @brief 已就绪 */
    AI_KIT_DISPLAY_STATE_ERROR = 3           /**< @brief 初始化失败 */
} ai_kit_display_state_t;

/**
 * @brief   触摸子系统状态
 */
typedef enum
{
    AI_KIT_TOUCH_STATE_DISABLED = 0,         /**< @brief 未初始化 */
    AI_KIT_TOUCH_STATE_INITIALIZING = 1,     /**< @brief 初始化中 */
    AI_KIT_TOUCH_STATE_READY = 2,            /**< @brief 已就绪 */
    AI_KIT_TOUCH_STATE_ERROR = 3            /**< @brief 初始化失败 */
} ai_kit_touch_state_t;

/**
 * @brief   GPU（VG-Lite）子系统状态
 */
typedef enum
{
    AI_KIT_GPU_STATE_DISABLED = 0,        /**< @brief 未初始化 */
    AI_KIT_GPU_STATE_INITIALIZING = 1,    /**< @brief 初始化中 */
    AI_KIT_GPU_STATE_READY = 2,           /**< @brief 已就绪 */
    AI_KIT_GPU_STATE_IRQ_ERROR = 3,       /**< @brief GPU 中断注册失败 */
    AI_KIT_GPU_STATE_VG_LITE_ERROR = 4    /**< @brief vg_lite_init 失败 */
} ai_kit_gpu_state_t;

/**
 * @brief   PSRAM 子系统状态
 */
typedef enum
{
    AI_KIT_PSRAM_STATE_DISABLED = 0,  /**< @brief 未初始化 */
    AI_KIT_PSRAM_STATE_READY = 1,     /**< @brief 已就绪 */
    AI_KIT_PSRAM_STATE_ERROR = 2      /**< @brief 初始化失败 */
} ai_kit_psram_state_t;

/**
 * @brief   CM55 整机状态区（跨核共享，位于 AI_KIT_CM55_STATUS_ADDRESS）
 *
 * @details CM55 是状态合并发起者：把显示/触摸/GPU/PSRAM 各子系统状态、
 *          当前触摸坐标、整机基准目录 benchmarks 全部写入本结构，再
 *          SCB_CleanDCache_by_Addr 写回共享 SRAM。CM33 通过共享 SRAM
 *          直接读取，无需 IPC 帧即可拿到整机快照。
 *
 *          magic + abi_version 用于 CM33 校验结构版本匹配；
 *          heartbeat 每次 CM55 publish 时自增，CM33 用它判断 CM55 是否
 *          仍在运行。
 */
typedef struct
{
    uint32_t magic;                /**< @brief == AI_KIT_CM55_STATUS_MAGIC */
    uint32_t abi_version;          /**< @brief == AI_KIT_CM55_STATUS_ABI_VERSION */
    uint32_t state;                /**< @brief ai_kit_cm55_state_t */
    uint32_t heartbeat;            /**< @brief 每次 publish 递增的心跳 */
    uint32_t display_state;        /**< @brief ai_kit_display_state_t */
    uint32_t display_error;        /**< @brief 显示子系统错误码 */
    uint32_t display_fb_address;   /**< @brief 当前 DC 扫描的 framebuffer 地址 */
    uint32_t display_scan_position; /**< @brief 当前扫描行号 */
    uint32_t touch_state;          /**< @brief ai_kit_touch_state_t */
    uint32_t touch_error;          /**< @brief 触摸子系统错误码 */
    uint32_t touch_pressed;        /**< @brief 1=按下 0=抬起 */
    uint32_t touch_event;          /**< @brief 触摸事件类型 */
    uint32_t touch_x;              /**< @brief 最新样本 X */
    uint32_t touch_y;              /**< @brief 最新样本 Y */
    uint32_t touch_sequence;       /**< @brief 最新样本序号 */
    uint32_t gpu_state;            /**< @brief ai_kit_gpu_state_t */
    ai_kit_benchmark_catalog_t benchmarks; /**< @brief 整机基准目录 */
    uint32_t psram_state;          /**< @brief ai_kit_psram_state_t */
    uint32_t psram_error;          /**< @brief PSRAM 子系统错误码 */
    uint32_t psram_private_base;   /**< @brief PSRAM 私有映射起始地址 */
    uint32_t psram_private_size;   /**< @brief PSRAM 私有映射字节数 */
} ai_kit_cm55_status_t;

/** @brief 编译期断言：状态区结构大小必须与 AI_KIT_CM55_STATUS_SIZE 一致 */
typedef char ai_kit_cm55_status_size_must_match[
    (sizeof(ai_kit_cm55_status_t) == AI_KIT_CM55_STATUS_SIZE) ? 1 : -1];

#endif
