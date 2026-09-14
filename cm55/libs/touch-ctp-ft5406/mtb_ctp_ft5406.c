/*******************************************************************************
* \file mtb_ctp_ft5406.c
* \version 1.0.0
*
* \brief
* Provides implementation of the FT5406 touch panel driver library.
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
#include "mtb_ctp_ft5406.h"


/*******************************************************************************
* Macros
*******************************************************************************/
#define RETRY_COUNT                        (5U)

#define I2C_TIMEOUT_COUNT                  (5U)

#define I2C_DELAY_MS                       (1U)

#define RESET_VAL                          (0U)

/* Combine I2C controller error statuses in single mask  */
#define I2C_CONTROLLER_ERROR_MASK          (CY_SCB_I2C_MASTER_DATA_NAK | \
                                            CY_SCB_I2C_MASTER_ADDR_NAK | \
                                            CY_SCB_I2C_MASTER_ARB_LOST | \
                                            CY_SCB_I2C_MASTER_ABORT_START | \
                                            CY_SCB_I2C_MASTER_BUS_ERR)


#define TOUCH_POINT_GET_EVENT(T) ((mtb_ctp_touch_event_t)(uint8_t)((T).XH >> 6U))
#define TOUCH_POINT_GET_ID(T)    ((T).YH >> 4)
#define TOUCH_POINT_GET_X(T)     (int)((((uint16_t)(T).XH & 0x0fU) << 8) | \
                                      (uint16_t)(T).XL)
#define TOUCH_POINT_GET_Y(T)     (int)((((uint16_t)(T).YH & 0x0fU) << 8) | \
                                      (uint16_t)(T).YL)


/*******************************************************************************
* Global Variables
*******************************************************************************/
static mtb_ctp_ft5406_config_t* ft5406_config = NULL;


