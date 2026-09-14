#include <stdbool.h>

#include <rtthread.h>

#include "mtb_ctp_ft5406.h"
#include "ai_kit_hmi_bus.h"
#include "ai_kit_touch.h"

#define AI_KIT_TOUCH_POLL_MS       (33U)
#define AI_KIT_TOUCH_THREAD_STACK  (2048U)
#define AI_KIT_TOUCH_THREAD_PRIO   (20U)
#define AI_KIT_TOUCH_THREAD_TICK   (5U)

static mtb_ctp_ft5406_config_t g_ft5406_config;
static ai_kit_touch_sample_t g_touch_sample;
static rt_thread_t g_touch_thread;

static void ai_kit_touch_update(const ai_kit_touch_sample_t *sample)
{
    rt_base_t level = rt_hw_interrupt_disable();

    g_touch_sample = *sample;
    rt_hw_interrupt_enable(level);
}

static void ai_kit_touch_poll(void *parameter)
{
    ai_kit_touch_sample_t sample = {0};
    bool was_pressed = false;

    RT_UNUSED(parameter);

    while (1)
    {
        mtb_ctp_touch_event_t event = MTB_CTP_TOUCH_RESERVED;
        int x = 0;
        int y = 0;
        cy_en_scb_i2c_status_t status;
        bool pressed;

        status = mtb_ctp_ft5406_get_single_touch(&event, &x, &y);
        sample.error = (uint32_t)status;

        if (status == CY_SCB_I2C_SUCCESS)
        {
            pressed = (event == MTB_CTP_TOUCH_DOWN) ||
                      (event == MTB_CTP_TOUCH_CONTACT);
            sample.pressed = pressed ? 1U : 0U;

            if (pressed)
            {
                sample.event = (uint32_t)event;
                sample.x = (uint32_t)x;
                sample.y = (uint32_t)y;

                if (!was_pressed)
                {
                    sample.sequence++;
                }
            }

            was_pressed = pressed;
        }

        ai_kit_touch_update(&sample);
        rt_thread_mdelay(AI_KIT_TOUCH_POLL_MS);
    }
}

ai_kit_touch_result_t ai_kit_touch_init(void)
{
    cy_en_scb_i2c_status_t status;

    if (ai_kit_hmi_bus_init() != AI_KIT_HMI_BUS_OK)
    {
        return AI_KIT_TOUCH_ERROR_HMI_BUS;
    }

    g_ft5406_config.i2c_base = ai_kit_hmi_bus_base();
    g_ft5406_config.i2c_context = ai_kit_hmi_bus_context();

    status = mtb_ctp_ft5406_init(&g_ft5406_config);
    if (status != CY_SCB_I2C_SUCCESS)
    {
        g_touch_sample.error = (uint32_t)status;
        return AI_KIT_TOUCH_ERROR_CONTROLLER_INIT;
    }

    g_touch_thread = rt_thread_create("ft5406",
                                      ai_kit_touch_poll,
                                      RT_NULL,
                                      AI_KIT_TOUCH_THREAD_STACK,
                                      AI_KIT_TOUCH_THREAD_PRIO,
                                      AI_KIT_TOUCH_THREAD_TICK);
    if (g_touch_thread == RT_NULL)
    {
        return AI_KIT_TOUCH_ERROR_THREAD_CREATE;
    }

    rt_thread_startup(g_touch_thread);
    return AI_KIT_TOUCH_OK;
}

void ai_kit_touch_get_sample(ai_kit_touch_sample_t *sample)
{
    rt_base_t level;

    RT_ASSERT(sample != RT_NULL);

    level = rt_hw_interrupt_disable();
    *sample = g_touch_sample;
    rt_hw_interrupt_enable(level);
}
