/**
 * @file    board_test.c
 * @brief   AI Kit 板级外设诊断 msh 命令集合
 *
 * @details 该文件提供一组只读的 msh 命令，用于在 RT-Thread shell 中
 *          诊断 AI Kit E84 板级外设的就绪状态：
 *          - ai_devices：枚举 uart2/i2c0/spi3/adc1/pwm_led/psram 是否就绪
 *          - i2c_scan  ：在 0x08..0x77 范围内扫描 I2C 总线上的设备
 *          - spi_info  ：显示板载 BGT60TR13C 雷达所在 SPI3 总线引脚映射
 *          - adc_read  ：读取 SAR ADC 通道电压（毫伏）
 *          - pwm_led   ：设置板载 PWM LED 占空比（0..100%）
 *          - flash_info：只读显示 XSPI Flash 信息及代码段 FNV32 校验
 *          - psram_status：只读检视 SMIF1 状态及 PSRAM 槽位 CTL 寄存器
 *
 *          所有诊断均不发起破坏性写操作，确保固件执行现场不被破坏。
 *
 * @note    本文件仅添加注释，未改动任何可执行代码。
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <stdint.h>
#include <stdlib.h>
#include "cy_smif.h"
#include "cycfg_peripherals.h"

#ifdef BSP_USING_PSRAM
#include "ai_kit_psram.h"
#endif

#define AI_KIT_XSPI_FLASH_BASE 0x60000000UL  /**< XSPI Flash 内存映射基地址（S25HS512T） */
#define AI_KIT_XSPI_FLASH_SIZE 0x04000000UL  /**< XSPI Flash 容量：64 MB */

/**
 * @brief   打印单个 RT-Thread 设备的就绪状态
 *
 * @param   name  RT-Thread 设备名称（如 "uart2"、"spi3"）
 *
 * @note    仅通过 rt_device_find 判断是否完成注册，不发起任何 I/O 操作。
 */
static void print_device(const char *name)
{
    rt_kprintf("%-8s : %s\n", name,
               (rt_device_find(name) != RT_NULL) ? "ready" : "missing");
}

/**
 * @brief   枚举 AI Kit 关键 RT-Thread 设备的就绪状态
 *
 * @details 顺序检视 uart2 / i2c0 / spi3 / adc1 / pwm_led；当启用 PSRAM
 *          配置（BSP_USING_PSRAM）时，额外通过 ai_kit_psram_is_ready
 *          显示 PSRAM 状态。
 *
 * @param   argc  msh 传入参数个数（未使用）
 * @param   argv  msh 传入参数数组（未使用）
 *
 * @return  int   固定返回 0
 *
 * @par     示例
 * @code
 *   msh />ai_devices
 *   uart2    : ready
 *   i2c0     : ready
 *   spi3     : ready
 *   adc1     : ready
 *   pwm_led  : ready
 *   psram    : ready
 * @endcode
 */
static int ai_devices(int argc, char **argv)
{
    RT_UNUSED(argc);
    RT_UNUSED(argv);

    print_device("uart2");
    print_device("i2c0");
    print_device("spi3");
    print_device("adc1");
    print_device("pwm_led");
#ifdef BSP_USING_PSRAM
    rt_kprintf("%-8s : %s\n", "psram",
               ai_kit_psram_is_ready() ? "ready" : "missing");
#endif
    return 0;
}
MSH_CMD_EXPORT(ai_devices, show AI Kit RT-Thread device status);

/**
 * @brief   扫描 I2C 总线上的设备地址
 *
 * @details 对 0x08..0x77 范围内的每个地址发起长度为 0 的 I2C 传输，
 *          返回 ACK 的地址视为在线设备并打印。默认扫描 i2c0；可通过
 *          argv[1] 指定其他总线名（如 i2c1）。
 *
 * @param   argc  msh 参数个数
 * @param   argv  argv[1] 可选：I2C 总线名称，默认 "i2c0"
 *
 * @retval  RT_EOK      至少发现一个设备
 * @retval  -RT_ERROR   未发现任何设备或总线不存在
 */
static int i2c_scan(int argc, char **argv)
{
    const char *bus_name = (argc > 1) ? argv[1] : "i2c0";
    struct rt_i2c_bus_device *bus;
    struct rt_i2c_msg msg;
    rt_uint8_t dummy = 0;
    int found = 0;
    int address;

    bus = (struct rt_i2c_bus_device *)rt_device_find(bus_name);
    if (bus == RT_NULL)
    {
        rt_kprintf("I2C bus %s not found\n", bus_name);
        return -RT_ERROR;
    }

    rt_kprintf("Scanning %s...\n", bus_name);
    for (address = 0x08; address <= 0x77; address++)
    {
        msg.addr = (rt_uint16_t)address;
        msg.flags = 0;
        msg.buf = &dummy;
        msg.len = 0;
        if (rt_i2c_transfer(bus, &msg, 1) == 1)
        {
            rt_kprintf("  found 0x%02x\n", address);
            found++;
        }
    }
    rt_kprintf("I2C scan complete: %d device(s)\n", found);
    return (found > 0) ? RT_EOK : -RT_ERROR;
}
MSH_CMD_EXPORT(i2c_scan, scan AI Kit I2C bus; usage: i2c_scan [i2c0]);

