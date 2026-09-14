#include <limits.h>
#include <rtthread.h>
#include <rtdevice.h>
#include <wlan_mgnt.h>
#include <wlan_cfg.h>
#include "cy_pdl.h"
#include "ai_kit_wifi.h"
#include "ai_kit_wifi_credentials.h"

#define WIFI_VALIDATION_MAGIC            (0x57494649UL)
#define WIFI_VALIDATION_VERSION          (5UL)
#define WIFI_VALIDATION_DEVICE_WAIT_MS   (15000UL)
#define WIFI_SCAN_THREAD_STACK_SIZE      (4096U)
#define WIFI_SCAN_THREAD_PRIORITY        (12U)
#define WIFI_SCAN_THREAD_TIMESLICE       (10U)
#define WIFI_CONNECT_READY_TIMEOUT_MS    (30000U)

typedef enum
{
    WIFI_JOB_NONE = 0,
    WIFI_JOB_SCAN,
    WIFI_JOB_CONNECT
} wifi_job_t;

typedef enum
{
    WIFI_VALIDATION_RESET = 0,
    WIFI_VALIDATION_WAIT_DEVICE = 1,
    WIFI_VALIDATION_SCANNING = 2,
    WIFI_VALIDATION_PASS = 3,
    WIFI_VALIDATION_ERROR = 4
} wifi_validation_state_t;

typedef struct
{
    uint32_t magic;
    uint32_t version;
    uint32_t state;
    int32_t result;
    uint32_t network_count;
    int32_t strongest_rssi;
    uint32_t strongest_channel;
    uint32_t heartbeat;
    char strongest_ssid[32];
} wifi_validation_status_t;

typedef char wifi_validation_status_size_must_be_64[
    (sizeof(wifi_validation_status_t) == 64U) ? 1 : -1];

__attribute__((section(".wifi_validation_status"), used, aligned(64)))
static volatile wifi_validation_status_t wifi_status;
static struct rt_mutex wifi_snapshot_lock;
static struct rt_semaphore wifi_scan_request;
static ai_kit_wifi_snapshot_t wifi_snapshot;
static ai_kit_wifi_network_t wifi_scan_networks[AI_KIT_WIFI_MAX_NETWORKS];
static uint32_t wifi_scan_network_count;
static uint32_t wifi_scan_total_count;
static rt_bool_t wifi_service_started;
static wifi_job_t wifi_pending_job;
static char wifi_connect_ssid[AI_KIT_WIFI_SSID_SIZE];
static char wifi_connect_password[AI_KIT_WIFI_PASSWORD_SIZE];
static char wifi_ready_ipv4[AI_KIT_WIFI_IPV4_SIZE];

static rt_bool_t wifi_network_is_visible(const struct rt_wlan_ssid *ssid)
{
    uint32_t index;

    for (index = 0U; index < wifi_scan_network_count; index++)
    {
        size_t visible_length = rt_strlen(wifi_scan_networks[index].ssid);
        if ((visible_length == ssid->len) &&
            (rt_memcmp(wifi_scan_networks[index].ssid, ssid->val,
                       visible_length) == 0))
        {
            return RT_TRUE;
        }
    }
    return RT_FALSE;
}

static void wifi_status_publish(void)
{
    SCB_CleanDCache_by_Addr((void *)&wifi_status, sizeof(wifi_status));
}

static rt_bool_t wifi_network_matches(const ai_kit_wifi_network_t *network,
                                      const struct rt_wlan_info *info)
{
    uint32_t length = info->ssid.len;

    if (length >= AI_KIT_WIFI_SSID_SIZE)
    {
        length = AI_KIT_WIFI_SSID_SIZE - 1U;
    }
    return (network->ssid[length] == '\0') &&
           (rt_strlen(network->ssid) == length) &&
           (rt_memcmp(network->ssid, info->ssid.val, length) == 0);
}

