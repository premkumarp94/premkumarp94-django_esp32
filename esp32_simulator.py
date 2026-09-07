import time
import random
import requests
import json
from datetime import datetime

# Configurations
DEVICE_ID = "esp8266_device_01"  # Unique device name
loop_interval = 5  # sends data once every 5 seconds
pending_device_msg = "esp8266 tank monitor booted normally"
iteration_count = 0


def find_working_telemetry_url(device_id):
    """
    Tries IP addresses from 192.168.1.1 to 192.168.1.5.
    If none respond, connects to the PythonAnywhere site.
    """
    candidate_urls = [
        "http://192.168.1.1:8000/api/telemetry/",
        "http://192.168.1.2:8000/api/telemetry/",
        "http://192.168.1.3:8000/api/telemetry/",
        "http://192.168.1.4:8000/api/telemetry/",
        "http://192.168.1.5:8000/api/telemetry/",
        "https://premkumarp94.pythonanywhere.com/api/telemetry/",
    ]
    
    print("\n[Connection Manager] Probing servers (192.168.1.1 to 192.168.1.5 -> PythonAnywhere)...")
    for url in candidate_urls:
        try:
            print(f"  - Probing {url} ...", end=" ", flush=True)
            resp = requests.post(
                url,
                data=json.dumps({"id": device_id, "message": "ping"}),
                headers={'Content-Type': 'application/json'},
                timeout=1.0
            )
            if resp.status_code == 200:
                print("CONNECTED!")
                return url
            else:
                print(f"HTTP {resp.status_code}")
        except Exception:
            print("No response")
            
    fallback = "https://premkumarp94.pythonanywhere.com/api/telemetry/"
    print(f"  => Defaulting to PythonAnywhere: {fallback}")
    return fallback


# Auto-detect active server URL at startup
TELEMETRY_URL = find_working_telemetry_url(DEVICE_ID)

print("=" * 60)
print(f"   ESP8266 4-PROBE TANK SIMULATOR (Device ID: {DEVICE_ID})")
print(f"   Active Server URL: {TELEMETRY_URL}")
print("=" * 60)

# 4 Discrete probe levels: 0% (Empty), 25% (Low), 50% (Mid-Low), 75% (Mid-High), 100% (Full)
PROBE_LEVELS = [0, 25, 50, 75, 100]

try:
    while True:
        timestamp = datetime.now().strftime('%Y-%m-%d %H:%M:%S')
        
        # Simulate discrete 4-probe water level
        water_level = random.choice(PROBE_LEVELS)
        probe_delay_ms = 20 if water_level in [0, 25, 50] else (50 if water_level in [75, 100] else 30)
        
        # Prepare payload
        ack_val = "dummy_ack"
        device_msg = pending_device_msg
        pending_device_msg = ""  # clear so it only sends once
        
        payload = {
            "id": DEVICE_ID,
            "sensor values": {
                "water_level": water_level
            },
            "ack": ack_val,
            "message": device_msg
        }
        
        print(f"\n[{timestamp}] Probe delay used: {probe_delay_ms} ms | Send JSON (to {TELEMETRY_URL}):")
        print(json.dumps(payload, indent=2))
        
        motor_running = False
        try:
            headers = {'Content-Type': 'application/json'}
            response = requests.post(TELEMETRY_URL, data=json.dumps(payload), headers=headers, timeout=5)
            
            if response.status_code == 200:
                resp_dict = json.loads(response.text)
                print(f"[{timestamp}] Receive JSON:")
                print(json.dumps(resp_dict, indent=2))
                motor_running = resp_dict.get("motor_running", False)
            else:
                print(f"[{timestamp}] Error: HTTP status code: {response.status_code}")
                
        except requests.exceptions.ConnectionError:
            print(f"[{timestamp}] Error: Connection to {TELEMETRY_URL} failed.")
            print(f"[{timestamp}] Re-trying server discovery...")
            TELEMETRY_URL = find_working_telemetry_url(DEVICE_ID)
        except Exception as e:
            print(f"[{timestamp}] Error: {e}")
            
        # If this was the boot run, prepare the message for the first iteration
        if iteration_count == 0:
            pending_device_msg = "came to first iteration"
        iteration_count += 1

        # Motor running: check once every 10s. Else check once every 60s (1 minute).
        loop_interval = 10 if motor_running else 60
        print(f"[{timestamp}] Motor Running: {motor_running} -> Next check in {loop_interval} seconds")
        print("-" * 40)
        time.sleep(loop_interval)

except KeyboardInterrupt:
    print("\nSimulator stopped.")
