/**
 * @file    ai_kit_ui.c
 * @brief   AI Kit HMI Launcher LVGL 界面实现
 *
 * @details 该文件在 CM55 上实现整机 HMI Launcher，基于 LVGL 9.2 构建，
 *          共 6 个页面：
 *          - Home         ：系统总览、CPU/内存/NPU 速览
 *          - Hardware     ：硬件配置与版本信息
 *          - Performance  ：CoreMark / 内存 / NPU 各项基准按钮与结果展示
 *          - Camera       ：J2 USB UVC 实时预览与流生命周期控制
 *          - Settings     ：深色 / 浅色主题切换等设置
 *          - Wi-Fi        ：扫描结果、连接对话、连接状态
 *          - About        ：BSP / PDL / RT-Thread / LVGL 版本基线
 *
 *          数据来源：
 *          - CPU 占用率   ：cpu_usage 组件
 *          - 基准结果     ：ai_kit_benchmark_get_catalog / get_result
 *          - Wi-Fi 快照   ：ai_kit_wifi_get_snapshot
 *
 *          入口为 lv_user_gui_init，由 LVGL 在系统启动后调用。
 *          Performance 页定时器 s_performance_timer 周期刷新基准目录；
 *          Wi-Fi 页定时器 s_wifi_timer 周期刷新扫描结果。
 */
#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"
#include <rtthread.h>
#include "cpu_usage.h"
#include "ai_kit_benchmark.h"
#include "ai_kit_wifi.h"
#include "ai_kit_camera.h"

/** @brief 显示可见宽度（px） */
#define UI_VISIBLE_WIDTH       800
/** @brief 内容面板起始 X 坐标 */
#define UI_PAGE_PANEL_X         24
/** @brief 内容面板起始 Y 坐标 */
#define UI_PAGE_PANEL_Y         88
/** @brief 内容面板宽度 */
#define UI_PAGE_PANEL_WIDTH    752
/** @brief 内容面板高度 */
#define UI_PAGE_PANEL_HEIGHT   360

/**
 * @brief   HMI 页面枚举
 *
 * @details 对应顶部导航按钮与 ui_show_page 入参。
 */
typedef enum
{
    UI_PAGE_HOME = 0,    /**< @brief 首页 */
    UI_PAGE_HARDWARE,    /**< @brief 硬件信息 */
    UI_PAGE_PERFORMANCE, /**< @brief 性能基准 */
    UI_PAGE_CAMERA,      /**< @brief USB UVC 摄像头 */
    UI_PAGE_SETTINGS,    /**< @brief 设置 */
    UI_PAGE_WIFI,        /**< @brief Wi-Fi 管理 */
    UI_PAGE_ABOUT        /**< @brief 关于 */
} ui_page_t;

static bool s_dark_mode = true;
static lv_timer_t *s_performance_timer;
static lv_obj_t *s_cpu_value_label;
static lv_obj_t *s_coremark_value_label;
static lv_obj_t *s_coremark_state_label;
static lv_obj_t *s_coremark_button;
static lv_obj_t *s_m33_coremark_value_label;
static lv_obj_t *s_m33_coremark_state_label;
static lv_obj_t *s_ipc_value_label;
static lv_obj_t *s_ipc_state_label;
static lv_obj_t *s_memory_value_label;
static lv_obj_t *s_m33_memory_value_label;
static lv_obj_t *s_memory_button;
static lv_obj_t *s_psram_value_label;
static lv_obj_t *s_m33_psram_value_label;
static lv_obj_t *s_psram_button;
static lv_obj_t *s_person_npu_value_label;
static lv_obj_t *s_person_npu_state_label;
static lv_obj_t *s_person_npu_button;
static lv_obj_t *s_person_peak_value_label;
static lv_obj_t *s_person_peak_state_label;
static lv_obj_t *s_person_peak_button;
static lv_timer_t *s_wifi_timer;
static lv_obj_t *s_wifi_state_label;
static lv_obj_t *s_wifi_count_label;
static lv_obj_t *s_wifi_list;
static lv_obj_t *s_wifi_refresh_button;
static uint32_t s_wifi_generation = UINT32_MAX;
static ai_kit_wifi_snapshot_t s_wifi_page_snapshot;
static lv_obj_t *s_wifi_dialog;
static lv_obj_t *s_wifi_password;
static char s_wifi_selected_ssid[AI_KIT_WIFI_SSID_SIZE];
static ui_page_t s_current_page = UI_PAGE_HOME;
static lv_timer_t *s_camera_timer;
static lv_obj_t *s_camera_image;
static lv_obj_t *s_camera_overlay;
static lv_obj_t *s_camera_state_label;
static lv_obj_t *s_camera_device_label;
static lv_obj_t *s_camera_capture_label;
static lv_obj_t *s_camera_display_label;
static lv_obj_t *s_camera_drop_label;
static lv_obj_t *s_camera_pause_label;
static ai_kit_camera_frame_t s_camera_frame;
static bool s_camera_frame_held;
static lv_image_dsc_t
    s_camera_image_dsc[AI_KIT_CAMERA_RGB_BUFFER_COUNT];

static lv_color_t ui_color_background(void)
{
    return lv_color_hex(s_dark_mode ? 0x0b1220 : 0xf1f5f9);
}

static lv_color_t ui_color_surface(void)
{
    return lv_color_hex(s_dark_mode ? 0x162033 : 0xffffff);
}

static lv_color_t ui_color_surface_pressed(void)
{
    return lv_color_hex(s_dark_mode ? 0x22304a : 0xe2e8f0);
}

static lv_color_t ui_color_text(void)
{
    return lv_color_hex(s_dark_mode ? 0xf8fafc : 0x0f172a);
}

static lv_color_t ui_color_muted(void)
{
    return lv_color_hex(s_dark_mode ? 0x94a3b8 : 0x64748b);
}

static lv_color_t ui_color_border(void)
{
    return lv_color_hex(s_dark_mode ? 0x2b3a55 : 0xcbd5e1);
}

static lv_color_t ui_color_accent(void)
{
    return lv_color_hex(0x38bdf8);
}

static lv_obj_t *ui_add_label(lv_obj_t *parent,
                              const char *text,
                              const lv_font_t *font,
                              lv_color_t color,
                              int32_t x,
                              int32_t y)
{
    lv_obj_t *label = lv_label_create(parent);

    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, color, LV_PART_MAIN);
    lv_obj_set_pos(label, x, y);
    return label;
}

