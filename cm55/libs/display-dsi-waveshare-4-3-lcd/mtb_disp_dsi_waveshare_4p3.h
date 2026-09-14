/*******************************************************************************
* \file mtb_disp_dsi_waveshare_4p3.h
*
* \brief
* Provides constants, parameter values, and API prototypes for the
* Waveshare 4.3-inch DSI display driver library.
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

#ifndef MTB_DISP_DSI_WAVESHARE_4P3_H
#define MTB_DISP_DSI_WAVESHARE_4P3_H


#if defined(__cplusplus)
extern "C" {
#endif


/*******************************************************************************
* Header Files
*******************************************************************************/
#include "cy_graphics.h"
#include "cy_pdl.h"
#include "cy_result.h"


/*******************************************************************************
* Macros
*******************************************************************************/
/* Waveshare 4.3-inch display panel register(s) and value(s) */
#define MTB_DISP_WAVESHARE_4P3_ID_REG                 (0x80U)
#define MTB_DISP_WAVESHARE_4P3_CTRL_REG               (0x85U)
#define MTB_DISP_WAVESHARE_4P3_POWERON_REG            (0x81U)
#define MTB_DISP_WAVESHARE_4P3_BRIGHTNESS_CTRL_REG    (0x86U)

#define MTB_DISP_WAVESHARE_4P3_ID_1                   (0xC3U)
#define MTB_DISP_WAVESHARE_4P3_ID_2                   (0xDEU)
#define MTB_DISP_WAVESHARE_4P3_NUM_ID_BYTES           (0x01U)

#define MTB_DISP_WAVESHARE_4P3_DISABLE_CMD            (0x00U)
#define MTB_DISP_WAVESHARE_4P3_ENABLE_CMD             (0x01U)
#define MTB_DISP_WAVESHARE_4P3_POWERON_CMPLT_CMD      (0x04U)

/* Waveshare 4.3-inch display panel I2C details */
#define MTB_DISP_WAVESHARE_4P3_I2C_ADDR               (0x45U)
#define MTB_DISP_WAVESHARE_4P3_I2C_PKT_SIZE           (2U)

/* Device max DPHY clock in Hz unit */
#define MIPI_MAX_PHY_CLK_HZ                           (2500000000UL)

/* Display specific configuration parameters */
#define MTB_DISP_WAVESHARE_4P3_NUM_LANES              (1U)

/* Default horizontal display resolution (in pixels) for the 16-bit color
 * format use case.
 */
/* Per the PSOC Edge display controller spec, the stride must be 128-byte
 * aligned, requiring the horizontal resolution to be adjusted.
 * For the 32-bit color format, the horizontal resolution should remain 800,
 * matching the actual display resolution, as it ensures the stride is
 * also 128-byte aligned.
 */
#define MTB_DISP_WAVESHARE_4P3_HOR_RES                (832U)

/* Default display resolution in vertical direction (in pixels) */
#define MTB_DISP_WAVESHARE_4P3_VER_RES                (480U)

/* Waveshare 4.3-inch display panel timings for 1 lane */
/* HSYNC width (Horizontal Sync Width) measured in pixel clock cycles
 * HFP (Horizontal Front Porch) measured in pixel clock cycles
 * HBP (Horizontal Back Porch) measured in pixel clock cycles
 * VSYNC width (Vertical Sync Width) measured in lines or rows
 * VFP (Vertical Front Porch) measured in lines or rows
 * VBP (Vertical Back Porch) measured in lines or rows
 */
#define MTB_DISP_WAVESHARE_4P3_HSYNC_WIDTH            (10U)
#define MTB_DISP_WAVESHARE_4P3_HFP                    (210U)
#define MTB_DISP_WAVESHARE_4P3_HBP                    (20U)
#define MTB_DISP_WAVESHARE_4P3_VSYNC_WIDTH            (5U)
#define MTB_DISP_WAVESHARE_4P3_VFP                    (20U)
#define MTB_DISP_WAVESHARE_4P3_VBP                    (20U)

#define MTB_DISP_WAVESHARE_4P3_PER_LANE_MBPS          (810U)

/* Pixel clock in KHz */
/* Pixel clock = ((hsync_width + hfp + hbp + display_width) *
 * (vsync_width + vfp + vbp + display_height) * fps / 1000)
 * Here fps is considered as 60 to achieve bit rate upto 810 Mbps to
 * match the display specification.
 */
#define MTB_DISP_WAVESHARE_4P3_PIXEL_CLK_KHZ          (33768U)


/*******************************************************************************
* Global Variables
*******************************************************************************/
/* Display specific MIPI DSI configurations */
extern cy_stc_mipidsi_display_params_t mtb_disp_waveshare_4p3_display_params;
extern cy_stc_mipidsi_config_t mtb_disp_waveshare_4p3_dsi_config;


/*******************************************************************************
* Function Prototypes
*******************************************************************************/
cy_en_scb_i2c_status_t mtb_disp_waveshare_4p3_init(CySCB_Type* i2c_base,
                                                   cy_stc_scb_i2c_context_t* i2c_context);

cy_en_scb_i2c_status_t mtb_disp_waveshare_4p3_set_brightness(CySCB_Type* i2c_base,
                                                             cy_stc_scb_i2c_context_t* i2c_context,
                                                             uint8_t brightness_percent);
cy_en_scb_i2c_status_t mtb_disp_waveshare_4p3_deinit(CySCB_Type* i2c_base,
                                                     cy_stc_scb_i2c_context_t* i2c_context);


#if defined(__cplusplus)
}
#endif /* __cplusplus */


#endif /* MTB_DISP_DSI_WAVESHARE_4P3_H */


/* [] END OF FILE */
