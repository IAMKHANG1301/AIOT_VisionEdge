#ifndef BOARD_CONFIG_H
#define BOARD_CONFIG_H

#include "driver/gpio.h"

// Define GPIOs based on user's exact wiring
#define RELAY_PIN       ((gpio_num_t)47)
#define BUZZER_PIN      GPIO_NUM_2

// Sonar (HC-SR04)
#define SONAR_TRIG_PIN  GPIO_NUM_21
#define SONAR_ECHO_PIN  GPIO_NUM_41
#define SONAR_THRESHOLD_CM 100 // Detect person if closer than 1 meter (100 cm)

// Màn hình TFT ST7789 (SPI)
#define TFT_SPI_MOSI    GPIO_NUM_42
#define TFT_SPI_SCLK    GPIO_NUM_45
#define TFT_DC_PIN      GPIO_NUM_46
#define TFT_CS_PIN      -1
#define TFT_RST_PIN     -1

// SD Card
#define SDMMC_CMD_PIN   GPIO_NUM_38
#define SDMMC_CLK_PIN   GPIO_NUM_39
#define SDMMC_D0_PIN    GPIO_NUM_40

// Camera OV2640
#define CAM_PIN_PWDN    -1
#define CAM_PIN_RESET   -1
#define CAM_PIN_XCLK    GPIO_NUM_15
#define CAM_PIN_PCLK    GPIO_NUM_13
#define CAM_PIN_VSYNC   GPIO_NUM_6
#define CAM_PIN_HREF    GPIO_NUM_7
#define CAM_PIN_SIOD    GPIO_NUM_4
#define CAM_PIN_SIOC    GPIO_NUM_5
#define CAM_PIN_D7      GPIO_NUM_16
#define CAM_PIN_D6      GPIO_NUM_17
#define CAM_PIN_D5      GPIO_NUM_18
#define CAM_PIN_D4      GPIO_NUM_12
#define CAM_PIN_D3      GPIO_NUM_10
#define CAM_PIN_D2      GPIO_NUM_8
#define CAM_PIN_D1      GPIO_NUM_9
#define CAM_PIN_D0      GPIO_NUM_11

// Audio I2S (Full-Duplex Shared Bus)
// Micro INMP441 (I2S RX)
#define I2S_MIC_SCK_PIN   GPIO_NUM_3
#define I2S_MIC_WS_PIN    GPIO_NUM_14
#define I2S_MIC_SD_PIN    ((gpio_num_t)48)

// Speaker MAX98357A (I2S TX)
#define I2S_SPK_BCLK_PIN  GPIO_NUM_3
#define I2S_SPK_LRC_PIN   GPIO_NUM_14
#define I2S_SPK_DIN_PIN   GPIO_NUM_1

#define AUDIO_I2S_NUM      I2S_NUM_0
#define AUDIO_SAMPLE_RATE  16000

#endif // BOARD_CONFIG_H
