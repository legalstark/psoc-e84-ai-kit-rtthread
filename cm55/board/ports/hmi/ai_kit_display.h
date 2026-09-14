#ifndef AI_KIT_DISPLAY_H
#define AI_KIT_DISPLAY_H

#include <stdint.h>

/**
 * @file    ai_kit_display.h
 * @brief   AI Kit 显示子系统对外接口
 *
 * @details 该头文件声明 CM55 显示子系统初始化与帧缓冲管理接口：
 *          - 800×480 RGB565 面板
 *          - 双缓冲 + VSync 同步
 *          - GPU 中断清屏
 *          实现位于 ai_kit_display.c，依赖 HMI I2C 总线初始化显示桥
 *          （DSI/SPI 桥），并维护内部 framebuffer 数组供 LVGL 直接绘制。
 */

/** @brief 帧缓冲行跨度（pixels，>= WIDTH，对齐 GPU/DC 要求） */
#define AI_KIT_DISPLAY_STRIDE_PIXELS (832U)
/** @brief 显示可见宽度（pixels） */
#define AI_KIT_DISPLAY_WIDTH         (800U)
/** @brief 显示可见高度（pixels） */
#define AI_KIT_DISPLAY_HEIGHT        (480U)

/**
 * @brief   显示初始化结果
 */
typedef enum
{
    AI_KIT_DISPLAY_OK = 0,                           /**< @brief 成功 */
    AI_KIT_DISPLAY_ERROR_GFX_INIT = 1,               /**< @brief GPU/2D 加速器初始化失败 */
    AI_KIT_DISPLAY_ERROR_I2C_INIT = 2,               /**< @brief 显示桥 I2C 初始化失败 */
    AI_KIT_DISPLAY_ERROR_PANEL_INIT = 3,             /**< @brief 面板时序初始化失败 */
    AI_KIT_DISPLAY_ERROR_FRAMEBUFFER_COMMIT = 4,    /**< @brief 帧缓冲 commit 失败 */
    AI_KIT_DISPLAY_ERROR_DC_IRQ_INIT = 5,            /**< @brief DisplayController 中断注册失败 */
    AI_KIT_DISPLAY_ERROR_VSYNC_TIMEOUT = 6           /**< @brief 等待 VSync 超时 */
} ai_kit_display_result_t;

/**
 * @brief   初始化显示子系统
 *
 * @details 完成：GPU/2D 加速器、显示桥 I2C、面板时序、DisplayController
 *          与 VSync 中断、初始化两个 framebuffer 并填入 rgb565_color。
 *          成功后即可调用 ai_kit_display_framebuffer / present。
 *
 * @param   rgb565_color  初始填充色（RGB565）
 *
 * @return  AI_KIT_DISPLAY_OK 或具体错误码
 */
ai_kit_display_result_t ai_kit_display_init(uint16_t rgb565_color);

/**
 * @brief   获取指定索引的 framebuffer 起始地址
 *
 * @param   index  framebuffer 索引（0 或 1，双缓冲）
 *
 * @return  指向 STRIDE*HEIGHT 个 RGB565 像素的缓冲
 */
uint16_t *ai_kit_display_framebuffer(uint32_t index);

/**
 * @brief   返回单个 framebuffer 的字节大小
 *
 * @return  STRIDE_PIXELS * HEIGHT * sizeof(uint16_t)
 */
uint32_t ai_kit_display_framebuffer_size(void);

/**
 * @brief   把指定 framebuffer 提交给 DisplayController 输出
 *
 * @details 内部会等待 VSync 后切换 DC 的 framebuffer 起址，保证
 *          tear-free 切换。
 *
 * @param   framebuffer  要显示的 framebuffer 指针（必须来自
 *                       ai_kit_display_framebuffer）
 *
 * @return  AI_KIT_DISPLAY_OK 或 VSYNC_TIMEOUT / FRAMEBUFFER_COMMIT
 */
ai_kit_display_result_t ai_kit_display_present(uint16_t *framebuffer);

/**
 * @brief   查询上次操作的错误码（不清除）
 *
 * @return  最近一次错误；无错误返回 AI_KIT_DISPLAY_OK
 */
ai_kit_display_result_t ai_kit_display_last_error(void);

/**
 * @brief   获取 DC 当前正在扫描的 framebuffer 地址与扫描行
 *
 * @details 用于与 VSync 同步的 UI 刷新逻辑判断当前帧进度。
 *
 * @param   framebuffer_address  输出参数，当前 DC 扫描的 framebuffer 物理地址
 * @param   scan_position        输出参数，当前扫描行号
 */
void ai_kit_display_get_scan_status(uint32_t *framebuffer_address,
                                    uint32_t *scan_position);

/**
 * @brief   清除 GPU 中断标志（在 GPU 中断 ISR 末尾调用）
 */
void ai_kit_display_clear_gpu_interrupt(void);

#endif
