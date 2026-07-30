import serial
import time
import sys

def read_port(port):
    print(f"Trying {port}...")
    try:
        s = serial.Serial(port, 115200, timeout=1)
        start = time.time()
        output = b""
        while time.time() - start < 3:
            if s.in_waiting:
                output += s.read(s.in_waiting)
        s.close()
        if output:
            print(f"--- Data from {port} ---")
            print(output.decode('utf-8', errors='ignore'))
            return True
    except Exception as e:
        print(f"Error on {port}: {e}")
    return False

for p in ['COM3', 'COM4', 'COM5']:
    if read_port(p):
        break
