#include "hardware.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

// For TFT SPI standard interface
#include "driver/spi_master.h"

static const char *TAG = "HARDWARE";

// Keypad mapping table
static const char keypad_map[4][4] = {
    {'1', '2', '3', 'A'},
    {'4', '5', '6', 'B'},
    {'7', '8', '9', 'C'},
    {'*', '0', '#', 'D'}
};

static const gpio_num_t keypad_rows[4] = {KEYPAD_ROW0, KEYPAD_ROW1, KEYPAD_ROW2, KEYPAD_ROW3};
static const gpio_num_t keypad_cols[4] = {KEYPAD_COL0, KEYPAD_COL1, KEYPAD_COL2, KEYPAD_COL3};

void hardware_init(void) {
    ESP_LOGI(TAG, "Initializing Hardware Control (Relay, Buzzer, Sonar, Keypad, TFT)");

    gpio_config_t io_conf = {};
    
    // 1. Config Relay
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << RELAY_PIN);
    io_conf.pull_down_en = 0;
    io_conf.pull_up_en = 0;
    gpio_config(&io_conf);
    gpio_set_level(RELAY_PIN, 0); // Default OFF

    // 2. Config Buzzer
    io_conf.pin_bit_mask = (1ULL << BUZZER_PIN);
    gpio_config(&io_conf);
    gpio_set_level(BUZZER_PIN, 0); // Default OFF

    // 3. Config Sonar (HC-SR04)
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

    // 4. Config Keypad Rows (Outputs, default HIGH)
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

    // 6. TFT Display ST7789 Initial setup
    /*
     * SPI/LCD configuration pseudocode for ESP-IDF v5:
     * - Configure SPI bus using spi_bus_initialize().
     * - Configure LCD Panel IO using esp_lcd_new_panel_io_spi().
     * - Create LCD Panel driver using esp_lcd_new_panel_st7789().
     * - Reset LCD panel and initialize.
     */
    ESP_LOGI(TAG, "TFT ST7789 SPI interface configured on MOSI:%d, SCLK:%d, CS:%d, DC:%d, RST:%d", 
             TFT_SPI_MOSI, TFT_SPI_SCLK, TFT_CS_PIN, TFT_DC_PIN, TFT_RST_PIN);
}

void open_door(void) {
    tft_display_status("DOOR OPENING");
    ESP_LOGI(TAG, "Opening Door...");
    gpio_set_level(RELAY_PIN, 1);
    
    // Keep the door open for 5 seconds
    vTaskDelay(pdMS_TO_TICKS(5000)); 
    
    gpio_set_level(RELAY_PIN, 0);
    ESP_LOGI(TAG, "Door Closed.");
    tft_display_status("DOOR CLOSED");
}

void trigger_alarm(void) {
    tft_display_status("ALARM ACTIVE");
    ESP_LOGW(TAG, "ALARM TRIGGERED!");
    gpio_set_level(BUZZER_PIN, 1);
}

void stop_alarm(void) {
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

    // 2. Measure Echo High pulse duration
    // Wait for Echo to go high
    int64_t start_time = esp_timer_get_time();
    int64_t timeout = start_time + 30000; // 30ms timeout (max range ~5m)
    while (gpio_get_level(SONAR_ECHO_PIN) == 0) {
        if (esp_timer_get_time() > timeout) {
            return 999; // Timeout / sensor error
        }
    }

    start_time = esp_timer_get_time();
    timeout = start_time + 30000;
    while (gpio_get_level(SONAR_ECHO_PIN) == 1) {
        if (esp_timer_get_time() > timeout) {
            return 999; // Timeout / signal lost
        }
    }
    int64_t end_time = esp_timer_get_time();
    int64_t duration = end_time - start_time;

    // 3. Distance calculation (speed of sound ~343 m/s -> 0.0343 cm/us)
    uint32_t distance = (uint32_t)((duration * 0.0343) / 2);
    return distance;
}

bool is_person_near(void) {
    uint32_t distance = get_sonar_distance_cm();
    ESP_LOGD(TAG, "Sonar distance: %u cm", distance);
    return (distance < SONAR_THRESHOLD_CM);
}

char read_keypad(void) {
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
    return '\0'; // No key pressed
}

void tft_display_status(const char* status) {
    ESP_LOGI(TAG, "TFT Display Status: >>> %s <<<", status);
    /*
     * Under the hood, this will draw the status string on the ST7789 panel:
     * - esp_lcd_panel_draw_bitmap() or graphics library (like LVGL) to update UI.
     */
}
