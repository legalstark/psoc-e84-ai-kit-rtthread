# PSoC Edge E84 AI Kit RT-Thread Base Project

This project targets KITPSE84AITOBO1 (PSoC Edge E84 AI Kit) and runs
RT-Thread 5.0.2 on the Cortex-M33 Non-Secure domain.

The project originated from the RT-Thread Studio PSOC_E84-EDGI-TALK BSP 1.4.0
template. Its board BSP, clocks, pins, memory layout, linker script, and Secure
firmware now use the matching AI Kit configuration.

## Baseline

- Device: PSE846GPS2DBZC4A
- Core: CM33 Non-Secure
- CM55: paired release image from `AI_Kit_RTThread_M55_Demo`
- Console: uart2
- Debug UART: SCB2, RX=P6.5, TX=P6.7, 115200 baud
- Non-Secure PDL/HAL compile library: mtb-dsl-pse8xxgp 1.1.1.824
- Secure board artifact: mtb-dsl-pse8xxgp 1.6.0
- CMSIS: 6.1.0
- core-lib: 1.8.0
- mtb-srf headers: 1.2.1

RT-Thread Studio downloads Debug/rtthread.hex. The SCons Release build produces
`build/rtthread_app.hex` for CM33 Non-Secure only. Final Secure + CM33 + CM55
packaging is owned by `AI_Kit_RTThread_M55_Demo/firmware_bundle.json`; this M33
project deliberately has no path or build dependency on an M55 project.

The Secure artifact initializes the onboard S70KS1283 PSRAM and enforces the
validated memory layout:

    M33 private  0x64000000 / 2 MiB
    M55 private  0x64200000 / 2 MiB
    shared       0x64400000 / 12 MiB

The Non-Secure projects expose this layout through `ai_kit_psram`. Private
read/write and restore-safe self-test APIs are available to each core. Shared
memory requires an explicit ownership and cache-maintenance protocol and is not
part of the RT-Thread default heap.

The Device Configurator source is located at:

    libs/TARGET_APP_KIT_PSE84_AI/config/design.modus

Regenerate the Non-Secure RT-Thread configuration only with the matching
mtb-device-support-pse8xxgp 1.1.1.824 personality. Do not copy the complete
1.6.0 GeneratedSource tree into this project. Changes affecting the Secure
domain, clocks, SMIF1, protection, or memory layout must instead be built and
validated in `AI_Kit_PSRAM_Validation`, then installed as a new signed Secure
artifact at tools/secure/proj_cm33_s_signed.hex.
