#ifndef AI_KIT_TOUCH_H
#define AI_KIT_TOUCH_H

#include <stdint.h>

/**
 * @file    ai_kit_touch.h
 * @brief   AI Kit 触摸屏采样对外接口
 *
 * @details 该头文件声明触摸控制器初始化与最新样本查询接口。
 *          实现位于 ai_kit_touch.c：依赖 ai_kit_hmi_bus（I2C），
 *          内部跑一个独立采样线程周期读 GT911 等控制器寄存器，
 *          把最新样本写入原子变量供 LVGL indev 驱动读取。
 */

/**
 * @brief   触摸初始化结果
 */
typedef enum
{
    AI_KIT_TOUCH_OK = 0,                       /**< @brief 成功 */
    AI_KIT_TOUCH_ERROR_HMI_BUS = 1,            /**< @brief HMI I2C 总线未就绪 */
    AI_KIT_TOUCH_ERROR_CONTROLLER_INIT = 2,    /**< @brief 触控 IC 初始化失败 */
    AI_KIT_TOUCH_ERROR_THREAD_CREATE = 3        /**< @brief 采样线程创建失败 */
} ai_kit_touch_result_t;

/**
 * @brief   触摸样本（由采样线程维护）
 */
typedef struct
{
    uint32_t pressed;   /**< @brief 1=按下，0=抬起 */
    uint32_t event;      /**< @brief 事件类型（按下 / 抬起 / 移动） */
    uint32_t x;          /**< @brief X 坐标（px） */
    uint32_t y;          /**< @brief Y 坐标（px） */
    uint32_t sequence;   /**< @brief 样本序号，每次更新递增 */
    uint32_t error;      /**< @brief 采样过程中累积的错误码 */
} ai_kit_touch_sample_t;

/**
 * @brief   初始化触摸子系统（I2C + 控制器 + 采样线程）
 *
 * @retval  AI_KIT_TOUCH_OK       成功
 * @retval  AI_KIT_TOUCH_ERROR_*  对应失败原因
 */
ai_kit_touch_result_t ai_kit_touch_init(void);

/**
 * @brief   获取最新触摸样本（非阻塞，从原子变量读取）
 *
 * @param   sample  输出参数，写入最新样本
 */
void ai_kit_touch_get_sample(ai_kit_touch_sample_t *sample);

#endif
