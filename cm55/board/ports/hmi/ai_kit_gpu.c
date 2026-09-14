#include <rtthread.h>

#include "cy_graphics.h"
#include "cy_pdl.h"
#include "vg_lite.h"
#include "vg_lite_platform.h"

#include "ai_kit_display.h"
#include "ai_kit_gpu.h"

#define AI_KIT_GPU_IRQ_PRIORITY             (3U)
#define AI_KIT_GPU_BUFFER_COUNT             (2U)
#define AI_KIT_GPU_COMMAND_BUFFER_SIZE      (64U * 1024U)
#define AI_KIT_GPU_TESSELLATION_BUFFER_SIZE \
    (AI_KIT_DISPLAY_HEIGHT * 128U)
#define AI_KIT_GPU_HEAP_SIZE \
    ((AI_KIT_GPU_COMMAND_BUFFER_SIZE * AI_KIT_GPU_BUFFER_COUNT) + \
     (AI_KIT_GPU_TESSELLATION_BUFFER_SIZE * AI_KIT_GPU_BUFFER_COUNT))

__attribute__((section(".cy_gpu_buf"), used, aligned(64)))
static uint8_t g_vg_lite_heap[AI_KIT_GPU_HEAP_SIZE];

static const cy_stc_sysint_t g_gpu_irq_config =
{
    .intrSrc = gfxss_interrupt_gpu_IRQn,
    .intrPriority = AI_KIT_GPU_IRQ_PRIORITY,
};

static void ai_kit_gpu_irq_handler(void)
{
    rt_interrupt_enter();
    ai_kit_display_clear_gpu_interrupt();
    vg_lite_IRQHandler();
    rt_interrupt_leave();
}

ai_kit_gpu_result_t ai_kit_gpu_init(void)
{
    vg_module_parameters_t parameters = {0};
    vg_lite_error_t vg_lite_result;

    if (Cy_SysInt_Init(&g_gpu_irq_config,
                       ai_kit_gpu_irq_handler) != CY_SYSINT_SUCCESS)
    {
        return AI_KIT_GPU_ERROR_IRQ_INIT;
    }

    Cy_GFXSS_Enable_GPU_Interrupt(GFXSS);
    NVIC_EnableIRQ(gfxss_interrupt_gpu_IRQn);

    parameters.register_mem_base = (uint32_t)GFXSS_GFXSS_GPU_GCNANO;
    parameters.gpu_mem_base[0] = 0U;
    parameters.contiguous_mem_base[0] = g_vg_lite_heap;
    parameters.contiguous_mem_size[0] = sizeof(g_vg_lite_heap);
    vg_lite_init_mem(&parameters);

    vg_lite_result = vg_lite_init(AI_KIT_DISPLAY_STRIDE_PIXELS / 4U,
                                  AI_KIT_DISPLAY_HEIGHT / 4U);
    if (vg_lite_result != VG_LITE_SUCCESS)
    {
        NVIC_DisableIRQ(gfxss_interrupt_gpu_IRQn);
        return AI_KIT_GPU_ERROR_VG_LITE_INIT;
    }

    return AI_KIT_GPU_OK;
}
