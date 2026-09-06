import json
import datetime
from django.http import JsonResponse
from django.views.decorators.csrf import csrf_exempt
from api.models import TelemetryReading, DeviceCommand, DeviceLog

request_counter = 0

@csrf_exempt
def telemetry(request):
    global request_counter
    if request.method == 'POST':
        try:
            data = json.loads(request.body)
            
            device_id = data.get("id", "unknown")
            sensor_values = data.get("sensor values", {})
            temperature = sensor_values.get("temperature")
            humidity = sensor_values.get("humidity")
            water_level = sensor_values.get("water_level")
            ack = data.get("ack", "none")
            message = data.get("message", "none")
            
            # Save telemetry reading to database
            if temperature is not None or humidity is not None or water_level is not None:
                TelemetryReading.objects.create(
                    device_id=device_id,
                    temperature=float(temperature) if temperature is not None else None,
                    humidity=float(humidity) if humidity is not None else None,
                    water_level=float(water_level) if water_level is not None else None
                )

            # Auto-prune old readings periodically to prevent unbounded DB growth
            request_counter += 1
            if request_counter % 50 == 0:
                try:
                    excess_ids = list(TelemetryReading.objects.values_list('id', flat=True).order_by('-timestamp')[300:])
                    if excess_ids:
                        TelemetryReading.objects.filter(id__in=excess_ids).delete()
                except Exception as prune_err:
                    print(f"Telemetry pruning warning: {prune_err}")

            # Save device log
            if message and message not in ["none", "", "device_normal_operation"]:
                DeviceLog.objects.create(
                    device_id=device_id,
                    message=message
                )
            
            print(f"\n[Django Telemetry] Device: {device_id}")
            print(f"  - Sensor Values: Water Level = {water_level}%, Temp = {temperature} C, Humidity = {humidity} %")
            print(f"  - Acknowledgment: {ack} | Message: {message}")

            # Check pending queued commands for this reporting device
            pending_cmd = DeviceCommand.objects.filter(device_id=device_id, is_executed=False).first()
            if pending_cmd:
                server_cmd = pending_cmd.command
                pending_cmd.is_executed = True
                pending_cmd.save()
            else:
                server_cmd = ""

            # Get latest water level reading from tank device (esp8266_device_01)
            dev1_reading = TelemetryReading.objects.filter(device_id="esp8266_device_01").order_by('-timestamp').first()
            latest_water_lvl = dev1_reading.water_level if (dev1_reading and dev1_reading.water_level is not None) else water_level

            # Compute IST time (UTC+5:30) formatted as YYYYMMDD:HH:MM:SS
            ist_offset = datetime.timezone(datetime.timedelta(hours=5, minutes=30))
            now_ist = datetime.datetime.now(ist_offset)
            ist_time_str = now_ist.strftime("%Y%m%d:%H:%M:%S")

            print(f"  => Response to {device_id}: Water Level={latest_water_lvl}% | IST={ist_time_str}")
            
            return JsonResponse({
                "status": "success",
                "command": server_cmd,
                "water_level": latest_water_lvl,
                "ist_time": ist_time_str
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