static void wifi_network_copy(ai_kit_wifi_network_t *network,
                              const struct rt_wlan_info *info)
{
    uint32_t length = info->ssid.len;

    if (length >= AI_KIT_WIFI_SSID_SIZE)
    {
        length = AI_KIT_WIFI_SSID_SIZE - 1U;
    }
    rt_memset(network, 0, sizeof(*network));
    rt_memcpy(network->ssid, info->ssid.val, length);
    network->rssi = info->rssi;
    network->channel = (uint32_t)(uint16_t)info->channel;
}

static void wifi_network_insert(const struct rt_wlan_info *info)
{
    uint32_t index;
    uint32_t weakest = 0U;

    if (info->ssid.len == 0U)
    {
        return;
    }

    wifi_scan_total_count++;
    for (index = 0U; index < wifi_scan_network_count; index++)
    {
        if (wifi_network_matches(&wifi_scan_networks[index], info))
        {
            if (info->rssi > wifi_scan_networks[index].rssi)
            {
                wifi_network_copy(&wifi_scan_networks[index], info);
            }
            return;
        }
        if (wifi_scan_networks[index].rssi < wifi_scan_networks[weakest].rssi)
        {
            weakest = index;
        }
    }

    if (wifi_scan_network_count < AI_KIT_WIFI_MAX_NETWORKS)
    {
        wifi_network_copy(&wifi_scan_networks[wifi_scan_network_count], info);
        wifi_scan_network_count++;
    }
    else if (info->rssi > wifi_scan_networks[weakest].rssi)
    {
        wifi_network_copy(&wifi_scan_networks[weakest], info);
    }
}

static void wifi_network_sort(void)
{
    uint32_t left;
    uint32_t right;

    for (left = 0U; left < wifi_scan_network_count; left++)
    {
        for (right = left + 1U; right < wifi_scan_network_count; right++)
        {
            if (wifi_scan_networks[right].rssi >
                wifi_scan_networks[left].rssi)
            {
                ai_kit_wifi_network_t temporary = wifi_scan_networks[left];
                wifi_scan_networks[left] = wifi_scan_networks[right];
                wifi_scan_networks[right] = temporary;
            }
        }
    }
}

static void wifi_scan_report(int event, struct rt_wlan_buff *buff,
                             void *parameter)
{
    const struct rt_wlan_info *info;
    RT_UNUSED(parameter);
    if ((event != RT_WLAN_EVT_SCAN_REPORT) || (buff == RT_NULL) ||
        (buff->data == RT_NULL))
    {
        return;
    }

    info = (const struct rt_wlan_info *)buff->data;
    wifi_network_insert(info);
}

static void wifi_snapshot_set_state(ai_kit_wifi_state_t state,
                                    rt_err_t result)
{
    rt_mutex_take(&wifi_snapshot_lock, RT_WAITING_FOREVER);
    wifi_snapshot.state = state;
    wifi_snapshot.result = result;
    rt_mutex_release(&wifi_snapshot_lock);
}

static void wifi_scan_once(void)
{
    rt_err_t result;

    wifi_snapshot_set_state(AI_KIT_WIFI_STATE_SCANNING, RT_EOK);
    rt_memset(wifi_scan_networks, 0, sizeof(wifi_scan_networks));
    wifi_scan_network_count = 0U;
    wifi_scan_total_count = 0U;

    result = rt_wlan_register_event_handler(RT_WLAN_EVT_SCAN_REPORT,
                                             wifi_scan_report, RT_NULL);
    if (result == RT_EOK)
    {
        result = rt_wlan_scan_with_info(RT_NULL);
        rt_wlan_unregister_event_handler(RT_WLAN_EVT_SCAN_REPORT);
    }
    wifi_network_sort();

    rt_mutex_take(&wifi_snapshot_lock, RT_WAITING_FOREVER);
    wifi_snapshot.result = result;
    wifi_snapshot.state = ((result == RT_EOK) &&
                           (wifi_scan_total_count > 0U)) ?
                          AI_KIT_WIFI_STATE_READY :
                          AI_KIT_WIFI_STATE_ERROR;
    wifi_snapshot.total_count = wifi_scan_total_count;
    wifi_snapshot.visible_count = wifi_scan_network_count;
    rt_memcpy(wifi_snapshot.networks, wifi_scan_networks,
              sizeof(wifi_snapshot.networks));
    wifi_snapshot.generation++;
    rt_mutex_release(&wifi_snapshot_lock);

    rt_memset((void *)&wifi_status, 0, sizeof(wifi_status));
    wifi_status.magic = WIFI_VALIDATION_MAGIC;
    wifi_status.version = WIFI_VALIDATION_VERSION;
    wifi_status.result = result;
    wifi_status.network_count = wifi_scan_total_count;
    wifi_status.heartbeat = wifi_snapshot.generation;
    if ((result == RT_EOK) && (wifi_scan_network_count > 0U))
    {
        wifi_status.state = WIFI_VALIDATION_PASS;
        wifi_status.strongest_rssi = wifi_scan_networks[0].rssi;
        wifi_status.strongest_channel = wifi_scan_networks[0].channel;
        rt_memcpy((void *)wifi_status.strongest_ssid,
                  wifi_scan_networks[0].ssid,
                  sizeof(wifi_status.strongest_ssid));
    }
    else
    {
        wifi_status.state = WIFI_VALIDATION_ERROR;
        wifi_status.strongest_rssi = INT32_MIN;
    }
    wifi_status_publish();
}

