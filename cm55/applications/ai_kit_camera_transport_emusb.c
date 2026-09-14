/**
 * @file ai_kit_camera_transport_emusb.c
 * @brief PSoC E84 USB camera transport based on Infineon emUSB-Host.
 *
 * The device selection and stream setup follow Infineon's Deploy Vision
 * reference for the bundled HBVCAM 058F:5608 camera: 320x240 YUYV at 30 FPS.
 */

#include "ai_kit_camera_transport.h"

#include <string.h>

#include "cy_pdl.h"
#include "USBH.h"
#include "USBH_VIDEO.h"
#include "ai_kit_camera.h"

#define CAMERA_RAW_BUFFER_COUNT 4U
#define CAMERA_USB_THREAD_STACK 8192U
#define CAMERA_USB_MAIN_PRIORITY 5U
#define CAMERA_USB_ISR_PRIORITY 4U
#define CAMERA_USB_CONTROL_PRIORITY 12U
#define CAMERA_USB_FRAME_INTERVAL 333332UL
#define CAMERA_USB_VENDOR_ID 0x058FU
#define CAMERA_USB_PRODUCT_ID 0x5608U
#define CAMERA_BUFFER_SECTION \
    __attribute__((section(".ai_kit_camera_psram"), aligned(32), used))

enum raw_buffer_state
{
    RAW_BUFFER_FREE = 0,
    RAW_BUFFER_WRITING,
    RAW_BUFFER_READY,
    RAW_BUFFER_HELD
};

static CAMERA_BUFFER_SECTION uint8_t
    s_raw_buffers[CAMERA_RAW_BUFFER_COUNT][AI_KIT_CAMERA_FRAME_BYTES];
static uint8_t s_raw_state[CAMERA_RAW_BUFFER_COUNT];
static uint32_t s_raw_generation[CAMERA_RAW_BUFFER_COUNT];
static uint32_t s_next_generation;
static uint8_t s_write_index = CAMERA_RAW_BUFFER_COUNT;
static uint32_t s_write_bytes;
static rt_bool_t s_discarding_frame;

static struct rt_semaphore s_control_sem;
static struct rt_semaphore s_frame_sem;
static USBH_NOTIFICATION_HOOK s_notification_hook;
static USBH_VIDEO_DEVICE_HANDLE s_device_handle = USBH_VIDEO_INVALID_HANDLE;
static volatile rt_bool_t s_initialized;
static volatile rt_bool_t s_requested;
static volatile rt_bool_t s_session_abort;
static volatile uint8_t s_device_index;
static uint32_t s_consecutive_errors;
static ai_kit_camera_transport_status_t s_status;

static rt_base_t transport_lock(void)
{
    return rt_hw_interrupt_disable();
}

static void transport_unlock(rt_base_t level)
{
    rt_hw_interrupt_enable(level);
}

static uint8_t transport_find_free_buffer(void)
{
    uint8_t index;

    for (index = 0U; index < CAMERA_RAW_BUFFER_COUNT; index++)
    {
        if (s_raw_state[index] == RAW_BUFFER_FREE)
        {
            return index;
        }
    }
    return CAMERA_RAW_BUFFER_COUNT;
}

static void transport_reset_frames(void)
{
    rt_base_t level = transport_lock();

    memset(s_raw_state, RAW_BUFFER_FREE, sizeof(s_raw_state));
    memset(s_raw_generation, 0, sizeof(s_raw_generation));
    s_write_index = 0U;
    s_raw_state[0] = RAW_BUFFER_WRITING;
    s_write_bytes = 0U;
    s_discarding_frame = RT_FALSE;
    s_next_generation = 0U;
    transport_unlock(level);

    while (rt_sem_trytake(&s_frame_sem) == RT_EOK)
    {
    }
}

static void transport_select_next_writer(void)
{
    uint8_t next = transport_find_free_buffer();

    s_write_index = next;
    s_write_bytes = 0U;
    s_discarding_frame = (next == CAMERA_RAW_BUFFER_COUNT);
    if (next < CAMERA_RAW_BUFFER_COUNT)
    {
        s_raw_state[next] = RAW_BUFFER_WRITING;
    }
}

