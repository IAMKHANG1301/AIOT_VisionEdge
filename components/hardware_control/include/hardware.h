#ifndef HARDWARE_H
#define HARDWARE_H

#include "driver/gpio.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "board_config.h"

/**
 * @brief Initialize all hardware pins (Relay, Buzzer, Sonar, Keypad, TFT)
 */
void hardware_init(void);

/**
 * @brief Initialize the SD Card (SDMMC 1-bit mode) and mount FAT filesystem
 * @return true if mounted successfully, false otherwise
 */
bool sd_card_init(void);

/**
 * @brief Activate the relay to open the door for 5 seconds
 */
void open_door(void);

/**
 * @brief Force close the relay door lock
 */
void close_door(void);

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
