# Waveshare 4.3-inch DSI display driver library API reference guide

## General description

A basic set of APIs to initialize and control the brightness of the Waveshare 4.3 inch DSI video mode display. 

For more information about the display, see [Waveshare 4.3 inch DSI Capacitive Touch Display](https://www.waveshare.com/4.3inch-dsi-lcd.html). <br>


## Code snippets

### Snippet 1: Display driver initialization and deinitialization as part of graphics application 

The following snippet initializes the display controller and then renders graphics artifacts using the display controller.

```
/* I2C controller at 100 kHz and GFXSS to be configured using Device Configurator. */
/* Displays specific MIPI DSI configurations. */
GFXSS_config.mipi_dsi_cfg = &mtb_disp_waveshare_4p3_dsi_config;

/* Sets frame buffer address to the GFXSS configuration structure. */
GFXSS_config.dc_cfg->gfx_layer_config->buffer_address    = frame_buffer;
GFXSS_config.dc_cfg->gfx_layer_config->uv_buffer_address = frame_buffer;

/* Initializes the graphics system according to the configuration. */
gfx_status = Cy_GFXSS_Init(GFXSS, &GFXSS_config, &gfx_context);

if (CY_GFX_SUCCESS == gfx_status)
{
   /* Initializes the I2C in controller mode. */
   i2c_result = Cy_SCB_I2C_Init(CYBSP_I2C_CONTROLLER_HW,
                                &CYBSP_I2C_CONTROLLER_config, 
                                &i2c_controller_context);

   if (CY_SCB_I2C_SUCCESS != i2c_result)
   {
      /* Handles possible errors. */
      CY_ASSERT(0);
   }

   /* Initializes the I2C interrupt. */
   sysint_status = Cy_SysInt_Init(&i2c_controller_irq_cfg,
                                  &i2c_controller_interrupt);

   if (CY_SYSINT_SUCCESS != sysint_status)
   {
      /* Handles possible errors. */
      CY_ASSERT(0);
   }

   /* Enables the I2C interrupts. */
   NVIC_EnableIRQ(i2c_controller_irq_cfg.intrSrc);

   /* Enables the I2C. */
   Cy_SCB_I2C_Enable(CYBSP_I2C_CONTROLLER_HW);

   /* Initializes the 4.3 inch display. */
   i2c_result = mtb_disp_waveshare_4p3_init(CYBSP_I2C_CONTROLLER_HW,
                                            &i2c_controller_context);

   if (CY_SCB_I2C_SUCCESS != i2c_result)
   {
      /* Handles possible errors. */
      CY_ASSERT(0);
   }

   /* Renders a graphics frame on the display as per application use case. 
    * An example of sending a graphics frame to the display is shown below,
    * assuming img_ptr holds the image frame.
    */

   Cy_GFXSS_Set_FrameBuffer((GFXSS_Type*) GFXSS, (uint32_t*) img_ptr,
                            &gfx_context);

   ...
   ...
   ...

   /* De-initializes the 4.3 inch display for safe shutdown. */
   i2c_result mtb_disp_waveshare_4p3_deinit(CYBSP_I2C_CONTROLLER_HW,
                                            &i2c_controller_context);
   
   if (CY_SCB_I2C_SUCCESS != i2c_result)
   {
      /* Handles possible errors. */
      CY_ASSERT(0);
   }
}
```

### Snippet 2: Changing display brightness

The following snippet shows how to use the brightness control API.

```
/* Calls this API from anywhere in the application code to set 
 * display brightness once backlight initialization is complete. 
 */
#define BRIGHTNESS_PERCENT     (70U)

/* Sets display brightness to the desired level (0% to 100%). */
i2c_result = mtb_disp_waveshare_4p3_set_brightness(CYBSP_I2C_CONTROLLER_HW,
                                                   &i2c_controller_context,
                                                   BRIGHTNESS_PERCENT);

if (CY_SCB_I2C_SUCCESS != i2c_result)
{
   /* Handles possible errors. */
   CY_ASSERT(0);
}
```

## Macros

```
MTB_DISP_WAVESHARE_4P3_ID_REG                 Displays panel ID register
MTB_DISP_WAVESHARE_4P3_CTRL_REG               Displays panel control register
MTB_DISP_WAVESHARE_4P3_POWERON_REG            Displays panel power on register
MTB_DISP_WAVESHARE_4P3_BRIGHTNESS_CTRL_REG    Displays panel brightness control register

MTB_DISP_WAVESHARE_4P3_ID_1                   Displays panel controller ID (Atmel)
MTB_DISP_WAVESHARE_4P3_ID_2                   Displays panel controller ID (STM)
MTB_DISP_WAVESHARE_4P3_NUM_ID_BYTES           Number of ID bytes 

MTB_DISP_WAVESHARE_4P3_DISABLE_CMD            Displays panel disable command
MTB_DISP_WAVESHARE_4P3_ENABLE_CMD             Displays panel enable command
MTB_DISP_WAVESHARE_4P3_POWERON_CMPLT          Displays panel power on complete command

MTB_DISP_WAVESHARE_4P3_I2C_ADDR               Displays panel I2C target address
MTB_DISP_WAVESHARE_4P3_I2C_PKT_SIZE           Displays panel I2C target packet size

MIPI_MAX_PHY_CLK_HZ                           Maximum DSI PHY clock value

MTB_DISP_WAVESHARE_4P3_NUM_LANES              Number of lanes supported by display panel

MTB_DISP_WAVESHARE_4P3_HOR_RES                Displays panel horizontal resolution  
MTB_DISP_WAVESHARE_4P3_VER_RES                Displays panel vertical resolution 

MTB_DISP_WAVESHARE_4P3_HSYNC_WIDTH            Displays panel HSYNC width
MTB_DISP_WAVESHARE_4P3_HFP                    Displays panel HFP
MTB_DISP_WAVESHARE_4P3_HBP                    Displays panel HBP
MTB_DISP_WAVESHARE_4P3_VSYNC_WIDTH            Displays panel VSYNC width
MTB_DISP_WAVESHARE_4P3_VFP                    Displays panel VFP width
MTB_DISP_WAVESHARE_4P3_VBP                    Displays panel VBP width

MTB_DISP_WAVESHARE_4P3_PER_LANE_MBPS          Displays panel per lane bit rate
MTB_DISP_WAVESHARE_4P3_PIXEL_CLK_KHZ          Pixel clock supported by display panel (in KHz)
```

## Global variables
```
extern cy_stc_mipidsi_display_params_t mtb_disp_waveshare_4p3_display_params  Structure holding display-specific MIPI DSI parameters 
extern cy_stc_mipidsi_config_t mtb_disp_waveshare_4p3_dsi_config              Structure holding display-specific MIPI DSI configuration
```

## Functions

cy_en_scb_i2c_status_t `mtb_disp_waveshare_4p3_init(CySCB_Type* i2c_base, cy_stc_scb_i2c_context_t* i2c_context)`
- Performs display initialization using an I2C interface

cy_en_scb_i2c_status_t `mtb_disp_waveshare_4p3_set_brightness(CySCB_Type* i2c_base, cy_stc_scb_i2c_context_t* i2c_context, uint8_t brightness_percent)`
- Controls the brightness of Waveshare 4.3-inch DSI display using an I2C interface

cy_en_scb_i2c_status_t `mtb_disp_waveshare_4p3_deinit(CySCB_Type* i2c_base, cy_stc_scb_i2c_context_t* i2c_context)`
- De-initializes the display panel


## Global variable documentation 

Data type         |    Variable                |           Description     
:-------         |  :------------           |  :------------
cy_stc_mipidsi_display_params_t          |    mtb_disp_waveshare_4p3_display_params        | Structure holding display-specific MIPI DSI parameters 
cy_stc_mipidsi_config_t          |    mtb_disp_waveshare_4p3_dsi_config         | Structure holding display-specific MIPI DSI configuration

<br>

## Function documentation

#### mtb_disp_waveshare_4p3_init

- cy_en_scb_i2c_status_t `mtb_disp_waveshare_4p3_init(CySCB_Type* i2c_base, cy_stc_scb_i2c_context_t* i2c_context)`

   **Summary:** Performs display initialization using an I2C interface.

   **Parameter**
   Parameters            |  Description       
   :-------              |  :------------
   [in] i2c_base    |  Pointer to the I2C instance base address
   [in] i2c_context    |  Pointer to the I2C context structure
   
   <br>
   
   **Return**
   - cy_en_scb_i2c_status_t: Display panel initialization status based on I2C communication

#### mtb_disp_waveshare_4p3_set_brightness

- cy_en_scb_i2c_status_t `mtb_disp_waveshare_4p3_set_brightness(CySCB_Type* i2c_base, cy_stc_scb_i2c_context_t* i2c_context, uint8_t brightness_percent)`

   **Summary:** Controls the brightness of Waveshare 4.3 inch DSI display using an I2C interface.

   **Parameter**
   Parameters            |  Description       
   :-------              |  :------------
   [in] i2c_base    |  Pointer to the I2C instance base address
   [in] i2c_context    |  Pointer to the I2C context structure
   [in] brightness_percent    |  Target brightness percentage for the display
   
   <br>

   **Return**
   - cy_en_scb_i2c_status_t: Display brightness control operation status based on I2C communication

#### mtb_disp_waveshare_4p3_deinit

- cy_en_scb_i2c_status_t `mtb_disp_waveshare_4p3_deinit(CySCB_Type* i2c_base, cy_stc_scb_i2c_context_t* i2c_context)`

   **Summary:** De-initializes the display using an I2C interface.

   **Parameter**
   Parameters            |  Description       
   :-------              |  :------------
   [in] i2c_base    |  Pointer to the I2C instance base address
   [in] i2c_context    |  Pointer to the I2C context structure

   <br>
   
   **Return**
   - cy_en_scb_i2c_status_t: Display panel de-initialization status based on I2C communication

---
© 2025, Cypress Semiconductor Corporation (an Infineon company)
