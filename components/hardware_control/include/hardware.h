#ifndef HARDWARE_H
#define HARDWARE_H

#include "driver/gpio.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Define GPIOs based on typical ESP32-S3 boards. 
// Can be customized later based on actual wiring.
#define RELAY_PIN       GPIO_NUM_1
#define BUZZER_PIN      GPIO_NUM_2

// Sonar (HC-SR04)
#define SONAR_TRIG_PIN  GPIO_NUM_3
#define SONAR_ECHO_PIN  GPIO_NUM_8
#define SONAR_THRESHOLD_CM 100 // Detect person if closer than 1 meter (100 cm)

// Keypad Ma trận 4x4
#define KEYPAD_ROW0     GPIO_NUM_41
#define KEYPAD_ROW1     GPIO_NUM_42
#define KEYPAD_ROW2     GPIO_NUM_43
#define KEYPAD_ROW3     GPIO_NUM_0
#define KEYPAD_COL0     GPIO_NUM_45
#define KEYPAD_COL1     GPIO_NUM_46
#define KEYPAD_COL2     GPIO_NUM_47
#define KEYPAD_COL3     GPIO_NUM_48

// Màn hình TFT ST7789 (SPI)
#define TFT_SPI_MOSI    GPIO_NUM_19
#define TFT_SPI_SCLK    GPIO_NUM_20
#define TFT_CS_PIN      GPIO_NUM_21
#define TFT_DC_PIN      GPIO_NUM_38
#define TFT_RST_PIN     GPIO_NUM_39

/**
 * @brief Initialize all hardware pins (Relay, Buzzer, Sonar, Keypad, TFT)
 */
void hardware_init(void);

/**
 * @brief Activate the relay to open the door for 5 seconds
 */
void open_door(void);

/**
 * @brief Trigger the buzzer alarm
 */
void trigger_alarm(void);

/**
 * @brief Stop the buzzer alarm
 */
void stop_alarm(void);

/**
 * @brief Check if a person is near using the Sonar sensor (within threshold)
 * @return true if person detected, false otherwise
 */
bool is_person_near(void);

/**
 * @brief Read distance from Sonar HC-SR04 in centimeters
 * @return Distance in cm
 */
uint32_t get_sonar_distance_cm(void);

/**
 * @brief Read character from 4x4 Keypad matrix
 * @return Char pressed ('0'-'9', 'A'-'D', '*', '#'), or '\0' if no key pressed
 */
char read_keypad(void);

/**
 * @brief Display status or text on the TFT ST7789 screen
 * @param status The text status to display
 */
void tft_display_status(const char* status);

#ifdef __cplusplus
}
#endif

#endif // HARDWARE_H
