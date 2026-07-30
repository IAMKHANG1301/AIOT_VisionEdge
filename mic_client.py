import asyncio
import websockets
import json
import base64
import time
import sys
import threading

try:
    import pyaudio
except ImportError:
    print("❌ Vui lòng cài đặt thư viện 'pyaudio' trước: pip install pyaudio")
    print("Lưu ý: Nếu cài đặt lỗi trên Windows, hãy thử: pip install pipwin && pipwin install pyaudio")
    sys.exit(1)

# Cấu hình âm thanh
CHUNK = 1024
FORMAT = pyaudio.paInt16
CHANNELS = 1
RATE = 16000 # 16kHz là chuẩn tốt nhất cho nhận diện giọng nói

recording = False

import math

def get_rms(data):
    count = len(data) // 2
    if count == 0:
        return 0
    import struct
    shorts = struct.unpack(f"<{count}h", data)
    sum_squares = sum(s * s for s in shorts)
    return math.sqrt(sum_squares / count)

def record_audio(ws, loop):
    global recording
    p = pyaudio.PyAudio()
    stream = p.open(format=FORMAT,
                    channels=CHANNELS,
                    rate=RATE,
                    input=True,
                    frames_per_buffer=CHUNK)

    print("\n🎙️  Đang lắng nghe... (Hãy bắt đầu nói)")
    
    SILENCE_THRESHOLD = 500  # Ngưỡng âm lượng (tuỳ chỉnh)
    SILENCE_DURATION = 2.0   # 2 giây im lặng
    
    voice_detected = False
    silence_start_time = None
    
    try:
        while recording:
            data = stream.read(CHUNK, exception_on_overflow=False)
            rms = get_rms(data)
            
            if not voice_detected:
                if rms > SILENCE_THRESHOLD:
                    voice_detected = True
                    print(f"\n🗣️ Phát hiện giọng nói! Đang thu âm... (Âm lượng: {int(rms)})")
                    asyncio.run_coroutine_threadsafe(ws.send(data), loop)
            else:
                # Đang thu âm
                asyncio.run_coroutine_threadsafe(ws.send(data), loop)
                if rms < SILENCE_THRESHOLD:
                    if silence_start_time is None:
                        silence_start_time = time.time()
                    elif time.time() - silence_start_time > SILENCE_DURATION:
                        print("\n🤫 Đã ngừng nói quá 2 giây. Đóng gói âm thanh...")
                        recording = False # Dừng vòng lặp
                        break
                else:
                    silence_start_time = None # Có tiếng nói lại -> xoá đếm ngược
                    
    except Exception as e:
        print(f"Lỗi ghi âm: {e}")
    finally:
        stream.stop_stream()
        stream.close()
        p.terminate()
        print("Đã đóng microphone.")

async def simulate_edge_mic(image_file_path, uri="ws://localhost:8000/ws/audio"):
    global recording
    print(f"🔌 Bắt đầu kết nối tới {uri}")
    try:
        async with websockets.connect(uri) as websocket:
            print("✅ Đã kết nối tới Server.")
            
            # 1. Gửi ảnh (metadata) nếu có
            if image_file_path and image_file_path.lower() != "none":
                try:
                    with open(image_file_path, "rb") as img_file:
                        img_base64 = base64.b64encode(img_file.read()).decode('utf-8')
                        metadata = {
                            "action": "metadata",
                            "image_b64": img_base64
                        }
                        await websocket.send(json.dumps(metadata))
                        print(f"📸 Đã gửi ảnh metadata ({image_file_path})")
                except Exception as e:
                    print(f"⚠️ Không gửi được ảnh: {e}")
            
            # 2. Bắt đầu ghi âm và gửi stream
            loop = asyncio.get_running_loop()
            recording = True
            
            # Chạy hàm ghi âm trong một thread riêng biệt để không block event loop
            audio_thread = threading.Thread(target=record_audio, args=(websocket, loop))
            audio_thread.start()
            
            # Chờ luồng VAD tự động thu âm và đóng kết thúc (ngừng sau 2s im lặng)
            while recording:
                await asyncio.sleep(0.1)
            audio_thread.join()
            
            stop_time = time.time()
                
            # 3. Gửi tín hiệu stop_audio
            print("\n🛑 Gửi tín hiệu ngừng thu âm (stop_audio) lên server...")
            await websocket.send(json.dumps({"action": "stop_audio"}))
            
            # 4. Nhận phản hồi
            print("⏳ Đang chờ Audio TTS trả về từ Server...")
            output_audio_path = "output_reply_mic.wav"
            first_chunk_received = False
            latency = 0
            
            with open(output_audio_path, "wb") as out_file:
                while True:
                    try:
                        response = await websocket.recv()
                        
                        if isinstance(response, bytes):
                            if not first_chunk_received:
                                latency = time.time() - stop_time
                                print(f"⚡ Thời gian phản hồi (Tính từ lúc ngắt mic đến lúc nghe thấy tiếng): {latency:.2f} giây")
                                first_chunk_received = True
                            out_file.write(response)
                            sys.stdout.write("█")
                            sys.stdout.flush()
                        else:
                            print()
                            data = json.loads(response)
                            if data.get("action") == "complete":
                                print("\n✅ Đã nhận đủ toàn bộ luồng Audio. Kết thúc phiên.")
                                print(f"📝 AI nghe được: {data.get('transcript', 'Không có')}")
                                print(f"🤖 AI định nói: {data.get('ai_reply', 'Không có')}")
                                
                                if not data.get('ai_reply') and data.get('raw_output'):
                                    print(f"⚠️ Dữ liệu thô từ AI (Lỗi format): {data.get('raw_output')}")
                                break
                            elif "error" in data:
                                print(f"\n❌ Lỗi từ Server: {data['error']}")
                                break
                    except websockets.exceptions.ConnectionClosed:
                        print("\n🔌 Kết nối đã bị đóng bởi server.")
                        break
            
            if first_chunk_received:
                print(f"🎉 Đã lưu file âm thanh phản hồi tại: {output_audio_path}")
                print(f"🔊 Hãy mở file {output_audio_path} để nghe thử kết quả.")
            
    except ConnectionRefusedError:
        print("❌ Không thể kết nối. Hãy chắc chắn bạn đã bật backend bằng lệnh: uvicorn main:app --reload")

if __name__ == "__main__":
    image_path = "none"
    ws_uri = "ws://localhost:8000/ws/audio"
    
    if len(sys.argv) > 1:
        image_path = sys.argv[1]
    if len(sys.argv) > 2:
        ws_uri = sys.argv[2]
        
    print("--------------------------------------------------")
    print("📝 CÁCH SỬ DỤNG SCRIPT NÀY:")
    print("python mic_client.py [đường_dẫn_file_jpg|none] [websocket_url]")
    print(f"Đang chạy với: Ảnh = {image_path}, URL = {ws_uri}")
    print("--------------------------------------------------\n")
    
    asyncio.run(simulate_edge_mic(image_path, ws_uri))
