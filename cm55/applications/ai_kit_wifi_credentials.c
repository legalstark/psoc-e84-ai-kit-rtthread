#include <rtthread.h>
#include <wlan_cfg.h>
#include "cy_pdl.h"
#include "cymem_CM55_0.h"
#include "ai_kit_wifi_credentials.h"

#define WIFI_CREDENTIALS_ADDRESS   (CYMEM_CM55_0_user_nvm_C_START)
#define WIFI_CREDENTIALS_CAPACITY  (2048U)
#define WIFI_CREDENTIALS_MAGIC     (RT_WLAN_CFG_MAGIC)

typedef struct
{
    rt_uint32_t magic;
    rt_uint32_t len;
    rt_uint32_t num;
    rt_uint32_t crc;
} wifi_credentials_header_t;

static int wifi_credentials_read(void *buffer, int length)
{
    cy_en_rram_status_t status;

    if ((buffer == RT_NULL) || (length <= 0) ||
        ((rt_uint32_t)length > WIFI_CREDENTIALS_CAPACITY))
    {
        return 0;
    }

    status = Cy_RRAM_NvmReadByteArray(RRAMC0,
                                      WIFI_CREDENTIALS_ADDRESS,
                                      (uint8_t *)buffer,
                                      (uint32_t)length);
    return (status == CY_RRAM_SUCCESS) ? length : 0;
}

static int wifi_credentials_get_length(void)
{
    wifi_credentials_header_t header;

    if (wifi_credentials_read(&header, sizeof(header)) != sizeof(header))
    {
        return 0;
    }
    if ((header.magic != WIFI_CREDENTIALS_MAGIC) ||
        (header.len < sizeof(header)) ||
        (header.len > WIFI_CREDENTIALS_CAPACITY) ||
        (header.num > RT_WLAN_CFG_INFO_MAX))
    {
        return 0;
    }
    return (int)header.len;
}

static int wifi_credentials_write(void *buffer, int length)
{
    uint8_t verify[64];
    const uint8_t *source = (const uint8_t *)buffer;
    uint32_t offset = 0U;
    cy_en_rram_status_t status;

    if ((buffer == RT_NULL) ||
        (length < (int)sizeof(wifi_credentials_header_t)) ||
        ((rt_uint32_t)length > WIFI_CREDENTIALS_CAPACITY))
    {
        return 0;
    }

    status = Cy_RRAM_NvmWriteByteArray(RRAMC0,
                                       WIFI_CREDENTIALS_ADDRESS,
                                       source,
                                       (uint32_t)length);
    if (status != CY_RRAM_SUCCESS)
    {
        return 0;
    }

    while (offset < (uint32_t)length)
    {
        uint32_t chunk = (uint32_t)length - offset;
        if (chunk > sizeof(verify))
        {
            chunk = sizeof(verify);
        }
        status = Cy_RRAM_NvmReadByteArray(RRAMC0,
                                          WIFI_CREDENTIALS_ADDRESS + offset,
                                          verify, chunk);
        if ((status != CY_RRAM_SUCCESS) ||
            (rt_memcmp(verify, source + offset, chunk) != 0))
        {
            rt_memset(verify, 0, sizeof(verify));
            return 0;
        }
        offset += chunk;
    }
    rt_memset(verify, 0, sizeof(verify));
    return length;
}

static const struct rt_wlan_cfg_ops wifi_credentials_ops =
{
    .read_cfg = wifi_credentials_read,
    .get_len = wifi_credentials_get_length,
    .write_cfg = wifi_credentials_write,
};

rt_err_t ai_kit_wifi_credentials_init(void)
{
    rt_wlan_cfg_set_ops(&wifi_credentials_ops);

    /* An empty or CRC-invalid region is the normal first-boot condition. */
    if (wifi_credentials_get_length() == 0)
    {
        return RT_EOK;
    }
    return rt_wlan_cfg_cache_refresh();
}
