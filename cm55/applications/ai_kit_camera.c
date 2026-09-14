/**
 * @file ai_kit_camera.c
 * @brief AI Kit J2 USB UVC camera service for the CM55 RT-Thread image.
 *
 * Infineon emUSB-Host owns enumeration and isochronous UVC transfers. This
 * module owns the page lifecycle, YUYV conversion and
 * a three-buffer handoff to the LVGL thread.  Camera pixels live in the CM55
 * private PSRAM linker section and are never added to the RT-Thread heap.
 */

#include "ai_kit_camera.h"

#include <rtdevice.h>
#include <string.h>

#include "cy_device.h"
#include "cy_pdl.h"
#include "ai_kit_camera_transport.h"

#define CAMERA_RGB_BUFFER_COUNT AI_KIT_CAMERA_RGB_BUFFER_COUNT
#define CAMERA_DISPLAY_PERIOD_MS 200U
#define CAMERA_THREAD_STACK_SIZE 8192U
#define CAMERA_THREAD_PRIORITY 21U
#define CAMERA_BUFFER_SECTION __attribute__((section(".ai_kit_camera_psram"), aligned(32), used))
#define CAMERA_FAST_CODE __attribute__((section(".cy_itcm")))
#define CAMERA_SOCMEM_DATA __attribute__((section(".cy_socmem_data"), aligned(32)))

enum camera_rgb_state
{
    CAMERA_RGB_FREE = 0,
    CAMERA_RGB_WRITING,
    CAMERA_RGB_READY,
    CAMERA_RGB_HELD
};

static CAMERA_BUFFER_SECTION uint16_t
    s_rgb_buffers[CAMERA_RGB_BUFFER_COUNT][AI_KIT_CAMERA_WIDTH * AI_KIT_CAMERA_HEIGHT];

static volatile rt_bool_t s_initialized;
static volatile rt_bool_t s_requested;
static volatile rt_bool_t s_paused;
static volatile rt_bool_t s_stream_active;
static ai_kit_camera_status_t s_status;
static uint8_t s_rgb_state[CAMERA_RGB_BUFFER_COUNT];
static uint32_t s_rgb_generation[CAMERA_RGB_BUFFER_COUNT];
static uint32_t s_next_generation;
static rt_tick_t s_last_convert_tick;
static rt_tick_t s_capture_window_tick;
static rt_tick_t s_display_window_tick;
static uint32_t s_capture_window_count;
static uint32_t s_display_window_count;
static uint32_t s_processing_dropped;

static CAMERA_SOCMEM_DATA int32_t s_y_lut[256];
static CAMERA_SOCMEM_DATA int32_t s_u_to_b_lut[256];
static CAMERA_SOCMEM_DATA int32_t s_u_to_g_lut[256];
static CAMERA_SOCMEM_DATA int32_t s_v_to_r_lut[256];
static CAMERA_SOCMEM_DATA int32_t s_v_to_g_lut[256];

static rt_base_t camera_lock(void)
{
    return rt_hw_interrupt_disable();
}

static void camera_unlock(rt_base_t level)
{
    rt_hw_interrupt_enable(level);
}

static int camera_clamp8(int value)
{
    if (value < 0)
    {
        return 0;
    }
    if (value > 255)
    {
        return 255;
    }
    return value;
}

