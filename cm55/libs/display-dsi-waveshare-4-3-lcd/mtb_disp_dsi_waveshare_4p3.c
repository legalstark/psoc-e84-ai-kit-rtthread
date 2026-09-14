/*******************************************************************************
* \file mtb_disp_dsi_waveshare_4p3.c
* \version 1.0.0
*
* \brief
* Provides implementation of the Waveshare 4.3-inch DSI display driver library.
*
********************************************************************************
* \copyright
* Copyright 2025 Cypress Semiconductor Corporation (an Infineon company)
*
* SPDX-License-Identifier: Apache-2.0
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*******************************************************************************/

/*******************************************************************************
* Header Files
*******************************************************************************/
#include "mtb_disp_dsi_waveshare_4p3.h"


/*******************************************************************************
* Macros
*******************************************************************************/
#define RESET_VAL                             (0U)
#define RETRY_COUNT                           (5U)
#define I2C_TIMEOUT_COUNT                     (5U)
#define I2C_DELAY_MS                          (1U)
#define MTB_DISP_WAVESHARE_4P3_SYS_DELAY_MS   (100U)

/* Combine I2C controller error statuses in single mask  */
#define I2C_CONTROLLER_ERROR_MASK             (CY_SCB_I2C_MASTER_DATA_NAK | \
                                               CY_SCB_I2C_MASTER_ADDR_NAK | \
                                               CY_SCB_I2C_MASTER_ARB_LOST | \
                                               CY_SCB_I2C_MASTER_ABORT_START | \
                                               CY_SCB_I2C_MASTER_BUS_ERR)

#define MAX_BRIGHTNESS_PERCENT                (100U)
#define CMD_POS                               (0U)
#define VAL_POS                               (1U)

/* Macro to convert brightness percentage to brightness count */
/* Count corresponding to maximum brightness: 255 */
#define BRIGHTNESS_PERCENT_TO_COUNT(percentage) \
    ((uint8_t)(((percentage) * 255) / 100))


/*******************************************************************************
* Global Variables
*******************************************************************************/
/* Display specific MIPI DSI params (can be configured using gfxss personality
 * in ModusToolbox device configurator tool)
 */
cy_stc_mipidsi_display_params_t mtb_disp_waveshare_4p3_display_params =
{
    .pixel_clock    = MTB_DISP_WAVESHARE_4P3_PIXEL_CLK_KHZ,
    .hdisplay       = MTB_DISP_WAVESHARE_4P3_HOR_RES,
    .hsync_width    = MTB_DISP_WAVESHARE_4P3_HSYNC_WIDTH,
    .hfp            = MTB_DISP_WAVESHARE_4P3_HFP,
    .hbp            = MTB_DISP_WAVESHARE_4P3_HBP,
    .vdisplay       = MTB_DISP_WAVESHARE_4P3_VER_RES,
    .vsync_width    = MTB_DISP_WAVESHARE_4P3_VSYNC_WIDTH,
    .vfp            = MTB_DISP_WAVESHARE_4P3_VFP,
    .vbp            = MTB_DISP_WAVESHARE_4P3_VBP,
    .polarity_flags = RESET_VAL,
};

/* MIPI DSI block configuration */
cy_stc_mipidsi_config_t mtb_disp_waveshare_4p3_dsi_config =
{
    .virtual_ch     = RESET_VAL,
    .num_of_lanes   = MTB_DISP_WAVESHARE_4P3_NUM_LANES,
    .per_lane_mbps  = MTB_DISP_WAVESHARE_4P3_PER_LANE_MBPS,
    .dpi_fmt        = CY_MIPIDSI_FMT_RGB888,
    .dsi_mode       = DSI_VIDEO_MODE,
    .max_phy_clk    = MIPI_MAX_PHY_CLK_HZ,
    .mode_flags     = VID_MODE_TYPE_BURST | ENABLE_LOW_POWER_CMD | \
                      ENABLE_LOW_POWER,
    .display_params = &mtb_disp_waveshare_4p3_display_params,
};


