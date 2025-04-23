#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "lwip/err.h"
#include "lwip/sys.h"

#include "wifi_config.h"
#include "wifi_interface.h"

#include "esp_netif.h"

#include "esp_sntp.h"

#include "mbedtls/aes.h"
#include "mbedtls/gcm.h"
#include <string.h>
#include <stdlib.h>

#define AES_KEY_SIZE 16 // 128-bit key
#define CBC_IV_SIZE 16  // 16 bytes for CBC mode

// Your encryption key (should be stored securely, e.g., in flash)
static const uint8_t encryption_key[AES_KEY_SIZE] = {
    0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
    0xfe, 0xdc, 0xba, 0x98, 0x76, 0x54, 0x32, 0x10};

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

// Converts a hex string to bytes
static esp_err_t hex_to_bytes(const char *hex, uint8_t *bytes, size_t bytes_len)
{
    size_t hex_len = strlen(hex);
    if (hex_len % 2 != 0 || bytes_len < hex_len / 2)
    {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t i = 0; i < hex_len; i += 2)
    {
        char hex_byte[3] = {hex[i], hex[i + 1], 0};
        bytes[i / 2] = strtol(hex_byte, NULL, 16);
    }

    return ESP_OK;
}
esp_err_t decrypt_password(const char *encrypted_hex, char *output, size_t output_size)
{
    ESP_LOGI(TAG, "Attempting to decrypt: %s", encrypted_hex);

    // Check if password is encrypted
    if (!encrypted_hex || strlen(encrypted_hex) == 0)
    {
        ESP_LOGE(TAG, "No input supplied - password string is empty or NULL");
        return ESP_ERR_INVALID_ARG;
    }

    if (strncmp(encrypted_hex, "ENC:", 4) != 0)
    {
        ESP_LOGI(TAG, "Password is not encrypted (no ENC: prefix)");
        // Not encrypted, just copy it
        if (strlen(encrypted_hex) >= output_size)
        {
            ESP_LOGE(TAG, "Unencrypted password too long for buffer");
            return ESP_ERR_INVALID_SIZE;
        }
        strcpy(output, encrypted_hex);
        return ESP_OK;
    }

    // Skip the "ENC:" prefix
    encrypted_hex += 4;
    size_t encrypted_hex_len = strlen(encrypted_hex);
    ESP_LOGI(TAG, "After removing prefix, encrypted hex length: %d", encrypted_hex_len);

    // Calculate sizes
    if (encrypted_hex_len % 2 != 0)
    {
        ESP_LOGE(TAG, "Invalid hex string length (must be even)");
        return ESP_ERR_INVALID_ARG;
    }

    size_t encrypted_bytes_len = encrypted_hex_len / 2;

    // Make sure we have enough space for IV + at least some data
    if (encrypted_bytes_len <= CBC_IV_SIZE)
    {
        ESP_LOGE(TAG, "Encrypted data too short to contain IV");
        return ESP_ERR_INVALID_SIZE;
    }

    // Allocate buffer for the encrypted data
    uint8_t *encrypted_bytes = malloc(encrypted_bytes_len);
    if (!encrypted_bytes)
    {
        ESP_LOGE(TAG, "Failed to allocate memory for encrypted data");
        return ESP_ERR_NO_MEM;
    }

    // Convert hex to bytes
    for (size_t i = 0; i < encrypted_hex_len; i += 2)
    {
        char hex_byte[3] = {encrypted_hex[i], encrypted_hex[i + 1], 0};
        encrypted_bytes[i / 2] = strtol(hex_byte, NULL, 16);
    }

    ESP_LOGI(TAG, "Successfully converted hex to bytes, encrypted_bytes_len: %d", encrypted_bytes_len);

    // Extract the IV (first 16 bytes)
    uint8_t iv[CBC_IV_SIZE];
    memcpy(iv, encrypted_bytes, CBC_IV_SIZE);

    // Extract ciphertext
    uint8_t *ciphertext = encrypted_bytes + CBC_IV_SIZE;
    size_t ciphertext_len = encrypted_bytes_len - CBC_IV_SIZE;

    ESP_LOGI(TAG, "IV size: %d, Ciphertext size: %d", CBC_IV_SIZE, ciphertext_len);

    // Check if ciphertext length is a multiple of 16 bytes (AES block size)
    if (ciphertext_len % 16 != 0)
    {
        ESP_LOGE(TAG, "Ciphertext length (%d) is not a multiple of 16 bytes", ciphertext_len);
        ESP_LOGE(TAG, "First few bytes: %02x %02x %02x %02x",
                 encrypted_bytes[0], encrypted_bytes[1], encrypted_bytes[2], encrypted_bytes[3]);
        free(encrypted_bytes);
        return ESP_ERR_INVALID_SIZE;
    }

    // Set up AES context
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);

    // Set the key for decryption
    int ret = mbedtls_aes_setkey_dec(&aes, encryption_key, AES_KEY_SIZE * 8);
    if (ret != 0)
    {
        ESP_LOGE(TAG, "mbedtls_aes_setkey_dec failed: -0x%x", -ret);
        mbedtls_aes_free(&aes);
        free(encrypted_bytes);
        return ESP_FAIL;
    }

    // Allocate buffer for the plaintext
    uint8_t *plaintext = malloc(ciphertext_len);
    if (!plaintext)
    {
        ESP_LOGE(TAG, "Failed to allocate memory for plaintext");
        mbedtls_aes_free(&aes);
        free(encrypted_bytes);
        return ESP_ERR_NO_MEM;
    }

    // Decrypt using CBC mode
    ret = mbedtls_aes_crypt_cbc(&aes,
                                MBEDTLS_AES_DECRYPT,
                                ciphertext_len,
                                iv,
                                ciphertext,
                                plaintext);

    if (ret != 0)
    {
        ESP_LOGE(TAG, "mbedtls_aes_crypt_cbc failed: -0x%x", -ret);
        mbedtls_aes_free(&aes);
        free(encrypted_bytes);
        free(plaintext);
        return ESP_FAIL;
    }

    // Handle PKCS#7 padding (browser adds this automatically)
    uint8_t padding_value = plaintext[ciphertext_len - 1];

    // Validate padding (PKCS#7 padding value must be between 1 and 16)
    if (padding_value < 1 || padding_value > 16)
    {
        ESP_LOGE(TAG, "Invalid padding value: %d", padding_value);
        mbedtls_aes_free(&aes);
        free(encrypted_bytes);
        free(plaintext);
        return ESP_ERR_INVALID_ARG;
    }

    // Verify padding bytes (all padding bytes should be equal to padding_value)
    for (int i = 1; i <= padding_value; i++)
    {
        if (plaintext[ciphertext_len - i] != padding_value)
        {
            ESP_LOGE(TAG, "Invalid padding pattern at position %d", ciphertext_len - i);
            mbedtls_aes_free(&aes);
            free(encrypted_bytes);
            free(plaintext);
            return ESP_ERR_INVALID_ARG;
        }
    }

    size_t actual_len = ciphertext_len - padding_value;
    ESP_LOGI(TAG, "After removing padding, plaintext length: %d", actual_len);

    if (actual_len >= output_size)
    {
        ESP_LOGE(TAG, "Decrypted password too long for output buffer");
        mbedtls_aes_free(&aes);
        free(encrypted_bytes);
        free(plaintext);
        return ESP_ERR_INVALID_SIZE;
    }

    // Copy decrypted data to output buffer
    memcpy(output, plaintext, actual_len);
    output[actual_len] = '\0';

    ESP_LOGI(TAG, "Successfully decrypted password");

    // Clean up
    mbedtls_aes_free(&aes);
    free(encrypted_bytes);
    free(plaintext);

    return ESP_OK;
}

