# PSoC Edge E84 AI Kit RT-Thread 双核 Demo

这是面向 **KITPSE84AITOBO1 / PSoC Edge E84 AI Kit** 的社区 RT-Thread
双核工程。项目以 RT-Thread Studio 的 Edgi-Talk BSP 1.4.0 为同芯片基础，
建立了独立的 AI Kit 板级配置，并同时运行 Cortex-M33 和 Cortex-M55。

> 本项目不是 Infineon 或 RT-Thread 官方发布的 AI Kit BSP。Edgi-Talk 的
> 外设配置不能直接用于 AI Kit；ECO、引脚、Secure 资源归属和存储布局均以
> AI Kit 配置为准。

## 已实现功能

- Cortex-M33 与 Cortex-M55 双核 RT-Thread 5.0.2。
- M33 `msh`、GPIO、UART、I2C、SPI、ADC、PWM 和 IPC 控制。
- M33/M55 CoreMark、IPC、SRAM 与 PSRAM 性能测试。
- 16 MB S70KS1283 PSRAM 板级支持。
- Waveshare 5inch DSI LCD (B)，800×480，FT5406 触摸。
- LVGL 9.2、VG-Lite、双帧缓冲和 VSYNC。
- Launcher、Settings、Hardware、Performance、Wi-Fi、Camera 和 About 页面。
- CYW55513 SDIO/WHD Wi-Fi，扫描、连接、DHCP 和最多三组凭据持久化。
- Ethos-U55 NPU Person Detection 基准。
- J2 USB Host UVC 摄像头实时预览。

## 固定基线

| 项目 | 版本 |
|---|---|
| 开发板 | KITPSE84AITOBO1 |
| RT-Thread Studio | 2.3.0 |
| Edgi-Talk BSP | 1.4.0 |
| RT-Thread | 5.0.2 |
| Device Support / PDL | 1.1.1.824 |
| LVGL | 9.2 |
| GNU Arm Embedded | 13.3 |

## 仓库结构

```text
cm33/       Cortex-M33 Non-Secure RT-Thread Studio 工程
cm55/       Cortex-M55 RT-Thread Studio 工程及三镜像合并配置
libraries/  两个核心共享的 BSP、驱动与中间件
rt-thread/  RT-Thread 5.0.2 源码
scripts/    私有组件准备、构建和发布检查
docs/       构建说明与第三方许可证清单
```

仓库像 Edgi-Talk 官方 SDK 一样直接包含可再分发的 BSP 与 RT-Thread 源码。
SEGGER emUSB-Host 与 Secure HEX 的条款不允许按普通开源文件重新发布，因此
不进入 Git；完整 Camera 构建前只需执行一次私有组件准备脚本。

## 快速开始

1. 安装 RT-Thread Studio 2.3.0 和 `PSOC_E84-EDGI-TALK` BSP 1.4.0。
2. 使用官方 AI Kit `PSOC_Edge_Hello_World` 工程生成
   `proj_cm33_s_signed.hex`。
3. 在仓库根目录执行：

```powershell
.\scripts\prepare_private_components.ps1 `
    -SecureHex '<AI_KIT_OFFICIAL_PROJECT>\build\project_hex\proj_cm33_s_signed.hex' `
    -AcceptInfineonEula
```

4. 构建 M33、M55 并生成完整三镜像：

```powershell
.\scripts\build_release.ps1 -StudioRoot '<RT_STUDIO_ROOT>'
```

5. 连接 KitProg3 后烧录并校验完整镜像：

```powershell
.\scripts\flash.ps1 -StudioRoot '<RT_STUDIO_ROOT>'
```

也可以在 RT-Thread Studio 中分别导入 `cm33` 和 `cm55` 进行开发。
图形界面构建和手工烧录说明见 [BUILD_AND_FLASH.md](docs/BUILD_AND_FLASH.md)。

## 开源边界

项目原创代码采用 Apache-2.0。第三方代码、固件、模型和预编译库继续服从
各自许可证，详见 [THIRD_PARTY_NOTICES.md](docs/THIRD_PARTY_NOTICES.md)。

公开版本不包含来源许可未明确的本地 ResNet/MNIST 研究资产。NPU 页面只保留
可随 Apache-2.0 notice 再分发的 TensorFlow Lite Micro Person Detection
工作负载。

## 当前已知限制

- Bluetooth 尚未实现。
- 连续快速触发两次 reboot 偶尔会导致显示绿屏或黑屏，需要重新上电。
- Camera 画面链路已经清晰流畅；退出并重新进入页面的生命周期修复仍需完成
  最终十次循环板端验收。

## 文档

- [构建、合并与烧录](docs/BUILD_AND_FLASH.md)
- [第三方依赖及许可证](docs/THIRD_PARTY_NOTICES.md)

## 致谢

- [RT-Thread](https://github.com/RT-Thread/rt-thread)
- [Infineon PSoC Edge](https://github.com/Infineon)
- [LVGL](https://github.com/lvgl/lvgl)
- [Infineon Wi-Fi Host Driver](https://github.com/Infineon/wifi-host-driver)
- [Infineon emUSB-Host](https://github.com/Infineon/emusb-host)
