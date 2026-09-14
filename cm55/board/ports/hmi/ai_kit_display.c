#include <rtthread.h>
#include "cy_graphics.h"
#include "cy_pdl.h"
#include "mtb_disp_dsi_waveshare_4p3.h"

#include "ai_kit_display.h"
#include "ai_kit_hmi_bus.h"

#define AI_KIT_GFX_CLOCK_HZ (399999999U)
#define AI_KIT_DISPLAY_BUFFER_COUNT (2U)
#define AI_KIT_DISPLAY_VSYNC_TIMEOUT_MS (100U)

__attribute__((section(".cy_gpu_buf"), used, aligned(64)))
static uint16_t g_frame_buffers[AI_KIT_DISPLAY_BUFFER_COUNT]
                               [AI_KIT_DISPLAY_STRIDE_PIXELS *
                                AI_KIT_DISPLAY_HEIGHT];

static cy_stc_gfx_context_t g_gfx_context;
static struct rt_semaphore g_vsync_semaphore;
static volatile ai_kit_display_result_t g_last_error = AI_KIT_DISPLAY_OK;

static const cy_stc_sysint_t g_dc_irq_config =
{
    .intrSrc = gfxss_interrupt_dc_IRQn,
    .intrPriority = 3U,
};

static cy_stc_gfx_layer_config_t g_graphics_layer =
{
    .layer_type = GFX_LAYER_GRAPHICS,
    .buffer_address = (gctADDRESS *)g_frame_buffers[0],
    .uv_buffer_address = (gctADDRESS *)g_frame_buffers[0],
    .input_format_type = vivRGB565,
    .tiling_type = vivLINEAR,
    .pos_x = 0,
    .pos_y = 0,
    .width = AI_KIT_DISPLAY_STRIDE_PIXELS,
    .height = AI_KIT_DISPLAY_HEIGHT,
    .zorder = 0,
    .layer_enable = true,
    .visibility = true,
};

static cy_stc_gfx_dc_config_t g_dc_config =
{
    .gfx_layer_config = &g_graphics_layer,
    .ovl0_layer_config = NULL,
    .ovl1_layer_config = NULL,
    .display_type = GFX_DISP_TYPE_DSI_DPI,
    .display_format = vivD24,
    .display_size = vivDISPLAY_CUSTOMIZED,
    .display_width = AI_KIT_DISPLAY_STRIDE_PIXELS,
    .display_height = AI_KIT_DISPLAY_HEIGHT,
};

static cy_stc_gfx_gpu_cfg_t g_gpu_config =
{
    .enable = true,
};

static cy_stc_gfx_config_t g_gfx_config =
{
    .dc_cfg = &g_dc_config,
    .gpu_cfg = &g_gpu_config,
    .mipi_dsi_cfg = &mtb_disp_waveshare_4p3_dsi_config,
    .display_update_type = GFX_DOUBLE_BUFFER,
    .clockHz = AI_KIT_GFX_CLOCK_HZ,
};

static void ai_kit_display_fill(uint16_t color)
{
    uint32_t buffer;
    uint32_t index;

    for (buffer = 0; buffer < AI_KIT_DISPLAY_BUFFER_COUNT; buffer++)
    {
        for (index = 0; index < (AI_KIT_DISPLAY_STRIDE_PIXELS *
                                AI_KIT_DISPLAY_HEIGHT); index++)
        {
            g_frame_buffers[buffer][index] = color;
        }
    }
}

static void ai_kit_display_dc_irq_handler(void)
{
    rt_interrupt_enter();
    Cy_GFXSS_Clear_DC_Interrupt(GFXSS, &g_gfx_context);
    rt_sem_release(&g_vsync_semaphore);
    rt_interrupt_leave();
}

