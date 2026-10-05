import json
import datetime
from django.http import JsonResponse
from django.views.decorators.csrf import csrf_exempt
from api.models import TelemetryReading, DeviceCommand, DeviceLog, get_system_setting, set_system_setting
from api.views.status import get_esp32motor_status

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
            probe_25 = sensor_values.get("probe_25")
            probe_50 = sensor_values.get("probe_50")
            probe_75 = sensor_values.get("probe_75")
            probe_100 = sensor_values.get("probe_100")
            
            # Motor status values reported by esp32motor
            motor_status = data.get("motor_status") or sensor_values.get("motor_status")
            motor_running = data.get("motor_running")
            if motor_running is None and "motor_running" in sensor_values:
                motor_running = sensor_values.get("motor_running")
            servo_angle = data.get("servo_angle") or sensor_values.get("servo_angle")
            hold_ms = data.get("hold_ms") or sensor_values.get("hold_ms")
            angle_on = data.get("angle_on") or sensor_values.get("angle_on")
            angle_off = data.get("angle_off") or sensor_values.get("angle_off")

            if hold_ms is not None:
                try:
                    set_system_setting("motor_hold_ms", str(int(hold_ms)))
                except (ValueError, TypeError):
                    pass
            if angle_on is not None:
                try:
                    set_system_setting("motor_angle_on", str(int(angle_on)))
                except (ValueError, TypeError):
                    pass
            if angle_off is not None:
                try:
                    set_system_setting("motor_angle_off", str(int(angle_off)))
                except (ValueError, TypeError):
                    pass
            
            ack = data.get("ack", "none")
            message = data.get("message", "none")
            
            # Save telemetry reading to database
            if (temperature is not None or humidity is not None or water_level is not None or 
                probe_25 is not None or motor_status is not None or motor_running is not None or
                "motor" in device_id.lower()):
                
                TelemetryReading.objects.create(
                    device_id=device_id,
                    temperature=float(temperature) if temperature is not None else None,
                    humidity=float(humidity) if humidity is not None else None,
                    water_level=float(water_level) if water_level is not None else None,
                    probe_25=bool(probe_25) if probe_25 is not None else None,
                    probe_50=bool(probe_50) if probe_50 is not None else None,
                    probe_75=bool(probe_75) if probe_75 is not None else None,
                    probe_100=bool(probe_100) if probe_100 is not None else None,
                    motor_status=str(motor_status) if motor_status is not None else None,
                    motor_running=bool(motor_running) if motor_running is not None else None,
                    servo_angle=int(servo_angle) if servo_angle is not None else None,
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
            if water_level is not None:
                print(f"  - Sensor Values: Water Level = {water_level}%, Temp = {temperature} C, Humidity = {humidity} %")
            if motor_status is not None or motor_running is not None:
                print(f"  - Motor Report from esp32motor: Status={motor_status}, Running={motor_running}, Servo Angle={servo_angle}°")
            print(f"  - Acknowledgment: {ack} | Message: {message}")

            # Get latest water level reading
            latest_water_reading = TelemetryReading.objects.filter(water_level__isnull=False).order_by('-timestamp').first()
            latest_water_lvl = latest_water_reading.water_level if latest_water_reading else water_level

            # Get ACTUAL motor running status from esp32motor.ino (stored in DB)
            motor_info = get_esp32motor_status()
            real_motor_running = motor_info["motor_running"]
            if "motor" in device_id.lower() and motor_running is not None:
                real_motor_running = bool(motor_running)

            # AUTOMATIC MOTOR OFF TRIGGER ON 100% WATER LEVEL:
            # At ANY time server checks water level & motor status. If water level is 100% and motor is running, trigger MOTOR_OFF
            if latest_water_lvl is not None and latest_water_lvl >= 100 and real_motor_running:
                has_pending_off = DeviceCommand.objects.filter(
                    device_id__in=["esp32motor", "esp32_motor_01"],
                    command__icontains="OFF",
                    is_executed=False
                ).exists()
                if not has_pending_off:
                    DeviceCommand.objects.create(
                        device_id="esp32motor",
                        command="MOTOR_OFF"
                    )
                    DeviceLog.objects.create(
                        device_id="SERVER_AUTO",
                        message="[AUTO-SHUTOFF] Tank Full (100%) detected on server! Automatically queued MOTOR_OFF for esp32motor."
                    )
                    print(f"  [AUTO-SHUTOFF] Tank Full (100%). Queued MOTOR_OFF command for esp32motor.")

            # Check pending queued commands
            pending_cmd = DeviceCommand.objects.filter(is_executed=False).filter(
                device_id__in=[device_id, "esp32motor", "esp32_motor_01", "esp8266_device_01", "esp32_device_01"]
            ).first() or DeviceCommand.objects.filter(is_executed=False).first()
            
            if pending_cmd:
                server_cmd = pending_cmd.command
                pending_cmd.is_executed = True
                pending_cmd.save()
                print(f"  [COMMAND QUEUED] Delivering command '{server_cmd}' to device {device_id}")
            else:
                server_cmd = ""

            # Direct Failsafe: if water level is 100% and motor is running, ensure server_cmd is MOTOR_OFF when motor device calls telemetry
            if latest_water_lvl is not None and latest_water_lvl >= 100 and real_motor_running:
                if "motor" in device_id.lower() and not server_cmd:
                    server_cmd = "MOTOR_OFF"

            # Get current active sync interval setting from DB
            sync_interval_str = get_system_setting("sync_interval", "60")
            try:
                sync_interval = int(sync_interval_str)
            except ValueError:
                sync_interval = 60

            # Compute IST time (UTC+5:30) formatted as YYYYMMDD:HH:MM:SS
            ist_offset = datetime.timezone(datetime.timedelta(hours=5, minutes=30))
            now_ist = datetime.datetime.now(ist_offset)
            ist_time_str = now_ist.strftime("%Y%m%d:%H:%M:%S")

            print(f"  => Response to {device_id}: Water Level={latest_water_lvl}% | Motor Running={real_motor_running} | Sync Interval={sync_interval}s | IST={ist_time_str}")
            
            return JsonResponse({
                "status": "success",
                "command": server_cmd,
                "water_level": latest_water_lvl,
                "motor_running": real_motor_running,
                "motor_status": motor_info["motor_status"],
                "servo_angle": motor_info["servo_angle"],
                "interval_seconds": sync_interval,
                "sleep_seconds": sync_interval,
                "sync_interval": sync_interval,
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
