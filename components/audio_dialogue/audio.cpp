
// =============================================================================
// audio.cpp — I2S Full-Duplex Audio Driver
// Hardware: INMP441 Microphone (RX) + MAX98357A Speaker Amp (TX)
// Protocol: Standard Philips I2S, 16kHz Mono
// ESP-IDF: v5.x new I2S driver (driver/i2s_std.h)
// =============================================================================
#include "audio.h"
#include "esp_log.h"
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "network.h"
#include "esp_websocket_client.h"
#include <math.h>
#include <string.h>
#include "hardware.h"
#include <stdio.h>

static const char *TAG = "AUDIO";

// I2S channel handles (full-duplex, both linked to I2S_NUM_0)
static i2s_chan_handle_t s_tx_chan = NULL;   // Speaker (MAX98357A)
static i2s_chan_handle_t s_rx_chan = NULL;   // Microphone (INMP441)
static bool              s_initialized = false;

volatile uint32_t g_last_play_time_ms = 0;

// =============================================================================
// audio_init — Initialize I2S Full-Duplex Bus
// =============================================================================
void audio_init(void) {
    if (s_initialized) {
        ESP_LOGW(TAG, "Audio already initialized, skipping.");
        return;
    }

    // Enable ESP-IDF internal I2S driver debug logs
    esp_log_level_set("i2s_common", ESP_LOG_DEBUG);
    esp_log_level_set("i2s_std", ESP_LOG_DEBUG);

    ESP_LOGI(TAG, "Initializing I2S full-duplex audio...");
    ESP_LOGI(TAG, "  BCLK  = GPIO %d (shared MIC+SPK)", I2S_BCLK_PIN);
    ESP_LOGI(TAG, "  WS    = GPIO %d (shared MIC+SPK)", I2S_WS_PIN);
    ESP_LOGI(TAG, "  SPK   = GPIO %d (TX → MAX98357A DIN)", I2S_SPK_DOUT_PIN);
    ESP_LOGI(TAG, "  MIC   = GPIO %d (RX ← INMP441 SD)",  I2S_MIC_DIN_PIN);

    // -------------------------------------------------------------------------
    // Step 1: Create full-duplex channel pair on I2S_NUM_0
    //   Passing non-NULL for both tx and rx creates a linked duplex pair:
    //   they share the same I2S controller and clock signals.
    // -------------------------------------------------------------------------
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(AUDIO_I2S_NUM, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;  // Zero-fill TX buffer when idle (prevents noise)
    
    // Giảm bộ đệm DMA để nhường RAM cho mbedTLS (chứng chỉ HTTPS/WSS rất tốn RAM)
    chan_cfg.dma_desc_num = 4;     // Dùng 4 khối DMA
    chan_cfg.dma_frame_num = 256;  // Mỗi khối chứa 256 frames

    esp_err_t err = i2s_new_channel(&chan_cfg, &s_tx_chan, &s_rx_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(err));
        return;
    }

    // -------------------------------------------------------------------------
    // Step 2: Configure Standard I2S (Philips) mode
    //   - Sample rate: 16 kHz (suitable for voice)
    //   - Slot: 32-bit word, 24-bit data (matches INMP441 output format)
    //   - Mode: Mono — INMP441 with L/R=GND outputs on LEFT channel only
    //   - Full GPIO config is specified here; RX reuses BCLK+WS from TX.
    // -------------------------------------------------------------------------
    i2s_std_config_t i2s_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_32BIT,  // 32-bit slot (INMP441 is 24-bit padded)
                        I2S_SLOT_MODE_STEREO),     // Stereo — ensures 50% duty cycle WS clock for MAX98357A
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,                // INMP441 doesn't need MCLK
            .bclk = (gpio_num_t)I2S_BCLK_PIN,      // Bit clock → MIC SCK + SPK BCLK
            .ws   = (gpio_num_t)I2S_WS_PIN,        // Word select → MIC WS + SPK LRC
            .dout = (gpio_num_t)I2S_SPK_DOUT_PIN,  // Data out → MAX98357A DIN
            .din  = (gpio_num_t)I2S_MIC_DIN_PIN,   // Data in ← INMP441 SD
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };

    // -------------------------------------------------------------------------
    // Step 3: Initialize TX channel (speaker) — sets up clock + GPIO
    // -------------------------------------------------------------------------
    err = i2s_channel_init_std_mode(s_tx_chan, &i2s_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "TX channel init failed: %s", esp_err_to_name(err));
        goto cleanup;
    }

    // -------------------------------------------------------------------------
    // Step 4: Initialize RX channel (microphone) — reuses TX clock
    // -------------------------------------------------------------------------
    err = i2s_channel_init_std_mode(s_rx_chan, &i2s_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RX channel init failed: %s", esp_err_to_name(err));
        goto cleanup;
    }

    // -------------------------------------------------------------------------
    // Step 5: Enable both channels (starts DMA transfers)
    // -------------------------------------------------------------------------
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx_chan));
    ESP_ERROR_CHECK(i2s_channel_enable(s_rx_chan));

    s_initialized = true;
    ESP_LOGI(TAG, "Audio I2S initialized OK ✓  (16kHz, 32-bit slot, Stereo Mode)");
    return;

