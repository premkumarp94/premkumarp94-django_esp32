import json
from django.http import JsonResponse
from django.views.decorators.csrf import csrf_exempt
from api.models import TelemetryReading, DeviceCommand, DeviceLog

# Simple in-memory counter to simulate commands periodically
request_counter = 0

@csrf_exempt
def telemetry(request):
    if request.method == 'POST':
        try:
            # Decode the incoming JSON payload
            data = json.loads(request.body)
            
            # Extract data points based on new format
            device_id = data.get("id", "unknown")
            sensor_values = data.get("sensor values", {})
            temperature = sensor_values.get("temperature")
            humidity = sensor_values.get("humidity")
            water_level = sensor_values.get("water_level")
            motor_status = sensor_values.get("motor_status", "stopped")
            ack = data.get("ack", "none")
            message = data.get("message", "none")
            
            # Save telemetry reading to the database
            if temperature is not None or humidity is not None or water_level is not None:
                TelemetryReading.objects.create(
                    device_id=device_id,
                    temperature=float(temperature) if temperature is not None else None,
                    humidity=float(humidity) if humidity is not None else None,
                    water_level=float(water_level) if water_level is not None else None,
                    motor_status=motor_status
                )

            # Save transition log if a valid message is sent by device
            if message and message not in ["none", "", "device_normal_operation"]:
                DeviceLog.objects.create(
                    device_id=device_id,
                    message=message
                )
            
            # Print to Django server log
            print(f"\n[Django Telemetry] Device: {device_id}")
            print(f"  - Sensor Values: Water Level = {water_level}%, Temp = {temperature} C, Humidity = {humidity} %, Motor = {motor_status}")
            print(f"  - Device Acknowledgment: {ack}")
            print(f"  - Device Message: {message}")
            
            # Check for any pending queued commands
            pending_cmd = DeviceCommand.objects.filter(device_id=device_id, is_executed=False).first()
            if pending_cmd:
                if pending_cmd.command in ["start_motor", "stop_motor", "gear_front", "gear_back", "gear_stop", "front", "back", "stop"] or pending_cmd.command.startswith("set_servo"):
                    server_cmd = pending_cmd.command
                else:
                    server_cmd = ""
                
                # Mark as executed since it has been processed
                pending_cmd.is_executed = True
                pending_cmd.save()

                # Optimistically update latest reading for this device so web page reflects state immediately
                latest_reading = TelemetryReading.objects.filter(device_id=device_id).order_by('-timestamp').first()
                if latest_reading:
                    if server_cmd in ["start_motor", "gear_front", "front"]:
                        latest_reading.motor_status = "started" if device_id == "esp32_device_01" else "front"
                        latest_reading.save()
                    elif server_cmd in ["stop_motor", "gear_stop", "stop"]:
                        latest_reading.motor_status = "stopped"
                        latest_reading.save()
                    elif server_cmd in ["gear_back", "back"]:
                        latest_reading.motor_status = "back"
                        latest_reading.save()
            else:
                server_cmd = ""
                
            print(f"  => Sending response to {device_id}: Command='{server_cmd}'")
            
            return JsonResponse({
                "status": "success",
                "command": server_cmd
            })
            
        except (json.JSONDecodeError, KeyError) as e:
            return JsonResponse({
                "status": "error",
                "message": f"Invalid JSON payload: {str(e)}"
            }, status=400)
            
    return JsonResponse({
        "status": "error",
        "message": "Only POST requests are allowed"
    }, status=405)

