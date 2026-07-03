#ifndef AUDIO_H
#define AUDIO_H

#include <stdint.h>
#include <stdbool.h>
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

// Define I2S Pins
// Micro INMP441 (I2S RX)
#define I2S_MIC_WS_PIN    GPIO_NUM_10
#define I2S_MIC_SD_PIN    GPIO_NUM_11
#define I2S_MIC_SCK_PIN   GPIO_NUM_12

// Speaker MAX98357A (I2S TX)
#define I2S_SPK_BCLK_PIN  GPIO_NUM_14
#define I2S_SPK_LRC_PIN   GPIO_NUM_15
#define I2S_SPK_DIN_PIN   GPIO_NUM_17

#define AUDIO_SAMPLE_RATE 16000
#define AUDIO_CHANNELS    1
#define VAD_SILENCE_TIMEOUT_MS 1500

/**
 * @brief Initialize I2S buses for Microphone and Speaker
 */
void audio_init(void);

/**
 * @brief Start recording audio from the microphone
 * Stop automatically when VAD detects silence for 1.5 seconds.
 * @param buffer Pointer to store the recorded WAV/PCM data
 * @param max_len Maximum length of the buffer
 * @return The actual length of recorded data
 */
int audio_record(uint8_t *buffer, int max_len);

/**
 * @brief Play audio data through the speaker
 * @param data Pointer to the audio data (WAV/PCM)
 * @param len Length of the audio data
 */
void audio_play(const uint8_t *data, int len);

#ifdef __cplusplus
}
#endif

#endif // AUDIO_H
