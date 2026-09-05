import serial
import time
import sys

PORT = "/dev/ttyUSB0"
BAUD = 9600

print(f"Connecting to Arduino Mega on {PORT} at {BAUD} baud...")
try:
    ser = serial.Serial(PORT, BAUD, timeout=1.0)
    time.sleep(2)  # Wait for Arduino to reset upon serial connection
    print("Connected! Sending 'SAMPLE' command to trigger the actuator and NPK sensor...\n")
    
    # Send the SAMPLE command
    ser.write(b"SAMPLE\n")
    ser.flush()
    
    print("Waiting for response... (This will take ~10 seconds as the probe lowers, then 10s monitoring)")
    
    start_time = time.time()
    npk_received = False
    
    # Listen for 30 seconds
    while time.time() - start_time < 30:
        line = ser.readline()
        if line:
            decoded = line.decode('utf-8', errors='ignore').strip()
            if decoded:
                print(f"[MEGA] {decoded}")
            if "DATA NPK" in decoded:
                print("\n✅ SUCCESS! Received NPK data from the Arduino Mega!")
                npk_received = True
                break
            if "FAULT NPK_TIMEOUT" in decoded:
                print(f"\n❌ FAILED! Arduino reported a fault: {decoded}")
                print("This means the Arduino sent the Modbus query, but the NPK sensor did not reply.")
                break

    if not npk_received:
        print("\n⚠️ Test timed out. Did not receive 'DATA NPK' within 30 seconds.")
        
    ser.close()
    print("\nTest complete.")

except Exception as e:
    print(f"Error: {e}")
