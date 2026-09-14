/**
 * @file    main.c
 * @brief   PSoC Edge E84 AI Kit CM55 主入口与整机状态合并器
 *
 * @details CM55 在本套双核架构中承担：
 *          - HMI Launcher UI（LVGL + GPU + 触摸）
 *          - Wi-Fi 子系统
 *          - NPU/AI 推理（Ethos-U55 + mtb_ml）
 *          - IPC 服务端响应 M33 的请求
 *          - 整机合并发起者：把 CM33 / CM55 / NPU 三方基准结果合并
 *            到 g_ai_kit_cm55_status 状态区，供 CM33 msh 读取与导出
 *
 *          启动流程：
 *          1. ai_kit_benchmark_init / ai_kit_ipc_service_init 初始化目录
 *             与 IPC 服务线程
 *          2. 初始化 g_ai_kit_cm55_status（magic / ABI / state=RUNNING）
 *          3. PSRAM 自检：失败仍继续启动但状态区记录错误
 *          4. PSRAM 自检通过后初始化一次 USB Host/UVC 摄像头服务
 *          5. ai_kit_display_init / ai_kit_gpu_init / ai_kit_touch_init
 *             三件套；成功则启动 lvgl_thread_init
 *          6. 进入主循环：每 1 s 刷新心跳、显示扫描位置、触摸采样、
 *             基准目录快照，并通过 DCache 清洁发布到共享 SRAM
 *
 * @note    摄像头缓冲区位于 CM55 私有 PSRAM，因此 PSRAM 自检失败时
 *          不启动 USB Host，避免 DMA 访问无效的外部存储器。
 */

#include <rtthread.h>
#include "cy_pdl.h"
#include "ai_kit_multicore.h"
#include "ai_kit_display.h"
#include "ai_kit_gpu.h"
#include "ai_kit_touch.h"
#include "ai_kit_benchmark.h"
#include "ai_kit_ipc_service.h"
#include "ai_kit_psram.h"
#ifdef BSP_USB_ROLE_HOST_UVC
#include "ai_kit_camera.h"
#endif

#ifdef BSP_USING_LVGL
extern int lvgl_thread_init(void);  /**< LVGL 主线程初始化入口，由 lvgl_port 模块提供 */
#endif

/**
 * @brief   CM55 对外共享状态区实例
 *
 * @details 放置在 .cm55_boot_status 段，链接到固定物理地址
 *          AI_KIT_CM55_STATUS_ADDRESS（0x262FC000），64 字节对齐
 *          保证 cache line 友好。CM33 通过 cm55_status 命令只读访问。
 */
__attribute__((section(".cm55_boot_status"), used, aligned(64)))
volatile ai_kit_cm55_status_t g_ai_kit_cm55_status;

/**
 * @brief   把状态区写回共享 SRAM 的缓存维护操作
 *
 * @details 调用 SCB_CleanDCache_by_Addr 把整个
 *          AI_KIT_CM55_STATUS_SIZE 字节的状态区从 DCache 写回 SRAM，
 *          确保 CM33 一侧可见最新内容。CM55 写状态区后必须调用本函数。
 */
static void cm55_status_publish(void)
{
    SCB_CleanDCache_by_Addr(&g_ai_kit_cm55_status,
                            (int32_t)AI_KIT_CM55_STATUS_SIZE);
}

/**
 * @brief   CM55 RT-Thread 主入口
 *
 * @details 完成子系统初始化、状态区首发布、LVGL 启动，并进入主循环
 *          周期更新心跳 / 显示 / 触摸 / 基准目录快照。
 *
 * @return  int 不会返回；while 循环保证主线程常驻。
 */
