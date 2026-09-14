/**
 * @file    board.c
 * @brief   AI Kit E84 板级初始化与启动桥接
 *
 * @details 该文件实现：
 *          1. cy_bsp_all_init()：调用 cybsp_init 完成 PSoC Edge E84
 *             BSP 初始化；若启用 SOC_Enable_CM55，则以 10 个等待周期
 *             使能 CM55 并跳转到 CY_CM55_APP_BOOT_ADDR。
 *          2. _start()：C 启动入口，调用 RT-Thread entry() 进入
 *             RT-Thread 调度器，理论上不会返回；返回则陷入死循环。
 *
 * @note    本文件仅添加注释，未改动任何可执行代码。
 */

/*
 * Copyright (c) 2006-2023, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "board.h"

/**
 * @brief   AI Kit E84 板级统一初始化入口
 *
 * @details 调用 cybsp_init 完成 GPIO / 时钟 / 外设等 BSP 配置，
 *          失败时触发 CY_ASSERT(0) 进入 fault。当构建配置定义
 *          SOC_Enable_CM55 时，紧接着用 Cy_SysEnableCM55 启动 CM55：
 *          - 参数 1：MXCM55 实例指针
 *          - 参数 2：CY_CM55_APP_BOOT_ADDR，CM55 应用镜像地址
 *          - 参数 3：等待 CM55 复位释放的周期数（10）
 *
 *          本函数在 RT-Thread 启动早期由 BSP 调用，无须应用层介入。
 */
void cy_bsp_all_init(void)
{
    cy_rslt_t result = cybsp_init();

    if (result != CY_RSLT_SUCCESS)
    {
        CY_ASSERT(0);
    }

#ifdef SOC_Enable_CM55
    Cy_SysEnableCM55(MXCM55, CY_CM55_APP_BOOT_ADDR, 10U);
#endif
}

/**
 * @brief   C 运行时启动入口
 *
 * @details 由启动汇编调用，进入 RT-Thread 的 entry()；entry 内部
 *          完成 rt_hw_init / rt_system_init / main 调度等。entry
 *          正常情况下不会返回；若返回则进入死循环防止跑飞。
 */
void _start(void)
{
    extern int entry(void);

    entry();

    while (1)
    {
    }
}
