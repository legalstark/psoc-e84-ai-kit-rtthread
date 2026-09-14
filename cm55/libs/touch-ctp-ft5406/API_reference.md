# FT5406 capacitive touch panel driver library API reference guide

## General description

This library provides a basic set of functions to support Capacitive Touch Panel (CTP) of Waveshare 4.3 inch DSI display driven by [FT5406](https://www.displayfuture.com/Display/datasheet/controller/FT5x06.pdf) CTP controller. 

For more information about the display, see [Waveshare 4.3 inch DSI Capacitive Touch Display](https://www.waveshare.com/4.3inch-dsi-lcd.htm).


## Code snippets

### Snippet 1: Touch driver initialization and reading touch data

The following snippet initializes the touch panel driver and reads touch data from it.

```
/* FT5406 touch controller configuration. */
mtb_ctp_ft5406_config_t ft5406_config =
{
	.i2c_base    = CYBSP_I2C_CONTROLLER_HW,
	.i2c_context = &i2c_controller_context
};

/* Perform graphics subsystem and I2C initialization prior to touch driver initialization. */

/* Initializes FT5406 touch driver. */
result = mtb_ctp_ft5406_init(&ft5406_config);
if (CY_SCB_I2C_SUCCESS != result)
{
    /* Handles possible errors. */
    CY_ASSERT(0);
}

/* Variables to store the touch event and its X, Y coordinates. */
static int touch_x = 0;
static int touch_y = 0;
mtb_ctp_touch_event_t touch_event;

/* Reads the touch coordinates when a touch event is detected.
 * Invokes this function within the input device read callback registered to
 * the graphics framework or periodically/callback to the touch interrupt in 
 * a bare metal application.
 */

/* Reads the touch coordinates. */
result = mtb_ctp_ft5406_get_single_touch(&touch_event, &touch_x, &touch_y);

if ((CY_SCB_I2C_SUCCESS == result) && ((touch_event == MTB_CTP_TOUCH_DOWN) || (touch_event == MTB_CTP_TOUCH_CONTACT)))
{
    /* After receiving a touch down or touch contact event, pass the read 
     * touch coordinates to the graphics framework to take action on the UI 
     * elements.
     */
    state = PRESSED_STATE;

    /* 4.3 inch display has inverted touch input. */ 
    point.x = (DISPLAY_HORIZONTAL_RES - 1) - touch_x;
    point.y = (DISPLAY_VERTICAL_RES - 1) - touch_y;
}
```

Multitouch
```
int touch_count;
mtb_ctp_multi_touch_point_t touch_array[MTB_CTP_FT5406_MAX_TOUCHES];

/* Multitouch can also be detected using the following API. */
result = mtb_ctp_ft5406_get_multi_touch(&touch_count, touch_array);

if (CY_SCB_I2C_SUCCESS == result) 
{
    /* Processes multitouch coordinates and take action accordingly on 
     * the UI elements.
     */
}
```

## Macros

```
MTB_CTP_FT5406_MAX_TOUCHES                  Maximum number of touch points detected simultaneously by FT5406

MTB_CTP_FT5406_TOUCH_DATA_SUBADDR           FT5406 register address where touch data begins
MTB_CTP_FT5406_TOUCH_DATA_LEN               FT5406 raw touch data length

MTB_CTP_FT5406_DEVICE_MODE                  FT5406 device mode register (supports Active, Monitor, and Hibernate modes)   
MTB_CTP_FT5406_NORMAL_MODE                  Active/Normal mode of FT5406
MTB_CTP_REG_READ_TOUCH_DATA                 FT5406 touch data register

MTB_CTP_FT5406_I2C_ADDRESS                  I2C address of FT5406
                            
```

## Data structures
```
enum mtb_ctp_touch_event_t                  Type of touch events
struct mtb_ctp_ft5406_touch_point_t         Touch point definition structure
struct mtb_ctp_ft5406_touch_data_t          Structure to store touch data 
struct mtb_ctp_multi_touch_point_t          Structure to store touch data for each detected touch point in case of multitouch
struct mtb_ctp_ft5406_config_t              FT5406 touch controller configuration structure
```

## Functions
cy_en_scb_i2c_status_t `mtb_ctp_ft5406_init(mtb_ctp_ft5406_config_t* mtb_ctp_ft5406_config)`
- Performs FT5406 touch panel controller initialization using an I2C interface

cy_en_scb_i2c_status_t `mtb_ctp_ft5406_get_single_touch(mtb_ctp_touch_event_t *touch_event, int* touch_x, int* touch_y)` 
- Reads single touch event coordinates from the FT5406 touch panel controller using an I2C interface

cy_en_scb_i2c_status_t `mtb_ctp_ft5406_get_multi_touch(int *touch_count, mtb_ctp_multi_touch_point_t touch_array[MTB_CTP_FT5406_MAX_TOUCHES])`
- Reads multitouch event coordinates from the FT5406 touch panel controller using an I2C interface

## Data structure documentation

- mtb_ctp_touch_event_t

  Member variable      |    Description
  :------------         |  :------------
  MTB_CTP_TOUCH_DOWN             |  The state changed to touched
  MTB_CTP_TOUCH_UP             |  The state changed to not touched
  MTB_CTP_TOUCH_CONTACT             |  There is a continuous touch being detected
  MTB_CTP_TOUCH_RESERVED             |  No touch information available

  <br>

- mtb_ctp_ft5406_touch_point_t

  Data type                 | Member variable      |    Description
  :-------                  |:------------         |  :------------
  uint8_t                   | XH                   | MSB byte for X touch coordinate
  uint8_t                   | XL                   | LSB byte for X touch coordinate
  uint8_t                   | YH                   | MSB byte for Y touch coordinate
  uint8_t                   | YL                   | LSB byte for Y touch coordinate
  uint8_t                   | reserved[2]          | Reserved

  <br>

- mtb_ctp_ft5406_touch_data_t

  Data type                 | Member variable      |    Description
  :-------                  |:------------         |  :------------
  uint8_t                   | gesture_id                   | Detected gesture ID
  uint8_t                   | touch_detection_count                   | Number of touch detected
  mtb_ctp_ft5406_touch_point_t                   | touch_points[MTB_CTP_FT5406_MAX_TOUCHES]             | Touch point value structure to support 5 point multitouch 

  <br>

 - mtb_ctp_multi_touch_point_t

   Data type                 | Member variable      |    Description
   :-------                  |:------------         |  :------------
   mtb_ctp_touch_event_t                   | touch_event                   | Indicates the state or event of the touch point
   uint8_t                   | touch_id                   | ID of the detected touch point
   uint16_t                   | touch_x        | 16-bit X touch coordinate
   uint16_t                   | touch_y                   | 16-bit Y touch coordinate

   <br>

 - mtb_ctp_ft5406_config_t

   Data type                 | Member variable      |    Description
   :-------                  |:------------         |  :------------
   CySCB_Type*                   | i2c_base                   | Pointer to I2C instance's base address
   cy_stc_scb_i2c_context_t*                   | i2c_context                   | Pointer to valid I2C context
   uint8_t                   | touch_buff[MTB_CTP_FT5406_TOUCH_DATA_LEN]       | Touch buffer to store 32-byte captured touch read data

   <br>

## Function documentation

#### mtb_ctp_ft5406_init

- cy_en_scb_i2c_status_t `mtb_ctp_ft5406_init(mtb_ctp_ft5406_config_t* mtb_ft5406_config)`

  **Summary:** Performs FT5406 Touch panel controller initialization using an I2C interface.
 
  **Parameter**
  Parameters            |  Description       
  :-------              |  :------------
  [in] mtb_ft5406_config    |  Pointer to the FT5406 configuration structure

  <br>
 
  **Return**
  - cy_en_scb_i2c_status_t: Touch panel initialization status based on I2C communication

#### mtb_ctp_ft5406_get_single_touch

- cy_en_scb_i2c_status_t `mtb_ctp_ft5406_get_single_touch(mtb_ctp_touch_event_t* touch_event, int* touch_x, int* touch_y)`

  **Summary:** This function fetches raw touch coordinates using an internal "mtb_ctp_ft5406_read_raw_touch_data" function, and returns actual x, y touch coordinates.
 
  **Parameter**
   Parameters            |  Description       
   :-------              |  :------------
   [in] touch_event    |  Pointer to the variable for capturing the generated touch event
   [in] touch_x    |   Pointer to the variable for capturing the X touch coordinate of the generated touch event
   [in] touch_y    |   Pointer to the variable for capturing the Y touch coordinate of the generated touch event

   <br>

   **Return**
   - cy_en_scb_i2c_status_t: Single touch read operation status based on I2C communication

#### mtb_ctp_ft5406_get_multi_touch

- cy_en_scb_i2c_status_t `mtb_ctp_ft5406_get_multi_touch(int* touch_count, mtb_ctp_multi_touch_point_t touch_array[MTB_CTP_FT5406_MAX_TOUCHES])`

  **Summary:** Reads multitouch event coordinates from the FT5406 touch panel controller using an I2C interface.
 
  **Parameter:**
   Parameters            |  Description       
   :-------              |  :------------
   [in] touch_count  |   Pointer to the variable holding multitouch counts
   [in] touch_array    |   Pointer to the array for storing touch data for each detected touch point(s)

   <br>

  **Return**
  - cy_en_scb_i2c_status_t: Multitouch read operation status based on I2C communication

---
© 2025, Cypress Semiconductor Corporation (an Infineon company)