int main(void)
{
    ai_kit_display_result_t display_result;
    ai_kit_gpu_result_t gpu_result = AI_KIT_GPU_ERROR_VG_LITE_INIT;
    ai_kit_touch_result_t touch_result;
    ai_kit_touch_sample_t touch_sample;
    ai_kit_benchmark_catalog_t benchmark_catalog;
    const ai_kit_psram_region_t *psram_region;
    rt_err_t psram_result;
    uint32_t framebuffer_address;
    uint32_t scan_position;

    ai_kit_benchmark_init();
    ai_kit_ipc_service_init();
    rt_memset((void *)&g_ai_kit_cm55_status, 0,
              sizeof(g_ai_kit_cm55_status));
    g_ai_kit_cm55_status.magic = AI_KIT_CM55_STATUS_MAGIC;
    g_ai_kit_cm55_status.abi_version = AI_KIT_CM55_STATUS_ABI_VERSION;
    g_ai_kit_cm55_status.state = AI_KIT_CM55_STATE_RUNNING;
    g_ai_kit_cm55_status.heartbeat = 0;
    g_ai_kit_cm55_status.display_state = AI_KIT_DISPLAY_STATE_INITIALIZING;
    g_ai_kit_cm55_status.display_error = 0;
    g_ai_kit_cm55_status.display_fb_address = 0;
    g_ai_kit_cm55_status.display_scan_position = 0;
    g_ai_kit_cm55_status.touch_state = AI_KIT_TOUCH_STATE_INITIALIZING;
    g_ai_kit_cm55_status.touch_error = 0;
    g_ai_kit_cm55_status.touch_pressed = 0;
    g_ai_kit_cm55_status.touch_event = 0;
    g_ai_kit_cm55_status.touch_x = 0;
    g_ai_kit_cm55_status.touch_y = 0;
    g_ai_kit_cm55_status.touch_sequence = 0;
    g_ai_kit_cm55_status.gpu_state = AI_KIT_GPU_STATE_DISABLED;
    psram_region = ai_kit_psram_local_region();
    psram_result = ai_kit_psram_is_ready() ?
        ai_kit_psram_self_test_private() : -RT_ERROR;
    g_ai_kit_cm55_status.psram_state =
        (psram_result == RT_EOK) ?
        AI_KIT_PSRAM_STATE_READY : AI_KIT_PSRAM_STATE_ERROR;
    g_ai_kit_cm55_status.psram_error = (uint32_t)(int32_t)psram_result;
    g_ai_kit_cm55_status.psram_private_base = (uint32_t)psram_region->base;
    g_ai_kit_cm55_status.psram_private_size = (uint32_t)psram_region->size;
    ai_kit_benchmark_get_catalog(&benchmark_catalog);
    g_ai_kit_cm55_status.benchmarks = benchmark_catalog;
    cm55_status_publish();

#ifdef BSP_USB_ROLE_HOST_UVC
    if ((psram_result == RT_EOK) &&
        (ai_kit_camera_init() != RT_EOK))
    {
        rt_kprintf("[camera] host initialization failed\r\n");
    }
    else if (psram_result != RT_EOK)
    {
        rt_kprintf("[camera] disabled because PSRAM self-test failed\r\n");
    }
#endif

    display_result = ai_kit_display_init(0x07E0U);
    g_ai_kit_cm55_status.display_error = (uint32_t)display_result;
    g_ai_kit_cm55_status.display_state =
        (display_result == AI_KIT_DISPLAY_OK) ?
        AI_KIT_DISPLAY_STATE_READY : AI_KIT_DISPLAY_STATE_ERROR;

    if (display_result == AI_KIT_DISPLAY_OK)
    {
        g_ai_kit_cm55_status.gpu_state = AI_KIT_GPU_STATE_INITIALIZING;
        cm55_status_publish();
        gpu_result = ai_kit_gpu_init();
        if (gpu_result == AI_KIT_GPU_OK)
        {
            g_ai_kit_cm55_status.gpu_state = AI_KIT_GPU_STATE_READY;
        }
        else if (gpu_result == AI_KIT_GPU_ERROR_IRQ_INIT)
        {
            g_ai_kit_cm55_status.gpu_state = AI_KIT_GPU_STATE_IRQ_ERROR;
        }
        else
        {
            g_ai_kit_cm55_status.gpu_state = AI_KIT_GPU_STATE_VG_LITE_ERROR;
        }
    }

    touch_result = ai_kit_touch_init();
    g_ai_kit_cm55_status.touch_error = (uint32_t)touch_result;
    g_ai_kit_cm55_status.touch_state =
        (touch_result == AI_KIT_TOUCH_OK) ?
        AI_KIT_TOUCH_STATE_READY : AI_KIT_TOUCH_STATE_ERROR;
    cm55_status_publish();

#ifdef BSP_USING_LVGL
    if ((display_result == AI_KIT_DISPLAY_OK) &&
        (gpu_result == AI_KIT_GPU_OK) &&
        (touch_result == AI_KIT_TOUCH_OK))
    {
        lvgl_thread_init();
    }
#endif

    while (1)
    {
        g_ai_kit_cm55_status.heartbeat++;
        g_ai_kit_cm55_status.display_error =
            (uint32_t)ai_kit_display_last_error();
        ai_kit_display_get_scan_status(&framebuffer_address, &scan_position);
        g_ai_kit_cm55_status.display_fb_address = framebuffer_address;
        g_ai_kit_cm55_status.display_scan_position = scan_position;
        ai_kit_touch_get_sample(&touch_sample);
        g_ai_kit_cm55_status.touch_error = touch_sample.error;
        g_ai_kit_cm55_status.touch_pressed = touch_sample.pressed;
        g_ai_kit_cm55_status.touch_event = touch_sample.event;
        g_ai_kit_cm55_status.touch_x = touch_sample.x;
        g_ai_kit_cm55_status.touch_y = touch_sample.y;
        g_ai_kit_cm55_status.touch_sequence = touch_sample.sequence;
        ai_kit_benchmark_get_catalog(&benchmark_catalog);
        g_ai_kit_cm55_status.benchmarks = benchmark_catalog;
        cm55_status_publish();
        rt_thread_mdelay(1000);
    }
}
