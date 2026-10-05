import json
from django.http import JsonResponse
from django.views.decorators.csrf import csrf_exempt
from api.models import DeviceCommand, DeviceLog, set_system_setting

@csrf_exempt
def send_command(request):
    if request.method == 'POST':
        try:
            data = json.loads(request.body)
            device_id = data.get("device_id", "esp8266_device_01")
            command = data.get("command", "")

            if not command:
                return JsonResponse({"status": "error", "message": "Command cannot be empty."}, status=400)

            # Check if updating reading/sync interval or motor settings
            cmd_upper = command.strip().upper()
            if cmd_upper.startswith("INTERVAL:") or cmd_upper.startswith("SET_INTERVAL:") or cmd_upper.startswith("SLEEP:"):
                try:
                    val = int(cmd_upper.split(":")[-1])
                    if 1 <= val <= 86400:
                        set_system_setting("sync_interval", str(val))
                        print(f"[SERVER SYSTEM SETTING] Saved sync_interval = {val}s to database")
                except ValueError:
                    pass
            elif cmd_upper.startswith("SET_HOLD_SEC:") or cmd_upper.startswith("HOLD_SEC:") or cmd_upper.startswith("HOLD_TIME:"):
                try:
                    sec = float(cmd_upper.split(":")[-1])
                    val = int(sec * 1000.0 + 0.5)
                    if 100 <= val <= 30000:
                        set_system_setting("motor_hold_ms", str(val))
                        print(f"[SERVER SYSTEM SETTING] Saved motor_hold_ms = {val}ms to database")
                except ValueError:
                    pass
            elif cmd_upper.startswith("SET_HOLD_TIME:") or cmd_upper.startswith("HOLD_MS:") or cmd_upper.startswith("SET_HOLD_MS:"):
                try:
                    val = int(cmd_upper.split(":")[-1])
                    if 100 <= val <= 30000:
                        set_system_setting("motor_hold_ms", str(val))
                        print(f"[SERVER SYSTEM SETTING] Saved motor_hold_ms = {val}ms to database")
                except ValueError:
                    pass
            elif cmd_upper.startswith("SET_ON_ANGLE:") or cmd_upper.startswith("ANGLE_ON:"):
                try:
                    val = int(cmd_upper.split(":")[-1])
                    if 0 <= val <= 180:
                        set_system_setting("motor_angle_on", str(val))
                except ValueError:
                    pass
            elif cmd_upper.startswith("SET_OFF_ANGLE:") or cmd_upper.startswith("ANGLE_OFF:"):
                try:
                    val = int(cmd_upper.split(":")[-1])
                    if 0 <= val <= 180:
                        set_system_setting("motor_angle_off", str(val))
                except ValueError:
                    pass
            elif cmd_upper.startswith("SET_ANGLES:"):
                try:
                    rest = cmd_upper.split(":")[-1]
                    parts = rest.split(",")
                    if len(parts) == 2:
                        on_val = int(parts[0].strip())
                        off_val = int(parts[1].strip())
                        if 0 <= on_val <= 180:
                            set_system_setting("motor_angle_on", str(on_val))
                        if 0 <= off_val <= 180:
                            set_system_setting("motor_angle_off", str(off_val))
                except ValueError:
                    pass

            elif cmd_upper.startswith("SET_MAX_RUN_MIN:") or cmd_upper.startswith("MAX_RUN_MIN:"):
                try:
                    val = int(cmd_upper.split(":")[-1])
                    if 1 <= val <= 1440:
                        set_system_setting("motor_max_run_min", str(val))
                        print(f"[SERVER SYSTEM SETTING] Saved motor_max_run_min = {val} min to database")
                except ValueError:
                    pass

            if cmd_upper.startswith("MOTOR_ON") or cmd_upper in ["ON", "START"]:
                log_msg = "Motor ON requested from mobile / web UI"
            elif cmd_upper.startswith("MOTOR_OFF") or cmd_upper in ["OFF", "STOP"]:
                log_msg = "Motor OFF requested from mobile / web UI"
            elif cmd_upper.startswith("INTERVAL:") or cmd_upper.startswith("SET_INTERVAL:"):
                val = cmd_upper.split(":")[-1]
                log_msg = f"Reading interval set to {val}s"
            elif cmd_upper.startswith("SET_HOLD_SEC:") or cmd_upper.startswith("HOLD_SEC:"):
                sec = cmd_upper.split(":")[-1]
                log_msg = f"Motor return delay set to {sec}s"
            elif cmd_upper.startswith("SET_MAX_RUN_MIN:") or cmd_upper.startswith("MAX_RUN_MIN:"):
                val = cmd_upper.split(":")[-1]
                log_msg = f"Auto turn OFF max run limit set to {val} min"
            elif cmd_upper.startswith("SET_ANGLES:"):
                angles = cmd_upper.split(":")[-1]
                log_msg = f"Motor angles updated ({angles})"
            else:
                log_msg = f"Command queued: {command}"

            DeviceCommand.objects.create(device_id=device_id, command=command)
            DeviceLog.objects.create(device_id=device_id, message=log_msg)

            return JsonResponse({"status": "success", "message": f"Command '{command}' sent to {device_id}."})
        except Exception as e:
            return JsonResponse({"status": "error", "message": str(e)}, status=400)
    return JsonResponse({"status": "error", "message": "POST method required."}, status=405)