/*******************************************************************************
* Function Name: mtb_ctp_i2c_controller_read
********************************************************************************
*
* This function configures the I2C controller to read an entire buffer of data
* from touch panel controller acting as I2C target.
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
static cy_en_scb_i2c_status_t mtb_ctp_i2c_controller_read(uint8_t* read_buff,
                                                          uint32_t buff_size)
{
    cy_stc_scb_i2c_master_xfer_config_t transfer_config;
    cy_en_scb_i2c_status_t i2c_status = CY_SCB_I2C_SUCCESS;
    uint8_t retry_count               = RETRY_COUNT;
    uint32_t i2c_controller_stat      = RESET_VAL;
    uint32_t timeout_count;

    CY_ASSERT(NULL != ft5406_config);
    CY_ASSERT(NULL != read_buff);

    /* I2C controller transfer configuration */
    transfer_config.slaveAddress      = MTB_CTP_FT5406_I2C_ADDRESS;
    transfer_config.buffer            = read_buff;
    transfer_config.bufferSize        = buff_size;
    transfer_config.xferPending       = false;

    do
    {
        /* Initiate read transaction. */
        i2c_status = Cy_SCB_I2C_MasterRead(ft5406_config->i2c_base,
                                           &transfer_config,
                                           ft5406_config->i2c_context);

        if (CY_SCB_I2C_SUCCESS == i2c_status)
        {
            /* Total timeout 5 ms */
            timeout_count = I2C_TIMEOUT_COUNT;

            /* Wait until controller complete read transfer or time out has occurred */
            do
            {
                i2c_controller_stat = Cy_SCB_I2C_MasterGetStatus(ft5406_config->i2c_base,
                                                                 ft5406_config->i2c_context);
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
                Cy_SCB_I2C_Disable(ft5406_config->i2c_base, ft5406_config->i2c_context);
                Cy_SCB_I2C_Enable(ft5406_config->i2c_base);
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
* Function Name: mtb_ctp_i2c_controller_write
********************************************************************************
*
* This function configures the I2C controller to automatically write an entire
* buffer of data to touch panel controller acting as I2C target.
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
static cy_en_scb_i2c_status_t mtb_ctp_i2c_controller_write(uint8_t* write_buff,
                                                           uint32_t buff_size)
{
    cy_stc_scb_i2c_master_xfer_config_t transfer_config;
    cy_en_scb_i2c_status_t i2c_status = CY_SCB_I2C_SUCCESS;
    uint8_t retry_count               = RETRY_COUNT;
    uint32_t i2c_controller_stat      = RESET_VAL;
    uint32_t timeout_count;

    CY_ASSERT(NULL != ft5406_config);
    CY_ASSERT(NULL != write_buff);

    /* I2C controller transfer configuration */
    transfer_config.slaveAddress      = MTB_CTP_FT5406_I2C_ADDRESS;
    transfer_config.buffer            = write_buff;
    transfer_config.bufferSize        = buff_size;
    /* Generate Stop condition at the end of transaction */
    transfer_config.xferPending       = false;

    do
    {
        /* Initiate write transaction */
        /* The Start condition is generated to begin this transaction */
        i2c_status = Cy_SCB_I2C_MasterWrite(ft5406_config->i2c_base,
                                            &transfer_config,
                                            ft5406_config->i2c_context);

        if (CY_SCB_I2C_SUCCESS == i2c_status)
        {
            /* Total timeout 5 ms */
            timeout_count = I2C_TIMEOUT_COUNT;

            /* Wait until controller complete read transfer or time out has occurred */
            do
            {
                i2c_controller_stat = Cy_SCB_I2C_MasterGetStatus(ft5406_config->i2c_base,
                                                                 ft5406_config->i2c_context);
                Cy_SysLib_Delay(I2C_DELAY_MS);
                timeout_count--;
            } while ((CY_SCB_I2C_MASTER_BUSY & i2c_controller_stat) &&
                     timeout_count);

            if ((!(I2C_CONTROLLER_ERROR_MASK & i2c_controller_stat)) &&
                (transfer_config.bufferSize ==
                 Cy_SCB_I2C_MasterGetTransferCount(ft5406_config->i2c_base,
                                                   ft5406_config->i2c_context)))
            {
                i2c_status  = CY_SCB_I2C_SUCCESS;
                retry_count = RESET_VAL;
            }
            else
            {
                /* Timeout/error recovery */
                Cy_SCB_I2C_Disable(ft5406_config->i2c_base,
                                   ft5406_config->i2c_context);
                Cy_SCB_I2C_Enable(ft5406_config->i2c_base);
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
* Function Name: mtb_ctp_ft5406_read_raw_touch_data
********************************************************************************
*
* This function reads the raw x, y coordinate data from the touch panel
* controller.
*
* \param touch_data
* Pointer to the buffer to store touch data.
*
* \param size
* Size of the buffer to store touch data.
*
* \return cy_en_scb_i2c_status_t
* Read operation status based on I2C communication.
*
*******************************************************************************/
static cy_en_scb_i2c_status_t mtb_ctp_ft5406_read_raw_touch_data(uint8_t* touch_data,
                                                                 int size)
{
    cy_en_scb_i2c_status_t i2c_status = CY_SCB_I2C_SUCCESS;
    uint8_t get_touch_cmd = MTB_CTP_REG_READ_TOUCH_DATA;

    CY_ASSERT(NULL != touch_data);

    i2c_status = mtb_ctp_i2c_controller_write(&get_touch_cmd,
                                              sizeof(get_touch_cmd));

    if (CY_SCB_I2C_SUCCESS == i2c_status)
    {
        i2c_status = mtb_ctp_i2c_controller_read(touch_data, size);
    }

    return i2c_status;
}


/*******************************************************************************
* Function Name: mtb_ctp_ft5406_init
********************************************************************************
*
* Performs FT5406 Touch panel controller initialization using I2C interface.
*
* \param mtb_ft5406_config
* Pointer to the FT5406 configuration structure.
*
* \return cy_en_scb_i2c_status_t
* Touch panel initialization status based on I2C communication.
*
* \func usage
* \snippet snippet/main.c snippet_mtb_ctp_ft5406_init
*
*******************************************************************************/
cy_en_scb_i2c_status_t mtb_ctp_ft5406_init(mtb_ctp_ft5406_config_t* mtb_ft5406_config)
{
    cy_en_scb_i2c_status_t i2c_status = CY_SCB_I2C_SUCCESS;
    uint8_t device_mode[] = { MTB_CTP_FT5406_DEVICE_MODE, MTB_CTP_FT5406_NORMAL_MODE };

    CY_ASSERT(NULL != mtb_ft5406_config);

    ft5406_config = mtb_ft5406_config;

    i2c_status = mtb_ctp_i2c_controller_write(device_mode, sizeof(device_mode));

    return i2c_status;
}


/*******************************************************************************
* Function name: mtb_ctp_ft5406_get_single_touch
********************************************************************************
*
* Reads single touch event coordinates from the FT5406 touch panel controller
* using I2C interface.
*
* \param touch_event
* Pointer to the variable for capturing generated touch event.
*
* \param touch_x
* Pointer to the variable for capturing X touch co-ordinate of the generated
* touch event.
*
* \param touch_y
* Pointer to the variable for capturing Y touch co-ordinate of the generated
* touch event.
*
* \return cy_en_scb_i2c_status_t
* Single touch read operation status based on I2C communication.
*
*******************************************************************************/
cy_en_scb_i2c_status_t mtb_ctp_ft5406_get_single_touch(mtb_ctp_touch_event_t* touch_event,
                                                       int* touch_x,
                                                       int* touch_y)
{
    cy_en_scb_i2c_status_t i2c_status = CY_SCB_I2C_SUCCESS;

    CY_ASSERT(NULL != touch_event);
    CY_ASSERT(NULL != touch_x);
    CY_ASSERT(NULL != touch_y);

    /* Reset touch event */
    *touch_event = MTB_CTP_TOUCH_RESERVED;

    i2c_status = mtb_ctp_ft5406_read_raw_touch_data(ft5406_config->touch_buff,
                                                    MTB_CTP_FT5406_TOUCH_DATA_LEN);

    if (CY_SCB_I2C_SUCCESS == i2c_status)
    {
        mtb_ctp_ft5406_touch_data_t* touch_data =
            (mtb_ctp_ft5406_touch_data_t*)(void*)(ft5406_config->touch_buff);

        /* Get touch event for 1st touch point */
        *touch_event = TOUCH_POINT_GET_EVENT(touch_data->touch_points[0]);

        /* Update coordinates only if there is touch detected */
        if ((MTB_CTP_TOUCH_DOWN == *touch_event) \
            || (MTB_CTP_TOUCH_CONTACT == *touch_event))
        {
            if (touch_x)
            {
                *touch_x = TOUCH_POINT_GET_X(touch_data->touch_points[0]);
            }
            if (touch_y)
            {
                *touch_y = TOUCH_POINT_GET_Y(touch_data->touch_points[0]);
            }
        }
    }

    return i2c_status;
}


/*******************************************************************************
* Function name: mtb_ctp_ft5406_get_multi_touch
********************************************************************************
*
* Reads multi touch event coordinates from the FT5406 touch panel controller
* using I2C interface.
*
* \param touch_count
* Pointer to the variable holding multi touch counts.
*
* \param touch_array
* Pointer to the array for storing touch data for each detected touch point(s).
*
* \return cy_en_scb_i2c_status_t
* Multi touch read operation status based on I2C communication.
*
*******************************************************************************/
cy_en_scb_i2c_status_t mtb_ctp_ft5406_get_multi_touch(int* touch_count,
                                                      mtb_ctp_multi_touch_point_t touch_array[MTB_CTP_FT5406_MAX_TOUCHES])
{
    cy_en_scb_i2c_status_t i2c_status = CY_SCB_I2C_SUCCESS;
    uint8_t i = RESET_VAL;

    CY_ASSERT(NULL != touch_count);
    CY_ASSERT(NULL != touch_array);

    i2c_status = mtb_ctp_ft5406_read_raw_touch_data(ft5406_config->touch_buff,
                                                    MTB_CTP_FT5406_TOUCH_DATA_LEN);

    if (CY_SCB_I2C_SUCCESS == i2c_status)
    {
        mtb_ctp_ft5406_touch_data_t* touch_data =
            (mtb_ctp_ft5406_touch_data_t*)(void*)(ft5406_config->touch_buff);

        /* Check for valid number of touches - otherwise ignore touch information */
        if (MTB_CTP_FT5406_MAX_TOUCHES < touch_data->touch_detection_count)
        {
            touch_data->touch_detection_count = RESET_VAL;
        }

        /* Decode number of touches */
        if (touch_count)
        {
            *touch_count = touch_data->touch_detection_count;
        }

        /* Decode valid touch points */
        for (i = 0; i < touch_data->touch_detection_count; i++)
        {
            touch_array[i].touch_event = TOUCH_POINT_GET_EVENT(touch_data->touch_points[i]);
            touch_array[i].touch_id    = TOUCH_POINT_GET_ID(touch_data->touch_points[i]);
            touch_array[i].touch_x     = TOUCH_POINT_GET_X(touch_data->touch_points[i]);
            touch_array[i].touch_y     = TOUCH_POINT_GET_Y(touch_data->touch_points[i]);
        }

        /* Clear vacant elements of touch_array */
        for (i = 0; i < MTB_CTP_FT5406_MAX_TOUCHES; i++)
        {
            touch_array[i].touch_event = MTB_CTP_TOUCH_RESERVED;
            touch_array[i].touch_id    = RESET_VAL;
            touch_array[i].touch_x     = RESET_VAL;
            touch_array[i].touch_y     = RESET_VAL;
        }
    }

    return i2c_status;
}


/* [] END OF FILE */
