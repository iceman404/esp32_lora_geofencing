#include <stdio.h>
#include <inttypes.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nmea_parser.h"
#include "driver/uart.h"

#include "lora.h"

#include "ssd1306.h"  // Include LED module only if CONFIG_RECEIVER is defined
//#include "font8x8_basic.h"


#define TAG "Lora"
//#define CONFIG_FREERTOS_IDLE_TASK_STACKSIZE 1024  // Adjust if necessary

#define TIME_ZONE_HOURS (5)   // Time zone in hours
#define TIME_ZONE_MINUTES (45) // Additional minutes

// Calculate total offset in minutes
#define TIME_ZONE_OFFSET_MINUTES (TIME_ZONE_HOURS * 60 + TIME_ZONE_MINUTES)

#define YEAR_BASE (2000) // Date in GPS starts from 2000
#define TX_TASK_STACK_SIZE (1024*3)
#define RX_TASK_STACK_SIZE (1024*3)

// Function to adjust the GPS time with the time zone offset
void adjust_time_with_timezone(gps_t *gps) {
    // Convert GPS time to total minutes since midnight
    int total_minutes = gps->tim.hour * 60 + gps->tim.minute;

    // Adjust time by adding the time zone offset
    total_minutes += TIME_ZONE_OFFSET_MINUTES;

    // Calculate new hour and minute
    int new_hour = (total_minutes / 60) % 24;
    int new_minute = total_minutes % 60;

    // Update GPS time with adjusted values
    gps->tim.hour = new_hour;
    gps->tim.minute = new_minute;
}

// Function to handle GPS events
static void gps_event_handler(void *event_handler_arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    gps_t *gps = NULL;
    char message[256];

    switch (event_id) {
        case GPS_UPDATE:
            gps = (gps_t *)event_data;

            // Adjust GPS time with the time zone offset
            adjust_time_with_timezone(gps);

            // Log adjusted GPS data to serial monitor
            ESP_LOGI(TAG, "%d/%d/%d %d:%d:%d => \r\n"
            "\t\t\t\t\t\tlatitude   = %.05f°N\r\n"
            "\t\t\t\t\t\tlongitude = %.05f°E\r\n"
            "\t\t\t\t\t\taltitude   = %.02fm\r\n"
            "\t\t\t\t\t\tspeed      = %.02fm/s",
            gps->date.year + YEAR_BASE, gps->date.month, gps->date.day,
            gps->tim.hour, gps->tim.minute, gps->tim.second,
            gps->latitude, gps->longitude, gps->altitude, gps->speed);

            // Format and send GPS data via LoRa
            int send_len = snprintf(message, sizeof(message),
                                    "Lat: %.05f, Lon: %.05f, Alt: %.02fm, Speed: %.02fm/s",
                                    gps->latitude, gps->longitude, gps->altitude, gps->speed);
            lora_send_packet((uint8_t *)message, send_len);
            ESP_LOGI(TAG, "%d byte GPS packet sent...", send_len);
            break;

        case GPS_UNKNOWN:
            // Log any unknown NMEA sentences
            ESP_LOGW(TAG, "Unknown statement: %s", (char *)event_data);
            break;

        default:
            break;
    }
}


#if CONFIG_SENDER
void task_tx(void *pvParameters)
{
    ESP_LOGI(pcTaskGetName(NULL), "TX Task Started");

    // NMEA parser configuration
    nmea_parser_config_t config = NMEA_PARSER_CONFIG_DEFAULT();
    // Initialize NMEA parser library
    nmea_parser_handle_t nmea_hdl = nmea_parser_init(&config);

    // Register GPS event handler
    nmea_parser_add_handler(nmea_hdl, gps_event_handler, NULL);

    // The main loop can still perform additional LoRa operations or just idle
    while (1) {
        // For example, send a periodic status message if needed
        uint8_t buf[256];
        int send_len = sprintf((char *)buf, "LoRa status: Running...");
        lora_send_packet(buf, send_len);
        ESP_LOGI(pcTaskGetName(NULL), "%d byte status packet sent...", send_len);

        vTaskDelay(pdMS_TO_TICKS(10000)); // Adjust delay as needed (10 seconds here)
    }

    // Cleanup (unlikely to be reached unless the task is deleted)
    nmea_parser_remove_handler(nmea_hdl, gps_event_handler);
    nmea_parser_deinit(nmea_hdl);
}
#endif // CONFIG_SENDER



