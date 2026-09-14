#include <stdbool.h>

#include "cycfg_peripherals.h"
#include "ai_kit_hmi_bus.h"

static cy_stc_scb_i2c_context_t g_hmi_i2c_context;
static bool g_hmi_i2c_ready;

static const cy_stc_scb_i2c_config_t g_hmi_i2c_config =
{
    .i2cMode = CY_SCB_I2C_MASTER,
    .useRxFifo = true,
    .useTxFifo = true,
    .slaveAddress = 0U,
    .slaveAddressMask = 0U,
    .acceptAddrInFifo = false,
    .ackGeneralAddr = false,
    .enableWakeFromSleep = false,
    .enableDigitalFilter = false,
    .lowPhaseDutyCycle = 16U,
    .highPhaseDutyCycle = 16U,
};

static const cy_stc_sysint_t g_hmi_i2c_irq_config =
{
    .intrSrc = CYBSP_I2C_DISPLAY_CONTROLLER_IRQ,
    .intrPriority = 3U,
};

static void ai_kit_hmi_i2c_irq_handler(void)
{
    Cy_SCB_I2C_Interrupt(CYBSP_I2C_DISPLAY_CONTROLLER_HW,
                         &g_hmi_i2c_context);
}

ai_kit_hmi_bus_result_t ai_kit_hmi_bus_init(void)
{
    if (g_hmi_i2c_ready)
    {
        return AI_KIT_HMI_BUS_OK;
    }

    if (Cy_SCB_I2C_Init(CYBSP_I2C_DISPLAY_CONTROLLER_HW,
                        &g_hmi_i2c_config,
                        &g_hmi_i2c_context) != CY_SCB_I2C_SUCCESS)
    {
        return AI_KIT_HMI_BUS_ERROR_I2C_INIT;
    }

    if (Cy_SysInt_Init(&g_hmi_i2c_irq_config,
                       ai_kit_hmi_i2c_irq_handler) != CY_SYSINT_SUCCESS)
    {
        return AI_KIT_HMI_BUS_ERROR_IRQ_INIT;
    }

    NVIC_ClearPendingIRQ(g_hmi_i2c_irq_config.intrSrc);
    NVIC_EnableIRQ(g_hmi_i2c_irq_config.intrSrc);
    Cy_SCB_I2C_Enable(CYBSP_I2C_DISPLAY_CONTROLLER_HW);
    g_hmi_i2c_ready = true;

    return AI_KIT_HMI_BUS_OK;
}

CySCB_Type *ai_kit_hmi_bus_base(void)
{
    return CYBSP_I2C_DISPLAY_CONTROLLER_HW;
}

cy_stc_scb_i2c_context_t *ai_kit_hmi_bus_context(void)
{
    return &g_hmi_i2c_context;
}
