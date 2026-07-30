#include "hardware.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "mqtt_service.h"

// For TFT SPI standard interface
#include "driver/spi_master.h"

static const char *TAG = "HARDWARE";

#ifdef KEYPAD_ROW0
// Keypad mapping table
static const char keypad_map[4][4] = {
    {'1', '2', '3', 'A'},
    {'4', '5', '6', 'B'},
    {'7', '8', '9', 'C'},
    {'*', '0', '#', 'D'}
};

static const gpio_num_t keypad_rows[4] = {KEYPAD_ROW0, KEYPAD_ROW1, KEYPAD_ROW2, KEYPAD_ROW3};
static const gpio_num_t keypad_cols[4] = {KEYPAD_COL0, KEYPAD_COL1, KEYPAD_COL2, KEYPAD_COL3};
#endif

void hardware_init(void) {
    ESP_LOGI(TAG, "Initializing Hardware Control (Relay, Buzzer, Sonar, Keypad, TFT)");

    gpio_config_t io_conf = {};
    
    // 1. Config Relay (Fix for 5V Active-LOW relays driven by 3.3V ESP32)
    gpio_reset_pin(RELAY_PIN); // Required for JTAG pin (GPIO 42) to work as normal GPIO
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT_OD; // OPEN-DRAIN MODE (Critical for 5V Relays)
    io_conf.pin_bit_mask = (1ULL << RELAY_PIN);
    io_conf.pull_down_en = 0;
    io_conf.pull_up_en = 0;
    gpio_config(&io_conf);
    gpio_set_level(RELAY_PIN, 1); // Open-Drain: 1 = Float (High-Z) -> Relay OFF

    // 2. Config Buzzer
    io_conf.pin_bit_mask = (1ULL << BUZZER_PIN);
    gpio_config(&io_conf);
    gpio_set_level(BUZZER_PIN, 0); // Default OFF

    // 3. Sonar Sensor Initial setup
    gpio_reset_pin(SONAR_TRIG_PIN);
    gpio_reset_pin(SONAR_ECHO_PIN); // Ensure GPIO 41 is reset from JTAG!
    
    // Trig pin: Output
    io_conf.pin_bit_mask = (1ULL << SONAR_TRIG_PIN);
    gpio_config(&io_conf);
    gpio_set_level(SONAR_TRIG_PIN, 0);

    // Echo pin: Input
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ULL << SONAR_ECHO_PIN);
    io_conf.pull_down_en = 1;
    io_conf.pull_up_en = 0;
    gpio_config(&io_conf);

#ifdef KEYPAD_ROW0
    // 4. Config Keypad Rows (Outputs, default HIGH)
    // Reset JTAG pins to GPIO (39-42)
    gpio_reset_pin(KEYPAD_ROW0); // 41
    gpio_reset_pin(KEYPAD_ROW1); // 42
    gpio_reset_pin(KEYPAD_ROW2); // 43
    gpio_reset_pin(KEYPAD_ROW3); // 0
    gpio_reset_pin(KEYPAD_COL0); // 45
    gpio_reset_pin(KEYPAD_COL1); // 46
    gpio_reset_pin(KEYPAD_COL2); // 47
    gpio_reset_pin(KEYPAD_COL3); // 48
    
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pull_down_en = 0;
    io_conf.pull_up_en = 0;
    uint64_t row_mask = 0;
    for (int i = 0; i < 4; i++) {
        row_mask |= (1ULL << keypad_rows[i]);
    }
    io_conf.pin_bit_mask = row_mask;
    gpio_config(&io_conf);
    for (int i = 0; i < 4; i++) {
        gpio_set_level(keypad_rows[i], 1);
    }

    // 5. Config Keypad Columns (Inputs with internal Pull-up)
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_down_en = 0;
    io_conf.pull_up_en = 1; // Pull-up is necessary to detect GND when row is pulled low
    uint64_t col_mask = 0;
    for (int i = 0; i < 4; i++) {
        col_mask |= (1ULL << keypad_cols[i]);
    }
    io_conf.pin_bit_mask = col_mask;
    gpio_config(&io_conf);