/**
 * @brief   显示板载雷达所在 SPI3 总线的引脚映射
 *
 * @details 只读命令，不会发起任何 SPI 盲传输出，避免影响 BGT60TR13C
 *          雷达协议栈。后续应通过专用雷达协议驱动访问。
 *
 * @param   argc  msh 参数个数（未使用）
 * @param   argv  msh 参数数组（未使用）
 *
 * @retval  RT_EOK      spi3 已就绪，引脚信息已打印
 * @retval  -RT_ERROR   spi3 设备未注册
 */
static int spi_info(int argc, char **argv)
{
    RT_UNUSED(argc);
    RT_UNUSED(argv);

    if (rt_device_find("spi3") == RT_NULL)
    {
        rt_kprintf("spi3 is missing\n");
        return -RT_ERROR;
    }

    rt_kprintf("spi3 ready: SCB3, MISO=P21.4, MOSI=P21.5, CLK=P21.6, CS=P21.7\n");
    rt_kprintf("This bus is connected to the onboard BGT60TR13C radar.\n");
    rt_kprintf("No blind transfer is issued; use the radar protocol driver next.\n");
    return RT_EOK;
}
MSH_CMD_EXPORT(spi_info, show the onboard radar SPI mapping without transferring data);

/**
 * @brief   读取 SAR ADC 通道电压（毫伏）
 *
 * @details 当前仅生成逻辑通道 0 可用。命令先使能 ADC，再调用
 *          rt_adc_read 得到毫伏值。
 *
 * @param   argc  msh 参数个数
 * @param   argv  argv[1] 可选：通道号，默认 0（当前仅支持 0）
 *
 * @retval  RT_EOK       读取成功
 * @retval  -RT_ERROR    adc1 未注册或使能失败
 * @retval  -RT_EINVAL   请求了不支持的通道号
 */
static int adc_read(int argc, char **argv)
{
    rt_adc_device_t adc;
    int channel = (argc > 1) ? atoi(argv[1]) : 0;
    rt_uint32_t millivolts;

    adc = (rt_adc_device_t)rt_device_find("adc1");
    if (adc == RT_NULL)
    {
        rt_kprintf("adc1 is missing\n");
        return -RT_ERROR;
    }
    if (channel != 0)
    {
        rt_kprintf("Only generated logical channel 0 is available\n");
        return -RT_EINVAL;
    }
    if (rt_adc_enable(adc, channel) != RT_EOK)
    {
        rt_kprintf("adc1 channel %d enable failed\n", channel);
        return -RT_ERROR;
    }

    millivolts = rt_adc_read(adc, channel);
    rt_kprintf("adc1 channel %d = %u mV\n", channel, millivolts);
    return RT_EOK;
}
MSH_CMD_EXPORT(adc_read, read generated SAR ADC channel; usage: adc_read [0]);

/**
 * @brief   设置板载 PWM LED 的占空比
 *
 * @details 固定周期为 1 ms（频率 1 kHz），通过 argv[1] 设置占空比百分比。
 *          调用 rt_pwm_set 同时配置周期与脉宽，再使能通道 0。
 *
 * @param   argc  msh 参数个数，必须为 2
 * @param   argv  argv[1] 必填：占空比 0..100
 *
 * @retval  RT_EOK       设置成功
 * @retval  -RT_EINVAL   参数缺失或超出 0..100 范围
 * @retval  -RT_ERROR    pwm_led 设备未注册或 PWM 配置失败
 */
static int pwm_led(int argc, char **argv)
{
    struct rt_device_pwm *pwm;
    int duty;
    const rt_uint32_t period_ns = 1000000U;
    rt_uint32_t pulse_ns;

    if (argc != 2)
    {
        rt_kprintf("usage: pwm_led <0..100>\n");
        return -RT_EINVAL;
    }
    duty = atoi(argv[1]);
    if ((duty < 0) || (duty > 100))
    {
        rt_kprintf("duty must be 0..100\n");
        return -RT_EINVAL;
    }

    pwm = (struct rt_device_pwm *)rt_device_find("pwm_led");
    if (pwm == RT_NULL)
    {
        rt_kprintf("pwm_led is missing\n");
        return -RT_ERROR;
    }

    pulse_ns = (period_ns / 100U) * (rt_uint32_t)duty;
    if ((rt_pwm_set(pwm, 0, period_ns, pulse_ns) != RT_EOK) ||
        (rt_pwm_enable(pwm, 0) != RT_EOK))
    {
        rt_kprintf("PWM setup failed\n");
        return -RT_ERROR;
    }
    rt_kprintf("pwm_led duty=%d%%, frequency=1 kHz\n", duty);
    return RT_EOK;
}
MSH_CMD_EXPORT(pwm_led, set onboard PWM LED duty; usage: pwm_led 0..100);