static void transport_data_callback(USBH_VIDEO_DEVICE_HANDLE device,
                                    USBH_VIDEO_STREAM_HANDLE stream,
                                    USBH_STATUS status,
                                    const U8 *data,
                                    unsigned num_bytes,
                                    U32 flags,
                                    void *context)
{
    rt_bool_t end_of_frame;
    rt_bool_t publish_frame = RT_FALSE;
    rt_base_t level;

    (void)device;
    (void)context;

    if (status != USBH_STATUS_SUCCESS)
    {
        I8 stream_stopped = 0;

        level = transport_lock();
        s_status.transfer_errors++;
        s_status.dropped_frames++;
        s_status.last_error = (int32_t)status;
        s_consecutive_errors++;
        if (s_write_index < CAMERA_RAW_BUFFER_COUNT)
        {
            s_raw_state[s_write_index] = RAW_BUFFER_FREE;
        }
        s_write_bytes = 0U;
        s_discarding_frame = RT_TRUE;
        transport_unlock(level);

        if ((status == USBH_STATUS_DEVICE_REMOVED) ||
            (s_consecutive_errors >= 10U))
        {
            s_session_abort = RT_TRUE;
            rt_sem_release(&s_control_sem);
        }
        else if ((USBH_VIDEO_GetStreamState(stream, &stream_stopped) ==
                  USBH_STATUS_SUCCESS) && (stream_stopped != 0))
        {
            (void)USBH_VIDEO_RestartStream(stream);
        }
        (void)USBH_VIDEO_Ack(stream);
        return;
    }
    s_consecutive_errors = 0U;

    end_of_frame = ((flags & USBH_UVC_END_OF_FRAME) != 0U);
    if (!s_status.streaming || s_discarding_frame ||
        (s_write_index >= CAMERA_RAW_BUFFER_COUNT))
    {
        if (end_of_frame)
        {
            level = transport_lock();
            s_status.dropped_frames++;
            transport_select_next_writer();
            transport_unlock(level);
        }
        (void)USBH_VIDEO_Ack(stream);
        return;
    }

    if ((s_write_bytes + num_bytes) > AI_KIT_CAMERA_FRAME_BYTES)
    {
        level = transport_lock();
        s_status.dropped_frames++;
        s_raw_state[s_write_index] = RAW_BUFFER_FREE;
        s_write_bytes = 0U;
        s_discarding_frame = RT_TRUE;
        transport_unlock(level);
        (void)USBH_VIDEO_Ack(stream);
        return;
    }

    memcpy(&s_raw_buffers[s_write_index][s_write_bytes], data, num_bytes);
    s_write_bytes += num_bytes;

    if (end_of_frame)
    {
        level = transport_lock();
        if (s_write_bytes == AI_KIT_CAMERA_FRAME_BYTES)
        {
            s_raw_generation[s_write_index] = ++s_next_generation;
            s_raw_state[s_write_index] = RAW_BUFFER_READY;
            __DMB();
            publish_frame = RT_TRUE;
        }
        else
        {
            s_raw_state[s_write_index] = RAW_BUFFER_FREE;
            s_status.dropped_frames++;
        }
        transport_select_next_writer();
        transport_unlock(level);
        if (publish_frame)
        {
            rt_sem_release(&s_frame_sem);
        }
    }

    (void)USBH_VIDEO_Ack(stream);
}

static void transport_device_callback(void *context,
                                      U8 device_index,
                                      USBH_DEVICE_EVENT event)
{
    rt_base_t level;

    (void)context;
    level = transport_lock();
    if (event == USBH_DEVICE_EVENT_ADD)
    {
        s_device_index = device_index;
        s_status.connected = RT_TRUE;
        s_status.last_error = 0;
    }
    else
    {
        s_status.connected = RT_FALSE;
        s_status.streaming = RT_FALSE;
        s_status.vendor_id = 0U;
        s_status.product_id = 0U;
    }
    transport_unlock(level);
    rt_sem_release(&s_control_sem);
}

static rt_err_t transport_find_mode(USBH_VIDEO_DEVICE_HANDLE device,
                                    USBH_VIDEO_STREAM_CONFIG *config)
{
    USBH_VIDEO_INPUT_HEADER_INFO input;
    USBH_VIDEO_FORMAT_INFO format;
    USBH_VIDEO_FRAME_INFO frame;
    USBH_STATUS status;
    unsigned format_index;

    status = USBH_VIDEO_GetInputHeader(device, &input);
    if (status != USBH_STATUS_SUCCESS)
    {
        return -RT_ERROR;
    }

    for (format_index = 0U; format_index < input.bNumFormats; format_index++)
    {
        unsigned frame_count;
        unsigned frame_index;

        status = USBH_VIDEO_GetFormatInfo(device, format_index, &format);
        if ((status != USBH_STATUS_SUCCESS) ||
            (format.FormatType != USBH_VIDEO_VS_FORMAT_UNCOMPRESSED))
        {
            continue;
        }
        frame_count = format.u.UncompressedFormat.bNumFrameDescriptors;
        for (frame_index = 0U; frame_index < frame_count; frame_index++)
        {
            unsigned interval_index;

            status = USBH_VIDEO_GetFrameInfo(device, format_index,
                                             frame_index, &frame);
            if ((status != USBH_STATUS_SUCCESS) ||
                (frame.wWidth != AI_KIT_CAMERA_WIDTH) ||
                (frame.wHeight != AI_KIT_CAMERA_HEIGHT) ||
                (frame.bFrameIntervalType == 0U))
            {
                continue;
            }
            for (interval_index = 0U;
                 interval_index < frame.bFrameIntervalType;
                 interval_index++)
            {
                if (frame.u.dwFrameInterval[interval_index] ==
                    CAMERA_USB_FRAME_INTERVAL)
                {
                    memset(config, 0, sizeof(*config));
                    config->FormatIdx = (U8)format_index;
                    config->FrameIdx = (U8)frame_index;
                    config->FrameIntervalIdx = (U8)(interval_index + 1U);
                    config->pfDataCallback = transport_data_callback;
                    return RT_EOK;
                }
            }
        }
    }
    return -RT_ENOSYS;
}

