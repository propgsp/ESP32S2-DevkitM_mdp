import sounddevice as sd
import numpy as np
import serial
import time
import wave

# --- CONFIGURATION ---
ESP32_PORT = "COM3" 
BAUD_RATE = 115200
SAMPLE_RATE = 16000
VOLUME_THRESHOLD = 800

print(f"Connecting to ESP32 on {ESP32_PORT}...")
esp32 = serial.Serial()
esp32.port = ESP32_PORT
esp32.baudrate = BAUD_RATE
esp32.timeout = 1
esp32.dtr = False 
esp32.rts = False 
esp32.open()

time.sleep(3)
esp32.reset_input_buffer()
print("Connected! AI Brain is ready.\n")

audio_buffer = np.zeros((SAMPLE_RATE, 1), dtype='int16')

def audio_callback(indata, frames, time_info, status):
    global audio_buffer
    audio_buffer = np.roll(audio_buffer, -frames, axis=0)
    audio_buffer[-frames:] = indata

# --- FIX: Flag to block new sends while ESP32 is busy ---
waiting_for_esp = False

print("🎧 Listening constantly... Say 'ON' whenever you are ready.")

with sd.InputStream(samplerate=SAMPLE_RATE, channels=1, dtype='int16', callback=audio_callback):
    while True:
        time.sleep(0.05)

        # --- FIX: Check for ESP32 responses at all times ---
        while esp32.in_waiting > 0:
            response = esp32.readline().decode('utf-8', errors='ignore').strip()
            if response:
                print(f"➡️ {response}")
                # When ESP32 says it's done, unlock sending
                if "READY" in response:
                    waiting_for_esp = False
                    print("-" * 40)
                    print("🎧 Resuming listening...")
                    # Wipe buffer so old noise doesn't re-trigger
                    audio_buffer = np.zeros((SAMPLE_RATE, 1), dtype='int16')

        # --- FIX: Don't send new audio if ESP32 is still busy ---
        if waiting_for_esp:
            continue

        recent_audio = audio_buffer[-int(SAMPLE_RATE * 0.2):]
        current_volume = np.max(np.abs(recent_audio))
        
        if current_volume > VOLUME_THRESHOLD:
            print(f"\n🗣️ Voice detected! (Volume: {current_volume})")
            
            time.sleep(0.5) 
            audio_to_send = audio_buffer.copy()
            
            with wave.open("ai_debug.wav", "w") as f:
                f.setnchannels(1)
                f.setsampwidth(2)
                f.setframerate(SAMPLE_RATE)
                f.writeframes(audio_to_send.tobytes())
                
            print("Data captured. Sending to ESP32...")
            
            # --- FIX: Flush any stale data before sending ---
            esp32.reset_input_buffer()
            
            esp32.write(b"AUDIO_START\n")
            esp32.write(audio_to_send.tobytes())
            
            # --- FIX: Lock further sends until ESP32 says READY ---
            waiting_for_esp = True
            audio_buffer = np.zeros((SAMPLE_RATE, 1), dtype='int16')