static uint8_t mtb_disp_waveshare_4p3_init_cmds[][MTB_DISP_WAVESHARE_4P3_I2C_PKT_SIZE] =
{
    { MTB_DISP_WAVESHARE_4P3_CTRL_REG,            MTB_DISP_WAVESHARE_4P3_DISABLE_CMD },
    { MTB_DISP_WAVESHARE_4P3_CTRL_REG,            MTB_DISP_WAVESHARE_4P3_ENABLE_CMD  },
    { MTB_DISP_WAVESHARE_4P3_POWERON_REG,
      MTB_DISP_WAVESHARE_4P3_POWERON_CMPLT_CMD },
    { MTB_DISP_WAVESHARE_4P3_BRIGHTNESS_CTRL_REG,
      BRIGHTNESS_PERCENT_TO_COUNT(MAX_BRIGHTNESS_PERCENT) },
};


/*******************************************************************************
* Function name: mtb_disp_i2c_controller_read
********************************************************************************
*
* This function configures the I2C controller to read an entire buffer of data
* from Waveshare 4.3-inch display controller acting as I2C target.
*
* \param i2c_base
* Pointer to the I2C instance base address.
*
* \param i2c_context
* Pointer to the I2C context structure.
*
* \param read_buff
* Pointer to the I2C buffer for data to be read from the target.
*
* \param buff_size
* Size of the I2C read buffer.
*
* \return cy_en_scb_i2c_status_t
* I2C controller read status
*
*******************************************************************************/
static cy_en_scb_i2c_status_t mtb_disp_i2c_controller_read(CySCB_Type* i2c_base,
                                                           cy_stc_scb_i2c_context_t* i2c_context,
                                                           uint8_t* read_buff,
                                                           uint32_t buff_size)
{
    cy_stc_scb_i2c_master_xfer_config_t transfer_config;
    cy_en_scb_i2c_status_t i2c_status = CY_SCB_I2C_SUCCESS;
    uint8_t retry_count               = RETRY_COUNT;
    uint32_t i2c_controller_stat      = RESET_VAL;
    uint32_t timeout_count;

    CY_ASSERT(NULL != i2c_base);
    CY_ASSERT(NULL != i2c_context);
    CY_ASSERT(NULL != read_buff);

    /* I2C controller transfer configuration */
    transfer_config.slaveAddress      = MTB_DISP_WAVESHARE_4P3_I2C_ADDR;
    transfer_config.buffer            = read_buff;
    transfer_config.bufferSize        = buff_size;
    transfer_config.xferPending       = false;

    do
    {
        /* Initiate read transaction. */
        i2c_status = Cy_SCB_I2C_MasterRead(i2c_base, &transfer_config,
                                           i2c_context);

        if (CY_SCB_I2C_SUCCESS == i2c_status)
        {
            /* Total timeout 5 ms */
            timeout_count = I2C_TIMEOUT_COUNT;

            /* Wait until controller complete read transfer or time out has occurred */
            do
            {
                i2c_controller_stat = Cy_SCB_I2C_MasterGetStatus(i2c_base,
                                                                 i2c_context);
                Cy_SysLib_Delay(I2C_DELAY_MS);
                timeout_count--;
            } while ((CY_SCB_I2C_MASTER_BUSY & i2c_controller_stat) &&
                     timeout_count);

            if (!(I2C_CONTROLLER_ERROR_MASK & i2c_controller_stat))
            {
                i2c_status  = CY_SCB_I2C_SUCCESS;
                retry_count = RESET_VAL;
            }
            else
            {
                /* Timeout/error recovery */
                Cy_SCB_I2C_Disable(i2c_base, i2c_context);
                Cy_SCB_I2C_Enable(i2c_base);
                retry_count--;
            }
        }
        else
        {
            retry_count--;
        }
    } while (retry_count);

    return i2c_status;
}


