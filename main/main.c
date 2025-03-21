 #include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "mqtt_client.h"
#include "driver/adc.h"
#include "time.h"
#include "esp_sntp.h"

#define WIFI_SSID CONFIG_WIFI_SSID
#define WIFI_PASSWORD CONFIG_WIFI_PASSWORD

#define MQTT_BROKER_URI CONFIG_MQTT_BROKER_URI
#define MQTT_USER CONFIG_MQTT_USER
#define MQTT_PASSWORD CONFIG_MQTT_PASSWORD

// Event Group for Wi-Fi and MQTT connection status
static EventGroupHandle_t wifi_event_group;
const int WIFI_CONNECTED_BIT = BIT0;
const int MQTT_CONNECTED_BIT = BIT1;

// Logging tag
static const char *TAG = "ESP32_MQTT_ADC";

// MQTT client handle
static esp_mqtt_client_handle_t mqtt_client;

// ADC Configuration
#define SENSOR_A1 ADC1_CHANNEL_1 // GPIO 1 for ADC1 Channel 1 on ESP32-S3

// Initialize SNTP for time synchronization
void initialize_sntp(void) {
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "pool.ntp.org");
    sntp_init();
}

// Function to get the current hour and minute
bool get_current_time(int *hour, int *minute) {
    time_t now;
    struct tm timeinfo;

    // Get the current time
    time(&now);
    localtime_r(&now, &timeinfo);

    // Check if the time is valid
    if (timeinfo.tm_year < (2023 - 1900)) { // If year is less than 2023, time is not synchronized
        return false;
    }

    *hour = timeinfo.tm_hour;
    *minute = timeinfo.tm_min;
    return true;
}

// Wi-Fi event handler
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

// Initialize Wi-Fi
static void wifi_init(void) {
    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASSWORD,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "Wi-Fi initialization complete");
}

// MQTT event handler
static esp_err_t mqtt_event_handler_cb(esp_mqtt_event_handle_t event) {
    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT connected");
            xEventGroupSetBits(wifi_event_group, MQTT_CONNECTED_BIT); // Set MQTT connected bit
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGE(TAG, "MQTT disconnected");
            xEventGroupClearBits(wifi_event_group, MQTT_CONNECTED_BIT); // Clear MQTT connected bit
            break;
        case MQTT_EVENT_PUBLISHED:
            ESP_LOGI(TAG, "Message published");
            break;
        default:
            break;
    }
    return ESP_OK;
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    mqtt_event_handler_cb(event_data);
}

// Initialize MQTT
static void mqtt_app_start(void) {
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_BROKER_URI,
        .credentials.username = MQTT_USER,
        .credentials.authentication.password = MQTT_PASSWORD,
    };
    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, mqtt_client);
    esp_mqtt_client_start(mqtt_client);
}

// Task to read ADC and publish data via MQTT
// Task to read ADC and publish data via MQTT
static void adc_mqtt_task(void *pvParameters) {
    // Configure ADC
    adc1_config_width(ADC_WIDTH_BIT_12); // 12-bit resolution
    adc1_config_channel_atten(SENSOR_A1, ADC_ATTEN_DB_12); // 0-3.9V range

    // Initialize SNTP
    initialize_sntp();

    bool sent_morning = false, sent_afternoon = false, sent_evening = false; // Flags to track if data was sent

    while (1) {
        // Wait for both Wi-Fi and MQTT to be connected
        xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT | MQTT_CONNECTED_BIT, false, true, portMAX_DELAY);

        // Get the current time
        int current_hour, current_minute;
        if (!get_current_time(&current_hour, &current_minute)) {
            vTaskDelay(pdMS_TO_TICKS(5000)); // Retry after 5 seconds
            continue;
        }

        // Reset flags at midnight (local time)
        if (current_hour == 0 && current_minute == 0) {
            sent_morning = false;
            sent_afternoon = false;
            sent_evening = false;
        }

        // Check if it's time to send data (local times: 9:00, 14:00, 18:00)
        if ((current_hour == 5 && current_minute == 0 && !sent_morning) ||
            (current_hour == 10 && current_minute == 0 && !sent_afternoon) ||
            (current_hour == 14 && current_minute == 0 && !sent_evening)) {

            // Read ADC value
            int value = adc1_get_raw(SENSOR_A1);

            float percent_value = 100 - ((value - 2300.0) * 100.0 / 1795.0);

            // Clamp percentage value between 0% and 100%
            if (percent_value > 100) {
                percent_value = 100;
            } else if (percent_value < 0) {
                percent_value = 0;
            }

            // Log the soil moisture percentage
            ESP_LOGI(TAG, "ADC Value: %d, Percentage: %.2f%%", value, percent_value);

            char payload[50];
            snprintf(payload, sizeof(payload), "%.2f%%", percent_value);
            esp_mqtt_client_publish(mqtt_client, "garden/soil-moisture/s-1", payload, 0, 1, 0);

            // Update flags
            if (current_hour == 5) {
                sent_morning = true;
            } else if (current_hour == 10) {
                sent_afternoon = true;
            } else if (current_hour == 14) {
                sent_evening = true;
            }
        }

        // Delay for 1 second before checking the time again
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void app_main(void) {
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "ESP32-S3 MQTT + ADC Example");

    gpio_set_direction(GPIO_NUM_9, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_NUM_9, 1);

    // Initialize Wi-Fi
    wifi_init();

    // Initialize MQTT
    mqtt_app_start();

    // Start ADC + MQTT task
    xTaskCreate(adc_mqtt_task, "adc_mqtt_task", 4096, NULL, 5, NULL);
}