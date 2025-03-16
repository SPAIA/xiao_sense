#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "cJSON.h"

#include "lwip/err.h"
#include "lwip/sys.h"

#include "wifi_config.h"
#include "wifi_interface.h"

#include "esp_netif.h"

#include "esp_sntp.h"

static volatile bool wifi_connected = false;
static wifi_status_callback_t status_callback = NULL;
static EventGroupHandle_t s_wifi_event_group;

/* The event group allows multiple bits for each event, but we only care about two events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

static const char *TAG = "wifi station";

static int s_retry_num = 0;

esp_err_t read_settings_from_json(const char *file_path, wifi_config_t *wifi_config)
{
    if (!file_path || !wifi_config)
    {
        return ESP_ERR_INVALID_ARG;
    }

    FILE *file = fopen(file_path, "r");
    if (!file)
    {
        ESP_LOGE(TAG, "Failed to open file: %s", file_path);
        return ESP_FAIL;
    }

    // Get file size
    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    rewind(file);

    // Allocate buffer
    char *buffer = malloc(file_size + 1);
    if (!buffer)
    {
        fclose(file);
        ESP_LOGE(TAG, "Memory allocation failed");
        return ESP_ERR_NO_MEM;
    }

    // Read file
    size_t read_size = fread(buffer, 1, file_size, file);
    fclose(file);

    if (read_size != file_size)
    {
        free(buffer);
        ESP_LOGE(TAG, "Failed to read file completely");
        return ESP_FAIL;
    }
    buffer[file_size] = '\0';

    // Parse JSON
    cJSON *root = cJSON_Parse(buffer);
    free(buffer); // Free buffer as soon as we're done with it

    if (!root)
    {
        ESP_LOGE(TAG, "Failed to parse JSON");
        return ESP_FAIL;
    }

    // Get wifi object
    cJSON *wifi = cJSON_GetObjectItem(root, "wifi");
    if (!wifi)
    {
        ESP_LOGE(TAG, "Missing 'wifi' settings");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    // Get individual settings with error checking
    cJSON *ssid = cJSON_GetObjectItem(wifi, "ssid");
    cJSON *password = cJSON_GetObjectItem(wifi, "password");
    cJSON *auth_mode = cJSON_GetObjectItem(wifi, "auth_mode");
    cJSON *scan_method = cJSON_GetObjectItem(wifi, "scan_method");
    cJSON *sort_method = cJSON_GetObjectItem(wifi, "sort_method");
    cJSON *retry_count = cJSON_GetObjectItem(wifi, "retry_count");

    if (!ssid || !cJSON_IsString(ssid) ||
        !password || !cJSON_IsString(password) ||
        !auth_mode || !cJSON_IsNumber(auth_mode) ||
        !scan_method || !cJSON_IsNumber(scan_method) ||
        !sort_method || !cJSON_IsNumber(sort_method) ||
        !retry_count || !cJSON_IsNumber(retry_count))
    {
        ESP_LOGE(TAG, "Missing or invalid wifi settings");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    // Copy values with bounds checking
    size_t ssid_len = strlen(ssid->valuestring);
    size_t pass_len = strlen(password->valuestring);

    if (ssid_len >= sizeof(wifi_config->sta.ssid) ||
        pass_len >= sizeof(wifi_config->sta.password))
    {
        ESP_LOGE(TAG, "SSID or password too long");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_SIZE;
    }

    // Safe copy of strings
    memset(wifi_config->sta.ssid, 0, sizeof(wifi_config->sta.ssid));
    memset(wifi_config->sta.password, 0, sizeof(wifi_config->sta.password));
    memcpy(wifi_config->sta.ssid, ssid->valuestring, ssid_len);
    memcpy(wifi_config->sta.password, password->valuestring, pass_len);

    // Set other parameters
    wifi_config->sta.threshold.authmode = auth_mode->valueint;
    wifi_config->sta.scan_method = scan_method->valueint;
    wifi_config->sta.sort_method = sort_method->valueint;
    wifi_config->sta.failure_retry_cnt = retry_count->valueint;

    cJSON_Delete(root);
    return ESP_OK;
}

void obtain_time(void *pvParameters)
{
    ESP_LOGI(TAG, "Starting NTP sync task.");

    // Initialize SNTP
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "pool.ntp.org"); // Use default NTP server, you can replace with a specific one
    sntp_init();

    // Wait for time to be set via NTP
    time_t now = 0;
    struct tm timeinfo = {0};
    int retry = 0;
    const int retry_count = 10;

    while (timeinfo.tm_year < (1970 - 1900) && ++retry < retry_count)
    {
        ESP_LOGI(TAG, "Waiting for system time to be set... (%d/%d)", retry, retry_count);
        vTaskDelay(2000 / portTICK_PERIOD_MS);
        time(&now);
        localtime_r(&now, &timeinfo);
    }

    if (retry < retry_count)
    {
        ESP_LOGI(TAG, "System time set successfully.");
    }
    else
    {
        ESP_LOGE(TAG, "Failed to get time from NTP server.");
    }

    // Once done, delete the task
    vTaskDelete(NULL);
}

static void update_wifi_status(bool new_status)
{
    if (wifi_connected != new_status)
    {
        wifi_connected = new_status;
        if (status_callback != NULL)
        {
            status_callback(new_status);
        }
    }
}

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    static int s_retry_num = 0;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        update_wifi_status(false);
        if (s_retry_num < EXAMPLE_ESP_MAXIMUM_RETRY)
        {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        }
        else
        {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG, "connect to the AP fail");
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        update_wifi_status(true);
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        // TODO: move this somehere more logical
        //  Create a FreeRTOS task to fetch NTP time once connected
        xTaskCreatePinnedToCore(obtain_time, "obtain_time", 4096, NULL, 7, NULL, PRO_CPU_NUM);
    }
}
bool is_wifi_connected(void)
{
    return wifi_connected;
}
esp_err_t register_wifi_status_callback(wifi_status_callback_t callback)
{
    if (callback == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    status_callback = callback;
    return ESP_OK;
}

void wifi_init_sta(void)
{
    esp_log_level_set("wifi", ESP_LOG_VERBOSE);
    esp_log_level_set("wifi_init", ESP_LOG_VERBOSE);
    esp_log_level_set("wpa", ESP_LOG_VERBOSE);
    esp_log_level_set("wpa_supplicant", ESP_LOG_VERBOSE);
    esp_log_level_set("esp_netif_handlers", ESP_LOG_VERBOSE);
    esp_log_level_set("system_api", ESP_LOG_VERBOSE);
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    // Initialize wifi_config with zeros
    wifi_config_t wifi_config = {0};

    // Read configuration from SD card
    esp_err_t err = read_settings_from_json("/sd/spaia/config.json", &wifi_config);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to read WiFi configuration from SD card");
        // You might want to load fallback configuration here
        return;
    }
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    esp_wifi_set_ps(WIFI_PS_NONE);
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_init_sta finished.");

    /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
     * number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above) */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           portMAX_DELAY);

    /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
     * happened. */
    if (bits & WIFI_CONNECTED_BIT)
    {
        ESP_LOGI(TAG, "connected to ap SSID:%s password:%s",
                 EXAMPLE_ESP_WIFI_SSID, EXAMPLE_ESP_WIFI_PASS);
        esp_netif_ip_info_t ip_info;
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (netif)
        {
            ESP_ERROR_CHECK(esp_netif_get_ip_info(netif, &ip_info));
            ESP_LOGI(TAG, "IP Address: " IPSTR, IP2STR(&ip_info.ip));
            ESP_LOGI(TAG, "Subnet Mask: " IPSTR, IP2STR(&ip_info.netmask));
            ESP_LOGI(TAG, "Gateway: " IPSTR, IP2STR(&ip_info.gw));
        }
        else
        {
            ESP_LOGE(TAG, "Failed to get netif handle");
        }
    }
    else if (bits & WIFI_FAIL_BIT)
    {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s, password:%s",
                 EXAMPLE_ESP_WIFI_SSID, EXAMPLE_ESP_WIFI_PASS);
    }
    else
    {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
    }
}

void initialize_wifi(void)
{
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "ESP_WIFI_MODE_STA");
    wifi_init_sta();
}
