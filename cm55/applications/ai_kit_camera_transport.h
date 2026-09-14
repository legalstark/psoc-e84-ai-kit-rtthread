#ifndef AI_KIT_CAMERA_TRANSPORT_H
#define AI_KIT_CAMERA_TRANSPORT_H

#include <rtthread.h>
#include <stdint.h>

typedef struct
{
    const uint8_t *data;
    uint32_t size;
    uint8_t buffer_index;
} ai_kit_camera_raw_frame_t;

typedef struct
{
    rt_bool_t connected;
    rt_bool_t streaming;
    uint16_t vendor_id;
    uint16_t product_id;
    uint32_t dropped_frames;
    uint32_t transfer_errors;
    int32_t last_error;
} ai_kit_camera_transport_status_t;

rt_err_t ai_kit_camera_transport_init(void);
rt_err_t ai_kit_camera_transport_start(void);
rt_err_t ai_kit_camera_transport_stop(void);
rt_bool_t ai_kit_camera_transport_dequeue(ai_kit_camera_raw_frame_t *frame,
                                          rt_int32_t timeout);
void ai_kit_camera_transport_release(const ai_kit_camera_raw_frame_t *frame);
void ai_kit_camera_transport_get_status(ai_kit_camera_transport_status_t *status);

#endif