/**
 * @brief   只读显示 XSPI Flash 信息及代码段 FNV32 校验
 *
 * @details 由于 CM33 固件执行自该 XIP Flash，本命令有意禁用写/擦测试。
 *          对 flash_info 函数地址对齐取 256 字节，计算 FNV-1a 32 位哈希，
 *          用于诊断构建版本一致性。
 *
 * @param   argc  msh 参数个数（未使用）
 * @param   argv  msh 参数数组（未使用）
 *
 * @return  int   固定返回 RT_EOK
 */
static int flash_info(int argc, char **argv)
{
    const uint8_t *code;
    uintptr_t code_addr;
    uint32_t hash = 2166136261UL;
    int i;

    RT_UNUSED(argc);
    RT_UNUSED(argv);

    code_addr = ((uintptr_t)&flash_info) & ~(uintptr_t)1U;
    code = (const uint8_t *)code_addr;
    for (i = 0; i < 256; i++)
    {
        hash ^= code[i];
        hash *= 16777619UL;
    }

    rt_kprintf("XSPI Flash: S25HS512T, base=0x%08x, size=%u MB\n",
               AI_KIT_XSPI_FLASH_BASE, AI_KIT_XSPI_FLASH_SIZE / (1024U * 1024U));
    rt_kprintf("CM33 code address=0x%08x, read-only FNV32(256 B)=0x%08x\n",
               (rt_uint32_t)code_addr, hash);
    rt_kprintf("Write/erase test is intentionally disabled because firmware executes from this Flash.\n");
    return RT_EOK;
}
MSH_CMD_EXPORT(flash_info, show read-only XIP Flash information and code checksum);

/**
 * @brief   只读检视 SMIF1 状态与 PSRAM 槽位 CTL 寄存器
 *
 * @details 不访问 PSRAM 内存窗口，仅打印：
 *          - SMIF1 使能状态、XIP/MMIO 模式、中断状态/掩码
 *          - 四个槽位（CORE_DEVICE0..3）的 CTL 寄存器、使能位、写使能位
 *
 *          用于排查 S70KS1283 PSRAM（16 MB，NS 窗口 0x64000000）的
 *          SMIF 控制器配置问题。
 *
 * @param   argc  msh 参数个数（未使用）
 * @param   argv  msh 参数数组（未使用）
 *
 * @return  int   固定返回 RT_EOK
 */
static int psram_status(int argc, char **argv)
{
    const uint32_t slot_ctl[] =
    {
        SMIF_DEVICE_CTL(SMIF1_CORE_DEVICE0),
        SMIF_DEVICE_CTL(SMIF1_CORE_DEVICE1),
        SMIF_DEVICE_CTL(SMIF1_CORE_DEVICE2),
        SMIF_DEVICE_CTL(SMIF1_CORE_DEVICE3),
    };
    unsigned int i;

    RT_UNUSED(argc);
    RT_UNUSED(argv);

    rt_kprintf("AI Kit PSRAM: S70KS1283, 16 MB, NS window=0x64000000\n");
    rt_kprintf("SMIF1: enabled=%u mode=%s intr=0x%08x mask=0x%08x\n",
               Cy_SMIF_IsEnabled(SMIF1_CORE) ? 1U : 0U,
               ((SMIF_CTL(SMIF1_CORE) & SMIF_CTL_XIP_MODE_Msk) != 0U) ? "XIP" : "MMIO",
               Cy_SMIF_GetInterruptStatus(SMIF1_CORE),
               Cy_SMIF_GetInterruptMask(SMIF1_CORE));

    for (i = 0U; i < (sizeof(slot_ctl) / sizeof(slot_ctl[0])); i++)
    {
        rt_kprintf("slot%u CTL=0x%08x enabled=%u write=%u\n",
                   i,
                   slot_ctl[i],
                   ((slot_ctl[i] & SMIF_DEVICE_CTL_ENABLED_Msk) != 0U) ? 1U : 0U,
                   ((slot_ctl[i] & SMIF_DEVICE_CTL_WR_EN_Msk) != 0U) ? 1U : 0U);
    }

    rt_kprintf("Probe is read-only; no access to the PSRAM memory window was issued.\n");
    return RT_EOK;
}
MSH_CMD_EXPORT(psram_status, inspect SMIF1 state without accessing PSRAM);