cleanup:
    if (s_tx_chan) { i2s_del_channel(s_tx_chan); s_tx_chan = NULL; }
    if (s_rx_chan) { i2s_del_channel(s_rx_chan); s_rx_chan = NULL; }
    ESP_LOGE(TAG, "Audio initialization FAILED — check wiring!");
}

// =============================================================================
bool audio_is_initialized(void) {
    return s_initialized;
}

// =============================================================================
// audio_record — Record with Voice Activity Detection (VAD)
// =============================================================================
int audio_record(uint8_t *buffer, int max_len) {
    if (!s_initialized) {
        ESP_LOGE(TAG, "audio_record: not initialized");
        return 0;
    }

    ESP_LOGI(TAG, "Recording started (max %d bytes)...", max_len);

    // Each I2S read gives 32-bit samples; we downsample to 16-bit for storage
    const int DMA_BUF_SAMPLES = 256;
    const int DMA_BUF_BYTES   = DMA_BUF_SAMPLES * sizeof(int32_t);
    int32_t  *dma_buf         = (int32_t *)malloc(DMA_BUF_BYTES);
    if (!dma_buf) {
        ESP_LOGE(TAG, "audio_record: malloc failed");
        return 0;
    }

    int       total_written     = 0;
    int64_t   silence_start_ms  = 0;
    bool      silence_started   = false;
    int16_t  *out               = (int16_t *)buffer;
    int       max_samples_16bit = max_len / sizeof(int16_t);

    while (total_written < max_samples_16bit) {
        // Read 32-bit samples from INMP441 via I2S DMA
        size_t bytes_read = 0;
        esp_err_t err = i2s_channel_read(s_rx_chan, dma_buf, DMA_BUF_BYTES,
                                         &bytes_read, pdMS_TO_TICKS(100));
        if (err != ESP_OK || bytes_read == 0) continue;

        int samples_read = bytes_read / sizeof(int32_t);

        // Calculate mean (DC offset)
        int64_t sum = 0;
        int valid_samples = 0;
        for (int i = 0; i < samples_read; i += 2) {
            int32_t sample = (dma_buf[i] >> 16) * 4;
            if (sample > 32767) sample = 32767;
            if (sample < -32768) sample = -32768;
            sum += sample;
            valid_samples++;
        }
        int16_t mean = (valid_samples > 0) ? (int16_t)(sum / valid_samples) : 0;

        int64_t rms_sum = 0;
        for (int i = 0; i < samples_read && total_written < max_samples_16bit; i++) {
            if (i % 2 != 0) continue; // Skip RIGHT channel (always zero from INMP441)
            // Shift right 16 to get 16-bit, then boost gain x4 with clamp
            int32_t sample = (dma_buf[i] >> 16) * 4;
            if (sample > 32767) sample = 32767;
            if (sample < -32768) sample = -32768;
            int16_t sample_16 = (int16_t)sample;
            
            out[total_written++] = sample_16;
            
            // Remove DC offset for VAD RMS calculation
            int16_t ac_sample = sample_16 - mean;
            rms_sum += (int64_t)ac_sample * ac_sample;
        }

        // --- Voice Activity Detection (VAD) ---
        int64_t rms = (valid_samples > 0) ? (int64_t)sqrtf((float)(rms_sum / valid_samples)) : 0;
        if (rms < VAD_SILENCE_THRESHOLD) {
            if (!silence_started) {
                silence_started = true;
                silence_start_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
            } else {
                int64_t silence_dur = xTaskGetTickCount() * portTICK_PERIOD_MS - silence_start_ms;
                if (silence_dur > VAD_SILENCE_TIMEOUT_MS) {
                    ESP_LOGI(TAG, "VAD: silence detected, stopping. Recorded %d samples", total_written);
                    break;
                }
            }
        } else {
            // Voice detected — reset silence timer
            silence_started  = false;
            silence_start_ms = 0;
        }
    }

    free(dma_buf);
    int bytes_recorded = total_written * sizeof(int16_t);
    ESP_LOGI(TAG, "Recording done: %d bytes (%.1f s at 16kHz mono PCM-16)",
             bytes_recorded, (float)total_written / AUDIO_SAMPLE_RATE);
    return bytes_recorded;
}