static void wifi_ready_report(int event, struct rt_wlan_buff *buff,
                              void *parameter)
{
    const uint8_t *address;

    RT_UNUSED(parameter);
    if ((event != RT_WLAN_EVT_READY) || (buff == RT_NULL) ||
        (buff->data == RT_NULL) || (buff->len < 4U))
    {
        return;
    }

    address = (const uint8_t *)buff->data;
    rt_snprintf(wifi_ready_ipv4, sizeof(wifi_ready_ipv4),
                "%u.%u.%u.%u", address[0], address[1],
                address[2], address[3]);
}

static void wifi_connect_once(void)
{
    rt_tick_t deadline;
    rt_err_t result;

    rt_mutex_take(&wifi_snapshot_lock, RT_WAITING_FOREVER);
    wifi_snapshot.connection_state = AI_KIT_WIFI_CONNECTION_CONNECTING;
    wifi_snapshot.connection_result = RT_EOK;
    rt_memset(wifi_snapshot.connected_ssid, 0,
              sizeof(wifi_snapshot.connected_ssid));
    rt_memset(wifi_snapshot.ipv4_address, 0,
              sizeof(wifi_snapshot.ipv4_address));
    rt_memcpy(wifi_snapshot.connected_ssid, wifi_connect_ssid,
              rt_strlen(wifi_connect_ssid));
    wifi_snapshot.generation++;
    rt_mutex_release(&wifi_snapshot_lock);

    rt_memset(wifi_ready_ipv4, 0, sizeof(wifi_ready_ipv4));
    rt_wlan_config_autoreconnect(RT_FALSE);
    if (rt_wlan_is_connected())
    {
        (void)rt_wlan_disconnect();
    }

    result = rt_wlan_register_event_handler(RT_WLAN_EVT_READY,
                                             wifi_ready_report, RT_NULL);
    if (result == RT_EOK)
    {
        result = rt_wlan_connect(wifi_connect_ssid,
                                 wifi_connect_password);
    }
    rt_memset(wifi_connect_password, 0, sizeof(wifi_connect_password));

    if (result == RT_EOK)
    {
        deadline = rt_tick_get() + rt_tick_from_millisecond(
            WIFI_CONNECT_READY_TIMEOUT_MS);
        while (!rt_wlan_is_ready())
        {
            if ((rt_int32_t)(rt_tick_get() - deadline) >= 0)
            {
                result = -RT_ETIMEOUT;
                break;
            }
            rt_thread_mdelay(100U);
        }
    }
    (void)rt_wlan_unregister_event_handler(RT_WLAN_EVT_READY);

    rt_mutex_take(&wifi_snapshot_lock, RT_WAITING_FOREVER);
    wifi_snapshot.connection_result = result;
    if ((result == RT_EOK) && rt_wlan_is_ready())
    {
        wifi_snapshot.connection_state = AI_KIT_WIFI_CONNECTION_CONNECTED;
        rt_memcpy(wifi_snapshot.ipv4_address, wifi_ready_ipv4,
                  rt_strlen(wifi_ready_ipv4));
    }
    else
    {
        wifi_snapshot.connection_state = AI_KIT_WIFI_CONNECTION_ERROR;
    }
    wifi_snapshot.generation++;
    rt_mutex_release(&wifi_snapshot_lock);
}

