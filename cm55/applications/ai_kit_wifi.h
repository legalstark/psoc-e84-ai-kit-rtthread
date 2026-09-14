#ifndef AI_KIT_WIFI_H
#define AI_KIT_WIFI_H

#include <stdint.h>
#include <rtthread.h>

#define AI_KIT_WIFI_MAX_NETWORKS  (10U)
#define AI_KIT_WIFI_SSID_SIZE     (33U)
#define AI_KIT_WIFI_PASSWORD_SIZE (65U)
#define AI_KIT_WIFI_IPV4_SIZE     (16U)

typedef enum
{
    AI_KIT_WIFI_STATE_STARTING = 0,
    AI_KIT_WIFI_STATE_SCANNING,
    AI_KIT_WIFI_STATE_READY,
    AI_KIT_WIFI_STATE_ERROR
} ai_kit_wifi_state_t;

typedef enum
{
    AI_KIT_WIFI_CONNECTION_DISCONNECTED = 0,
    AI_KIT_WIFI_CONNECTION_CONNECTING,
    AI_KIT_WIFI_CONNECTION_CONNECTED,
    AI_KIT_WIFI_CONNECTION_ERROR
} ai_kit_wifi_connection_state_t;

typedef struct
{
    char ssid[AI_KIT_WIFI_SSID_SIZE];
    int32_t rssi;
    uint32_t channel;
} ai_kit_wifi_network_t;

typedef struct
{
    ai_kit_wifi_state_t state;
    rt_err_t result;
    uint32_t total_count;
    uint32_t visible_count;
    uint32_t generation;
    ai_kit_wifi_connection_state_t connection_state;
    rt_err_t connection_result;
    char connected_ssid[AI_KIT_WIFI_SSID_SIZE];
    char ipv4_address[AI_KIT_WIFI_IPV4_SIZE];
    ai_kit_wifi_network_t networks[AI_KIT_WIFI_MAX_NETWORKS];
} ai_kit_wifi_snapshot_t;

rt_err_t ai_kit_wifi_scan_async(void);
rt_err_t ai_kit_wifi_connect_async(const char *ssid, const char *password);
rt_err_t ai_kit_wifi_get_snapshot(ai_kit_wifi_snapshot_t *snapshot);

#endif