ai_kit_display_result_t ai_kit_display_init(uint16_t rgb565_color)
{
    ai_kit_hmi_bus_result_t bus_result;

    g_last_error = AI_KIT_DISPLAY_OK;
    ai_kit_display_fill(rgb565_color);

    Cy_SysClk_PeriGroupSlaveInit(CY_MMIO_GFXSS_GPU_PERI_NR,
                                 CY_MMIO_GFXSS_GPU_GROUP_NR,
                                 CY_MMIO_GFXSS_GPU_SLAVE_NR,
                                 CY_MMIO_GFXSS_GPU_CLK_HF_NR);
    Cy_SysClk_PeriGroupSlaveInit(CY_MMIO_GFXSS_DC_PERI_NR,
                                 CY_MMIO_GFXSS_DC_GROUP_NR,
                                 CY_MMIO_GFXSS_DC_SLAVE_NR,
                                 CY_MMIO_GFXSS_DC_CLK_HF_NR);
    Cy_SysClk_PeriGroupSlaveInit(CY_MMIO_GFXSS_MIPIDSI_PERI_NR,
                                 CY_MMIO_GFXSS_MIPIDSI_GROUP_NR,
                                 CY_MMIO_GFXSS_MIPIDSI_SLAVE_NR,
                                 CY_MMIO_GFXSS_MIPIDSI_CLK_HF_NR);

    if (Cy_GFXSS_Init(GFXSS, &g_gfx_config, &g_gfx_context) != CY_GFX_SUCCESS)
    {
        g_last_error = AI_KIT_DISPLAY_ERROR_GFX_INIT;
        return AI_KIT_DISPLAY_ERROR_GFX_INIT;
    }

    rt_sem_init(&g_vsync_semaphore, "gfx_vsync", 0, RT_IPC_FLAG_PRIO);
    if (Cy_SysInt_Init(&g_dc_irq_config,
                       ai_kit_display_dc_irq_handler) != CY_SYSINT_SUCCESS)
    {
        g_last_error = AI_KIT_DISPLAY_ERROR_DC_IRQ_INIT;
        return AI_KIT_DISPLAY_ERROR_DC_IRQ_INIT;
    }

    Cy_GFXSS_Clear_DC_Interrupt(GFXSS, &g_gfx_context);
    NVIC_ClearPendingIRQ(gfxss_interrupt_dc_IRQn);
    NVIC_EnableIRQ(gfxss_interrupt_dc_IRQn);

    if (Cy_GFXSS_Set_FrameBuffer(GFXSS,
                                 (uint32_t *)g_frame_buffers[0],
                                 &g_gfx_context) != CY_GFX_SUCCESS)
    {
        g_last_error = AI_KIT_DISPLAY_ERROR_FRAMEBUFFER_COMMIT;
        return AI_KIT_DISPLAY_ERROR_FRAMEBUFFER_COMMIT;
    }

    bus_result = ai_kit_hmi_bus_init();
    if (bus_result != AI_KIT_HMI_BUS_OK)
    {
        g_last_error = AI_KIT_DISPLAY_ERROR_I2C_INIT;
        return AI_KIT_DISPLAY_ERROR_I2C_INIT;
    }

    rt_thread_mdelay(500);

    if (mtb_disp_waveshare_4p3_init(ai_kit_hmi_bus_base(),
                                    ai_kit_hmi_bus_context()) !=
        CY_SCB_I2C_SUCCESS)
    {
        g_last_error = AI_KIT_DISPLAY_ERROR_PANEL_INIT;
        return AI_KIT_DISPLAY_ERROR_PANEL_INIT;
    }

    return AI_KIT_DISPLAY_OK;
}

uint16_t *ai_kit_display_framebuffer(uint32_t index)
{
    RT_ASSERT(index < AI_KIT_DISPLAY_BUFFER_COUNT);
    return g_frame_buffers[index];
}

uint32_t ai_kit_display_framebuffer_size(void)
{
    return sizeof(g_frame_buffers[0]);
}

ai_kit_display_result_t ai_kit_display_present(uint16_t *framebuffer)
{
    while (rt_sem_trytake(&g_vsync_semaphore) == RT_EOK)
    {
    }

    if (Cy_GFXSS_Set_FrameBuffer(GFXSS,
                                 (uint32_t *)framebuffer,
                                 &g_gfx_context) != CY_GFX_SUCCESS)
    {
        g_last_error = AI_KIT_DISPLAY_ERROR_FRAMEBUFFER_COMMIT;
        return g_last_error;
    }

    if (rt_sem_take(&g_vsync_semaphore,
                    rt_tick_from_millisecond(
                        AI_KIT_DISPLAY_VSYNC_TIMEOUT_MS)) != RT_EOK)
    {
        g_last_error = AI_KIT_DISPLAY_ERROR_VSYNC_TIMEOUT;
        return g_last_error;
    }

    g_last_error = AI_KIT_DISPLAY_OK;
    return AI_KIT_DISPLAY_OK;
}

ai_kit_display_result_t ai_kit_display_last_error(void)
{
    return g_last_error;
}

void ai_kit_display_get_scan_status(uint32_t *framebuffer_address,
                                    uint32_t *scan_position)
{
    if (framebuffer_address != NULL)
    {
        *framebuffer_address =
            GFXSS->GFXSS_DC.DCNANO.GCREGFRAMEBUFFERADDRESS;
    }

    if (scan_position != NULL)
    {
        *scan_position =
            GFXSS->GFXSS_DC.DCNANO.GCREGDISPLAYCURRENTLOCATION;
    }
}

void ai_kit_display_clear_gpu_interrupt(void)
{
    Cy_GFXSS_Clear_GPU_Interrupt(GFXSS, &g_gfx_context);
}
