import json
from django.http import JsonResponse
from django.views.decorators.csrf import csrf_exempt
from api.models import TelemetryReading, DeviceCommand, DeviceLog, WaterThreshold

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
            motor_status = sensor_values.get("motor_status", "stopped")
            ack = data.get("ack", "none")
            message = data.get("message", "none")
            
            # Save telemetry reading to the database
            if temperature is not None or humidity is not None or water_level is not None or motor_status is not None:
                TelemetryReading.objects.create(
                    device_id=device_id,
                    temperature=float(temperature) if temperature is not None else None,
                    humidity=float(humidity) if humidity is not None else None,
                    water_level=float(water_level) if water_level is not None else None,
                    motor_status=motor_status
                )

            # Auto-prune old readings periodically
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
            print(f"  - Sensor Values: Water Level = {water_level}%, Temp = {temperature} C, Humidity = {humidity} %, Motor = {motor_status}")
            print(f"  - Acknowledgment: {ack} | Message: {message}")
            
            # Fetch active water level thresholds
            t_obj, _ = WaterThreshold.objects.get_or_create(id=1, defaults={"start_level": 33.0, "stop_level": 100.0, "auto_mode": True})

            # Sync settings from sensor_values if reported by device
            min_pct = sensor_values.get("min_percentage")
            max_pct = sensor_values.get("max_percentage")
            wl_mode = sensor_values.get("water_level_mode")
            
            threshold_updated = False
            if min_pct is not None:
                t_obj.start_level = float(min_pct)
                threshold_updated = True
            if max_pct is not None:
                t_obj.stop_level = float(max_pct)
                threshold_updated = True
            if wl_mode is not None:
                t_obj.auto_mode = bool(wl_mode)
                threshold_updated = True
            if threshold_updated:
                t_obj.save()

            # AUTOMATIC THRESHOLD MOTOR CONTROL
            # Evaluate auto start/stop rules when water level is reported by device_01 or device_02
            if water_level is not None and t_obj.auto_mode:
                w_val = float(water_level)
                
                # Fetch latest status for motor controller (esp8266_device_02)
                latest_motor_dev = TelemetryReading.objects.filter(device_id="esp8266_device_02").order_by('-timestamp').first()
                curr_motor_status = latest_motor_dev.motor_status if latest_motor_dev else "stopped"

                if w_val <= t_obj.start_level and curr_motor_status != "started":
                    print(f"  [AUTO THRESHOLD] Water level ({w_val}%) <= Start threshold ({t_obj.start_level}%). Queueing START for esp8266_device_02")
                    DeviceCommand.objects.create(device_id="esp8266_device_02", command="start_motor")
                    TelemetryReading.objects.create(device_id="esp8266_device_02", motor_status="started")
                    DeviceLog.objects.create(
                        device_id="esp8266_device_02",
                        message=f"Auto Trigger: Tank water level ({w_val:.0f}%) <= Start Threshold ({t_obj.start_level:.0f}%). Motor STARTED."
                    )
                elif w_val >= t_obj.stop_level and curr_motor_status != "stopped":
                    print(f"  [AUTO THRESHOLD] Water level ({w_val}%) >= Stop threshold ({t_obj.stop_level}%). Queueing STOP for esp8266_device_02")
                    DeviceCommand.objects.create(device_id="esp8266_device_02", command="stop_motor")
                    TelemetryReading.objects.create(device_id="esp8266_device_02", motor_status="stopped")
                    DeviceLog.objects.create(
                        device_id="esp8266_device_02",
                        message=f"Auto Trigger: Tank water level ({w_val:.0f}%) >= Stop Threshold ({t_obj.stop_level:.0f}%). Motor STOPPED."
                    )

            # Check pending queued commands for this reporting device
            pending_cmd = DeviceCommand.objects.filter(device_id=device_id, is_executed=False).first()
            if pending_cmd:
                server_cmd = pending_cmd.command
                pending_cmd.is_executed = True
                pending_cmd.save()

                latest_reading = TelemetryReading.objects.filter(device_id=device_id).order_by('-timestamp').first()
                if latest_reading:
                    if server_cmd in ["start_motor", "gear_front", "front"]:
                        latest_reading.motor_status = "started"
                        latest_reading.save()
                    elif server_cmd in ["stop_motor", "gear_stop", "stop"]:
                        latest_reading.motor_status = "stopped"
                        latest_reading.save()
            else:
                # If esp8266_device_02 asks for command and auto mode is enabled, evaluate latest device_01 reading
                if device_id == "esp8266_device_02" and t_obj.auto_mode:
                    dev1_reading = TelemetryReading.objects.filter(device_id="esp8266_device_01").order_by('-timestamp').first()
                    if dev1_reading and dev1_reading.water_level is not None:
                        w_val = dev1_reading.water_level
                        if w_val <= t_obj.start_level and motor_status != "started":
                            server_cmd = "start_motor"
                        elif w_val >= t_obj.stop_level and motor_status != "stopped":
                            server_cmd = "stop_motor"
                        else:
                            server_cmd = ""
                    else:
                        server_cmd = ""
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
