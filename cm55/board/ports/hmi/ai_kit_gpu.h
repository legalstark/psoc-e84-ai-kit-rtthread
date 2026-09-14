#ifndef AI_KIT_GPU_H
#define AI_KIT_GPU_H

/**
 * @file    ai_kit_gpu.h
 * @brief   AI Kit GPU（VG-Lite 2D 加速器）初始化接口
 *
 * @details 该头文件声明 CM55 上 VG-Lite GPU 的初始化接口。
 *          实现位于 ai_kit_gpu.c：注册 GPU 中断、调用 vg_lite_init
 *          初始化加速器，供 LVGL GPU 绘图插件使用。
 */

/**
 * @brief   GPU 初始化结果
 */
typedef enum
{
    AI_KIT_GPU_OK = 0,                 /**< @brief 成功 */
    AI_KIT_GPU_ERROR_IRQ_INIT = 1,     /**< @brief GPU 中断注册失败 */
    AI_KIT_GPU_ERROR_VG_LITE_INIT = 2  /**< @brief vg_lite_init 失败 */
} ai_kit_gpu_result_t;

/**
 * @brief   初始化 VG-Lite GPU（注册中断 + vg_lite_init）
 *
 * @return  AI_KIT_GPU_OK 或具体错误码
 */
ai_kit_gpu_result_t ai_kit_gpu_init(void);

#endif