/*******************************************************************************
* Function name: mtb_disp_i2c_controller_write
********************************************************************************
*
* This function configures the I2C controller to automatically write an entire
* buffer of data to Waveshare 4.3-inch display controller acting as I2C target.
*
* \param i2c_base
* Pointer to the I2C instance base address.
*
* \param i2c_context
* Pointer to the I2C context structure.
*
* \param write_buff
* Pointer to the I2C buffer for data to write to the I2C target.
*
* \param buff_size
* Size of the I2C write buffer.
*
* \return cy_en_scb_i2c_status_t
* I2C controller write status
*
*******************************************************************************/
static cy_en_scb_i2c_status_t mtb_disp_i2c_controller_write(CySCB_Type* i2c_base,
                                                            cy_stc_scb_i2c_context_t* i2c_context,
                                                            uint8_t* write_buff,
                                                            uint32_t buff_size)
{
    cy_stc_scb_i2c_master_xfer_config_t transfer_config;
    cy_en_scb_i2c_status_t i2c_status = CY_SCB_I2C_SUCCESS;
    uint8_t retry_count               = RETRY_COUNT;
    uint32_t i2c_controller_stat      = RESET_VAL;
    uint32_t timeout_count;

    CY_ASSERT(NULL != i2c_base);
    CY_ASSERT(NULL != i2c_context);
    CY_ASSERT(NULL != write_buff);

    /* I2C controller transfer configuration */
    transfer_config.slaveAddress      = MTB_DISP_WAVESHARE_4P3_I2C_ADDR;
    transfer_config.buffer            = write_buff;
    transfer_config.bufferSize        = buff_size;
    /* Generate Stop condition the end of transaction */
    transfer_config.xferPending       = false;

    do
    {
        /* Initiate write transaction */
        /* The Start condition is generated to begin this transaction */
        i2c_status = Cy_SCB_I2C_MasterWrite(i2c_base, &transfer_config,
                                            i2c_context);

        if (CY_SCB_I2C_SUCCESS == i2c_status)
        {
            /* Total timeout 5 ms */
            timeout_count = I2C_TIMEOUT_COUNT;

            /* Wait until controller complete read transfer or time out has occurred */
            do
            {
                i2c_controller_stat = Cy_SCB_I2C_MasterGetStatus(i2c_base,
                                                                 i2c_context);
                Cy_SysLib_Delay(I2C_DELAY_MS);
                timeout_count--;
            } while ((CY_SCB_I2C_MASTER_BUSY & i2c_controller_stat) &&
                     timeout_count);

            if ((!(I2C_CONTROLLER_ERROR_MASK & i2c_controller_stat)) &&
                (transfer_config.bufferSize ==
                 Cy_SCB_I2C_MasterGetTransferCount(i2c_base, i2c_context)))
            {
                i2c_status  = CY_SCB_I2C_SUCCESS;
                retry_count = RESET_VAL;
            }
            else
            {
                /* Timeout/error recovery */
                Cy_SCB_I2C_Disable(i2c_base, i2c_context);
                Cy_SCB_I2C_Enable(i2c_base);
                retry_count--;
            }
        }
        else
        {
            retry_count--;
        }
    } while (retry_count);

    return i2c_status;
}


/*******************************************************************************
* Function name: mtb_disp_waveshare_4p3_init
********************************************************************************
*
* Initializes Waveshare 4.3-inch DSI display using I2C interface.
*
* \param i2c_base
* Pointer to the I2C instance base address.
*
* \param i2c_context
* Pointer to the I2C context structure.
*
* \return cy_en_scb_i2c_status_t
* Display panel initialization status based on I2C communication.
*
* \funcusage
* \snippet snippet/main.c snippet_mtb_disp_waveshare_4p3_init
*
*******************************************************************************/
cy_en_scb_i2c_status_t mtb_disp_waveshare_4p3_init(CySCB_Type* i2c_base,
                                                   cy_stc_scb_i2c_context_t* i2c_context)
{
    cy_en_scb_i2c_status_t status = CY_SCB_I2C_SUCCESS;
    uint8_t cmd = MTB_DISP_WAVESHARE_4P3_ID_REG;
    uint8_t device_id = RESET_VAL;
    uint8_t index     = RESET_VAL;

    CY_ASSERT(NULL != i2c_base);
    CY_ASSERT(NULL != i2c_context);

    /* Display controller stabilization delay */
    Cy_SysLib_Delay(MTB_DISP_WAVESHARE_4P3_SYS_DELAY_MS);

    status = mtb_disp_i2c_controller_write(i2c_base, i2c_context, &cmd,
                                           sizeof(cmd));

    if (CY_SCB_I2C_SUCCESS == status)
    {
        status = mtb_disp_i2c_controller_read(i2c_base,
                                              i2c_context,
                                              &device_id,
                                              MTB_DISP_WAVESHARE_4P3_NUM_ID_BYTES);

        if (CY_SCB_I2C_SUCCESS == status)
        {
            /* Check whether the device ID is valid or not */
            if ((MTB_DISP_WAVESHARE_4P3_ID_1 == device_id) ||
                (MTB_DISP_WAVESHARE_4P3_ID_2 == device_id))
            {
                do
                {
                    /* Send display init sequence */
                    status = mtb_disp_i2c_controller_write(i2c_base,
                                                           i2c_context,
                                                           mtb_disp_waveshare_4p3_init_cmds[index],
                                                           MTB_DISP_WAVESHARE_4P3_I2C_PKT_SIZE);

                    index++;

                    Cy_SysLib_Delay(MTB_DISP_WAVESHARE_4P3_SYS_DELAY_MS);
                } while ((CY_SCB_I2C_SUCCESS == status) &&
                         (index < CY_ARRAY_SIZE(mtb_disp_waveshare_4p3_init_cmds)));
            }
            else
            {
                /* Invalid device found */
                status = CY_SCB_I2C_BAD_PARAM;
            }
        }
    }

    return status;
}