static void wifi_connect_saved_network(void)
{
    struct rt_wlan_cfg_info config;
    int index;
    int count = rt_wlan_cfg_get_num();

    if (count > RT_WLAN_CFG_INFO_MAX)
    {
        count = RT_WLAN_CFG_INFO_MAX;
    }
    for (index = 0; index < count; index++)
    {
        rt_memset(&config, 0, sizeof(config));
        if (rt_wlan_cfg_read_index(&config, index) != 1)
        {
            continue;
        }
        if (!wifi_network_is_visible(&config.info.ssid) ||
            (config.key.len >= sizeof(wifi_connect_password)))
        {
            rt_memset(&config, 0, sizeof(config));
            continue;
        }

        rt_memset(wifi_connect_ssid, 0, sizeof(wifi_connect_ssid));
        rt_memset(wifi_connect_password, 0, sizeof(wifi_connect_password));
        rt_memcpy(wifi_connect_ssid, config.info.ssid.val,
                  config.info.ssid.len);
        rt_memcpy(wifi_connect_password, config.key.val, config.key.len);
        rt_memset(&config, 0, sizeof(config));
        wifi_connect_once();
        if (rt_wlan_is_ready())
        {
            return;
        }
    }
}

static void wifi_validation_thread(void *parameter)
{
    rt_tick_t deadline;
    wifi_job_t job;
    RT_UNUSED(parameter);
    rt_memset((void *)&wifi_status, 0, sizeof(wifi_status));
    wifi_status.magic = WIFI_VALIDATION_MAGIC;
    wifi_status.version = WIFI_VALIDATION_VERSION;
    wifi_status.state = WIFI_VALIDATION_WAIT_DEVICE;
    wifi_status.strongest_rssi = INT32_MIN;
    wifi_status_publish();

    deadline = rt_tick_get() + rt_tick_from_millisecond(
        WIFI_VALIDATION_DEVICE_WAIT_MS);
    while (rt_device_find(RT_WLAN_DEVICE_STA_NAME) == RT_NULL)
    {
        if ((rt_int32_t)(rt_tick_get() - deadline) >= 0)
        {
            wifi_status.result = -RT_ETIMEOUT;
            wifi_status.state = WIFI_VALIDATION_ERROR;
            wifi_status_publish();
            wifi_snapshot_set_state(AI_KIT_WIFI_STATE_ERROR,
                                    -RT_ETIMEOUT);
            return;
        }
        rt_thread_mdelay(100U);
    }

    (void)ai_kit_wifi_credentials_init();
    wifi_scan_once();
    wifi_connect_saved_network();
    while (1)
    {
        rt_sem_take(&wifi_scan_request, RT_WAITING_FOREVER);
        rt_mutex_take(&wifi_snapshot_lock, RT_WAITING_FOREVER);
        job = wifi_pending_job;
        wifi_pending_job = WIFI_JOB_NONE;
        rt_mutex_release(&wifi_snapshot_lock);

        if (job == WIFI_JOB_SCAN)
        {
            wifi_scan_once();
        }
        else if (job == WIFI_JOB_CONNECT)
        {
            wifi_connect_once();
        }
    }
}

rt_err_t ai_kit_wifi_scan_async(void)
{
    rt_err_t result = RT_EOK;

    if (!wifi_service_started)
    {
        return -RT_ENOSYS;
    }

    rt_mutex_take(&wifi_snapshot_lock, RT_WAITING_FOREVER);
    if ((wifi_snapshot.state == AI_KIT_WIFI_STATE_SCANNING) ||
        (wifi_snapshot.connection_state ==
         AI_KIT_WIFI_CONNECTION_CONNECTING) ||
        (wifi_pending_job != WIFI_JOB_NONE))
    {
        result = -RT_EBUSY;
    }
    else
    {
        wifi_pending_job = WIFI_JOB_SCAN;
    }
    rt_mutex_release(&wifi_snapshot_lock);

    if (result == RT_EOK)
    {
        result = rt_sem_release(&wifi_scan_request);
    }
    return result;
}