#else
    ESP_LOGI(TAG, "Keypad not configured (GPIOs commented out in board_config.h)");
#endif

#if defined(TFT_SPI_MOSI) && TFT_SPI_MOSI >= 0
    // 6. TFT Display ST7789 Initial setup
    gpio_reset_pin(TFT_SPI_MOSI); // 42 is JTAG MTMS
    gpio_reset_pin(TFT_SPI_SCLK);
    gpio_reset_pin(TFT_DC_PIN);
#if TFT_RST_PIN >= 0
    gpio_reset_pin(TFT_RST_PIN);
#endif
    ESP_LOGI(TAG, "TFT ST7789 SPI interface configured on MOSI:%d, SCLK:%d, CS:%d, DC:%d, RST:%d", 
             TFT_SPI_MOSI, TFT_SPI_SCLK, TFT_CS_PIN, TFT_DC_PIN, TFT_RST_PIN);
#else
    ESP_LOGI(TAG, "TFT ST7789 not configured");
#endif
}

void open_door(void) {
    tft_display_status("DOOR OPENING");
    ESP_LOGI(TAG, "Opening Door... (Active LOW relay OD: IN=0)");
    ESP_LOGI(TAG, "[WEB] Mở cửa thành công");
    
    // Báo MQTT mở cửa
    char mqtt_payload[128];
    snprintf(mqtt_payload, sizeof(mqtt_payload), "{\"action\":\"open_door\",\"status\":\"success\",\"message\":\"Cửa đã mở thành công\"}");
    mqtt_publish_status(mqtt_payload);
    
    gpio_set_level(RELAY_PIN, 0);   // Open-Drain: 0 = Pull to GND -> Relay ON
    
    // Keep the door open for 5 seconds
    vTaskDelay(pdMS_TO_TICKS(5000)); 
    
    gpio_set_level(RELAY_PIN, 1);   // Open-Drain: 1 = Float -> Relay OFF
    ESP_LOGI(TAG, "Door Closed.");
    tft_display_status("DOOR CLOSED");
}

void close_door(void) {
    ESP_LOGI(TAG, "Force Closing Door...");
    gpio_set_level(RELAY_PIN, 1);   // Open-Drain: 1 = Float -> Relay OFF
    tft_display_status("DOOR CLOSED");
}

static esp_timer_handle_t s_alarm_timer = NULL;

static void alarm_auto_stop_timer_cb(void *arg) {
    ESP_LOGI(TAG, "5-Second Alarm Timeout reached. Auto stopping buzzer.");
    gpio_set_level(BUZZER_PIN, 0);
    tft_display_status("ALARM STOPPED");
}

void trigger_alarm(void) {
    tft_display_status("ALARM ACTIVE");
    ESP_LOGW(TAG, "ALARM TRIGGERED! Will auto-stop in 5 seconds...");
    gpio_set_level(BUZZER_PIN, 1);

    if (s_alarm_timer == NULL) {
        const esp_timer_create_args_t timer_args = {
            .callback = &alarm_auto_stop_timer_cb,
            .name = "alarm_timer"
        };
        esp_timer_create(&timer_args, &s_alarm_timer);
    }
    
    esp_timer_stop(s_alarm_timer);
    esp_timer_start_once(s_alarm_timer, 5000000); // 5,000,000 us = 5 seconds
}

void stop_alarm(void) {
    if (s_alarm_timer != NULL) {
        esp_timer_stop(s_alarm_timer);
    }
    tft_display_status("ALARM STOPPED");
    ESP_LOGI(TAG, "Alarm Stopped.");
    gpio_set_level(BUZZER_PIN, 0);
}