/*******************************************************************************
* Function name: mtb_disp_waveshare_4p3_set_brightness
********************************************************************************
*
* Controls the brightness of Waveshare 4.3-inch DSI display using I2C interface.
*
* \param i2c_base
* Pointer to the I2C instance base address.
*
* \param i2c_context
* Pointer to the I2C context structure.
*
* \param brightness_percent
* Brightness percentage.
*
* \return cy_en_scb_i2c_status_t
* Display brightness control operation status based on I2C communication.
*
* \funcusage
* \snippet snippet/main.c snippet_mtb_disp_waveshare_4p3_set_brightness
*
*******************************************************************************/
cy_en_scb_i2c_status_t mtb_disp_waveshare_4p3_set_brightness(CySCB_Type* i2c_base,
                                                             cy_stc_scb_i2c_context_t* i2c_context,
                                                             uint8_t brightness_percent)
{
    cy_en_scb_i2c_status_t status = CY_SCB_I2C_SUCCESS;
    uint8_t cmd[MTB_DISP_WAVESHARE_4P3_I2C_PKT_SIZE] = { RESET_VAL };

    CY_ASSERT(NULL != i2c_base);
    CY_ASSERT(NULL != i2c_context);
    CY_ASSERT(MAX_BRIGHTNESS_PERCENT >= brightness_percent);

    cmd[CMD_POS] = MTB_DISP_WAVESHARE_4P3_BRIGHTNESS_CTRL_REG;
    cmd[VAL_POS] = BRIGHTNESS_PERCENT_TO_COUNT(brightness_percent);

    status = mtb_disp_i2c_controller_write(i2c_base, i2c_context, cmd,
                                           sizeof(cmd));

    return status;
}


/*******************************************************************************
* Function Name: mtb_disp_waveshare_4p3_deinit
********************************************************************************
*
* De-initializes Waveshare 4.3-inch DSI display using I2C interface.
*
* \param i2c_base
* Pointer to the I2C instance base address.
*
* \param i2c_context
* Pointer to the I2C context structure.
*
* \return cy_en_scb_i2c_status_t
* Display panel de-initialization status based on I2C communication.
*
*
*******************************************************************************/
cy_en_scb_i2c_status_t mtb_disp_waveshare_4p3_deinit(CySCB_Type* i2c_base,
                                                     cy_stc_scb_i2c_context_t* i2c_context)
{
    cy_en_scb_i2c_status_t status = CY_SCB_I2C_SUCCESS;
    uint8_t cmd[MTB_DISP_WAVESHARE_4P3_I2C_PKT_SIZE] = { RESET_VAL };

    CY_ASSERT(NULL != i2c_base);
    CY_ASSERT(NULL != i2c_context);

    cmd[CMD_POS] = MTB_DISP_WAVESHARE_4P3_CTRL_REG;
    cmd[VAL_POS] = MTB_DISP_WAVESHARE_4P3_DISABLE_CMD;

    status = mtb_disp_i2c_controller_write(i2c_base, i2c_context, cmd,
                                           sizeof(cmd));

    Cy_SysLib_Delay(MTB_DISP_WAVESHARE_4P3_SYS_DELAY_MS);

    return status;
}


/* [] END OF FILE */
