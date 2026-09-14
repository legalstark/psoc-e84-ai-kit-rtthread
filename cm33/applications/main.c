/**
 * @file    main.c
 * @brief   PSoC Edge E84 AI Kit CM33 Non-Secure 应用入口
 *
 * @details 该文件实现 RT-Thread 在 Cortex-M33 Non-Secure 域上的主入口。
 *          CM33 在本套双核架构中作为辅助核心：负责外设诊断、IPC 客户端、
 *          M33 侧性能基准测试；CM55 承担 HMI Launcher UI、Wi-Fi、NPU/AI、
 *          IPC 服务响应以及整机合并发起者职责。
 *
 *          本入口仅完成启动横幅输出并进入空闲循环，其余功能通过 msh
 *          命令（ai_devices / ipc_ping / m33_coremark 等）按需调用。
 *
 * @note    本文件仅添加注释，未改动任何可执行代码。
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <board.h>

/**
 * @brief   RT-Thread 主入口函数
 *
 * @details 输出 CM33 Non-Secure 启动横幅后进入 1 秒周期空闲循环。
 *          RT-Thread 调度器在 main 返回前已启动，主线程随后由 shell、
 *          IPC 客户端等用户线程接管实际工作。
 *
 * @return  int 不会返回；while 循环保证主线程常驻。
 */
int main(void)
{
    rt_kprintf("Hello RT-Thread on PSoC Edge E84 AI Kit\r\n");
    rt_kprintf("CM33 Non-Secure is running\r\n");
    while (1)
    {
        rt_thread_mdelay(1000);
    }
    return 0;
}
