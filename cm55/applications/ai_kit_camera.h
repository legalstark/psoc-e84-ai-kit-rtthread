#ifndef AI_KIT_CAMERA_H
#define AI_KIT_CAMERA_H

#include <rtthread.h>
#include <stdint.h>

#define AI_KIT_CAMERA_WIDTH       320U
#define AI_KIT_CAMERA_HEIGHT      240U
#define AI_KIT_CAMERA_STRIDE      (AI_KIT_CAMERA_WIDTH * 2U)
#define AI_KIT_CAMERA_FRAME_BYTES (AI_KIT_CAMERA_STRIDE * AI_KIT_CAMERA_HEIGHT)
#define AI_KIT_CAMERA_RGB_BUFFER_COUNT 3U

typedef enum
{
    AI_KIT_CAMERA_DISCONNECTED = 0,
    AI_KIT_CAMERA_WAITING,
    AI_KIT_CAMERA_STREAMING,
    AI_KIT_CAMERA_PAUSED,
    AI_KIT_CAMERA_ERROR
} ai_kit_camera_state_t;

typedef struct
{
    ai_kit_camera_state_t state;
    rt_bool_t connected;
    rt_bool_t running;
    uint16_t vendor_id;
    uint16_t product_id;
    uint16_t width;
    uint16_t height;
    uint32_t capture_fps;
    uint32_t display_fps;
    uint32_t skipped_frames;
    uint32_t dropped_frames;
    uint32_t transfer_errors;
    int32_t last_error;
} ai_kit_camera_status_t;

typedef struct
{
    const uint16_t *pixels;
    uint16_t width;
    uint16_t height;
    uint16_t stride;
    uint8_t buffer_index;
    uint32_t token;
} ai_kit_camera_frame_t;

rt_err_t ai_kit_camera_init(void);
rt_err_t ai_kit_camera_start(void);
rt_err_t ai_kit_camera_stop(void);
rt_err_t ai_kit_camera_pause(void);
rt_err_t ai_kit_camera_resume(void);
void ai_kit_camera_get_status(ai_kit_camera_status_t *status);
rt_bool_t ai_kit_camera_acquire_latest(ai_kit_camera_frame_t *frame);
void ai_kit_camera_release(const ai_kit_camera_frame_t *frame);

#endif