static uint16_t camera_yuv_to_rgb565(uint8_t y,
                                     int32_t u_to_b,
                                     int32_t u_to_g,
                                     int32_t v_to_r,
                                     int32_t v_to_g)
{
    int32_t y_term = s_y_lut[y];
    int r = camera_clamp8((int)((y_term + v_to_r + 128) >> 8));
    int g = camera_clamp8((int)((y_term + u_to_g + v_to_g + 128) >> 8));
    int b = camera_clamp8((int)((y_term + u_to_b + 128) >> 8));

    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static CAMERA_FAST_CODE void camera_convert_yuyv(const uint8_t *source,
                                                  uint16_t *destination)
{
    uint32_t pair;

    for (pair = 0U;
         pair < (AI_KIT_CAMERA_WIDTH * AI_KIT_CAMERA_HEIGHT / 2U);
         pair++)
    {
        uint8_t y0 = source[0];
        uint8_t u = source[1];
        uint8_t y1 = source[2];
        uint8_t v = source[3];
        int32_t u_to_b = s_u_to_b_lut[u];
        int32_t u_to_g = s_u_to_g_lut[u];
        int32_t v_to_r = s_v_to_r_lut[v];
        int32_t v_to_g = s_v_to_g_lut[v];

        destination[0] = camera_yuv_to_rgb565(y0, u_to_b, u_to_g,
                                               v_to_r, v_to_g);
        destination[1] = camera_yuv_to_rgb565(y1, u_to_b, u_to_g,
                                               v_to_r, v_to_g);
        source += 4;
        destination += 2;
    }
}

static void camera_prepare_luts(void)
{
    uint32_t index;

    for (index = 0U; index < 256U; index++)
    {
        int32_t y = (int32_t)index - 16;
        int32_t uv = (int32_t)index - 128;

        if (y < 0)
        {
            y = 0;
        }
        s_y_lut[index] = 298 * y;
        s_u_to_b_lut[index] = 516 * uv;
        s_u_to_g_lut[index] = -100 * uv;
        s_v_to_r_lut[index] = 409 * uv;
        s_v_to_g_lut[index] = -208 * uv;
    }
}

static void camera_update_rate(rt_tick_t now,
                               rt_tick_t *window_tick,
                               uint32_t *window_count,
                               uint32_t *rate)
{
    rt_tick_t elapsed;

    (*window_count)++;
    if (*window_tick == 0U)
    {
        *window_tick = now;
        return;
    }
    elapsed = now - *window_tick;
    if (elapsed >= RT_TICK_PER_SECOND)
    {
        *rate = (*window_count * RT_TICK_PER_SECOND) / elapsed;
        *window_count = 0U;
        *window_tick = now;
    }
}

static int camera_claim_rgb_buffer(void)
{
    rt_base_t level = camera_lock();
    uint32_t index;

    for (index = 0U; index < CAMERA_RGB_BUFFER_COUNT; index++)
    {
        if (s_rgb_state[index] == CAMERA_RGB_FREE)
        {
            s_rgb_state[index] = CAMERA_RGB_WRITING;
            camera_unlock(level);
            return (int)index;
        }
    }
    camera_unlock(level);
    return -1;
}

static void camera_publish_rgb_buffer(uint32_t index)
{
    rt_base_t level = camera_lock();
    uint32_t old_index;

    for (old_index = 0U; old_index < CAMERA_RGB_BUFFER_COUNT; old_index++)
    {
        if (s_rgb_state[old_index] == CAMERA_RGB_READY)
        {
            s_rgb_state[old_index] = CAMERA_RGB_FREE;
            s_status.skipped_frames++;
        }
    }
    s_rgb_generation[index] = ++s_next_generation;
    s_rgb_state[index] = CAMERA_RGB_READY;
    camera_unlock(level);
}

static void camera_process_frame(const ai_kit_camera_raw_frame_t *frame)
{
    rt_tick_t now = rt_tick_get();
    rt_tick_t period = rt_tick_from_millisecond(CAMERA_DISPLAY_PERIOD_MS);
    int rgb_index;

    camera_update_rate(now, &s_capture_window_tick, &s_capture_window_count,
                       &s_status.capture_fps);

    if (frame->size != AI_KIT_CAMERA_FRAME_BYTES)
    {
        s_processing_dropped++;
        s_status.last_error = -RT_EINVAL;
        return;
    }

    if ((s_last_convert_tick != 0U) &&
        ((now - s_last_convert_tick) < period))
    {
        s_status.skipped_frames++;
        return;
    }

    rgb_index = camera_claim_rgb_buffer();
    if (rgb_index < 0)
    {
        s_processing_dropped++;
        return;
    }

    camera_convert_yuyv(frame->data, s_rgb_buffers[rgb_index]);
    SCB_CleanDCache_by_Addr((void *)s_rgb_buffers[rgb_index],
                            (int32_t)AI_KIT_CAMERA_FRAME_BYTES);
    s_last_convert_tick = now;
    camera_publish_rgb_buffer((uint32_t)rgb_index);
}

static void camera_reset_session(void)
{
    rt_base_t level = camera_lock();
    uint32_t index;

    for (index = 0U; index < CAMERA_RGB_BUFFER_COUNT; index++)
    {
        /* A paused/disconnected page may still display this buffer. */
        if (s_rgb_state[index] != CAMERA_RGB_HELD)
        {
            s_rgb_state[index] = CAMERA_RGB_FREE;
            s_rgb_generation[index] = 0U;
        }
    }
    s_last_convert_tick = 0U;
    s_capture_window_tick = 0U;
    s_display_window_tick = 0U;
    s_capture_window_count = 0U;
    s_display_window_count = 0U;
    s_status.capture_fps = 0U;
    s_status.display_fps = 0U;
    s_status.skipped_frames = 0U;
    s_status.dropped_frames = 0U;
    s_processing_dropped = 0U;
    s_status.transfer_errors = 0U;
    s_status.last_error = 0;
    camera_unlock(level);
}

static rt_err_t camera_stream_start(void)
{
    rt_err_t result;

    camera_reset_session();
    result = ai_kit_camera_transport_start();
    if (result != RT_EOK)
    {
        s_status.state = AI_KIT_CAMERA_ERROR;
        s_status.last_error = result;
        return result;
    }

    s_status.state = AI_KIT_CAMERA_WAITING;
    return RT_EOK;
}

static void camera_stream_stop(void)
{
    s_stream_active = RT_FALSE;
    (void)ai_kit_camera_transport_stop();
    s_status.running = RT_FALSE;
    if (!s_status.connected)
    {
        s_status.state = AI_KIT_CAMERA_DISCONNECTED;
    }
    else if (s_paused)
    {
        s_status.state = AI_KIT_CAMERA_PAUSED;
    }
    else
    {
        s_status.state = AI_KIT_CAMERA_WAITING;
    }
}

static void camera_thread_entry(void *parameter)
{
    ai_kit_camera_raw_frame_t frame;
    ai_kit_camera_transport_status_t transport;
    (void)parameter;

    while (1)
    {
        ai_kit_camera_transport_get_status(&transport);
        s_status.connected = transport.connected;
        s_status.running = transport.streaming;
        s_status.vendor_id = transport.vendor_id;
        s_status.product_id = transport.product_id;
        s_status.transfer_errors = transport.transfer_errors;
        s_status.dropped_frames = transport.dropped_frames +
                                  s_processing_dropped;
        s_status.last_error = transport.last_error;
        s_stream_active = transport.streaming;

        if (!s_requested || !transport.connected)
            s_status.state = AI_KIT_CAMERA_DISCONNECTED;
        else if (s_paused)
            s_status.state = AI_KIT_CAMERA_PAUSED;
        else if (transport.streaming)
            s_status.state = AI_KIT_CAMERA_STREAMING;
        else if (transport.last_error != 0)
            s_status.state = AI_KIT_CAMERA_ERROR;
        else
            s_status.state = AI_KIT_CAMERA_WAITING;

        if (transport.streaming)
        {
            if (ai_kit_camera_transport_dequeue(
                    &frame, rt_tick_from_millisecond(50U)))
            {
                camera_process_frame(&frame);
                ai_kit_camera_transport_release(&frame);
                continue;
            }
        }
        rt_thread_mdelay(20U);
    }
}

rt_err_t ai_kit_camera_init(void)
{
    rt_thread_t thread;
    rt_err_t result;

    if (s_initialized)
    {
        return RT_EOK;
    }

    memset(&s_status, 0, sizeof(s_status));
    s_status.state = AI_KIT_CAMERA_DISCONNECTED;
    s_status.width = AI_KIT_CAMERA_WIDTH;
    s_status.height = AI_KIT_CAMERA_HEIGHT;
    camera_prepare_luts();

    result = ai_kit_camera_transport_init();
    if (result != RT_EOK)
    {
        s_status.state = AI_KIT_CAMERA_ERROR;
        s_status.last_error = result;
        return result;
    }

    thread = rt_thread_create("camera", camera_thread_entry, RT_NULL,
                              CAMERA_THREAD_STACK_SIZE,
                              CAMERA_THREAD_PRIORITY, 10U);
    if (thread == RT_NULL)
    {
        s_status.state = AI_KIT_CAMERA_ERROR;
        s_status.last_error = -RT_ENOMEM;
        return -RT_ENOMEM;
    }
    rt_thread_startup(thread);

    s_initialized = RT_TRUE;
    return RT_EOK;
}

rt_err_t ai_kit_camera_start(void)
{
    if (!s_initialized)
    {
        return -RT_EINVAL;
    }
    s_requested = RT_TRUE;
    s_paused = RT_FALSE;
    return camera_stream_start();
}

rt_err_t ai_kit_camera_stop(void)
{
    s_requested = RT_FALSE;
    s_paused = RT_FALSE;
    camera_stream_stop();
    return RT_EOK;
}

rt_err_t ai_kit_camera_pause(void)
{
    if (!s_requested)
    {
        return -RT_EINVAL;
    }
    s_paused = RT_TRUE;
    (void)ai_kit_camera_transport_stop();
    s_status.state = AI_KIT_CAMERA_PAUSED;
    return RT_EOK;
}

rt_err_t ai_kit_camera_resume(void)
{
    if (!s_requested)
    {
        return -RT_EINVAL;
    }
    s_paused = RT_FALSE;
    return ai_kit_camera_transport_start();
}

void ai_kit_camera_get_status(ai_kit_camera_status_t *status)
{
    rt_base_t level;

    if (status == RT_NULL)
    {
        return;
    }
    level = camera_lock();
    *status = s_status;
    camera_unlock(level);
}

rt_bool_t ai_kit_camera_acquire_latest(ai_kit_camera_frame_t *frame)
{
    rt_base_t level;
    uint32_t index;
    uint32_t newest_index = CAMERA_RGB_BUFFER_COUNT;
    uint32_t newest_generation = 0U;

    if (frame == RT_NULL)
    {
        return RT_FALSE;
    }
    level = camera_lock();
    for (index = 0U; index < CAMERA_RGB_BUFFER_COUNT; index++)
    {
        if ((s_rgb_state[index] == CAMERA_RGB_READY) &&
            (s_rgb_generation[index] >= newest_generation))
        {
            newest_index = index;
            newest_generation = s_rgb_generation[index];
        }
    }
    if (newest_index == CAMERA_RGB_BUFFER_COUNT)
    {
        camera_unlock(level);
        return RT_FALSE;
    }
    s_rgb_state[newest_index] = CAMERA_RGB_HELD;
    frame->pixels = s_rgb_buffers[newest_index];
    frame->width = AI_KIT_CAMERA_WIDTH;
    frame->height = AI_KIT_CAMERA_HEIGHT;
    frame->stride = AI_KIT_CAMERA_STRIDE;
    frame->buffer_index = (uint8_t)newest_index;
    frame->token = (newest_generation << 2) | newest_index;
    camera_update_rate(rt_tick_get(), &s_display_window_tick,
                       &s_display_window_count, &s_status.display_fps);
    camera_unlock(level);
    return RT_TRUE;
}

void ai_kit_camera_release(const ai_kit_camera_frame_t *frame)
{
    rt_base_t level;
    uint32_t index;
    uint32_t generation;

    if (frame == RT_NULL)
    {
        return;
    }
    index = frame->token & 0x3U;
    generation = frame->token >> 2;
    if (index >= CAMERA_RGB_BUFFER_COUNT)
    {
        return;
    }
    level = camera_lock();
    if ((s_rgb_state[index] == CAMERA_RGB_HELD) &&
        (s_rgb_generation[index] == generation))
    {
        s_rgb_state[index] = CAMERA_RGB_FREE;
    }
    camera_unlock(level);
}
