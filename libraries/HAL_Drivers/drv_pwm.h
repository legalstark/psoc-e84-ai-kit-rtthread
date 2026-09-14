/*
 * Copyright (c) 2006-2023, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2022-07-13     Rbb666       first version
 */

#ifndef __PWM_CONFIG_H__
#define __PWM_CONFIG_H__

#include <rtthread.h>
#include <board.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IFX_PWM_DEVICE(_cfg, _hal_cfg, _name) \
{                                              \
    .tcpwm_pwm_config = &(_cfg),               \
    .hal_cfg          = &(_hal_cfg),           \
    .name             = (_name),               \
},

#ifdef BSP_USING_PWM_LED
#define IFX_PWM_DEVICE_ITEM_LED \
    IFX_PWM_DEVICE(CYBSP_PWM_LED_CTRL_config, CYBSP_PWM_LED_CTRL_hal_config, "pwm_led")
#else
#define IFX_PWM_DEVICE_ITEM_LED
#endif

#define IFX_PWM_DEVICE_LIST \
    IFX_PWM_DEVICE_ITEM_LED

#ifdef __cplusplus
}
#endif

#endif /* __PWM_CONFIG_H__ */