void audio_play(const uint8_t *data, int len) {
    if (!s_initialized) {
        ESP_LOGE(TAG, "audio_play: not initialized");
        return;
    }
    if (!data || len <= 0) return;
    g_last_play_time_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;

    ESP_LOGI(TAG, "Playing %d bytes (%.1f s at 16kHz mono PCM-16)...",
             len, (float)(len / 2) / AUDIO_SAMPLE_RATE);

    // Upscale 16-bit PCM → 32-bit I2S samples (MAX98357A prefers full-scale signal)
    const int16_t *src          = (const int16_t *)data;
    int            sample_count = len / sizeof(int16_t);
    const int      CHUNK        = 128; // 128 mono samples -> 256 stereo samples
    int32_t        tx_buf[CHUNK * 2];

    for (int offset = 0; offset < sample_count; offset += CHUNK) {
        int n = (offset + CHUNK < sample_count) ? CHUNK : (sample_count - offset);
        for (int i = 0; i < n; i++) {
            int32_t val = (int32_t)src[offset + i] << 16;
            tx_buf[2 * i]     = val; // Left channel
            tx_buf[2 * i + 1] = val; // Right channel
        }
        size_t bytes_written = 0;
        esp_err_t err = i2s_channel_write(s_tx_chan, tx_buf, n * 2 * sizeof(int32_t),
                          &bytes_written, pdMS_TO_TICKS(200));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "i2s_channel_write (play) failed: %s (written %zu)", esp_err_to_name(err), bytes_written);
        }
    }

    // Flush with silence to prevent pop at end
    int32_t silence[128] = {0};
    size_t w = 0;
    esp_err_t err = i2s_channel_write(s_tx_chan, silence, sizeof(silence), &w, pdMS_TO_TICKS(100));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_write (silence) failed: %s", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "Playback complete ✓");
}

void audio_play_chunk(const uint8_t *data, int len) {
    if (!s_initialized || !data || len <= 0) return;
    g_last_play_time_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;

    const int16_t *src          = (const int16_t *)data;
    int            sample_count = len / sizeof(int16_t);
    const int      CHUNK        = 128;
    int32_t        tx_buf[CHUNK * 2];

    for (int offset = 0; offset < sample_count; offset += CHUNK) {
        int n = (offset + CHUNK < sample_count) ? CHUNK : (sample_count - offset);
        for (int i = 0; i < n; i++) {
            int32_t val = (int32_t)src[offset + i] << 16;
            tx_buf[2 * i]     = val; 
            tx_buf[2 * i + 1] = val; 
        }
        size_t bytes_written = 0;
        i2s_channel_write(s_tx_chan, tx_buf, n * 2 * sizeof(int32_t), &bytes_written, pdMS_TO_TICKS(200));
    }
    g_last_play_time_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
}

// =============================================================================
// audio_test_play_sine — TEST: Play 440 Hz sine wave for 2 seconds
//   EXPECTED OUTPUT: You hear a clear 440 Hz tone (note A4) from the speaker.
//   HOW TO VERIFY: Just listen — should be a steady, clean buzzing tone.
//   FAIL SYMPTOM: Silence → check MAX98357A DIN wire on GPIO 47.
// =============================================================================
void audio_test_play_sine(void) {
    if (!s_initialized) {
        ESP_LOGE(TAG, "audio_test_play_sine: call audio_init() first!");
        return;
    }

    ESP_LOGI(TAG, "=== TEST: Playing 440 Hz sine wave for 2 seconds ===");

    const int FREQ_HZ      = 440;
    const int DURATION_S   = 2;
    const int TOTAL_SMPLS  = AUDIO_SAMPLE_RATE * DURATION_S;
    const int CHUNK        = 128; // 128 mono samples -> 256 stereo samples
    const int AMPLITUDE    = 16000;  // Increase amplitude for testing
    int32_t   tx_buf[CHUNK * 2];
    double    phase        = 0.0;
    double    phase_inc    = 2.0 * M_PI * FREQ_HZ / AUDIO_SAMPLE_RATE;

    for (int offset = 0; offset < TOTAL_SMPLS; offset += CHUNK) {
        int n = (offset + CHUNK < TOTAL_SMPLS) ? CHUNK : (TOTAL_SMPLS - offset);
        for (int i = 0; i < n; i++) {
            int16_t sample = (int16_t)(AMPLITUDE * sin(phase));
            int32_t val = (int32_t)sample << 16;
            tx_buf[2 * i]     = val; // Left channel
            tx_buf[2 * i + 1] = val; // Right channel
            phase += phase_inc;
        }
        size_t bytes_written = 0;
        esp_err_t err = i2s_channel_write(s_tx_chan, tx_buf, n * 2 * sizeof(int32_t),
                          &bytes_written, pdMS_TO_TICKS(500));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "i2s_channel_write (sine) failed: %s (written %zu)", esp_err_to_name(err), bytes_written);
        }
    }

    // Flush silence to prevent pop
    int32_t silence[128] = {0};
    size_t w = 0;
    esp_err_t err = i2s_channel_write(s_tx_chan, silence, sizeof(silence), &w, pdMS_TO_TICKS(100));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_write (sine silence) failed: %s", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "=== TEST COMPLETE: Did you hear 440 Hz tone? ===");
}

