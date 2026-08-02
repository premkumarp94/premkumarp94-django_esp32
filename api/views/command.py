import json
from django.http import JsonResponse
from django.views.decorators.csrf import csrf_exempt
from api.models import DeviceCommand, TelemetryReading

@csrf_exempt
def send_command(request):
    if request.method == 'POST':
        try:
            data = json.loads(request.body)
            device_id = data.get("device_id", "esp32_device_01")
            command = data.get("command")
            if command in ["start_motor", "stop_motor", "gear_front", "gear_back", "gear_stop", "front", "back", "stop"] or command.startswith("set_servo"):
                DeviceCommand.objects.create(device_id=device_id, command=command)
                
                # Optimistically update latest reading for device in DB so status endpoint updates in < 10ms
                latest_reading = TelemetryReading.objects.filter(device_id=device_id).order_by('-timestamp').first()
                if latest_reading:
                    if command in ["start_motor", "gear_front", "front"]:
                        latest_reading.motor_status = "started" if device_id == "esp32_device_01" else "front"
                    elif command in ["stop_motor", "gear_stop", "stop"]:
                        latest_reading.motor_status = "stopped"
                    elif command in ["gear_back", "back"]:
                        latest_reading.motor_status = "back"
                    latest_reading.save()

                return JsonResponse({"status": "success", "message": f"Command '{command}' sent to {device_id}."})
            return JsonResponse({"status": "error", "message": f"Invalid command '{command}'."}, status=400)
        except Exception as e:
            return JsonResponse({"status": "error", "message": str(e)}, status=400)
    return JsonResponse({"status": "error", "message": "POST method required."}, status=405)