void trim_whitespace(char *str)
{
    int i = strlen(str) - 1;
    while (i >= 0 && (str[i] == ' ' || str[i] == '\n' || str[i] == '\r'))
    {
        str[i] = '\0';
        i--;
    }
}

esp_err_t read_settings_from_conf(const char *file_path, wifi_config_t *wifi_config)
{
    if (!file_path || !wifi_config)
    {
        return ESP_ERR_INVALID_ARG;
    }

    FILE *file = fopen(file_path, "r");
    if (!file)
    {
        ESP_LOGE(TAG, "Failed to open file: %s (errno: %d, %s)",
                 file_path, errno, strerror(errno));
        return ESP_FAIL;
    }

    char line[256];
    char ssid[64] = {0};
    char encrypted_password[256] = {0};
    char decrypted_password[64] = {0};

    while (fgets(line, sizeof(line), file))
    {
        line[strcspn(line, "\n")] = 0;

        if (strncmp(line, "ssid=", 5) == 0)
        {
            strncpy(ssid, line + 5, sizeof(ssid) - 1);
        }
        else if (strncmp(line, "wifiPassword=", 13) == 0)
        {
            strncpy(encrypted_password, line + 13, sizeof(encrypted_password) - 1);
        }
    }

    fclose(file);

    // Debug logging for encrypted password
    ESP_LOGI(TAG, "Encrypted password from file: %s", encrypted_password);

    if (strlen(ssid) == 0 || strlen(encrypted_password) == 0)
    {
        ESP_LOGE(TAG, "SSID or password not found in .conf file");
        return ESP_FAIL;
    }

    esp_err_t err = decrypt_password(encrypted_password, decrypted_password, sizeof(decrypted_password));
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to decrypt password: %d", err);
        return err;
    }

    // DEBUG ONLY - Log decrypted password (REMOVE THIS IN PRODUCTION)
    // ESP_LOGI(TAG, "DEBUG - Decrypted password: %s", decrypted_password);
    // ESP_LOGI(TAG, "DEBUG - Password length: %d", strlen(decrypted_password));

    size_t ssid_len = strlen(ssid);
    size_t pass_len = strlen(decrypted_password);

    if (ssid_len >= sizeof(wifi_config->sta.ssid) ||
        pass_len >= sizeof(wifi_config->sta.password))
    {
        ESP_LOGE(TAG, "SSID or password too long");
        return ESP_ERR_INVALID_SIZE;
    }

    trim_whitespace(ssid);
    trim_whitespace(decrypted_password);

    memset(wifi_config->sta.ssid, 0, sizeof(wifi_config->sta.ssid));
    memset(wifi_config->sta.password, 0, sizeof(wifi_config->sta.password));
    memcpy(wifi_config->sta.ssid, ssid, ssid_len);
    memcpy(wifi_config->sta.password, decrypted_password, pass_len);

    ESP_LOGI(TAG, "Read SSID: %s", wifi_config->sta.ssid);
    ESP_LOGI(TAG, "Password successfully decrypted and configured");

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

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)event_data;
        ESP_LOGI(TAG, "WiFi disconnected, reason: %d", event->reason);
        update_wifi_status(false);
        if (s_retry_num < MAXIMUM_RETRY)
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
        s_retry_num = 0;
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));

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
        ESP_LOGE(TAG, "register_wifi_status_callback called with NULL");
        return ESP_ERR_INVALID_ARG;
    }
    status_callback = callback;
    ESP_LOGI(TAG, "WiFi status callback registered successfully.");
    return ESP_OK;
}

