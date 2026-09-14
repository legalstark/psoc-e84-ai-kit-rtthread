#ifndef AI_KIT_HMI_BUS_H
#define AI_KIT_HMI_BUS_H

#include "cy_pdl.h"

/**
 * @file    ai_kit_hmi_bus.h
 * @brief   AI Kit HMI I2C 总线对外接口
 *
 * @details 该头文件声明 HMI 子系统共享的 SCB I2C 总线初始化与句柄接口。
 *          这条总线接触摸控制器、显示桥等外设，由 ai_kit_hmi_bus.c
 *          做单一初始化并暴露 base/context 给各驱动复用，避免重复 init。
 */

/**
 * @brief   HMI I2C 总线初始化结果
 */
typedef enum
{
    AI_KIT_HMI_BUS_OK = 0,                /**< @brief 成功 */
    AI_KIT_HMI_BUS_ERROR_I2C_INIT = 1,   /**< @brief I2C 硬件初始化失败 */
    AI_KIT_HMI_BUS_ERROR_IRQ_INIT = 2     /**< @brief I2C 中断注册失败 */
} ai_kit_hmi_bus_result_t;

/**
 * @brief   初始化 HMI I2C 总线（仅初始化一次，可重复调用）
 *
 * @return  AI_KIT_HMI_BUS_OK 或具体错误码
 */
ai_kit_hmi_bus_result_t ai_kit_hmi_bus_init(void);

/**
 * @brief   获取 HMI I2C 总线的 SCB 寄存器基址
 *
 * @return  指向 CySCB_Type 的指针
 */
CySCB_Type *ai_kit_hmi_bus_base(void);

/**
 * @brief   获取 HMI I2C 总线的 PDL 上下文（中断 / 状态用）
 *
 * @return  指向 cy_stc_scb_i2c_context_t 的指针
 */
cy_stc_scb_i2c_context_t *ai_kit_hmi_bus_context(void);

#endif
