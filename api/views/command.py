import json
from django.http import JsonResponse
from django.views.decorators.csrf import csrf_exempt
from api.models import DeviceCommand, DeviceLog

@csrf_exempt
def send_command(request):
    if request.method == 'POST':
        try:
            data = json.loads(request.body)
            device_id = data.get("device_id", "esp8266_device_01")
            command = data.get("command", "")

            if not command:
                return JsonResponse({"status": "error", "message": "Command cannot be empty."}, status=400)

            DeviceCommand.objects.create(device_id=device_id, command=command)
            DeviceLog.objects.create(device_id=device_id, message=f"Queued command: {command}")

            return JsonResponse({"status": "success", "message": f"Command '{command}' sent to {device_id}."})
        except Exception as e:
            return JsonResponse({"status": "error", "message": str(e)}, status=400)
    return JsonResponse({"status": "error", "message": "POST method required."}, status=405)