rt_err_t ai_kit_wifi_connect_async(const char *ssid, const char *password)
{
    rt_err_t result = RT_EOK;
    size_t ssid_length;
    size_t password_length;

    if (!wifi_service_started || (ssid == RT_NULL) || (password == RT_NULL))
    {
        return -RT_EINVAL;
    }
    ssid_length = rt_strlen(ssid);
    password_length = rt_strlen(password);
    if ((ssid_length == 0U) || (ssid_length >= AI_KIT_WIFI_SSID_SIZE) ||
        (password_length >= AI_KIT_WIFI_PASSWORD_SIZE))
    {
        return -RT_EINVAL;
    }

    rt_mutex_take(&wifi_snapshot_lock, RT_WAITING_FOREVER);
    if ((wifi_snapshot.state == AI_KIT_WIFI_STATE_SCANNING) ||
        (wifi_snapshot.connection_state ==
         AI_KIT_WIFI_CONNECTION_CONNECTING) ||
        (wifi_pending_job != WIFI_JOB_NONE))
    {
        result = -RT_EBUSY;
    }
    else
    {
        rt_memset(wifi_connect_ssid, 0, sizeof(wifi_connect_ssid));
        rt_memset(wifi_connect_password, 0,
                  sizeof(wifi_connect_password));
        rt_memcpy(wifi_connect_ssid, ssid, ssid_length);
        rt_memcpy(wifi_connect_password, password, password_length);
        wifi_pending_job = WIFI_JOB_CONNECT;
    }
    rt_mutex_release(&wifi_snapshot_lock);

    if (result == RT_EOK)
    {
        result = rt_sem_release(&wifi_scan_request);
    }
    return result;
}

rt_err_t ai_kit_wifi_get_snapshot(ai_kit_wifi_snapshot_t *snapshot)
{
    if ((snapshot == RT_NULL) || !wifi_service_started)
    {
        return -RT_EINVAL;
    }

    rt_mutex_take(&wifi_snapshot_lock, RT_WAITING_FOREVER);
    *snapshot = wifi_snapshot;
    rt_mutex_release(&wifi_snapshot_lock);
    return RT_EOK;
}

static int wifi_validation_start(void)
{
    rt_thread_t thread;
    rt_err_t result;

    result = rt_mutex_init(&wifi_snapshot_lock, "wifi_snap",
                           RT_IPC_FLAG_PRIO);
    if (result != RT_EOK)
    {
        return result;
    }
    result = rt_sem_init(&wifi_scan_request, "wifi_scan", 0U,
                         RT_IPC_FLAG_PRIO);
    if (result != RT_EOK)
    {
        rt_mutex_detach(&wifi_snapshot_lock);
        return result;
    }
    rt_memset(&wifi_snapshot, 0, sizeof(wifi_snapshot));
    wifi_snapshot.state = AI_KIT_WIFI_STATE_STARTING;
    wifi_snapshot.connection_state = AI_KIT_WIFI_CONNECTION_DISCONNECTED;
    wifi_service_started = RT_TRUE;

    thread = rt_thread_create("wifi_val", wifi_validation_thread, RT_NULL,
                              WIFI_SCAN_THREAD_STACK_SIZE,
                              WIFI_SCAN_THREAD_PRIORITY,
                              WIFI_SCAN_THREAD_TIMESLICE);
    if (thread == RT_NULL)
    {
        wifi_service_started = RT_FALSE;
        rt_sem_detach(&wifi_scan_request);
        rt_mutex_detach(&wifi_snapshot_lock);
        return -RT_ENOMEM;
    }
    rt_thread_startup(thread);
    return RT_EOK;
}
INIT_APP_EXPORT(wifi_validation_start);
