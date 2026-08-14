file(GLOB_RECURSE SRCS 
    "*.c"
    "*.cpp"
    "*.cc"
)

# Remove files from other ports
list(FILTER SRCS EXCLUDE REGEX "edge-impulse-sdk/porting/arduino/.*")
list(FILTER SRCS EXCLUDE REGEX "edge-impulse-sdk/porting/mbed/.*")
list(FILTER SRCS EXCLUDE REGEX "edge-impulse-sdk/porting/posix/.*")
list(FILTER SRCS EXCLUDE REGEX "edge-impulse-sdk/porting/silabs/.*")
list(FILTER SRCS EXCLUDE REGEX "edge-impulse-sdk/porting/stm32-cubeai/.*")
list(FILTER SRCS EXCLUDE REGEX "edge-impulse-sdk/porting/ti/.*")
list(FILTER SRCS EXCLUDE REGEX "edge-impulse-sdk/porting/zephyr/.*")
list(FILTER SRCS EXCLUDE REGEX "edge-impulse-sdk/porting/sony/.*")
list(FILTER SRCS EXCLUDE REGEX "edge-impulse-sdk/porting/renesas/.*")
list(FILTER SRCS EXCLUDE REGEX "edge-impulse-sdk/porting/mingw32/.*")
list(FILTER SRCS EXCLUDE REGEX "edge-impulse-sdk/porting/ethos-u/.*")
list(FILTER SRCS EXCLUDE REGEX "edge-impulse-sdk/porting/core-v/.*")
list(FILTER SRCS EXCLUDE REGEX "edge-impulse-sdk/porting/himax/.*")
list(FILTER SRCS EXCLUDE REGEX "edge-impulse-sdk/porting/synopsys/.*")
list(FILTER SRCS EXCLUDE REGEX "edge-impulse-sdk/porting/winnxt/.*")

idf_component_register(SRCS "${SRCS}"
    INCLUDE_DIRS "." 
                 "model-parameters" 
                 "tflite-model" 
                 "edge-impulse-sdk"
                 "edge-impulse-sdk/classifier"
                 "edge-impulse-sdk/dsp"
                 "edge-impulse-sdk/porting"
                 "edge-impulse-sdk/third_party/flatbuffers/include"
                 "edge-impulse-sdk/third_party/gemmlowp"
                 "edge-impulse-sdk/third_party/ruy"
                 "edge-impulse-sdk/porting/espressif/esp-dsp/modules/common/include"
                 "edge-impulse-sdk/porting/espressif/esp-dsp/modules/fft/include"
    REQUIRES esp_timer driver
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error -Wno-maybe-uninitialized -Wno-format -Wno-unused-function)

# Workaround for GCC Internal Compiler Error (ICE) on Xtensa (ESP32-S3)
set_source_files_properties(
    "edge-impulse-sdk/tensorflow/lite/kernels/internal/reference_portable_tensor_utils.cc" 
    PROPERTIES COMPILE_FLAGS "-O0"
)

target_compile_definitions(${COMPONENT_LIB} PUBLIC 
    EI_PORTING_ESPRESSIF=1
    EI_CLASSIFIER_TFLITE_ENABLE_ESP_NN=0
    portTICK_RATE_MS=portTICK_PERIOD_MS
)