uint32_t get_sonar_distance_cm(void) {
    // 1. Send trigger pulse (10us High)
    gpio_set_level(SONAR_TRIG_PIN, 0);
    esp_rom_delay_us(2);
    gpio_set_level(SONAR_TRIG_PIN, 1);
    esp_rom_delay_us(10);
    gpio_set_level(SONAR_TRIG_PIN, 0);

    // 2. Wait for Echo to go HIGH (timeout 30ms)
    // Dùng esp_timer_get_time() và taskYIELD() thành vòng lặp nhưỚng CPU
    // thay vì busy-wait cứng → tránh block Camera DMA và RTOS scheduler
    int64_t deadline = esp_timer_get_time() + 30000LL; // 30ms
    while (gpio_get_level(SONAR_ECHO_PIN) == 0) {
        if (esp_timer_get_time() > deadline) {
            ESP_LOGW(TAG, "Sonar Timeout: Echo pin never went HIGH. Check TRIG/ECHO wiring and 5V power.");
            return 998;
        }
        taskYIELD(); // nhường CPU cho các task khác (Camera ISR, RTOS)
    }

    // 3. Measure Echo HIGH duration
    int64_t start_time = esp_timer_get_time();
    deadline = start_time + 30000LL;
    while (gpio_get_level(SONAR_ECHO_PIN) == 1) {
        if (esp_timer_get_time() > deadline) {
            ESP_LOGW(TAG, "Sonar Timeout: Echo pin never went LOW. Out of range or no obstacle.");
            return 999;
        }
        taskYIELD();
    }
    int64_t end_time = esp_timer_get_time();
    int64_t duration = end_time - start_time;

    // 4. Distance calculation (speed of sound ~343 m/s -> 0.0343 cm/us)
    uint32_t distance = (uint32_t)((duration * 0.0343) / 2);
    return distance;
}

bool is_person_near(void) {
    uint32_t distance = get_sonar_distance_cm();
    ESP_LOGD(TAG, "Sonar distance: %u cm", (unsigned int)distance);
    return (distance < SONAR_THRESHOLD_CM);
}

char read_keypad(void) {
#ifdef KEYPAD_ROW0
    for (int r = 0; r < 4; r++) {
        // Set current row to LOW
        gpio_set_level(keypad_rows[r], 0);
        // Delay briefly for signal settling
        esp_rom_delay_us(10);

        for (int c = 0; c < 4; c++) {
            // If column reads LOW, key is pressed (due to pull-up)
            if (gpio_get_level(keypad_cols[c]) == 0) {
                // Wait for release to debounce simple press
                while (gpio_get_level(keypad_cols[c]) == 0) {
                    vTaskDelay(pdMS_TO_TICKS(10));
                }
                // Set row back to HIGH
                gpio_set_level(keypad_rows[r], 1);
                ESP_LOGI(TAG, "Keypad pressed: %c", keypad_map[r][c]);
                return keypad_map[r][c];
            }
        }
        // Set row back to HIGH
        gpio_set_level(keypad_rows[r], 1);
    }
#endif
    return '\0'; // No key pressed
}

void tft_display_status(const char* status) {
    ESP_LOGI(TAG, "TFT Display Status: >>> %s <<<", status);
    /*
     * Under the hood, this will draw the status string on the ST7789 panel:
     * - esp_lcd_panel_draw_bitmap() or graphics library (like LVGL) to update UI.
     */
}

bool sd_card_init(void) {
    ESP_LOGI(TAG, "Initializing SD Card in SDMMC 1-bit mode...");
    
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 10,
        .allocation_unit_size = 16 * 1024
    };
    
    sdmmc_card_t *card;
    const char mount_point[] = "/sdcard";
    ESP_LOGI(TAG, "Initializing SDMMC peripheral");

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    // 1-bit mode to save pins
    host.flags = SDMMC_HOST_FLAG_1BIT;
    
    // This initializes the slot without card detect (CD) and write protect (WP) signals.
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 1;
    slot_config.cmd = SDMMC_CMD_PIN;
    slot_config.clk = SDMMC_CLK_PIN;
    slot_config.d0 = SDMMC_D0_PIN;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    ESP_LOGI(TAG, "Mounting filesystem");
    esp_err_t ret = esp_vfs_fat_sdmmc_mount(mount_point, &host, &slot_config, &mount_config, &card);

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount filesystem.");
        } else {
            ESP_LOGE(TAG, "Failed to initialize the card (0x%x).", ret);
        }
        return false;
    }
    ESP_LOGI(TAG, "Filesystem mounted at %s", mount_point);
    sdmmc_card_print_info(stdout, card);
    return true;
}
