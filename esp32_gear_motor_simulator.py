import time
import random
import requests
import json
import threading
from datetime import datetime

# Configurations
DEVICE_ID = "esp32_gear_motor_02"  # Unique device name for Gear Motor ESP32
loop_interval = 5  # sends telemetry data every 5 seconds
motor_status = "stopped"  # "stopped", "front" (forward), "back" (reverse)
pending_device_msg = "ESP32 Gear Motor booted normally"
iteration_count = 0

# Servo Motor Configuration (Angle 1 and Angle 2)
SERVO_ANGLE_1 = 30   # First configured angle (0 to 180 degrees)
SERVO_ANGLE_2 = 120  # Second configured angle (0 to 180 degrees)
SERVO_INTERVAL = 2   # Alternate angle every 2 seconds


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


def run_servo_loop():
    """Background loop that writes/actuates between SERVO_ANGLE_1 and SERVO_ANGLE_2 every 2 seconds."""
    is_angle1 = True
    while True:
        current_angle = SERVO_ANGLE_1 if is_angle1 else SERVO_ANGLE_2
        print(f"   [Servo Motor Loop] Writing angle {current_angle} deg to Servo GPIO pin...")
        time.sleep(SERVO_INTERVAL)
        is_angle1 = not is_angle1


# Start the Servo Motor 2-second background thread
servo_thread = threading.Thread(target=run_servo_loop, daemon=True)
servo_thread.start()

print("=" * 60)
print(f"   ESP32 GEAR MOTOR & SERVO SIMULATOR STARTING")
print(f"   Device ID     : {DEVICE_ID}")
print(f"   Active Server : {TELEMETRY_URL}")
print(f"   Servo Config  : Angle 1 = {SERVO_ANGLE_1} deg, Angle 2 = {SERVO_ANGLE_2} deg (Loop: every 2s)")
print("=" * 60)

try:
    while True:
        timestamp = datetime.now().strftime('%Y-%m-%d %H:%M:%S')
        
        # Simulate motor temperature and speed RPM based on state
        if motor_status == "front":
            temp = round(random.uniform(32.0, 38.0), 1)
            humidity = round(random.uniform(150.0, 180.0), 1)  # Speed RPM in forward
        elif motor_status == "back":
            temp = round(random.uniform(33.0, 39.0), 1)
            humidity = round(random.uniform(140.0, 175.0), 1)  # Speed RPM in reverse
        else:
            temp = round(random.uniform(25.0, 29.0), 1)
            humidity = 0.0  # Stopped (0 RPM)

        ack_val = "dummy_ack"
        device_msg = pending_device_msg
        pending_device_msg = ""  # clear after sending once
        
        payload = {
            "id": DEVICE_ID,
            "sensor values": {
                "temperature": temp,
                "humidity": humidity,
                "motor_status": motor_status
            },
            "ack": ack_val,
            "message": device_msg
        }
        
        print(f"\n[{timestamp}] Send Telemetry JSON (to {TELEMETRY_URL}):")
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
                if command in ["gear_front", "front"]:
                    motor_status = "front"
                    pending_device_msg = "Gear Motor running FRONT (Forward)."
                    print(f"[{timestamp}] => Action executed: Gear Motor moving FRONT!")
                    cmd_executed = True
                elif command in ["gear_back", "back"]:
                    motor_status = "back"
                    pending_device_msg = "Gear Motor running BACK (Reverse)."
                    print(f"[{timestamp}] => Action executed: Gear Motor moving BACK!")
                    cmd_executed = True
                elif command in ["gear_stop", "stop_motor", "stop"]:
                    motor_status = "stopped"
                    pending_device_msg = "Gear Motor STOPPED."
                    print(f"[{timestamp}] => Action executed: Gear Motor STOPPED!")
                    cmd_executed = True
                elif command == "start_motor":
                    motor_status = "front"
                    pending_device_msg = "Gear Motor started FRONT."
                    print(f"[{timestamp}] => Action executed: Gear Motor started FRONT!")
                    cmd_executed = True
                elif command and command.startswith("set_servo"):
                    try:
                        clean_cmd = command.replace("set_servo", "").strip(" :;,")
                        parts = clean_cmd.replace(":", " ").replace(",", " ").split()
                        if len(parts) >= 2:
                            SERVO_ANGLE_1 = int(parts[0])
                            SERVO_ANGLE_2 = int(parts[1])
                            pending_device_msg = f"Servo updated: Angle1={SERVO_ANGLE_1} deg, Angle2={SERVO_ANGLE_2} deg."
                            print(f"[{timestamp}] => Action executed: Servo configuration updated to Angle 1 = {SERVO_ANGLE_1} deg, Angle 2 = {SERVO_ANGLE_2} deg!")
                            cmd_executed = True
                    except Exception as parse_err:
                        print(f"[{timestamp}] => Error parsing servo command '{command}': {parse_err}")
                else:
                    if command not in ["", "none", None]:
                        print(f"[{timestamp}] => Invalid or unhandled command '{command}' received.")
                    else:
                        print(f"[{timestamp}] => No command received.")

                # Send immediate telemetry confirmation to update server status instantly
                if cmd_executed:
                    print(f"[{timestamp}] => Sending immediate status update confirmation...")
                    if motor_status == "front":
                        rpm_val = round(random.uniform(150.0, 180.0), 1)
                    elif motor_status == "back":
                        rpm_val = round(random.uniform(140.0, 175.0), 1)
                    else:
                        rpm_val = 0.0

                    confirm_payload = {
                        "id": DEVICE_ID,
                        "sensor values": {
                            "temperature": temp,
                            "humidity": rpm_val,
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
            
        if iteration_count == 0:
            pending_device_msg = "First iteration running smoothly."
        iteration_count += 1

        print("-" * 40)
        time.sleep(loop_interval)

except KeyboardInterrupt:
    print("\nGear Motor & Servo Simulator stopped.")