static void transport_stream_session(uint8_t device_index)
{
    USBH_VIDEO_STREAM_HANDLE stream = USBH_VIDEO_INVALID_STREAM;
    USBH_VIDEO_INTERFACE_INFO info;
    USBH_VIDEO_STREAM_CONFIG config;
    USBH_STATUS usb_status;
    rt_err_t result;

    if (s_device_handle == USBH_VIDEO_INVALID_HANDLE)
    {
        usb_status = USBH_VIDEO_Open(device_index, &s_device_handle);
        if (usb_status != USBH_STATUS_SUCCESS)
        {
            s_status.last_error = (int32_t)usb_status;
            return;
        }
    }
    usb_status = USBH_VIDEO_GetInterfaceInfo(s_device_handle, &info);
    if (usb_status != USBH_STATUS_SUCCESS)
    {
        s_status.last_error = (int32_t)usb_status;
        USBH_VIDEO_Close(s_device_handle);
        s_device_handle = USBH_VIDEO_INVALID_HANDLE;
        return;
    }

    s_status.vendor_id = info.VendorId;
    s_status.product_id = info.ProductId;
    rt_kprintf("[camera] emUSB UVC connected VID=%04x PID=%04x\r\n",
               info.VendorId, info.ProductId);
    if ((info.VendorId != CAMERA_USB_VENDOR_ID) ||
        (info.ProductId != CAMERA_USB_PRODUCT_ID))
    {
        s_status.last_error = -RT_ENOSYS;
        rt_kprintf("[camera] unsupported USB camera\r\n");
        USBH_VIDEO_Close(s_device_handle);
        s_device_handle = USBH_VIDEO_INVALID_HANDLE;
        return;
    }

    result = transport_find_mode(s_device_handle, &config);
    if (result != RT_EOK)
    {
        s_status.last_error = result;
        rt_kprintf("[camera] 320x240 YUYV @ 30 FPS not advertised\r\n");
        return;
    }

    transport_reset_frames();
    s_session_abort = RT_FALSE;
    s_consecutive_errors = 0U;
    usb_status = USBH_VIDEO_OpenStream(s_device_handle, &config, &stream);
    if (usb_status != USBH_STATUS_SUCCESS)
    {
        s_status.last_error = (int32_t)usb_status;
        return;
    }
    s_status.streaming = RT_TRUE;
    s_status.last_error = 0;
    rt_kprintf("[camera] emUSB stream: 320x240 YUYV @ 30 FPS\r\n");

    while (s_requested && s_status.connected && !s_session_abort)
    {
        rt_sem_take(&s_control_sem, rt_tick_from_millisecond(100U));
    }

    s_status.streaming = RT_FALSE;
    usb_status = USBH_VIDEO_CloseStream(stream);
    if (!s_session_abort && s_status.connected)
    {
        s_status.last_error = 0;
    }
    else if ((usb_status != USBH_STATUS_SUCCESS) && s_status.connected)
    {
        s_status.last_error = (int32_t)usb_status;
    }
}

static void transport_control_entry(void *parameter)
{
    (void)parameter;

    while (1)
    {
        if (s_requested && s_status.connected && !s_status.streaming)
        {
            transport_stream_session(s_device_index);
        }
        if (!s_status.connected &&
            (s_device_handle != USBH_VIDEO_INVALID_HANDLE))
        {
            USBH_VIDEO_Close(s_device_handle);
            s_device_handle = USBH_VIDEO_INVALID_HANDLE;
        }
        rt_sem_take(&s_control_sem, rt_tick_from_millisecond(100U));
    }
}

static void transport_main_entry(void *parameter)
{
    (void)parameter;
    USBH_Task();
}

static void transport_isr_entry(void *parameter)
{
    (void)parameter;
    USBH_ISRTask();
}

