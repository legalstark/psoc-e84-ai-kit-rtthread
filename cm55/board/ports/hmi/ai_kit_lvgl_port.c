#include <rtthread.h>

#include "lvgl.h"
#include "ai_kit_display.h"
#include "ai_kit_touch.h"

static void ai_kit_lvgl_flush(lv_display_t *display,
                              const lv_area_t *area,
                              uint8_t *pixels)
{
    RT_UNUSED(area);
    ai_kit_display_present((uint16_t *)pixels);
    lv_display_flush_ready(display);
}

static void ai_kit_lvgl_touch_read(lv_indev_t *indev,
                                   lv_indev_data_t *data)
{
    static lv_point_t last_point;
    ai_kit_touch_sample_t sample;
    int32_t x;
    int32_t y;

    RT_UNUSED(indev);

    ai_kit_touch_get_sample(&sample);
    data->state = sample.pressed ? LV_INDEV_STATE_PRESSED :
                                   LV_INDEV_STATE_RELEASED;

    if (sample.pressed)
    {
        x = ((int32_t)AI_KIT_DISPLAY_WIDTH - 1) - (int32_t)sample.x;
        y = ((int32_t)AI_KIT_DISPLAY_HEIGHT - 1) - (int32_t)sample.y;

        if (x < 0)
        {
            x = 0;
        }
        else if (x >= (int32_t)AI_KIT_DISPLAY_WIDTH)
        {
            x = (int32_t)AI_KIT_DISPLAY_WIDTH - 1;
        }

        if (y < 0)
        {
            y = 0;
        }
        else if (y >= (int32_t)AI_KIT_DISPLAY_HEIGHT)
        {
            y = (int32_t)AI_KIT_DISPLAY_HEIGHT - 1;
        }

        last_point.x = (lv_coord_t)x;
        last_point.y = (lv_coord_t)y;
    }

    data->point = last_point;
}

void lv_port_disp_init(void)
{
    lv_display_t *display;

    display = lv_display_create(AI_KIT_DISPLAY_STRIDE_PIXELS,
                                AI_KIT_DISPLAY_HEIGHT);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(display, ai_kit_lvgl_flush);
    lv_display_set_buffers(display,
                           ai_kit_display_framebuffer(0),
                           ai_kit_display_framebuffer(1),
                           ai_kit_display_framebuffer_size(),
                           LV_DISPLAY_RENDER_MODE_FULL);
}

void lv_port_indev_init(void)
{
    lv_indev_t *touch;

    touch = lv_indev_create();
    lv_indev_set_type(touch, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(touch, ai_kit_lvgl_touch_read);
}