// =============================================================================
// audio_test_record_to_sd — TEST: Record 3s from mic → /sdcard/test_mic.raw
//   EXPECTED OUTPUT: /sdcard/test_mic.raw (~96 KB, raw PCM-16 mono 16kHz)
//   HOW TO VERIFY: Copy to PC → open in Audacity:
//     File > Import > Raw Data > 16kHz, 16-bit, Mono, Little-Endian
//   PASS: You hear your voice in the recording.
//   FAIL: File missing → check INMP441 SD wire on GPIO 48.
//         File full of zeros → check INMP441 VDD (3.3V) and L/R → GND.
// =============================================================================
void audio_test_record_to_sd(void) {
    if (!s_initialized) {
        ESP_LOGE(TAG, "audio_test_record_to_sd: call audio_init() first!");
        return;
    }

    ESP_LOGI(TAG, "=== Recording from mic (auto stops after 0.5s silence) ===");
    
    // Ghi tối đa 10 giây
    const int MAX_RECORD_SECONDS = 10;
    const int BUF_SIZE = AUDIO_SAMPLE_RATE * MAX_RECORD_SECONDS * sizeof(int16_t);

    uint8_t *pcm_buf = (uint8_t *)heap_caps_malloc(BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!pcm_buf) {
        pcm_buf = (uint8_t *)malloc(BUF_SIZE);
        if (!pcm_buf) {
            ESP_LOGE(TAG, "Failed to allocate %d bytes for recording buffer", BUF_SIZE);
            return;
        }
    }

    // Use the built-in VAD record function
    int bytes_recorded = audio_record(pcm_buf, BUF_SIZE);
    
    if (bytes_recorded <= 0) {
        ESP_LOGE(TAG, "Recording failed or empty.");
        free(pcm_buf);
        return;
    }

    // Write raw PCM to SD card for UI testing
    const char *filepath = "/sdcard/test_mic.raw";
    FILE *f = fopen(filepath, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open %s for writing! SD mounted?", filepath);
        free(pcm_buf);
        return;
    }

    size_t written = fwrite(pcm_buf, 1, bytes_recorded, f);
    fclose(f);
    free(pcm_buf);

    if (written == (size_t)bytes_recorded) {
        ESP_LOGI(TAG, "Saved RAW PCM file: %d bytes to %s", (int)written, filepath);
    } else {
        ESP_LOGE(TAG, "Only wrote %d/%d bytes to SD", (int)written, bytes_recorded);
    }
}

