# Hướng dẫn Đấu Nối Phần Cứng (Wiring Guide) - Toàn Hệ Thống

Tài liệu này hướng dẫn cách đấu nối toàn bộ hệ thống phần cứng cho dự án AIoT (bao gồm Mạch Chính và Mạch Phụ), đặc biệt nhấn mạnh phần giao tiếp liên mạch.

---

## 1. Hệ thống Nguồn Điện (Power Supply)

Hệ thống sử dụng Adapter 12V làm nguồn chính để có đủ lực kéo Solenoid, sau đó dùng mạch hạ áp DC-DC để nuôi ESP32 và cảm biến.

*   **Adapter 12V (Nguồn tổ ong / Adapter DC):**
    *   **Dây Dương (+12V):** Nối làm 2 nhánh. Nhánh 1 vào (IN+) mạch hạ áp DC-DC. Nhánh 2 vào chân **COM** của Relay.
    *   **Dây Âm (GND):** Nối làm 2 nhánh. Nhánh 1 vào (IN-) mạch hạ áp DC-DC. Nhánh 2 vào 1 trong 2 dây của cục Solenoid.
*   **Mạch Hạ Áp (DC-DC Buck Converter):**
    *   **Biến trở (Chiết áp):** Dùng tua-vít vặn để đầu ra đạt **chính xác 5.0V đến 5.2V** (Tuyệt đối không vượt quá 5.5V kẻo cháy ESP32).
    *   **OUT+ (5V):** Nối vào chân **5V (hoặc VIN/VBUS)** của ESP32 (cả 2 mạch nếu dùng chung 1 nguồn, hoặc 2 nguồn riêng) và chân **VCC** của mạch Relay.
    *   **OUT- (GND):** Nối vào chân **GND** của ESP32 (RẤT QUAN TRỌNG: **GND của Mạch Chính và Mạch Phụ phải được nối chung với nhau**).

---

## 2. Giao Tiếp Liên Mạch (UART)

Giao tiếp giữa Mạch Chính và Mạch Phụ để truyền tín hiệu hiển thị màn hình và kết quả xử lý.

| Chức năng | MẠCH CHÍNH (Receptionist) | MẠCH PHỤ (Peripheral) |
| :--- | :--- | :--- |
| **GND Chung** | Chân GND | Chân GND |
| **Đường truyền (TX -> RX)**| **GPIO 1** (TX) | **GPIO 18** (RX) |
| **Đường nhận (RX <- TX)** | **GPIO 48** (RX) | **GPIO 17** (TX) |

*(Lưu ý: Chân TX của mạch này phải cắm vào chân RX của mạch kia).*

---

## 3. Cấu hình Chân - MẠCH CHÍNH (AIoT Receptionist)

*Tính năng Âm thanh (Mic/Loa) và Màn hình TFT đã được vô hiệu hóa trên mạch này để nhường tài nguyên và nhường chân cho UART.*

### Khóa Điện Từ (Solenoid) & Relay 5V (Active LOW)
*   **VCC:** 5V (Từ mạch DC-DC)
*   **GND:** GND
*   **IN (Tín hiệu):** **GPIO 47**
*   *(Tiếp điểm Relay: COM nối +12V, NO nối Solenoid).*

### Cảm Biến Siêu Âm (HC-SR04)
*   **VCC / GND:** 5V / GND
*   **Trig:** **GPIO 21**
*   **Echo:** **GPIO 41**

### Loa Còi (Buzzer)
*   **Chân Dương (+):** **GPIO 2**
*   **Chân Âm (-):** GND

### Khe cắm Thẻ nhớ (SD Card - SDMMC 1-Bit)
*   **CMD:** **GPIO 38**
*   **CLK:** **GPIO 39**
*   **D0:** **GPIO 40**

### Camera (OV2640)
*   **XCLK:** GPIO 15 | **PCLK:** GPIO 13 | **VSYNC:** GPIO 6 | **HREF:** GPIO 7
*   **SDA (SIOD):** GPIO 4 | **SCL (SIOC):** GPIO 5
*   **Y9 (D7):** GPIO 16 | **Y8 (D6):** GPIO 17 | **Y7 (D5):** GPIO 18 | **Y6 (D4):** GPIO 12
*   **Y5 (D3):** GPIO 10 | **Y4 (D2):** GPIO 8 | **Y3 (D1):** GPIO 9 | **Y2 (D0):** GPIO 11

---

## 4. Cấu hình Chân - MẠCH PHỤ (AIoT Peripheral)

Mạch phụ đảm nhận chức năng giao tiếp giọng nói (Gemini) và hiển thị giao diện người dùng.

### Nút Nhấn (Voice Query Trigger)
*   **Nút vật lý:** Nút **BOOT** (GPIO 0) có sẵn trên board mạch. (Hoặc nối một nút bấm ngoài giữa **GPIO 0** và **GND**).

### Màn hình TFT (ST7789 SPI 240x320)
*   **VCC / GND:** 3.3V / GND
*   **SDA (MOSI):** **GPIO 42**
*   **SCL (SCLK):** **GPIO 45**
*   **DC:** **GPIO 46**
*   *(RST và CS nối theo mặc định hoặc không dùng).*

### Giao tiếp Âm thanh (I2S - INMP441 Mic & MAX98357A Loa)
Hai module này dùng chung bus I2S.
*   **VCC / GND:** 3.3V hoặc 5V / GND
*   **SCK (Mic) / BCLK (Loa):** **GPIO 3**
*   **WS (Mic) / LRC (Loa):** **GPIO 14**
*   **SD (Mic OUT - Dữ liệu Mic):** **GPIO 48**
*   **DIN (Spk IN - Dữ liệu Loa):** **GPIO 1**
*   *(L/R trên Mic nối GND. Kênh âm thanh có thể tùy chỉnh nếu cần).*
