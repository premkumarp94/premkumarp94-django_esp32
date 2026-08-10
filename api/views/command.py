import json
from django.http import JsonResponse
from django.views.decorators.csrf import csrf_exempt
from api.models import DeviceCommand, TelemetryReading, WaterThreshold, DeviceLog

@csrf_exempt
def send_command(request):
    if request.method == 'POST':
        try:
            data = json.loads(request.body)
            device_id = data.get("device_id", "esp8266_device_02")
            command = data.get("command", "")

            # Threshold Configuration Command
            if command.startswith("set_threshold:"):
                parts = command.split(":")
                start_val = float(parts[1]) if len(parts) > 1 else 33.0
                stop_val = float(parts[2]) if len(parts) > 2 else 100.0
                auto_mode = bool(int(parts[3])) if len(parts) > 3 else True

                t_obj, _ = WaterThreshold.objects.get_or_create(id=1)
                t_obj.start_level = start_val
                t_obj.stop_level = stop_val
                t_obj.auto_mode = auto_mode
                t_obj.save()

                DeviceLog.objects.create(
                    device_id="system",
                    message=f"Water level thresholds updated: Start at <= {start_val:.0f}%, Stop at >= {stop_val:.0f}% (Auto Mode: {'ON' if auto_mode else 'OFF'})"
                )

                # Queue command for ESP8266 motor device so it receives new settings
                DeviceCommand.objects.create(device_id=device_id, command=command)

                return JsonResponse({
                    "status": "success",
                    "message": f"Thresholds saved: Start <= {start_val:.0f}%, Stop >= {stop_val:.0f}%.",
                    "start_level": start_val,
                    "stop_level": stop_val,
                    "auto_mode": auto_mode
                })

            if command in ["start_motor", "stop_motor", "gear_front", "gear_back", "gear_stop", "front", "back", "stop"] or command.startswith("set_servo"):
                DeviceCommand.objects.create(device_id=device_id, command=command)
                
                # Optimistically update latest reading for device in DB so status endpoint updates in < 10ms
                latest_reading = TelemetryReading.objects.filter(device_id=device_id).order_by('-timestamp').first()
                if not latest_reading:
                    latest_reading = TelemetryReading.objects.create(device_id=device_id, motor_status="stopped")

                if command in ["start_motor", "gear_front", "front"]:
                    latest_reading.motor_status = "started"
                elif command in ["stop_motor", "gear_stop", "stop"]:
                    latest_reading.motor_status = "stopped"
                latest_reading.save()

                return JsonResponse({"status": "success", "message": f"Command '{command}' sent to {device_id}."})
                
            return JsonResponse({"status": "error", "message": f"Invalid command '{command}'."}, status=400)
        except Exception as e:
            return JsonResponse({"status": "error", "message": str(e)}, status=400)
    return JsonResponse({"status": "error", "message": "POST method required."}, status=405)