// =============================================================================
// audio_stream_to_ws
// =============================================================================
void audio_stream_to_ws(void *client_handle) {
    esp_websocket_client_handle_t client = (esp_websocket_client_handle_t)client_handle;
    const int CHUNK_SIZE = 4096;
    uint8_t *pcm_chunk = (uint8_t *)heap_caps_malloc(CHUNK_SIZE, MALLOC_CAP_SPIRAM);
    if (!pcm_chunk) return;
    
    int silent_chunks = 15; // Đặt sẵn 15 để ban đầu coi như đang im lặng, không gửi data thừa
    int away_chunks = 0;
    int chunk_counter = 0;
    
    while (1) {
        chunk_counter++;
        if (chunk_counter >= 5) { // Chỉ đo khoảng cách sau mỗi 5 chunk (~640ms)
            uint32_t dist = get_sonar_distance_cm();
            uint32_t current_time_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
            
            // Nếu AI đang nói hoặc vừa mới nói xong (trong vòng 5 giây), KHÔNG ngắt kết nối
            if (current_time_ms - g_last_play_time_ms < 5000) {
                away_chunks = 0;
            } 
            else if (dist >= 50 || dist == 0) {
                away_chunks++;
                if (away_chunks > 15) { // ~10 giây liên tục không có ai và AI không nói
                    ESP_LOGI(TAG, "Khách đã thực sự rời đi (Xa liên tục 10s), ngắt Streaming.");
                    break;
                }
            } else {
                away_chunks = 0; // Reset nếu khách lại gần
            }
            chunk_counter = 0;
        }

        size_t bytes_read = 0;
        esp_err_t err = i2s_channel_read(s_rx_chan, pcm_chunk, CHUNK_SIZE, &bytes_read, portMAX_DELAY);
        
        if (err == ESP_OK && bytes_read > 0) {
            uint32_t current_time_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
            if (current_time_ms - g_last_play_time_ms < 1000) {
                // Mute mic while playing to avoid echo and VAD false triggers
                silent_chunks = 15; // Reset VAD silence counter
                continue; // Do not process or send this audio chunk
            }

            int32_t *dma_buf = (int32_t *)pcm_chunk;
            int samples_read = bytes_read / sizeof(int32_t);
            
            // Calculate mean (DC offset)
            int64_t sum = 0;
            int valid_samples = 0;
            for (int i = 0; i < samples_read; i += 2) {
                int32_t sample = (dma_buf[i] >> 16);
                sum += sample;
                valid_samples++;
            }
            int16_t mean = (valid_samples > 0) ? (int16_t)(sum / valid_samples) : 0;
            
            int64_t sq_sum = 0;
            
            // Allocate 16-bit PCM buffer
            int16_t *pcm16 = (int16_t *)malloc(valid_samples * sizeof(int16_t));
            if (!pcm16) {
                static int malloc_err_counter = 0;
                malloc_err_counter++;
                if (malloc_err_counter % 10 == 0) {
                    ESP_LOGE(TAG, "Lỗi hết RAM: Không thể cấp phát %d byte cho pcm16", (int)(valid_samples * sizeof(int16_t)));
                }
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            
            int p_idx = 0;
            for (int i = 0; i < samples_read; i += 2) { // Skip RIGHT channel
                int32_t sample = (dma_buf[i] >> 16);
                sample = (sample - mean) * 4; // GAIN x4
                if (sample > 32767) sample = 32767;
                if (sample < -32768) sample = -32768;
                
                pcm16[p_idx++] = (int16_t)sample;
                sq_sum += (int64_t)sample * sample;
            }
            
            int rms = sqrt(sq_sum / (valid_samples > 0 ? valid_samples : 1));
            
            // -------------------------------------------------------------
            // VAD Logic (Mốc 1000 theo yêu cầu của user)
            // -------------------------------------------------------------
            if (rms < 1000) {
                if (silent_chunks < 15) {
                    silent_chunks++;
                    if (silent_chunks == 15) {
                        ESP_LOGI(TAG, "Đang im lặng (VAD), chốt câu gửi cho AI...");
                        extern void hf_ws_send_text(void *client_handle, const char *text);
                        hf_ws_send_text(client, "{\"action\":\"stop_audio\"}");
                    }
                }
            } else {
                if (silent_chunks >= 15) {
                    ESP_LOGI(TAG, "🎙️ Có tiếng động (RMS: %d), bắt đầu thu...", rms);
                }
                silent_chunks = 0;
            }
            
            // CHỈ gửi dữ liệu lên mây nếu đang trong trạng thái NÓI (silent_chunks < 15)
            if (silent_chunks < 15) {
                esp_websocket_client_send_bin(client, (const char*)pcm16, p_idx * sizeof(int16_t), portMAX_DELAY);
            }
            
            free(pcm16);
            
            static int log_counter = 0;
            log_counter++;
            if (log_counter >= 8) { // Khoảng 1 giây 1 lần
                ESP_LOGI(TAG, "Dữ liệu mic: RMS = %d | VAD: %s", rms, (silent_chunks < 15) ? "ĐANG THU GỬI MÂY" : "BỎ QUA");
                log_counter = 0;
            }
        } else {
            // Lỗi đọc I2S
            static int err_counter = 0;
            err_counter++;
            if (err_counter % 10 == 0) {
                ESP_LOGE(TAG, "Lỗi đọc I2S: err = %d (%s), bytes_read = %d", err, esp_err_to_name(err), (int)bytes_read);
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    free(pcm_chunk);
}