static rt_err_t transport_start_thread(const char *name,
                                       void (*entry)(void *),
                                       rt_uint8_t priority)
{
    rt_thread_t thread = rt_thread_create(name, entry, RT_NULL,
                                          CAMERA_USB_THREAD_STACK,
                                          priority, 10U);
    if (thread == RT_NULL)
    {
        return -RT_ENOMEM;
    }
    return rt_thread_startup(thread);
}

rt_err_t ai_kit_camera_transport_init(void)
{
    USBH_STATUS status;

    if (s_initialized)
    {
        return RT_EOK;
    }
    memset(&s_status, 0, sizeof(s_status));
    s_device_index = 0xFFU;
    rt_sem_init(&s_control_sem, "uvcctl", 0U, RT_IPC_FLAG_PRIO);
    rt_sem_init(&s_frame_sem, "uvcfrm", 0U, RT_IPC_FLAG_PRIO);

    USBH_Init();
    status = USBH_VIDEO_Init();
    if (status != USBH_STATUS_SUCCESS)
    {
        return -RT_ERROR;
    }
    status = USBH_VIDEO_AddNotification(&s_notification_hook,
                                        transport_device_callback, RT_NULL);
    if (status != USBH_STATUS_SUCCESS)
    {
        return -RT_ERROR;
    }
    if ((transport_start_thread("usbh", transport_main_entry,
                                CAMERA_USB_MAIN_PRIORITY) != RT_EOK) ||
        (transport_start_thread("usbhisr", transport_isr_entry,
                                CAMERA_USB_ISR_PRIORITY) != RT_EOK) ||
        (transport_start_thread("uvcctl", transport_control_entry,
                                CAMERA_USB_CONTROL_PRIORITY) != RT_EOK))
    {
        return -RT_ENOMEM;
    }
    s_initialized = RT_TRUE;
    rt_kprintf("[camera] Infineon emUSB-Host initialized\r\n");
    return RT_EOK;
}

rt_err_t ai_kit_camera_transport_start(void)
{
    if (!s_initialized)
    {
        return -RT_EINVAL;
    }
    s_status.last_error = 0;
    s_session_abort = RT_FALSE;
    s_requested = RT_TRUE;
    rt_sem_release(&s_control_sem);
    return RT_EOK;
}

rt_err_t ai_kit_camera_transport_stop(void)
{
    s_requested = RT_FALSE;
    rt_sem_release(&s_control_sem);
    return RT_EOK;
}

rt_bool_t ai_kit_camera_transport_dequeue(ai_kit_camera_raw_frame_t *frame,
                                          rt_int32_t timeout)
{
    rt_base_t level;
    uint8_t index;
    uint8_t newest = CAMERA_RAW_BUFFER_COUNT;
    uint32_t generation = 0U;

    if ((frame == RT_NULL) || (rt_sem_take(&s_frame_sem, timeout) != RT_EOK))
    {
        return RT_FALSE;
    }
    level = transport_lock();
    for (index = 0U; index < CAMERA_RAW_BUFFER_COUNT; index++)
    {
        if ((s_raw_state[index] == RAW_BUFFER_READY) &&
            (s_raw_generation[index] >= generation))
        {
            newest = index;
            generation = s_raw_generation[index];
        }
    }
    if (newest == CAMERA_RAW_BUFFER_COUNT)
    {
        transport_unlock(level);
        return RT_FALSE;
    }
    for (index = 0U; index < CAMERA_RAW_BUFFER_COUNT; index++)
    {
        if ((index != newest) && (s_raw_state[index] == RAW_BUFFER_READY))
        {
            s_raw_state[index] = RAW_BUFFER_FREE;
            s_status.dropped_frames++;
        }
    }
    s_raw_state[newest] = RAW_BUFFER_HELD;
    frame->data = s_raw_buffers[newest];
    frame->size = AI_KIT_CAMERA_FRAME_BYTES;
    frame->buffer_index = newest;
    transport_unlock(level);
    return RT_TRUE;
}

void ai_kit_camera_transport_release(const ai_kit_camera_raw_frame_t *frame)
{
    rt_base_t level;

    if ((frame == RT_NULL) || (frame->buffer_index >= CAMERA_RAW_BUFFER_COUNT))
    {
        return;
    }
    level = transport_lock();
    if (s_raw_state[frame->buffer_index] == RAW_BUFFER_HELD)
    {
        s_raw_state[frame->buffer_index] = RAW_BUFFER_FREE;
    }
    transport_unlock(level);
}

void ai_kit_camera_transport_get_status(ai_kit_camera_transport_status_t *status)
{
    rt_base_t level;

    if (status == RT_NULL)
    {
        return;
    }
    level = transport_lock();
    *status = s_status;
    transport_unlock(level);
}