void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, &instance_got_ip));

    ESP_LOGI(TAG, "Waiting 1 second before WiFi start");
    vTaskDelay(pdMS_TO_TICKS(1000));

    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .capable = true,
                .required = false},
        },
    };

    // Default fallback SSID/PWD
    strncpy((char *)wifi_config.sta.ssid, "SPAIA", sizeof(wifi_config.sta.ssid));
    strncpy((char *)wifi_config.sta.password, "bugslife", sizeof(wifi_config.sta.password));

    // Try to override from SD card
    if (read_settings_from_conf("/sd/spaia.conf", &wifi_config) != ESP_OK)
    {
        ESP_LOGW(TAG, "Using fallback WiFi credentials");
    }

    // Set protocol and disable power save before start
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    ESP_ERROR_CHECK(esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_init_sta finished.");

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           pdMS_TO_TICKS(15000)); // graceful timeout

    if (bits & WIFI_CONNECTED_BIT)
    {
        ESP_LOGI(TAG, "connected to ap");
        esp_netif_ip_info_t ip_info;
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (netif)
        {
            ESP_ERROR_CHECK(esp_netif_get_ip_info(netif, &ip_info));
            ESP_LOGI(TAG, "IP Address: " IPSTR, IP2STR(&ip_info.ip));
        }
    }
    else if (bits & WIFI_FAIL_BIT)
    {
        ESP_LOGW(TAG, "WiFi connection failed (event-driven)");
    }
    else
    {
        ESP_LOGW(TAG, "WiFi connection timed out");
        esp_wifi_stop(); // cleanup after failure
    }
}
esp_err_t wifi_enable(void)
{
    if (is_wifi_connected())
    {
        ESP_LOGI(TAG, "WiFi already enabled and connected");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Enabling WiFi...");

    // Initialize WiFi if not already initialized
    static bool wifi_initialized = false;
    if (!wifi_initialized)
    {
        // Initialize NVS if this is the first time
        esp_err_t ret = nvs_flash_init();
        if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
        {
            ESP_ERROR_CHECK(nvs_flash_erase());
            ret = nvs_flash_init();
        }
        ESP_ERROR_CHECK(ret);

        ESP_LOGI(TAG, "ESP_WIFI_MODE_STA");
        wifi_init_sta();
        wifi_initialized = true;
    }
    else
    {
        // If WiFi was previously initialized but disabled, just start it again
        ESP_LOGI(TAG, "Restarting WiFi...");
        ESP_ERROR_CHECK(esp_wifi_start());

        // Wait for connection or timeout
        int retry_count = 0;
        while (!is_wifi_connected() && retry_count < 20) // Wait up to 10 seconds
        {
            vTaskDelay(pdMS_TO_TICKS(500));
            retry_count++;
        }
    }

    if (is_wifi_connected())
    {
        ESP_LOGI(TAG, "WiFi enabled and connected successfully");
        return ESP_OK;
    }
    else
    {
        ESP_LOGW(TAG, "WiFi enabled but failed to connect");
        return ESP_FAIL;
    }
}

esp_err_t wifi_disable(void)
{
    if (!is_wifi_connected())
    {
        ESP_LOGI(TAG, "WiFi already disabled");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Disabling WiFi to save power...");
    esp_err_t ret = esp_wifi_stop();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to disable WiFi: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "WiFi disabled successfully");
    return ESP_OK;
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
