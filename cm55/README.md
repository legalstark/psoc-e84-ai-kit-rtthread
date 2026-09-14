# PSoC Edge E84 AI Kit RT-Thread M55 Demo

> This directory is the M55 subproject of the dual-core public repository. Read
> the root [`README.md`](../README.md) and
> [`docs/BUILD_AND_FLASH.md`](../docs/BUILD_AND_FLASH.md) first. Standard
> dependencies are restored by the root script, and a release image also needs
> the matching `cm33/` project.

[**中文**](./README_zh.md) | **English**

## Introduction

This is the current M55 release project for **KITPSE84AITOBO1 / PSoC Edge E84 AI Kit**. It runs RT-Thread 5.0.2 and LVGL 9.2 and contains the validated Waveshare 5-inch DSI display, FT5406 touch, VG-Lite, double-buffer/VSYNC, CM33 IPC, CPU/memory/NPU benchmarks, CYW55513 Wi-Fi UI, and persistent WLAN credentials.

The project originated from the Edgi-Talk BSP 1.4.0 M55 LVGL/Wi-Fi examples, but its board layer, generated configuration, memory layout, display path, Wi-Fi resources, and application are for the AI Kit. The final Secure + CM33 + CM55 release image is generated with `firmware_bundle.json` as `build/ai_kit_rtthread_cm33_cm55.hex`.

The paired M33 project produces `cm33/build/rtthread_app.hex`. This M55 project
is the sole final packaging owner. Build both cores with `BUILD=release`, then
run EdgeProtectTools with `firmware_bundle.json` from this directory. The root
build script performs these steps in one command.

### LVGL Overview

**LVGL** (Light and Versatile Graphics Library) is an open-source embedded GUI development framework designed for resource-constrained devices. It provides modern graphical interfaces with optimized CPU and memory usage, running efficiently on both low-end MCUs and more powerful MPU platforms.

#### Key Features

1. **Lightweight**
   Optimized for minimal memory and CPU usage, ideal for low-power devices and resource-constrained environments.

2. **Cross-platform**
   Runs on multiple operating systems (FreeRTOS, RT-Thread, Zephyr, Linux) or bare-metal platforms. Only requires display and input drivers to be ported.

3. **Rich Widgets**
   Includes buttons, labels, sliders, charts, tables, lists, etc., and allows custom widget extensions.

4. **Advanced Rendering**
   Supports anti-aliasing, transparency, gradients, shadows, rounded corners, and animations for modern UIs.

5. **Input Device Support**
   Supports touchscreens, capacitive touch, mouse, keyboard, encoder, and multi-touch. Events are unified via LVGL’s event system.

6. **Internationalization**
   UTF-8 encoding with support for bidirectional text (e.g., Arabic, Hebrew).

7. **Extensibility**
   Flexible themes, styles, and integration with file systems and image decoders.

#### Applications

LVGL is widely used in:

* Consumer electronics (smart home panels, smartwatches, appliances)
* Industrial HMI and instrumentation
* Automotive displays (central console, passenger screen, instrument cluster)
* Medical devices (portable monitors, handheld instruments)

#### Ecosystem & Community

LVGL is **MIT licensed** and supported by **SquareLine Studio** for GUI design and **LVGL Simulator** for PC-based development. A large community provides open-source widgets, themes, and porting examples.

## Hardware Description

### Backlight Interface

![alt text](figures/1.png)

### MIPI Interface

![alt text](figures/2.png)

### PWR Interface

![alt text](figures/3.png)

### BTB Socket

![alt text](figures/4.png)
![alt text](figures/5.png)

### MCU Interface

![alt text](figures/6.png)
![alt text](figures/7.png)

## Software Description

* Adapted for the **PSoC Edge E84 AI Kit**, running on the **M55 application core**.
* Example features:

  * Initialize **LVGL 9.2**, the LCD display driver, and the touch input driver
  * Run the AI Kit launcher, hardware diagnostics, performance, and Wi-Fi pages by default
  * Support switching to **lv_demo_music**, **lv_demo_benchmark**, **lv_demo_stress**, and other LVGL demos
  * Enable M55 I-Cache/D-Cache by default and use AXIDMAC to optimize RGB565 area copy
