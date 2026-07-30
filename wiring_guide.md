# AIoT Receptionist - Hướng dẫn Đấu Nối Phần Cứng (Wiring Guide)

Tài liệu này hướng dẫn cách đấu nối toàn bộ hệ thống phần cứng cho dự án AIoT Receptionist, đặc biệt nhấn mạnh phần nguồn điện và hệ thống Khóa điện từ (Solenoid) qua Rơ-le (Relay) để đảm bảo an toàn, không cháy mạch.

---

## 1. Hệ thống Nguồn Điện (Power Supply)

Hệ thống sử dụng Adapter 12V làm nguồn chính để có đủ lực kéo Solenoid, sau đó dùng mạch hạ áp DC-DC để nuôi ESP32 và cảm biến.

*   **Adapter 12V (Nguồn tổ ong / Adapter DC):**
    *   **Dây Dương (+12V):** Nối làm 2 nhánh.
        *   Nhánh 1: Nối vào đầu vào (IN+) của mạch hạ áp DC-DC.
        *   Nhánh 2: Nối trực tiếp vào chân **COM** của Relay.
    *   **Dây Âm (GND):** Nối làm 2 nhánh.
        *   Nhánh 1: Nối vào đầu vào (IN-) của mạch hạ áp DC-DC.
        *   Nhánh 2: Nối thẳng vào 1 trong 2 dây của cục Solenoid.

*   **Mạch Hạ Áp (DC-DC Buck Converter):**
    *   **IN+ / IN-:** Nhận nguồn 12V từ Adapter.
    *   **Biến trở (Chiết áp):** Dùng tua-vít vặn để đo đồng hồ vạn năng sao cho đầu ra đạt **chính xác 5.0V đến 5.2V** (Tuyệt đối không vượt quá 5.5V kẻo cháy ESP32).
    *   **OUT+ (5V):** Nối vào chân **5V (hoặc VIN/VBUS)** của ESP32 và chân **VCC** của mạch Relay.
    *   **OUT- (GND):** Nối vào chân **GND** của ESP32 và chân **GND** của mạch Relay.

---

## 2. Hệ thống Khóa Điện Từ (Solenoid) & Relay

Mạch ESP32 xuất tín hiệu 3.3V, nhưng Relay lại chạy 5V và là loại **Active LOW**. Do đó chúng ta cấu hình chân GPIO của ESP32 ở chế độ **Open-Drain (Máng hở)**.

*   **Đấu nối Mạch Relay 5V (1 Kênh):**
    *   **VCC:** Nối vào đường nguồn 5V (từ mạch DC-DC).
    *   **GND:** Nối vào đường GND chung.
    *   **IN (Tín hiệu):** Nối vào **GPIO 47** của ESP32. *(Lưu ý: Không dùng GPIO 19 vì đó là chân USB D- của ESP32-S3, khi cắm cáp USB máy tính sẽ gây xung đột làm Rơ-le bị liệt).*

*   **Đầu ra Rơ-le (Đóng cắt 12V) & Solenoid:**
    *   Trên cục Relay có 3 lỗ vặn ốc (NC, COM, NO).
    *   **COM (Ở giữa):** Cấp nguồn **+12V** chờ sẵn.
    *   **NO (Normally Open - Thường Mở):** Nối vào sợi dây còn lại của Solenoid. *(Tuyệt đối không nối vào lỗ NC, nếu không chốt sẽ liên tục thục vào và ngâm điện 12V gây nóng/cháy).*
    *   **Nguyên lý hoạt động:** Bình thường, COM và NO bị ngắt, Solenoid mất điện nảy ra (Khóa). Khi ESP32 kéo GPIO 47 xuống 0V, Relay kêu "Tạch", nối COM sang NO, dòng 12V chạy qua Solenoid làm chốt thụt vào (Mở cửa) trong đúng 5 giây rồi ngắt.

---

## 3. Các Cảm Biến & Thiết bị Ngoại Vi Khác (ESP32-S3)

### Cảm Biến Siêu Âm (HC-SR04) - Hoạt động ở mức 5V
*   **VCC:** Nối 5V
*   **GND:** Nối GND
*   **Trig:** GPIO 21
*   **Echo:** GPIO 41

### Loa Còi (Buzzer)
*   **Chân Dương (+):** Nối GPIO 2
*   **Chân Âm (-):** Nối GND

### Màn hình TFT (ST7789 SPI)
*   **SDA (MOSI):** GPIO 42
*   **SCL (SCLK):** GPIO 45
*   **DC:** GPIO 46
*   *(Các chân RST, CS không dùng hoặc nối cứng theo board)*

### Giao tiếp Âm thanh (I2S - INMP441 & MAX98357A)
*   **SCK / BCLK:** GPIO 3 (Dùng chung cho cả Mic và Loa)
*   **WS / LRC:** GPIO 14 (Dùng chung)
*   **SD (Mic OUT):** GPIO 48
*   **DIN (Spk IN):** GPIO 1

### Khe cắm Thẻ nhớ (SD Card - SDMMC 1-Bit)
*   **CMD:** GPIO 38
*   **CLK:** GPIO 39
*   **D0:** GPIO 40

### Camera (OV2640)
*   **XCLK:** GPIO 15
*   **PCLK:** GPIO 13
*   **VSYNC:** GPIO 6
*   **HREF:** GPIO 7
*   **SDA (SIOD):** GPIO 4
*   **SCL (SIOC):** GPIO 5
*   **Y9 (D7):** GPIO 16
*   **Y8 (D6):** GPIO 17
*   **Y7 (D5):** GPIO 18
*   **Y6 (D4):** GPIO 12
*   **Y5 (D3):** GPIO 10
*   **Y4 (D2):** GPIO 8
*   **Y3 (D1):** GPIO 9
*   **Y2 (D0):** GPIO 11