#if CONFIG_RECEIVER
void task_rx(void *pvParameters)
{
    SSD1306_t *dev = (SSD1306_t *)pvParameters; // Get the display struct from parameters
    ESP_LOGI(pcTaskGetName(NULL), "RX Task Started");



    uint8_t buf[256]; // Buffer for received data
    char msg[256]; // Buffer for message to display

    // Check if the display device is initialized
    if (dev == NULL) {
        ESP_LOGE(TAG, "SSD1306 device not initialized");
        vTaskDelete(NULL); // Stop task if display is not available
    }

    while (1) {
        lora_receive(); // Put LoRa module into receive mode
        if (lora_received()) {
            int rxLen = lora_receive_packet(buf, sizeof(buf));
            ESP_LOGI(pcTaskGetName(NULL), "%d byte packet received:[%.*s]", rxLen, rxLen, buf);

            // Print the received message to the serial monitor
            printf("Received: %.*s\n", rxLen, buf);

            // Display message on SSD1306
            int start_index = 0;
            for (int row = 0; row < 4; row++) {
                if (start_index < rxLen) {
                    int end_index = start_index + 13; // Number of characters per row
                    if (end_index > rxLen) {
                        end_index = rxLen;
                    }
                    // Ensure the string is null-terminated
                    char line[14] = {0};
                    strncpy(line, (char *)&buf[start_index], end_index - start_index);
                    ssd1306_display_text(dev, row, line, end_index - start_index, false);
                    start_index = end_index;
                } else {
                    ssd1306_display_text(dev, row, "", 0, false);
                }
            }
            vTaskDelay(pdMS_TO_TICKS(1000)); // Delay to ensure visibility
        }
        vTaskDelay(1); // Avoid WatchDog alerts
    }
}
#endif // CONFIG_RECEIVER



void app_main()
{

    #if CONFIG_RECEIVER
    SSD1306_t *dev = malloc(sizeof(SSD1306_t));
    if (dev == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for SSD1306_t");
        return;
    }
    #endif

    #if CONFIG_I2C_INTERFACE
    i2c_master_init(dev, CONFIG_SDA_GPIO, CONFIG_SCL_GPIO, CONFIG_RESET_GPIO);
    #endif

    #if CONFIG_SSD1306_128x64
    ESP_LOGI(TAG, "Panel is 128x64");
    ssd1306_init(dev, 128, 64);
    #endif


    if (lora_init() == 0) {
        ESP_LOGE(pcTaskGetName(NULL), "Does not recognize the module");
        while(1) {
            vTaskDelay(1);
        }
    }

    #if CONFIG_169MHZ
    ESP_LOGI(pcTaskGetName(NULL), "Frequency is 169MHz");
    lora_set_frequency(169e6); // 169MHz
    #elif CONFIG_433MHZ
    ESP_LOGI(pcTaskGetName(NULL), "Frequency is 433MHz");
    lora_set_frequency(433e6); // 433MHz
    #elif CONFIG_470MHZ
    ESP_LOGI(pcTaskGetName(NULL), "Frequency is 470MHz");
    lora_set_frequency(470e6); // 470MHz
    #elif CONFIG_866MHZ
    ESP_LOGI(pcTaskGetName(NULL), "Frequency is 866MHz");
    lora_set_frequency(866e6); // 866MHz
    #elif CONFIG_915MHZ
    ESP_LOGI(pcTaskGetName(NULL), "Frequency is 915MHz");
    lora_set_frequency(915e6); // 915MHz
    #elif CONFIG_OTHER
    ESP_LOGI(pcTaskGetName(NULL), "Frequency is %dMHz", CONFIG_OTHER_FREQUENCY);
    long frequency = CONFIG_OTHER_FREQUENCY * 1000000;
    lora_set_frequency(frequency);
    #endif

    lora_enable_crc();

    int cr = 1;
    int bw = 7;
    int sf = 7;
    #if CONFIF_ADVANCED
    cr = CONFIG_CODING_RATE
    bw = CONFIG_BANDWIDTH;
    sf = CONFIG_SF_RATE;
    #endif

    lora_set_coding_rate(cr);
    //lora_set_coding_rate(CONFIG_CODING_RATE);
    //cr = lora_get_coding_rate();
    ESP_LOGI(pcTaskGetName(NULL), "coding_rate=%d", cr);

    lora_set_bandwidth(bw);
    //lora_set_bandwidth(CONFIG_BANDWIDTH);
    //int bw = lora_get_bandwidth();
    ESP_LOGI(pcTaskGetName(NULL), "bandwidth=%d", bw);

    lora_set_spreading_factor(sf);
    //lora_set_spreading_factor(CONFIG_SF_RATE);
    //int sf = lora_get_spreading_factor();
    ESP_LOGI(pcTaskGetName(NULL), "spreading_factor=%d", sf);

    #if CONFIG_SENDER
    xTaskCreate(&task_tx, "TX", 1024*3, NULL, 5, NULL);
    #endif
    #if CONFIG_RECEIVER
    xTaskCreate(&task_rx, "RX", 1024*3, NULL, 5, NULL);
    #endif

    #if CONFIG_RECEIVER
    free(dev);
    #endif


}
