# PSoC Edge E84 AI Kit RT-Thread 基础工程

本工程面向 KITPSE84AITOBO1（PSoC Edge E84 AI Kit），在 Cortex-M33
Non-Secure 域运行 RT-Thread 5.0.2。

工程由 RT-Thread Studio 的 PSOC_E84-EDGI-TALK BSP 1.4.0 模板迁移而来，
但底层 BSP、时钟、引脚、内存布局、链接脚本和 Secure 固件均已切换为
AI Kit 官方配置。

## 当前基线

- MCU：PSE846GPS2DBZC4A
- 运行核心：CM33 Non-Secure
- CM55：关闭
- 控制台：uart2
- 调试串口：SCB2，RX=P6.5，TX=P6.7，115200 波特率
- PDL/HAL：mtb-dsl-pse8xxgp 1.6.0
- CMSIS：6.1.0
- core-lib：1.8.0
- mtb-srf 头文件：1.2.1

## 固件输出

RT-Thread Studio 下载使用的完整固件：

    Debug/rtthread.hex

SCons 验证构建的完整固件：

    build/rtthread.hex

两者均由 AI Kit Secure M33 固件和 RT-Thread CM33 Non-Secure 固件合并而成。
Secure 固件来源：

    tools/secure/proj_cm33_s_signed.hex

## Device Configurator

AI Kit 配置文件：

    libs/TARGET_APP_KIT_PSE84_AI/config/design.modus

GeneratedSource：

    libs/TARGET_APP_KIT_PSE84_AI/config/GeneratedSource

修改 design.modus 时必须使用匹配的 mtb-dsl-pse8xxgp 1.6.0。修改内容如果
影响 Secure 域、时钟、安全属性或内存布局，还必须在 AI Kit 官方
ModusToolbox 工程中重新构建 Secure M33，并同步替换
tools/secure/proj_cm33_s_signed.hex。

不能混用 Edgi-Talk 的 GeneratedSource、链接脚本或 Secure 固件。
