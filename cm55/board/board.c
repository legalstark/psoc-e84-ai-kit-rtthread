/*
 * Copyright (c) 2006-2023, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2022-06-29     Rbb666       first version
 * 2025-08-20     Hydevcode
 */

/**
 * @file    board.c
 * @brief   PSoC Edge E84 AI Kit CM55 板级初始化与启动入口
 *
 * @details 该文件提供 CM55 侧两个最基础函数：
 *          - cy_bsp_all_init ：封装 cybsp_init，初始化所有片上外设
 *            与板级 GPIO/PWM/IPC 等 BSP 资源
 *          - _start          ：C 运行时启动入口，调用 RT-Thread 的
 *            entry() 后进入死循环（理论上不会返回）
 */

#include "board.h"

/**
 * @brief   板级总初始化
 *
 * @details 调用 Infineon cybsp_init 完成所有片上外设与板级资源初始化
 *          （时钟、GPIO、IPC、外设等）。失败时调用 CY_ASSERT(0) 停机，
 *          避免在不稳定的硬件状态下继续启动。
 */
void cy_bsp_all_init(void)
{
    cy_rslt_t result;

    /* Initialize the device and board peripherals */
    result = cybsp_init();

    /* Board init failed. Stop program execution */
    if (result != CY_RSLT_SUCCESS)
    {
        CY_ASSERT(0);
    }
}

/**
 * @brief   C 运行时启动入口
 *
 * @details 由启动汇编调用，仅做一件事：跳转到 RT-Thread 的 entry()
 *          开始系统初始化。entry 不应返回；万一返回则进入死循环并
 *          通过 __builtin_unreachable 提示编译器该路径不可达。
 */
void _start(void)
{
    extern int entry(void);

    entry();

    while (1)
    {
    }

    __builtin_unreachable();
}
