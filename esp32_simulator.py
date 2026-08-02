import time
import random
import requests
import json
from datetime import datetime

# Configurations
DEVICE_ID = "esp32_device_01"  # Unique device name
loop_interval = 2  # sends data once every 2 seconds
motor_status = "stopped"
pending_device_msg = "esp32 booted normally"
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
print(f"   ESP32 SIMULATOR STARTING (Device ID: {DEVICE_ID})")
print(f"   Active Server URL: {TELEMETRY_URL}")
print("=" * 60)

try:
    while True:
        timestamp = datetime.now().strftime('%Y-%m-%d %H:%M:%S')
        
        # Simulate sensor readings
        water_level = random.randrange(0, 105, 5)
        
        # Prepare payload
        ack_val = "dummy_ack"
        device_msg = pending_device_msg
        pending_device_msg = ""  # clear so it only sends once
        
        payload = {
            "id": DEVICE_ID,
            "sensor values": {
                "water_level": water_level,
                "motor_status": motor_status
            },
            "ack": ack_val,
            "message": device_msg
        }
        
        print(f"\n[{timestamp}] Send JSON (to {TELEMETRY_URL}):")
        print(json.dumps(payload, indent=2))
        
        try:
            headers = {'Content-Type': 'application/json'}
            response = requests.post(TELEMETRY_URL, data=json.dumps(payload), headers=headers, timeout=5)
            
            if response.status_code == 200:
                resp_dict = json.loads(response.text)
                print(f"[{timestamp}] Receive JSON:")
                print(json.dumps(resp_dict, indent=2))
                
                command = resp_dict.get("command", "")
                
                cmd_executed = False
                if command == "start_motor":
                    motor_status = "started"
                    pending_device_msg = "Motor started successfully."
                    print(f"[{timestamp}] => Action executed: Motor started!")
                    cmd_executed = True
                elif command == "stop_motor":
                    motor_status = "stopped"
                    pending_device_msg = "Motor stopped successfully."
                    print(f"[{timestamp}] => Action executed: Motor stopped!")
                    cmd_executed = True
                else:
                    if command not in ["", "none", None]:
                        print(f"[{timestamp}] => Invalid command '{command}' received (treating as no command received).")
                    else:
                        print(f"[{timestamp}] => No command received.")
                
                # Send immediate telemetry confirmation to update server status instantly
                if cmd_executed:
                    print(f"[{timestamp}] => Sending immediate status update confirmation...")
                    confirm_payload = {
                        "id": DEVICE_ID,
                        "sensor values": {
                            "water_level": water_level,
                            "motor_status": motor_status
                        },
                        "ack": "cmd_executed_ack",
                        "message": pending_device_msg
                    }
                    pending_device_msg = ""
                    try:
                        confirm_resp = requests.post(TELEMETRY_URL, data=json.dumps(confirm_payload), headers=headers, timeout=5)
                        if confirm_resp.status_code == 200:
                            print(f"[{timestamp}] => Server state updated immediately!")
                    except Exception as confirm_err:
                        print(f"[{timestamp}] Error sending immediate status update: {confirm_err}")
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

        print("-" * 40)
        time.sleep(loop_interval)

except KeyboardInterrupt:
    print("\nSimulator stopped.")