static void ui_style_panel(lv_obj_t *object)
{
    lv_obj_set_style_bg_color(object, ui_color_surface(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(object, ui_color_border(), LV_PART_MAIN);
    lv_obj_set_style_border_width(object, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(object, 14, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(object, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(object, 0, LV_PART_MAIN);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
}

static lv_obj_t *ui_prepare_screen(void)
{
    lv_obj_t *screen = lv_screen_active();

    if (s_camera_timer != NULL)
    {
        lv_timer_delete(s_camera_timer);
        s_camera_timer = NULL;
    }
    if (s_camera_frame_held)
    {
        ai_kit_camera_release(&s_camera_frame);
        s_camera_frame_held = false;
    }
    if (s_current_page == UI_PAGE_CAMERA)
    {
        (void)ai_kit_camera_stop();
    }

    if (s_performance_timer != NULL)
    {
        lv_timer_delete(s_performance_timer);
        s_performance_timer = NULL;
    }
    if (s_wifi_timer != NULL)
    {
        lv_timer_delete(s_wifi_timer);
        s_wifi_timer = NULL;
    }
    s_cpu_value_label = NULL;
    s_coremark_value_label = NULL;
    s_coremark_state_label = NULL;
    s_coremark_button = NULL;
    s_m33_coremark_value_label = NULL;
    s_m33_coremark_state_label = NULL;
    s_ipc_value_label = NULL;
    s_ipc_state_label = NULL;
    s_memory_value_label = NULL;
    s_m33_memory_value_label = NULL;
    s_memory_button = NULL;
    s_psram_value_label = NULL;
    s_m33_psram_value_label = NULL;
    s_psram_button = NULL;
    s_person_npu_value_label = NULL;
    s_person_npu_state_label = NULL;
    s_person_npu_button = NULL;
    s_person_peak_value_label = NULL;
    s_person_peak_state_label = NULL;
    s_person_peak_button = NULL;
    s_wifi_state_label = NULL;
    s_wifi_count_label = NULL;
    s_wifi_list = NULL;
    s_wifi_refresh_button = NULL;
    s_wifi_generation = UINT32_MAX;
    s_wifi_dialog = NULL;
    s_wifi_password = NULL;
    s_camera_image = NULL;
    s_camera_overlay = NULL;
    s_camera_state_label = NULL;
    s_camera_device_label = NULL;
    s_camera_capture_label = NULL;
    s_camera_display_label = NULL;
    s_camera_drop_label = NULL;
    s_camera_pause_label = NULL;
    rt_memset(&s_wifi_page_snapshot, 0, sizeof(s_wifi_page_snapshot));
    rt_memset(s_wifi_selected_ssid, 0, sizeof(s_wifi_selected_ssid));

    lv_obj_clean(screen);
    lv_obj_set_style_bg_color(screen, ui_color_background(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(screen, 0, LV_PART_MAIN);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    return screen;
}

static void ui_show_page(ui_page_t page);

/**
 * @brief   异步切页包装（供 lv_async_call 调度）
 *
 * @param   user_data  编码后的 ui_page_t（按 uintptr_t 传递）
 */
static void ui_show_page_async(void *user_data)
{
    ui_show_page((ui_page_t)(uintptr_t)user_data);
}

/**
 * @brief   顶部导航按钮事件回调
 *
 * @details 从 event user_data 取出目标页 ID，通过 lv_async_call 把
 *          ui_show_page_async 调度到 LVGL 主线程执行，避免在事件回调中
 *          直接做破坏性 UI 操作（旧页面销毁 / 新页面创建）。
 *
 * @param   event  LVGL 事件对象
 */
static void ui_navigation_event(lv_event_t *event)
{
    ui_page_t page = (ui_page_t)(uintptr_t)lv_event_get_user_data(event);

    lv_async_call(ui_show_page_async, (void *)(uintptr_t)page);
}

static void ui_add_header(lv_obj_t *screen, const char *title, const char *subtitle)
{
    lv_obj_t *back = lv_button_create(screen);
    lv_obj_t *line;

    lv_obj_set_pos(back, 20, 16);
    lv_obj_set_size(back, 82, 42);
    ui_style_panel(back);
    lv_obj_set_style_bg_color(back, ui_color_surface_pressed(), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_add_event_cb(back, ui_navigation_event, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)UI_PAGE_HOME);
    ui_add_label(back, LV_SYMBOL_LEFT "  Home", &lv_font_montserrat_16,
                 ui_color_text(), 13, 11);

    ui_add_label(screen, title, &lv_font_montserrat_24, ui_color_text(), 122, 12);
    ui_add_label(screen, subtitle, &lv_font_montserrat_14, ui_color_muted(), 122, 43);

    line = lv_obj_create(screen);
    lv_obj_set_pos(line, 0, 71);
    lv_obj_set_size(line, UI_VISIBLE_WIDTH, 1);
    lv_obj_set_style_bg_color(line, ui_color_border(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(line, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(line, 0, LV_PART_MAIN);
}

static lv_obj_t *ui_add_page_panel(lv_obj_t *screen)
{
    lv_obj_t *panel = lv_obj_create(screen);

    lv_obj_set_pos(panel, UI_PAGE_PANEL_X, UI_PAGE_PANEL_Y);
    lv_obj_set_size(panel, UI_PAGE_PANEL_WIDTH, UI_PAGE_PANEL_HEIGHT);
    ui_style_panel(panel);
    return panel;
}

static void ui_add_status_row(lv_obj_t *parent,
                              int32_t y,
                              const char *name,
                              const char *detail,
                              const char *state,
                              bool ready)
{
    lv_obj_t *status;

    ui_add_label(parent, name, &lv_font_montserrat_16, ui_color_text(), 24, y);
    ui_add_label(parent, detail, &lv_font_montserrat_14, ui_color_muted(), 24, y + 25);

    status = ui_add_label(parent, state, &lv_font_montserrat_14,
                          ready ? lv_color_hex(0x22c55e) : ui_color_muted(),
                          520, y + 13);
    lv_obj_set_width(status, 110);
    lv_obj_set_style_text_align(status, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);

    if (y < 258)
    {
        lv_obj_t *line = lv_obj_create(parent);
        lv_obj_set_pos(line, 24, y + 57);
        lv_obj_set_size(line, 704, 1);
        lv_obj_set_style_bg_color(line, ui_color_border(), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(line, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(line, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(line, 0, LV_PART_MAIN);
    }
}

static void ui_add_app_card(lv_obj_t *screen,
                            int32_t x,
                            int32_t y,
                            const char *symbol,
                            const char *title,
                            const char *description,
                            ui_page_t page)
{
    lv_obj_t *card = lv_button_create(screen);

    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, 232, 112);
    ui_style_panel(card);
    lv_obj_set_style_bg_color(card, ui_color_surface_pressed(), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_add_event_cb(card, ui_navigation_event, LV_EVENT_CLICKED, (void *)(uintptr_t)page);

    ui_add_label(card, symbol, &lv_font_montserrat_20, ui_color_accent(), 16, 15);
    ui_add_label(card, title, &lv_font_montserrat_16, ui_color_text(), 50, 16);
    {
        lv_obj_t *description_label = ui_add_label(card, description,
                                                   &lv_font_montserrat_12,
                                                   ui_color_muted(), 16, 53);
        lv_obj_set_width(description_label, 188);
    }
    ui_add_label(card, LV_SYMBOL_RIGHT, &lv_font_montserrat_14,
                 ui_color_muted(), 204, 47);
}

static void ui_show_home(void)
{
    lv_obj_t *screen = ui_prepare_screen();
    lv_obj_t *runtime;
    lv_obj_t *connectivity;

    ui_add_label(screen, "PSoC Edge HMI", &lv_font_montserrat_24,
                 ui_color_text(), 24, 15);
    ui_add_label(screen, "AI Kit system launcher", &lv_font_montserrat_14,
                 ui_color_muted(), 24, 48);

    runtime = lv_obj_create(screen);
    lv_obj_set_pos(runtime, 572, 17);
    lv_obj_set_size(runtime, 204, 42);
    ui_style_panel(runtime);
    ui_add_label(runtime, LV_SYMBOL_OK "  CM55 + RT-Thread", &lv_font_montserrat_14,
                 lv_color_hex(0x22c55e), 14, 12);

    ui_add_app_card(screen, 24, 82, LV_SYMBOL_DRIVE, "Hardware",
                    "Display, touch and cores", UI_PAGE_HARDWARE);
    ui_add_app_card(screen, 284, 82, LV_SYMBOL_CHARGE, "Performance",
                    "Rendering and CPU status", UI_PAGE_PERFORMANCE);
    ui_add_app_card(screen, 544, 82, LV_SYMBOL_VIDEO, "Camera",
                    "USB UVC live preview", UI_PAGE_CAMERA);
    ui_add_app_card(screen, 24, 210, LV_SYMBOL_SETTINGS, "Settings",
                    "Appearance and system", UI_PAGE_SETTINGS);
    ui_add_app_card(screen, 284, 210, LV_SYMBOL_WIFI, "Wi-Fi",
                    "Scan and connect", UI_PAGE_WIFI);
    ui_add_app_card(screen, 544, 210, LV_SYMBOL_EYE_OPEN, "About",
                    "Software baseline and route", UI_PAGE_ABOUT);

    connectivity = lv_obj_create(screen);
    lv_obj_set_pos(connectivity, 24, 338);
    lv_obj_set_size(connectivity, 752, 114);
    ui_style_panel(connectivity);
    ui_add_label(connectivity, "Connectivity", &lv_font_montserrat_16,
                 ui_color_text(), 18, 10);
    ui_add_label(connectivity, LV_SYMBOL_WIFI "  Wi-Fi: Scan ready",
                 &lv_font_montserrat_14, lv_color_hex(0x22c55e), 190, 12);
    ui_add_label(connectivity, LV_SYMBOL_BLUETOOTH "  Bluetooth: Not available",
                 &lv_font_montserrat_14, ui_color_muted(), 448, 12);
    ui_add_label(connectivity, "M33 <-> M55 IPC: Ready / Wi-Fi runs on M55",
                 &lv_font_montserrat_14, lv_color_hex(0x22c55e), 18, 48);
    ui_add_label(connectivity, LV_SYMBOL_VIDEO "  Camera: J2 UVC host ready",
                 &lv_font_montserrat_14, lv_color_hex(0x22c55e), 18, 76);
}

static void ui_show_hardware(void)
{
    lv_obj_t *screen = ui_prepare_screen();
    lv_obj_t *panel;

    ui_add_header(screen, "Hardware", "Verified AI Kit board services");
    panel = ui_add_page_panel(screen);
    ui_add_status_row(panel, 22, "Display", "Waveshare DSI LCD(B) 800 x 480",
                      LV_SYMBOL_OK "  Ready", true);
    ui_add_status_row(panel, 88, "Touch", "FT5406 capacitive touch controller",
                      LV_SYMBOL_OK "  Ready", true);
    ui_add_status_row(panel, 154, "Renderer", "VG-Lite with double full framebuffers",
                      LV_SYMBOL_OK "  Ready", true);
    ui_add_status_row(panel, 220, "Cores", "Cortex-M33 Secure/NS + Cortex-M55 + IPC",
                      LV_SYMBOL_OK "  Running", true);
    ui_add_status_row(panel, 286, "Storage", "External Flash / PSRAM application services",
                      "Not integrated", false);
}

static bool ui_update_npu_suite(
    lv_obj_t *value_label,
    lv_obj_t *state_label,
    ai_kit_benchmark_test_t throughput_test,
    ai_kit_benchmark_test_t wall_test,
    ai_kit_benchmark_test_t cycles_test,
    ai_kit_benchmark_test_t init_test,
    ai_kit_benchmark_test_t first_test)
{
    ai_kit_benchmark_result_t result;

    (void)ai_kit_benchmark_get_result(throughput_test,
                                      AI_KIT_BENCHMARK_EXECUTOR_NPU,
                                      &result);
    if (result.state == AI_KIT_BENCHMARK_STATE_RUNNING)
    {
        lv_label_set_text(value_label, "Running 100 inferences...");
        lv_label_set_text(state_label,
                          "CM55 + Ethos-U55 / HMI remains active");
        lv_obj_set_style_text_color(value_label, ui_color_accent(),
                                    LV_PART_MAIN);
        lv_obj_set_style_text_color(state_label, ui_color_accent(),
                                    LV_PART_MAIN);
        return true;
    }
    if (result.state == AI_KIT_BENCHMARK_STATE_COMPLETE)
    {
        ai_kit_benchmark_result_t wall_latency;
        ai_kit_benchmark_result_t hardware_cycles;
        ai_kit_benchmark_result_t model_init;
        ai_kit_benchmark_result_t first_inference;
        bool details_complete;

        lv_label_set_text_fmt(value_label, "%lu.%03lu infer/s average",
                              (unsigned long)(result.metric_x1000 / 1000U),
                              (unsigned long)(result.metric_x1000 % 1000U));
        details_complete =
            (ai_kit_benchmark_get_result(wall_test,
                                         AI_KIT_BENCHMARK_EXECUTOR_NPU,
                                         &wall_latency) == RT_EOK) &&
            (ai_kit_benchmark_get_result(cycles_test,
                                         AI_KIT_BENCHMARK_EXECUTOR_NPU,
                                         &hardware_cycles) == RT_EOK) &&
            (ai_kit_benchmark_get_result(init_test,
                                         AI_KIT_BENCHMARK_EXECUTOR_NPU,
                                         &model_init) == RT_EOK) &&
            (ai_kit_benchmark_get_result(first_test,
                                         AI_KIT_BENCHMARK_EXECUTOR_NPU,
                                         &first_inference) == RT_EOK) &&
            (wall_latency.state == AI_KIT_BENCHMARK_STATE_COMPLETE) &&
            (hardware_cycles.state == AI_KIT_BENCHMARK_STATE_COMPLETE) &&
            (model_init.state == AI_KIT_BENCHMARK_STATE_COMPLETE) &&
            (first_inference.state == AI_KIT_BENCHMARK_STATE_COMPLETE) &&
            (wall_latency.sequence == result.sequence) &&
            (hardware_cycles.sequence == result.sequence) &&
            (model_init.sequence == result.sequence) &&
            (first_inference.sequence == result.sequence);
        if (details_complete)
        {
            lv_label_set_text_fmt(
                state_label,
                "hot %lu.%03lu us [%lu.%03lu, %lu.%03lu]\n"
                "first %lu.%03lu + init %lu.%03lu = cold %lu.%03lu us\n"
                "U55 %lu.%03lu kcy [%lu.%03lu, %lu.%03lu]",
                (unsigned long)(wall_latency.metric_x1000 / 1000U),
                (unsigned long)(wall_latency.metric_x1000 % 1000U),
                (unsigned long)(wall_latency.minimum_x1000 / 1000U),
                (unsigned long)(wall_latency.minimum_x1000 % 1000U),
                (unsigned long)(wall_latency.maximum_x1000 / 1000U),
                (unsigned long)(wall_latency.maximum_x1000 % 1000U),
                (unsigned long)(first_inference.metric_x1000 / 1000U),
                (unsigned long)(first_inference.metric_x1000 % 1000U),
                (unsigned long)(model_init.metric_x1000 / 1000U),
                (unsigned long)(model_init.metric_x1000 % 1000U),
                (unsigned long)((model_init.metric_x1000 +
                                 first_inference.metric_x1000) / 1000U),
                (unsigned long)((model_init.metric_x1000 +
                                 first_inference.metric_x1000) % 1000U),
                (unsigned long)(hardware_cycles.metric_x1000 / 1000U),
                (unsigned long)(hardware_cycles.metric_x1000 % 1000U),
                (unsigned long)(hardware_cycles.minimum_x1000 / 1000U),
                (unsigned long)(hardware_cycles.minimum_x1000 % 1000U),
                (unsigned long)(hardware_cycles.maximum_x1000 / 1000U),
                (unsigned long)(hardware_cycles.maximum_x1000 % 1000U));
        }
        else
        {
            lv_label_set_text(state_label,
                              "NPU detailed metric catalog incomplete");
        }
        lv_obj_set_style_text_color(value_label, lv_color_hex(0x22c55e),
                                    LV_PART_MAIN);
        lv_obj_set_style_text_color(state_label,
                                    details_complete ?
                                        lv_color_hex(0x22c55e) :
                                        lv_color_hex(0xef4444),
                                    LV_PART_MAIN);
        return false;
    }
    if (result.state == AI_KIT_BENCHMARK_STATE_ERROR)
    {
        lv_label_set_text(value_label, "NPU benchmark error");
        lv_label_set_text_fmt(state_label, "Error code %lu",
                              (unsigned long)result.error);
        lv_obj_set_style_text_color(value_label, lv_color_hex(0xef4444),
                                    LV_PART_MAIN);
        lv_obj_set_style_text_color(state_label, lv_color_hex(0xef4444),
                                    LV_PART_MAIN);
        return false;
    }

    lv_label_set_text(value_label, "Not run");
    lv_label_set_text(state_label, "5 warm-up / 100 measured / validated");
    lv_obj_set_style_text_color(value_label, ui_color_muted(), LV_PART_MAIN);
    lv_obj_set_style_text_color(state_label, ui_color_muted(), LV_PART_MAIN);
    return false;
}

static void ui_performance_timer_cb(lv_timer_t *timer)
{
    ai_kit_benchmark_result_t result;
    bool benchmark_busy = false;

    (void)timer;

    if (s_cpu_value_label != NULL)
    {
        uint32_t cpu_percent = (uint32_t)(cpu_load_average() + 0.5f);

        lv_label_set_text_fmt(s_cpu_value_label, "%lu%%", (unsigned long)cpu_percent);
    }

    if ((s_coremark_value_label == NULL) ||
        (s_coremark_state_label == NULL) ||
        (s_coremark_button == NULL) ||
        (s_m33_coremark_value_label == NULL) ||
        (s_m33_coremark_state_label == NULL) ||
        (s_ipc_value_label == NULL) ||
        (s_ipc_state_label == NULL) ||
        (s_memory_value_label == NULL) ||
        (s_m33_memory_value_label == NULL) ||
        (s_memory_button == NULL) ||
        (s_psram_value_label == NULL) ||
        (s_m33_psram_value_label == NULL) ||
        (s_psram_button == NULL) ||
        (s_person_npu_value_label == NULL) ||
        (s_person_npu_state_label == NULL) ||
        (s_person_npu_button == NULL) ||
        (s_person_peak_value_label == NULL) ||
        (s_person_peak_state_label == NULL) ||
        (s_person_peak_button == NULL))
    {
        return;
    }

    (void)ai_kit_benchmark_get_result(AI_KIT_BENCHMARK_TEST_COREMARK,
                                      AI_KIT_BENCHMARK_EXECUTOR_M55,
                                      &result);
    if (result.state == AI_KIT_BENCHMARK_STATE_RUNNING)
    {
        benchmark_busy = true;
        lv_label_set_text(s_coremark_value_label, "Running...");
        lv_label_set_text(s_coremark_state_label,
                          "Background RT-Thread worker / HMI remains active");
        lv_obj_set_style_text_color(s_coremark_state_label,
                                    ui_color_accent(), LV_PART_MAIN);
        lv_obj_add_state(s_coremark_button, LV_STATE_DISABLED);
    }
    else if (result.state == AI_KIT_BENCHMARK_STATE_COMPLETE)
    {
        lv_label_set_text_fmt(s_coremark_value_label,
                              "%lu.%03lu CoreMark/s",
                              (unsigned long)(result.metric_x1000 / 1000U),
                              (unsigned long)(result.metric_x1000 % 1000U));
        lv_label_set_text_fmt(s_coremark_state_label,
                              "Validated / %lu iterations / %lu ms",
                              (unsigned long)result.iterations,
                              (unsigned long)(result.elapsed_us / 1000U));
        lv_obj_set_style_text_color(s_coremark_state_label,
                                    lv_color_hex(0x22c55e), LV_PART_MAIN);
        lv_obj_remove_state(s_coremark_button, LV_STATE_DISABLED);
    }
    else if (result.state == AI_KIT_BENCHMARK_STATE_ERROR)
    {
        lv_label_set_text(s_coremark_value_label, "Benchmark error");
        lv_label_set_text_fmt(s_coremark_state_label, "Error code %lu",
                              (unsigned long)result.error);
        lv_obj_set_style_text_color(s_coremark_state_label,
                                    lv_color_hex(0xef4444), LV_PART_MAIN);
        lv_obj_remove_state(s_coremark_button, LV_STATE_DISABLED);
    }
    else
    {
        lv_label_set_text(s_coremark_value_label, "Not run");
        lv_label_set_text(s_coremark_state_label, "Run from this page");
        lv_obj_set_style_text_color(s_coremark_state_label,
                                    ui_color_muted(), LV_PART_MAIN);
        lv_obj_remove_state(s_coremark_button, LV_STATE_DISABLED);
    }

    (void)ai_kit_benchmark_get_result(AI_KIT_BENCHMARK_TEST_COREMARK,
                                      AI_KIT_BENCHMARK_EXECUTOR_M33,
                                      &result);
    if (result.state == AI_KIT_BENCHMARK_STATE_RUNNING)
    {
        lv_label_set_text(s_m33_coremark_value_label, "Running...");
        lv_label_set_text(s_m33_coremark_state_label,
                          "m33_coremark owns the M33 msh until complete");
        lv_obj_set_style_text_color(s_m33_coremark_state_label,
                                    ui_color_accent(), LV_PART_MAIN);
    }
    else if (result.state == AI_KIT_BENCHMARK_STATE_COMPLETE)
    {
        lv_label_set_text_fmt(s_m33_coremark_value_label,
                              "%lu.%03lu CoreMark/s",
                              (unsigned long)(result.metric_x1000 / 1000U),
                              (unsigned long)(result.metric_x1000 % 1000U));
        lv_label_set_text_fmt(s_m33_coremark_state_label,
                              "%lu iterations / %lu ms",
                              (unsigned long)result.iterations,
                              (unsigned long)(result.elapsed_us / 1000U));
        lv_obj_set_style_text_color(s_m33_coremark_state_label,
                                    lv_color_hex(0x22c55e), LV_PART_MAIN);
    }
    else if (result.state == AI_KIT_BENCHMARK_STATE_ERROR)
    {
        lv_label_set_text(s_m33_coremark_value_label, "Benchmark error");
        lv_label_set_text_fmt(s_m33_coremark_state_label, "Error code %lu",
                              (unsigned long)result.error);
        lv_obj_set_style_text_color(s_m33_coremark_state_label,
                                    lv_color_hex(0xef4444), LV_PART_MAIN);
    }
    else
    {
        lv_label_set_text(s_m33_coremark_value_label, "Not run");
        lv_label_set_text(s_m33_coremark_state_label,
                          "Run m33_coremark from msh");
        lv_obj_set_style_text_color(s_m33_coremark_state_label,
                                    ui_color_muted(), LV_PART_MAIN);
    }

    (void)ai_kit_benchmark_get_result(AI_KIT_BENCHMARK_TEST_IPC_LATENCY,
                                      AI_KIT_BENCHMARK_EXECUTOR_M33,
                                      &result);
    if (result.state == AI_KIT_BENCHMARK_STATE_COMPLETE)
    {
        lv_label_set_text_fmt(s_ipc_value_label,
                              "%lu.%03lu us average",
                              (unsigned long)(result.metric_x1000 / 1000U),
                              (unsigned long)(result.metric_x1000 % 1000U));
        lv_label_set_text_fmt(
            s_ipc_state_label,
            "min %lu.%03lu / max %lu.%03lu us / %lu samples",
            (unsigned long)(result.minimum_x1000 / 1000U),
            (unsigned long)(result.minimum_x1000 % 1000U),
            (unsigned long)(result.maximum_x1000 / 1000U),
            (unsigned long)(result.maximum_x1000 % 1000U),
            (unsigned long)result.iterations);
        lv_obj_set_style_text_color(s_ipc_state_label,
                                    lv_color_hex(0x22c55e), LV_PART_MAIN);
    }
    else if (result.state == AI_KIT_BENCHMARK_STATE_ERROR)
    {
        lv_label_set_text(s_ipc_value_label, "Benchmark error");
        lv_label_set_text_fmt(s_ipc_state_label, "Error code %lu",
                              (unsigned long)result.error);
        lv_obj_set_style_text_color(s_ipc_state_label,
                                    lv_color_hex(0xef4444), LV_PART_MAIN);
    }
    else
    {
        lv_label_set_text(s_ipc_value_label, "Not run");
        lv_label_set_text(s_ipc_state_label, "Run ipc_bench from the M33 msh");
        lv_obj_set_style_text_color(s_ipc_state_label,
                                    ui_color_muted(), LV_PART_MAIN);
    }

    (void)ai_kit_benchmark_get_result(
                                      AI_KIT_BENCHMARK_TEST_MEMORY_PORTABLE,
                                      AI_KIT_BENCHMARK_EXECUTOR_M55,
                                      &result);
    if (result.state == AI_KIT_BENCHMARK_STATE_RUNNING)
    {
        benchmark_busy = true;
        lv_label_set_text(s_memory_value_label, "Running 128 MiB per operation...");
        lv_obj_set_style_text_color(s_memory_value_label,
                                    ui_color_accent(), LV_PART_MAIN);
    }
    else if (result.state == AI_KIT_BENCHMARK_STATE_COMPLETE)
    {
        lv_label_set_text_fmt(
            s_memory_value_label,
            "M55 R %lu.%03lu / W %lu.%03lu / C %lu.%03lu MiB/s",
            (unsigned long)(result.metric_x1000 / 1000U),
            (unsigned long)(result.metric_x1000 % 1000U),
            (unsigned long)(result.minimum_x1000 / 1000U),
            (unsigned long)(result.minimum_x1000 % 1000U),
            (unsigned long)(result.maximum_x1000 / 1000U),
            (unsigned long)(result.maximum_x1000 % 1000U));
        lv_obj_set_style_text_color(s_memory_value_label,
                                    lv_color_hex(0x22c55e), LV_PART_MAIN);
    }
    else if (result.state == AI_KIT_BENCHMARK_STATE_ERROR)
    {
        lv_label_set_text_fmt(s_memory_value_label, "Scalar SRAM error %lu",
                              (unsigned long)result.error);
        lv_obj_set_style_text_color(s_memory_value_label,
                                    lv_color_hex(0xef4444), LV_PART_MAIN);
    }
    else
    {
        lv_label_set_text(s_memory_value_label, "Not run");
        lv_obj_set_style_text_color(s_memory_value_label,
                                    ui_color_muted(), LV_PART_MAIN);
    }

    (void)ai_kit_benchmark_get_result(
                                      AI_KIT_BENCHMARK_TEST_MEMORY_PORTABLE,
                                      AI_KIT_BENCHMARK_EXECUTOR_M33,
                                      &result);
    if (result.state == AI_KIT_BENCHMARK_STATE_RUNNING)
    {
        lv_label_set_text(s_m33_memory_value_label, "M33 running from msh...");
        lv_obj_set_style_text_color(s_m33_memory_value_label,
                                    ui_color_accent(), LV_PART_MAIN);
    }
    else if (result.state == AI_KIT_BENCHMARK_STATE_COMPLETE)
    {
        lv_label_set_text_fmt(
            s_m33_memory_value_label,
            "M33 R %lu.%03lu / W %lu.%03lu / C %lu.%03lu MiB/s",
            (unsigned long)(result.metric_x1000 / 1000U),
            (unsigned long)(result.metric_x1000 % 1000U),
            (unsigned long)(result.minimum_x1000 / 1000U),
            (unsigned long)(result.minimum_x1000 % 1000U),
            (unsigned long)(result.maximum_x1000 / 1000U),
            (unsigned long)(result.maximum_x1000 % 1000U));
        lv_obj_set_style_text_color(s_m33_memory_value_label,
                                    lv_color_hex(0x22c55e), LV_PART_MAIN);
    }
    else if (result.state == AI_KIT_BENCHMARK_STATE_ERROR)
    {
        lv_label_set_text_fmt(s_m33_memory_value_label,
                              "M33 scalar SRAM error %lu",
                              (unsigned long)result.error);
        lv_obj_set_style_text_color(s_m33_memory_value_label,
                                    lv_color_hex(0xef4444), LV_PART_MAIN);
    }
    else
    {
        lv_label_set_text(s_m33_memory_value_label,
                          "M33 not run (m33_sram_scalar)");
        lv_obj_set_style_text_color(s_m33_memory_value_label,
                                    ui_color_muted(), LV_PART_MAIN);
    }

    (void)ai_kit_benchmark_get_result(
                                      AI_KIT_BENCHMARK_TEST_PSRAM_PORTABLE,
                                      AI_KIT_BENCHMARK_EXECUTOR_M55,
                                      &result);
    if (result.state == AI_KIT_BENCHMARK_STATE_RUNNING)
    {
        benchmark_busy = true;
        lv_label_set_text(s_psram_value_label,
                          "M55 running / data will be restored...");
        lv_obj_set_style_text_color(s_psram_value_label,
                                    ui_color_accent(), LV_PART_MAIN);
    }
    else if (result.state == AI_KIT_BENCHMARK_STATE_COMPLETE)
    {
        lv_label_set_text_fmt(
            s_psram_value_label,
            "M55 R %lu.%03lu / W %lu.%03lu / C %lu.%03lu MiB/s",
            (unsigned long)(result.metric_x1000 / 1000U),
            (unsigned long)(result.metric_x1000 % 1000U),
            (unsigned long)(result.minimum_x1000 / 1000U),
            (unsigned long)(result.minimum_x1000 % 1000U),
            (unsigned long)(result.maximum_x1000 / 1000U),
            (unsigned long)(result.maximum_x1000 % 1000U));
        lv_obj_set_style_text_color(s_psram_value_label,
                                    lv_color_hex(0x22c55e), LV_PART_MAIN);
    }
    else if (result.state == AI_KIT_BENCHMARK_STATE_ERROR)
    {
        lv_label_set_text_fmt(s_psram_value_label, "M55 PSRAM error %lu",
                              (unsigned long)result.error);
        lv_obj_set_style_text_color(s_psram_value_label,
                                    lv_color_hex(0xef4444), LV_PART_MAIN);
    }
    else
    {
        lv_label_set_text(s_psram_value_label, "M55 not run");
        lv_obj_set_style_text_color(s_psram_value_label,
                                    ui_color_muted(), LV_PART_MAIN);
    }

    (void)ai_kit_benchmark_get_result(
                                      AI_KIT_BENCHMARK_TEST_PSRAM_PORTABLE,
                                      AI_KIT_BENCHMARK_EXECUTOR_M33,
                                      &result);
    if (result.state == AI_KIT_BENCHMARK_STATE_RUNNING)
    {
        lv_label_set_text(s_m33_psram_value_label,
                          "M33 running from msh...");
        lv_obj_set_style_text_color(s_m33_psram_value_label,
                                    ui_color_accent(), LV_PART_MAIN);
    }
    else if (result.state == AI_KIT_BENCHMARK_STATE_COMPLETE)
    {
        lv_label_set_text_fmt(
            s_m33_psram_value_label,
            "M33 R %lu.%03lu / W %lu.%03lu / C %lu.%03lu MiB/s",
            (unsigned long)(result.metric_x1000 / 1000U),
            (unsigned long)(result.metric_x1000 % 1000U),
            (unsigned long)(result.minimum_x1000 / 1000U),
            (unsigned long)(result.minimum_x1000 % 1000U),
            (unsigned long)(result.maximum_x1000 / 1000U),
            (unsigned long)(result.maximum_x1000 % 1000U));
        lv_obj_set_style_text_color(s_m33_psram_value_label,
                                    lv_color_hex(0x22c55e), LV_PART_MAIN);
    }
    else if (result.state == AI_KIT_BENCHMARK_STATE_ERROR)
    {
        lv_label_set_text_fmt(s_m33_psram_value_label,
                              "M33 PSRAM error %lu",
                              (unsigned long)result.error);
        lv_obj_set_style_text_color(s_m33_psram_value_label,
                                    lv_color_hex(0xef4444), LV_PART_MAIN);
    }
    else
    {
        lv_label_set_text(s_m33_psram_value_label,
                          "M33 not run (m33_psram_scalar)");
        lv_obj_set_style_text_color(s_m33_psram_value_label,
                                    ui_color_muted(), LV_PART_MAIN);
    }

    benchmark_busy |= ui_update_npu_suite(
        s_person_npu_value_label, s_person_npu_state_label,
        AI_KIT_BENCHMARK_TEST_NPU_PERSON_INFERENCE,
        AI_KIT_BENCHMARK_TEST_NPU_PERSON_WALL_LATENCY,
        AI_KIT_BENCHMARK_TEST_NPU_PERSON_HARDWARE_CYCLES,
        AI_KIT_BENCHMARK_TEST_NPU_PERSON_MODEL_INIT,
        AI_KIT_BENCHMARK_TEST_NPU_PERSON_FIRST_INFERENCE);
    benchmark_busy |= ui_update_npu_suite(
        s_person_peak_value_label, s_person_peak_state_label,
        AI_KIT_BENCHMARK_TEST_NPU_PERSON_PEAK_INFERENCE,
        AI_KIT_BENCHMARK_TEST_NPU_PERSON_PEAK_WALL_LATENCY,
        AI_KIT_BENCHMARK_TEST_NPU_PERSON_PEAK_HARDWARE_CYCLES,
        AI_KIT_BENCHMARK_TEST_NPU_PERSON_PEAK_MODEL_INIT,
        AI_KIT_BENCHMARK_TEST_NPU_PERSON_PEAK_FIRST_INFERENCE);

    if (benchmark_busy)
    {
        lv_obj_add_state(s_coremark_button, LV_STATE_DISABLED);
        lv_obj_add_state(s_memory_button, LV_STATE_DISABLED);
        lv_obj_add_state(s_psram_button, LV_STATE_DISABLED);
        lv_obj_add_state(s_person_npu_button, LV_STATE_DISABLED);
        lv_obj_add_state(s_person_peak_button, LV_STATE_DISABLED);
    }
    else
    {
        lv_obj_remove_state(s_coremark_button, LV_STATE_DISABLED);
        lv_obj_remove_state(s_memory_button, LV_STATE_DISABLED);
        lv_obj_remove_state(s_psram_button, LV_STATE_DISABLED);
        lv_obj_remove_state(s_person_npu_button, LV_STATE_DISABLED);
        lv_obj_remove_state(s_person_peak_button, LV_STATE_DISABLED);
    }
}

static void ui_benchmark_start_event(lv_event_t *event)
{
    RT_UNUSED(event);

    (void)ai_kit_benchmark_start(AI_KIT_BENCHMARK_TEST_COREMARK);
    ui_performance_timer_cb(NULL);
}

static void ui_memory_benchmark_start_event(lv_event_t *event)
{
    RT_UNUSED(event);

    (void)ai_kit_benchmark_start(AI_KIT_BENCHMARK_TEST_MEMORY_PORTABLE);
    ui_performance_timer_cb(NULL);
}

static void ui_psram_benchmark_start_event(lv_event_t *event)
{
    RT_UNUSED(event);

    (void)ai_kit_benchmark_start(AI_KIT_BENCHMARK_TEST_PSRAM_PORTABLE);
    ui_performance_timer_cb(NULL);
}

static void ui_person_npu_benchmark_start_event(lv_event_t *event)
{
    RT_UNUSED(event);

    (void)ai_kit_benchmark_start(
        AI_KIT_BENCHMARK_TEST_NPU_PERSON_INFERENCE);
    ui_performance_timer_cb(NULL);
}

static void ui_person_peak_benchmark_start_event(lv_event_t *event)
{
    RT_UNUSED(event);

    (void)ai_kit_benchmark_start(
        AI_KIT_BENCHMARK_TEST_NPU_PERSON_PEAK_INFERENCE);
    ui_performance_timer_cb(NULL);
}

static void ui_show_performance(void)
{
    lv_obj_t *screen = ui_prepare_screen();
    lv_obj_t *panel;

    ui_add_header(screen, "Performance", "Low-overhead runtime diagnostics");
    panel = ui_add_page_panel(screen);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(panel, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(panel, LV_SCROLLBAR_MODE_AUTO);

    ui_add_label(panel, "CM55 CPU load", &lv_font_montserrat_16,
                 ui_color_text(), 24, 12);
    ui_add_label(panel, "Updated once per second", &lv_font_montserrat_14,
                 ui_color_muted(), 24, 35);
    s_cpu_value_label = ui_add_label(panel, "--%", &lv_font_montserrat_24,
                                     ui_color_accent(), 620, 16);
    lv_obj_set_width(s_cpu_value_label, 100);
    lv_obj_set_style_text_align(s_cpu_value_label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);

    ui_add_label(panel, "EEMBC CoreMark 1.0", &lv_font_montserrat_16,
                 ui_color_text(), 24, 62);
    ui_add_label(panel, "CM55 / Release -O2 / RT-Thread and HMI remain active",
                 &lv_font_montserrat_14, ui_color_muted(), 24, 83);
    s_coremark_value_label = ui_add_label(panel, "Not run",
                                           &lv_font_montserrat_20,
                                           ui_color_accent(), 24, 104);
    s_coremark_state_label = ui_add_label(panel, "",
                                           &lv_font_montserrat_14,
                                           ui_color_muted(), 264, 109);

    s_coremark_button = lv_button_create(panel);
    lv_obj_set_pos(s_coremark_button, 560, 66);
    lv_obj_set_size(s_coremark_button, 152, 44);
    ui_style_panel(s_coremark_button);
    lv_obj_set_style_bg_color(s_coremark_button, ui_color_accent(), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_coremark_button, lv_color_hex(0x0284c7),
                              LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_add_event_cb(s_coremark_button, ui_benchmark_start_event,
                        LV_EVENT_CLICKED, NULL);
    ui_add_label(s_coremark_button, LV_SYMBOL_PLAY "  Run CoreMark",
                 &lv_font_montserrat_14,
                 lv_color_hex(0xffffff), 13, 13);

    ui_add_label(panel, "M33", &lv_font_montserrat_16,
                 ui_color_text(), 24, 140);
    s_m33_coremark_value_label = ui_add_label(panel, "Not run",
                                               &lv_font_montserrat_20,
                                               ui_color_accent(), 88, 136);
    s_m33_coremark_state_label = ui_add_label(panel, "",
                                               &lv_font_montserrat_14,
                                               ui_color_muted(), 336, 141);

    ui_add_label(panel, "M33 <-> M55 IPC round-trip latency",
                 &lv_font_montserrat_16, ui_color_text(), 24, 170);
    s_ipc_value_label = ui_add_label(panel, "Not run",
                                     &lv_font_montserrat_20,
                                     ui_color_accent(), 24, 192);
    s_ipc_state_label = ui_add_label(panel, "",
                                     &lv_font_montserrat_14,
                                     ui_color_muted(), 280, 197);

    ui_add_label(panel,
                 "Portable scalar SRAM / 32-bit / 64 KiB / 128 MiB per op",
                 &lv_font_montserrat_14, ui_color_text(), 24, 226);
    s_memory_value_label = ui_add_label(panel, "Not run",
                                         &lv_font_montserrat_14,
                                         ui_color_muted(), 24, 247);
    lv_obj_set_width(s_memory_value_label, 520);
    s_m33_memory_value_label = ui_add_label(panel,
                                             "M33 not run (m33_sram_scalar)",
                                             &lv_font_montserrat_14,
                                             ui_color_muted(), 24, 267);
    lv_obj_set_width(s_m33_memory_value_label, 520);

    s_memory_button = lv_button_create(panel);
    lv_obj_set_pos(s_memory_button, 560, 230);
    lv_obj_set_size(s_memory_button, 152, 44);
    ui_style_panel(s_memory_button);
    lv_obj_set_style_bg_color(s_memory_button, ui_color_accent(), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_memory_button, lv_color_hex(0x0284c7),
                              LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_add_event_cb(s_memory_button, ui_memory_benchmark_start_event,
                        LV_EVENT_CLICKED, NULL);
    ui_add_label(s_memory_button, LV_SYMBOL_PLAY "  Run scalar",
                 &lv_font_montserrat_14,
                 lv_color_hex(0xffffff), 18, 13);

    ui_add_label(panel,
                 "Private PSRAM / restore-safe / same scalar kernel",
                 &lv_font_montserrat_14, ui_color_text(), 24, 296);
    s_psram_value_label = ui_add_label(panel, "M55 not run",
                                        &lv_font_montserrat_14,
                                        ui_color_muted(), 24, 317);
    lv_obj_set_width(s_psram_value_label, 520);
    s_m33_psram_value_label = ui_add_label(
        panel, "M33 not run (m33_psram_scalar)",
        &lv_font_montserrat_14, ui_color_muted(), 24, 337);
    lv_obj_set_width(s_m33_psram_value_label, 520);

    s_psram_button = lv_button_create(panel);
    lv_obj_set_pos(s_psram_button, 560, 300);
    lv_obj_set_size(s_psram_button, 152, 44);
    ui_style_panel(s_psram_button);
    lv_obj_set_style_bg_color(s_psram_button, ui_color_accent(), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_psram_button, lv_color_hex(0x0284c7),
                              LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_add_event_cb(s_psram_button, ui_psram_benchmark_start_event,
                        LV_EVENT_CLICKED, NULL);
    ui_add_label(s_psram_button, LV_SYMBOL_PLAY "  Run PSRAM",
                 &lv_font_montserrat_14,
                 lv_color_hex(0xffffff), 15, 13);

    ui_add_label(panel,
                 "Ethos-U55 Person Detection / 96x96x1 / M 239744 / A 80896 B",
                 &lv_font_montserrat_14, ui_color_text(), 24, 366);
    ui_add_label(panel, "Active", &lv_font_montserrat_14,
                 ui_color_accent(), 24, 392);
    s_person_npu_value_label = ui_add_label(panel, "Not run",
                                             &lv_font_montserrat_20,
                                             ui_color_muted(), 92, 386);
    s_person_npu_state_label = ui_add_label(
        panel, "2 golden inputs / 5 warm-up / 100 measured",
        &lv_font_montserrat_14, ui_color_muted(), 24, 415);
    lv_obj_set_width(s_person_npu_state_label, 520);

    s_person_npu_button = lv_button_create(panel);
    lv_obj_set_pos(s_person_npu_button, 560, 374);
    lv_obj_set_size(s_person_npu_button, 152, 44);
    ui_style_panel(s_person_npu_button);
    lv_obj_set_style_bg_color(s_person_npu_button, ui_color_accent(),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_person_npu_button, lv_color_hex(0x0284c7),
                              LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_add_event_cb(s_person_npu_button,
                        ui_person_npu_benchmark_start_event,
                        LV_EVENT_CLICKED, NULL);
    ui_add_label(s_person_npu_button, LV_SYMBOL_PLAY "  Active",
                 &lv_font_montserrat_14,
                 lv_color_hex(0xffffff), 29, 13);

    ui_add_label(panel, "Priority peak", &lv_font_montserrat_14,
                 lv_color_hex(0xa78bfa), 24, 476);
    s_person_peak_value_label = ui_add_label(panel, "Not run",
                                              &lv_font_montserrat_20,
                                              ui_color_muted(), 126, 470);
    s_person_peak_state_label = ui_add_label(
        panel, "priority 8 / 2 golden inputs / 100 measured",
        &lv_font_montserrat_14, ui_color_muted(), 24, 499);
    lv_obj_set_width(s_person_peak_state_label, 520);

    s_person_peak_button = lv_button_create(panel);
    lv_obj_set_pos(s_person_peak_button, 560, 458);
    lv_obj_set_size(s_person_peak_button, 152, 44);
    ui_style_panel(s_person_peak_button);
    lv_obj_set_style_bg_color(s_person_peak_button, lv_color_hex(0x8b5cf6),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_person_peak_button, lv_color_hex(0x7c3aed),
                              LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_add_event_cb(s_person_peak_button,
                        ui_person_peak_benchmark_start_event,
                        LV_EVENT_CLICKED, NULL);
    ui_add_label(s_person_peak_button, LV_SYMBOL_PLAY "  Peak",
                 &lv_font_montserrat_14,
                 lv_color_hex(0xffffff), 35, 13);

    ui_performance_timer_cb(NULL);
    s_performance_timer = lv_timer_create(ui_performance_timer_cb, 1000, NULL);
}

static void ui_theme_event(lv_event_t *event)
{
    lv_obj_t *theme_switch = lv_event_get_target(event);

    s_dark_mode = lv_obj_has_state(theme_switch, LV_STATE_CHECKED);
    lv_async_call(ui_show_page_async, (void *)(uintptr_t)UI_PAGE_SETTINGS);
}

static const char *ui_wifi_state_text(ai_kit_wifi_state_t state)
{
    switch (state)
    {
    case AI_KIT_WIFI_STATE_SCANNING:
        return "Scanning";
    case AI_KIT_WIFI_STATE_READY:
        return "Ready";
    case AI_KIT_WIFI_STATE_ERROR:
        return "Error";
    case AI_KIT_WIFI_STATE_STARTING:
    default:
        return "Starting";
    }
}

static void ui_wifi_close_dialog(void)
{
    if (s_wifi_password != NULL)
    {
        lv_textarea_set_text(s_wifi_password, "");
    }
    if (s_wifi_dialog != NULL)
    {
        lv_obj_delete_async(s_wifi_dialog);
    }
    s_wifi_dialog = NULL;
    s_wifi_password = NULL;
}

static void ui_wifi_submit_connection(void)
{
    rt_err_t result;

    if (s_wifi_password == NULL)
    {
        return;
    }

    result = ai_kit_wifi_connect_async(
        s_wifi_selected_ssid,
        lv_textarea_get_text(s_wifi_password));
    ui_wifi_close_dialog();
    if ((result != RT_EOK) && (s_wifi_state_label != NULL))
    {
        lv_label_set_text_fmt(s_wifi_state_label,
                              "Connect request failed: %d", (int)result);
    }
}

static void ui_wifi_cancel_event(lv_event_t *event)
{
    (void)event;
    ui_wifi_close_dialog();
}

static void ui_wifi_connect_event(lv_event_t *event)
{
    (void)event;
    ui_wifi_submit_connection();
}

static void ui_wifi_keyboard_event(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);

    if (code == LV_EVENT_CANCEL)
    {
        ui_wifi_close_dialog();
        return;
    }
    if ((code != LV_EVENT_READY) || (s_wifi_password == NULL))
    {
        return;
    }
    ui_wifi_submit_connection();
}

static void ui_wifi_network_event(lv_event_t *event)
{
    uint32_t index = (uint32_t)(uintptr_t)lv_event_get_user_data(event);
    lv_obj_t *keyboard;
    lv_obj_t *cancel_button;
    lv_obj_t *connect_button;

    if ((index >= s_wifi_page_snapshot.visible_count) ||
        (s_wifi_dialog != NULL))
    {
        return;
    }

    rt_memset(s_wifi_selected_ssid, 0, sizeof(s_wifi_selected_ssid));
    rt_memcpy(s_wifi_selected_ssid,
              s_wifi_page_snapshot.networks[index].ssid,
              rt_strlen(s_wifi_page_snapshot.networks[index].ssid));

    s_wifi_dialog = lv_obj_create(lv_screen_active());
    lv_obj_set_pos(s_wifi_dialog, 12, 8);
    lv_obj_set_size(s_wifi_dialog, 776, 464);
    ui_style_panel(s_wifi_dialog);
    lv_obj_set_style_bg_color(s_wifi_dialog, ui_color_surface(), LV_PART_MAIN);

    ui_add_label(s_wifi_dialog, "Connect to", &lv_font_montserrat_14,
                 ui_color_muted(), 18, 12);
    ui_add_label(s_wifi_dialog, s_wifi_selected_ssid,
                 &lv_font_montserrat_20, ui_color_text(), 120, 8);

    s_wifi_password = lv_textarea_create(s_wifi_dialog);
    lv_obj_set_pos(s_wifi_password, 18, 44);
    lv_obj_set_size(s_wifi_password, 740, 44);
    lv_textarea_set_one_line(s_wifi_password, true);
    lv_textarea_set_password_mode(s_wifi_password, true);
    lv_textarea_set_max_length(s_wifi_password,
                               AI_KIT_WIFI_PASSWORD_SIZE - 1U);
    lv_textarea_set_placeholder_text(s_wifi_password,
                                     "Password (leave empty for open AP)");

    keyboard = lv_keyboard_create(s_wifi_dialog);
    lv_obj_set_size(keyboard, 740, 230);
    lv_obj_align_to(keyboard, s_wifi_password,
                    LV_ALIGN_OUT_BOTTOM_MID, 0, 6);
    lv_keyboard_set_textarea(keyboard, s_wifi_password);
    lv_obj_add_event_cb(keyboard, ui_wifi_keyboard_event,
                        LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(keyboard, ui_wifi_keyboard_event,
                        LV_EVENT_CANCEL, NULL);

    cancel_button = lv_button_create(s_wifi_dialog);
    lv_obj_set_pos(cancel_button, 18, 346);
    lv_obj_set_size(cancel_button, 190, 54);
    ui_style_panel(cancel_button);
    lv_obj_set_style_bg_color(cancel_button, ui_color_surface_pressed(),
                              LV_PART_MAIN);
    lv_obj_add_event_cb(cancel_button, ui_wifi_cancel_event,
                        LV_EVENT_CLICKED, NULL);
    ui_add_label(cancel_button, LV_SYMBOL_CLOSE "  Cancel",
                 &lv_font_montserrat_16, ui_color_text(), 48, 17);

    connect_button = lv_button_create(s_wifi_dialog);
    lv_obj_set_pos(connect_button, 548, 346);
    lv_obj_set_size(connect_button, 210, 54);
    ui_style_panel(connect_button);
    lv_obj_set_style_bg_color(connect_button, ui_color_accent(),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_color(connect_button, lv_color_hex(0x0284c7),
                              LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_add_event_cb(connect_button, ui_wifi_connect_event,
                        LV_EVENT_CLICKED, NULL);
    ui_add_label(connect_button, LV_SYMBOL_OK "  Connect",
                 &lv_font_montserrat_16, lv_color_hex(0xffffff), 52, 17);
}

static void ui_wifi_timer_cb(lv_timer_t *timer)
{
    ai_kit_wifi_snapshot_t snapshot;
    char row[96];
    uint32_t index;

    (void)timer;
    if ((s_wifi_state_label == NULL) || (s_wifi_count_label == NULL) ||
        (s_wifi_list == NULL) || (s_wifi_refresh_button == NULL))
    {
        return;
    }

    if (ai_kit_wifi_get_snapshot(&snapshot) != RT_EOK)
    {
        lv_label_set_text(s_wifi_state_label, "Service unavailable");
        return;
    }

    s_wifi_page_snapshot = snapshot;

    if (snapshot.connection_state == AI_KIT_WIFI_CONNECTION_CONNECTED)
    {
        lv_label_set_text_fmt(s_wifi_state_label, "Connected: %s / %s",
                              snapshot.connected_ssid,
                              snapshot.ipv4_address);
    }
    else if (snapshot.connection_state == AI_KIT_WIFI_CONNECTION_CONNECTING)
    {
        lv_label_set_text_fmt(s_wifi_state_label, "Connecting to %s",
                              snapshot.connected_ssid);
    }
    else if (snapshot.connection_state == AI_KIT_WIFI_CONNECTION_ERROR)
    {
        if (snapshot.connection_result == -RT_ETIMEOUT)
        {
            lv_label_set_text(s_wifi_state_label,
                              "Connected to AP, but DHCP timed out");
        }
        else if ((uint32_t)snapshot.connection_result == 0x020003EEUL)
        {
            lv_label_set_text(s_wifi_state_label,
                              "Authentication failed: check WPA2 password");
        }
        else
        {
            lv_label_set_text_fmt(s_wifi_state_label,
                                  "Connection failed: %d",
                                  (int)snapshot.connection_result);
        }
    }
    else
    {
        lv_label_set_text_fmt(s_wifi_state_label, "%s / result %d",
                              ui_wifi_state_text(snapshot.state),
                              (int)snapshot.result);
    }
    lv_label_set_text_fmt(s_wifi_count_label, "%lu AP reports / %lu networks shown",
                          (unsigned long)snapshot.total_count,
                          (unsigned long)snapshot.visible_count);

    if ((snapshot.state == AI_KIT_WIFI_STATE_SCANNING) ||
        (snapshot.connection_state ==
         AI_KIT_WIFI_CONNECTION_CONNECTING))
    {
        lv_obj_add_state(s_wifi_refresh_button, LV_STATE_DISABLED);
    }
    else
    {
        lv_obj_remove_state(s_wifi_refresh_button, LV_STATE_DISABLED);
    }

    if (snapshot.generation == s_wifi_generation)
    {
        return;
    }
    s_wifi_generation = snapshot.generation;
    lv_obj_clean(s_wifi_list);
    if (snapshot.visible_count == 0U)
    {
        lv_list_add_text(s_wifi_list, "No access points found");
        return;
    }

    for (index = 0U; index < snapshot.visible_count; index++)
    {
        rt_snprintf(row, sizeof(row), "%s    %ld dBm    ch %lu",
                    snapshot.networks[index].ssid,
                    (long)snapshot.networks[index].rssi,
                    (unsigned long)snapshot.networks[index].channel);
        lv_obj_t *network_button = lv_list_add_button(
            s_wifi_list, LV_SYMBOL_WIFI, row);
        lv_obj_add_event_cb(network_button, ui_wifi_network_event,
                            LV_EVENT_CLICKED,
                            (void *)(uintptr_t)index);
    }
}

static void ui_wifi_scan_event(lv_event_t *event)
{
    rt_err_t result;

    (void)event;
    result = ai_kit_wifi_scan_async();
    if ((result != RT_EOK) && (s_wifi_state_label != NULL))
    {
        lv_label_set_text_fmt(s_wifi_state_label,
                              "Scan request failed: %d", (int)result);
    }
    ui_wifi_timer_cb(NULL);
}

static void ui_show_wifi(void)
{
    lv_obj_t *screen = ui_prepare_screen();
    lv_obj_t *panel;

    ui_add_header(screen, "Wi-Fi", "AI Kit CYW55513 / RT-Thread WLAN scan");
    panel = ui_add_page_panel(screen);

    s_wifi_state_label = ui_add_label(panel, "Starting",
                                      &lv_font_montserrat_16,
                                      ui_color_text(), 24, 18);
    s_wifi_count_label = ui_add_label(panel, "Waiting for first scan",
                                      &lv_font_montserrat_14,
                                      ui_color_muted(), 24, 48);

    s_wifi_refresh_button = lv_button_create(panel);
    lv_obj_set_pos(s_wifi_refresh_button, 570, 16);
    lv_obj_set_size(s_wifi_refresh_button, 150, 48);
    ui_style_panel(s_wifi_refresh_button);
    lv_obj_set_style_bg_color(s_wifi_refresh_button, ui_color_accent(),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_wifi_refresh_button, lv_color_hex(0x0284c7),
                              LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_add_event_cb(s_wifi_refresh_button, ui_wifi_scan_event,
                        LV_EVENT_CLICKED, NULL);
    ui_add_label(s_wifi_refresh_button, LV_SYMBOL_REFRESH "  Scan",
                 &lv_font_montserrat_14, lv_color_hex(0xffffff), 35, 15);

    s_wifi_list = lv_list_create(panel);
    lv_obj_set_pos(s_wifi_list, 24, 82);
    lv_obj_set_size(s_wifi_list, 696, 250);
    lv_obj_set_style_bg_color(s_wifi_list, ui_color_background(), LV_PART_MAIN);
    lv_obj_set_style_border_color(s_wifi_list, ui_color_border(), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_wifi_list, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(s_wifi_list, 10, LV_PART_MAIN);

    ui_wifi_timer_cb(NULL);
    s_wifi_timer = lv_timer_create(ui_wifi_timer_cb, 500, NULL);
}

static const char *ui_camera_state_text(ai_kit_camera_state_t state)
{
    switch (state)
    {
    case AI_KIT_CAMERA_WAITING:
        return "Connected / Waiting";
    case AI_KIT_CAMERA_STREAMING:
        return "Streaming";
    case AI_KIT_CAMERA_PAUSED:
        return "Paused";
    case AI_KIT_CAMERA_ERROR:
        return "Error";
    case AI_KIT_CAMERA_DISCONNECTED:
    default:
        return "Disconnected";
    }
}

static void ui_camera_timer_cb(lv_timer_t *timer)
{
    ai_kit_camera_status_t status;
    ai_kit_camera_frame_t next_frame;

    (void)timer;
    if ((s_camera_image == NULL) || (s_camera_state_label == NULL))
    {
        return;
    }

    ai_kit_camera_get_status(&status);
    lv_label_set_text(s_camera_state_label, ui_camera_state_text(status.state));
    lv_label_set_text_fmt(s_camera_device_label, "VID %04X / PID %04X",
                          status.vendor_id, status.product_id);
    lv_obj_set_style_text_color(
        s_camera_state_label,
        (status.state == AI_KIT_CAMERA_STREAMING) ? lv_color_hex(0x22c55e) :
        (status.state == AI_KIT_CAMERA_ERROR) ? lv_color_hex(0xef4444) :
                                                ui_color_muted(),
        LV_PART_MAIN);
    lv_label_set_text_fmt(s_camera_capture_label, "Capture  %lu FPS",
                          (unsigned long)status.capture_fps);
    lv_label_set_text_fmt(s_camera_display_label, "Display  %lu FPS",
                          (unsigned long)status.display_fps);
    lv_label_set_text_fmt(s_camera_drop_label,
                          "Skipped %lu / USB errors %lu / Dropped %lu",
                          (unsigned long)status.skipped_frames,
                          (unsigned long)status.transfer_errors,
                          (unsigned long)status.dropped_frames);
    lv_label_set_text(s_camera_pause_label,
                      (status.state == AI_KIT_CAMERA_PAUSED) ? "Resume" : "Pause");

    if (s_camera_overlay != NULL)
    {
        if (status.state == AI_KIT_CAMERA_STREAMING)
        {
            lv_obj_add_flag(s_camera_overlay, LV_OBJ_FLAG_HIDDEN);
        }
        else
        {
            lv_label_set_text(s_camera_overlay, ui_camera_state_text(status.state));
            lv_obj_clear_flag(s_camera_overlay, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (ai_kit_camera_acquire_latest(&next_frame))
    {
        ai_kit_camera_frame_t previous_frame = s_camera_frame;
        bool previous_frame_held = s_camera_frame_held;

        RT_ASSERT(next_frame.buffer_index < AI_KIT_CAMERA_RGB_BUFFER_COUNT);
        if (s_camera_image_dsc[next_frame.buffer_index].data == NULL)
        {
            s_camera_image_dsc[next_frame.buffer_index].data =
                (const uint8_t *)next_frame.pixels;
        }
        RT_ASSERT(s_camera_image_dsc[next_frame.buffer_index].data ==
                  (const uint8_t *)next_frame.pixels);
        s_camera_frame = next_frame;
        s_camera_frame_held = true;
        lv_image_set_src(s_camera_image,
                         &s_camera_image_dsc[next_frame.buffer_index]);
        lv_obj_invalidate(s_camera_image);
        /* Keep the previous buffer protected until LVGL points at the new one. */
        if (previous_frame_held)
        {
            ai_kit_camera_release(&previous_frame);
        }
    }
}

static void ui_camera_pause_event(lv_event_t *event)
{
    ai_kit_camera_status_t status;

    (void)event;
    ai_kit_camera_get_status(&status);
    if (status.state == AI_KIT_CAMERA_PAUSED)
    {
        (void)ai_kit_camera_resume();
    }
    else
    {
        (void)ai_kit_camera_pause();
    }
    ui_camera_timer_cb(NULL);
}

static void ui_show_camera(void)
{
    lv_obj_t *screen = ui_prepare_screen();
    lv_obj_t *panel;
    lv_obj_t *preview;
    lv_obj_t *pause_button;
    uint32_t image_index;

    ui_add_header(screen, "Camera", "AI Kit J2 USB UVC live preview");
    panel = ui_add_page_panel(screen);

    preview = lv_obj_create(panel);
    lv_obj_set_pos(preview, 0, 0);
    lv_obj_set_size(preview, 480, 360);
    lv_obj_set_style_bg_color(preview, lv_color_hex(0x05070a), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(preview, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(preview, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(preview, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(preview, 0, LV_PART_MAIN);
    lv_obj_remove_flag(preview, LV_OBJ_FLAG_SCROLLABLE);

    rt_memset(s_camera_image_dsc, 0, sizeof(s_camera_image_dsc));
    for (image_index = 0U;
         image_index < AI_KIT_CAMERA_RGB_BUFFER_COUNT;
         image_index++)
    {
        s_camera_image_dsc[image_index].header.magic = LV_IMAGE_HEADER_MAGIC;
        s_camera_image_dsc[image_index].header.cf = LV_COLOR_FORMAT_RGB565;
        s_camera_image_dsc[image_index].header.flags =
            LV_IMAGE_FLAGS_MODIFIABLE;
        s_camera_image_dsc[image_index].header.w = AI_KIT_CAMERA_WIDTH;
        s_camera_image_dsc[image_index].header.h = AI_KIT_CAMERA_HEIGHT;
        s_camera_image_dsc[image_index].header.stride = AI_KIT_CAMERA_STRIDE;
        s_camera_image_dsc[image_index].data_size = AI_KIT_CAMERA_FRAME_BYTES;
    }

    s_camera_image = lv_image_create(preview);
    lv_obj_set_pos(s_camera_image, 0, 0);
    lv_image_set_pivot(s_camera_image, 0, 0);
    lv_image_set_scale(s_camera_image, 384U);

    s_camera_overlay = ui_add_label(preview, "Waiting for camera",
                                    &lv_font_montserrat_20,
                                    lv_color_hex(0xcbd5e1), 0, 165);
    lv_obj_set_width(s_camera_overlay, 480);
    lv_obj_set_style_text_align(s_camera_overlay, LV_TEXT_ALIGN_CENTER,
                                LV_PART_MAIN);

    ui_add_label(panel, "Status", &lv_font_montserrat_14,
                 ui_color_muted(), 504, 20);
    s_camera_state_label = ui_add_label(panel, "Disconnected",
                                        &lv_font_montserrat_16,
                                        ui_color_muted(), 504, 43);
    s_camera_device_label = ui_add_label(panel, "VID ---- / PID ----",
                                         &lv_font_montserrat_12,
                                         ui_color_muted(), 504, 78);
    ui_add_label(panel, "320 x 240  YUYV", &lv_font_montserrat_14,
                 ui_color_text(), 504, 105);
    s_camera_capture_label = ui_add_label(panel, "Capture  0 FPS",
                                          &lv_font_montserrat_14,
                                          ui_color_text(), 504, 141);
    s_camera_display_label = ui_add_label(panel, "Display  0 FPS",
                                          &lv_font_montserrat_14,
                                          ui_color_text(), 504, 171);
    s_camera_drop_label = ui_add_label(panel, "Skipped 0 / Dropped 0",
                                       &lv_font_montserrat_12,
                                       ui_color_muted(), 504, 207);
    lv_obj_set_width(s_camera_drop_label, 232);

    pause_button = lv_button_create(panel);
    lv_obj_set_pos(pause_button, 504, 286);
    lv_obj_set_size(pause_button, 216, 48);
    ui_style_panel(pause_button);
    lv_obj_set_style_bg_color(pause_button, ui_color_accent(), LV_PART_MAIN);
    lv_obj_set_style_bg_color(pause_button, lv_color_hex(0x0284c7),
                              LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_add_event_cb(pause_button, ui_camera_pause_event,
                        LV_EVENT_CLICKED, NULL);
    s_camera_pause_label = ui_add_label(pause_button, "Pause",
                                        &lv_font_montserrat_16,
                                        lv_color_hex(0x082f49), 0, 14);
    lv_obj_set_width(s_camera_pause_label, 216);
    lv_obj_set_style_text_align(s_camera_pause_label, LV_TEXT_ALIGN_CENTER,
                                LV_PART_MAIN);

    (void)ai_kit_camera_start();
    ui_camera_timer_cb(NULL);
    s_camera_timer = lv_timer_create(ui_camera_timer_cb, 100U, NULL);
}

static void ui_add_disabled_switch(lv_obj_t *parent, int32_t y)
{
    lv_obj_t *toggle = lv_switch_create(parent);

    lv_obj_set_pos(toggle, 650, y);
    lv_obj_set_size(toggle, 58, 30);
    lv_obj_add_state(toggle, LV_STATE_DISABLED);
}

static void ui_show_settings(void)
{
    lv_obj_t *screen = ui_prepare_screen();
    lv_obj_t *panel;
    lv_obj_t *theme_switch;
    lv_obj_t *wifi_button;

    ui_add_header(screen, "Settings", "Only implemented controls are enabled");
    panel = ui_add_page_panel(screen);

    ui_add_label(panel, "Appearance", &lv_font_montserrat_16,
                 ui_color_text(), 24, 24);
    ui_add_label(panel, "Dark theme (current session)", &lv_font_montserrat_14,
                 ui_color_muted(), 24, 51);
    theme_switch = lv_switch_create(panel);
    lv_obj_set_pos(theme_switch, 650, 31);
    lv_obj_set_size(theme_switch, 58, 30);
    if (s_dark_mode)
    {
        lv_obj_add_state(theme_switch, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(theme_switch, ui_theme_event, LV_EVENT_VALUE_CHANGED, NULL);

    ui_add_status_row(panel, 98, "Wi-Fi", "CYW55513 scan service runs on CM55",
                      "Ready", true);
    wifi_button = lv_button_create(panel);
    lv_obj_set_pos(wifi_button, 640, 108);
    lv_obj_set_size(wifi_button, 80, 42);
    ui_style_panel(wifi_button);
    lv_obj_set_style_bg_color(wifi_button, ui_color_accent(), LV_PART_MAIN);
    lv_obj_set_style_bg_color(wifi_button, lv_color_hex(0x0284c7),
                              LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_add_event_cb(wifi_button, ui_navigation_event, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)UI_PAGE_WIFI);
    ui_add_label(wifi_button, "Open", &lv_font_montserrat_14,
                 lv_color_hex(0xffffff), 22, 13);
    ui_add_status_row(panel, 174, "Bluetooth", "No Bluetooth driver/service is active",
                      "Unavailable", false);
    ui_add_disabled_switch(panel, 190);

    ui_add_label(panel,
                 "Wi-Fi scan is active. Connection and password entry are added only after this page passes.",
                 &lv_font_montserrat_14, ui_color_muted(), 24, 304);
}

static void ui_show_about(void)
{
    lv_obj_t *screen = ui_prepare_screen();
    lv_obj_t *panel;

    ui_add_header(screen, "About", "Reproducible software baseline");
    panel = ui_add_page_panel(screen);

    ui_add_label(panel, "PSoC Edge E84 AI Kit", &lv_font_montserrat_24,
                 ui_color_text(), 24, 22);
    ui_add_label(panel, "Open RT-Thread HMI foundation", &lv_font_montserrat_16,
                 ui_color_accent(), 24, 60);

    ui_add_status_row(panel, 104, "RT-Thread", "Kernel and device framework",
                      "5.0.2", true);
    ui_add_status_row(panel, 170, "LVGL", "User interface library",
                      "9.2", true);
    ui_add_status_row(panel, 236, "Board route", "Edgi-Talk same-SoC BSP adapted to AI Kit",
                      "B-1", true);

    ui_add_label(panel,
                 "Baseline: RT-Thread Studio 2.3.0 / Edgi-Talk BSP 1.4.0 / PDL 1.1.1.824",
                 &lv_font_montserrat_14, ui_color_muted(), 24, 322);
}

/**
 * @brief   切换到指定页面（实际页面构造函数）
 *
 * @details 销毁当前屏幕并按 page 调用对应的 ui_show_* 构造新屏幕。
 *          各 ui_show_* 内部会重建导航栏、内容面板与所有控件，并按需
 *          启动 / 停止页面级定时器（Performance / Wi-Fi）。
 *
 * @param   page  目标页面枚举
 */
static void ui_show_page(ui_page_t page)
{
    switch (page)
    {
    case UI_PAGE_HARDWARE:
        ui_show_hardware();
        break;

    case UI_PAGE_PERFORMANCE:
        ui_show_performance();
        break;

    case UI_PAGE_CAMERA:
        ui_show_camera();
        break;

    case UI_PAGE_SETTINGS:
        ui_show_settings();
        break;

    case UI_PAGE_WIFI:
        ui_show_wifi();
        break;

    case UI_PAGE_ABOUT:
        ui_show_about();
        break;

    case UI_PAGE_HOME:
    default:
        page = UI_PAGE_HOME;
        ui_show_home();
        break;
    }
    s_current_page = page;
}

/**
 * @brief   LVGL 用户 GUI 入口（由 LVGL 在系统启动后调用）
 *
 * @details LVGL 内部在 lv_init 完成后会调用本弱符号，作为整个 HMI
 *          Launcher 的启动入口。本实现直接切到首页 UI_PAGE_HOME，
 *          由 ui_show_page 后续通过顶部导航按钮切换其他页面。
 */
void lv_user_gui_init(void)
{
    ui_show_page(UI_PAGE_HOME);
}