* Code structure is clear for understanding display driver integration and LVGL porting.

## Demo Description

This project selects the LVGL demo through the `BSP_LVGL_DEMO_*` configuration options. The default configuration is `BSP_LVGL_DEMO_VIRTUAL3D_EMOJI`.

| Configuration | Demo | Description |
| --- | --- | --- |
| `BSP_LVGL_DEMO_VIRTUAL3D_EMOJI` | Virtual3D Animated Emoji | Optional upstream reference demo; it is not the current AI Kit launcher. |
| `BSP_LVGL_DEMO_MUSIC` | LVGL Music Demo | Official LVGL music UI demo, mainly used to verify complex widgets, layouts, styles, and animations. |
| `BSP_LVGL_DEMO_BENCHMARK` | LVGL Benchmark Demo | Official LVGL performance benchmark demo, used to observe rendering performance, frame rate, and score. |
| `BSP_LVGL_DEMO_STRESS` | LVGL Stress Demo | Official LVGL stress test demo. It repeatedly creates, refreshes, and destroys widgets to verify rendering stability and memory usage. |

To switch demos, modify the LVGL Demo configuration in **RT-Thread Settings** or `menuconfig`. It is recommended to select only one `BSP_LVGL_DEMO_*` option at a time, then regenerate the configuration, rebuild, and download the firmware.

![alt text](figures/demo_list.png)

## Usage

### Build and Download

1. Open and compile the project.
2. Connect the board USB to the PC using the **onboard debugger (DAP)**.
3. Flash the generated firmware to the board.

### Running Result

* After flashing and powering on, the example starts automatically.
* With the current configuration, the system starts the AI Kit launcher and exposes Settings, Hardware, Performance, About, and Wi-Fi pages.
* The serial console prints the current demo and LCD rotation angle, for example:

```
LVGL virtual3d_emoji demo start, lcd rotation=0
```

* The commands below belong to the optional upstream Virtual3D reference and are not part of the current launcher acceptance gate:

```
virtual3d_demo_stat
virtual3d_render_stat
```

* If the demo is switched to Music, Benchmark, or Stress, the LCD displays the corresponding official LVGL demo UI.

## Notes

> **⚠️ Note:** This project requires **RT-Thread Studio 2.2.9** or higher.

* To modify the **graphical configuration**, use:

```
tools/device-configurator/device-configurator.exe
libs/TARGET_APP_KIT_PSE84_EVAL_EPC2/config/design.modus
```

* Save and regenerate code after modifications.
* The default `BSP_LVGL_DEMO_VIRTUAL3D_EMOJI` demo does not support LCD rotation at 90 or 270 degrees. Use 0 or 180 degrees, or switch to another LVGL demo if landscape rotation is required.
* The default configuration enables `BSP_LVGL_ENABLE_CPU_CACHE` and `BSP_LCD_USE_AXIDMAC_AREA_COPY`. If cache, framebuffer, or LCD refresh settings are changed, also check display buffer coherency.
* If the screen shows no output, check:

  * LCD connections and power supply
  * `lv_port_disp.c` and `lv_port_indev.c` match the actual hardware
  * LCD rotation angle is compatible with the selected demo

## Startup Sequence

```
+------------------+
|   Secure M33     |
|  (Secure Core)   |
+------------------+
          |
          v
+------------------+
|       M33        |
| (Non-Secure Core)|
+------------------+
          |
          v
+-------------------+
|       M55         |
| (Application Core)|
+-------------------+
```

⚠️ Strictly follow the flashing order to ensure proper system operation.

---

* If the example fails, first flash **Edgi_Talk_M33_Template** to ensure proper initialization.
* To enable M55, configure in **M33 project**:

```
RT-Thread Settings --> Hardware --> select SOC Multi Core Mode --> Enable CM55 Core
```
