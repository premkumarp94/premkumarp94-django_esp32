import datetime
from django.utils import timezone
from django.http import JsonResponse
from django.shortcuts import render
from api.models import TelemetryReading, DeviceCommand, DeviceLog, get_system_setting

def format_duration(seconds):

    if seconds is None or seconds < 0:
        return "--"
    seconds = int(round(seconds))
    if seconds < 60:
        return f"{seconds}s"
    m = seconds // 60
    s = seconds % 60
    if m < 60:
        return f"{m}m {s:02d}s" if s > 0 else f"{m}m"
    h = m // 60
    m_rem = m % 60
    return f"{h}h {m_rem:02d}m"


def get_esp32motor_status():
    reading = TelemetryReading.objects.filter(
        device_id__icontains="motor"
    ).order_by('-timestamp').first()
    
    if not reading:
        reading = TelemetryReading.objects.filter(
            motor_status__isnull=False
        ).order_by('-timestamp').first()
        
    pending_cmd = DeviceCommand.objects.filter(
        device_id__icontains="motor",
        is_executed=False
    ).last()

    try:
        hold_ms = int(get_system_setting("motor_hold_ms", "1000"))
    except ValueError:
        hold_ms = 1000

    try:
        angle_on = int(get_system_setting("motor_angle_on", "45"))
    except ValueError:
        angle_on = 45

    try:
        angle_off = int(get_system_setting("motor_angle_off", "135"))
    except ValueError:
        angle_off = 135

    hold_sec = round(hold_ms / 1000.0, 2)

    if reading:
        is_on = reading.motor_running if reading.motor_running is not None else (str(reading.motor_status).upper() in ["ON", "RUNNING", "TRUE"])
        angle = reading.servo_angle if reading.servo_angle is not None else (angle_on if is_on else (angle_off if str(reading.motor_status).upper() == "OFF" else 90))
        status_label = "Motor ON" if is_on else "Motor OFF"
        status_text = f"{status_label} (Servo: {angle}°)"
        return {
            "has_data": True,
            "motor_running": is_on,
            "motor_status": status_text,
            "raw_status": reading.motor_status or ("ON" if is_on else "OFF"),
            "servo_angle": angle,
            "angle_on": angle_on,
            "angle_off": angle_off,
            "hold_ms": hold_ms,
            "hold_sec": hold_sec,
            "device_id": reading.device_id,
            "timestamp": reading.timestamp,
            "has_pending_command": pending_cmd is not None,
            "pending_command": pending_cmd.command if pending_cmd else ""
        }
    
    return {
        "has_data": False,
        "motor_running": False,
        "motor_status": "Motor OFF (Servo: 90°)",
        "raw_status": "OFF",
        "servo_angle": 90,
        "angle_on": angle_on,
        "angle_off": angle_off,
        "hold_ms": hold_ms,
        "hold_sec": hold_sec,
        "device_id": "esp32motor",
        "timestamp": None,
        "has_pending_command": pending_cmd is not None,
        "pending_command": pending_cmd.command if pending_cmd else ""
    }



def analyze_tank_timings(readings, now=None):
    if now is None:
        now = timezone.now()

    motor_info = get_esp32motor_status()

    result = {
        "is_filling": motor_info["motor_running"],
        "motor_detected": motor_info["motor_running"],
        "motor_status_text": motor_info["motor_status"],
        "servo_angle": motor_info["servo_angle"],
        "motor_info": motor_info,
        "timing_75_to_100_seconds": None,
        "timing_75_to_100_text": "--",
        "step_timings": {
            "0_to_25": {"seconds": None, "text": "--"},
            "25_to_50": {"seconds": None, "text": "--"},
            "50_to_75": {"seconds": None, "text": "--"},
            "75_to_100": {"seconds": None, "text": "--"},
        },
        "total_fill_time_seconds": None,
        "total_fill_time_text": "--",
        "eta_seconds": None,
        "eta_text": "Motor OFF",
        "eta_target_time": "--",
        "avg_step_seconds": 180.0
    }

    if not readings:
        return result

    # Sort chronological (oldest to newest)
    sorted_readings = sorted(readings, key=lambda r: r.timestamp)

    # Extract level transitions
    transitions = []
    prev_level = None
    for r in sorted_readings:
        if r.water_level is None:
            continue
        lvl = int(round(r.water_level))
        if prev_level is None or lvl != prev_level:
            transitions.append((lvl, r.timestamp))
            prev_level = lvl

    if not transitions:
        return result

    step_history = {
        "0_to_25": [],
        "25_to_50": [],
        "50_to_75": [],
        "75_to_100": []
    }

    start_times = {
        "at_0": None,
        "at_25": None,
        "at_50": None,
        "at_75": None,
    }

    prev_level = None

    for r in sorted_readings:
        if r.water_level is None:
            continue
        lvl = int(round(r.water_level))

        if prev_level is None:
            prev_level = lvl
            if lvl <= 0:
                start_times["at_0"] = r.timestamp
            elif lvl == 25:
                start_times["at_25"] = r.timestamp
            elif lvl == 50:
                start_times["at_50"] = r.timestamp
            elif lvl == 75:
                start_times["at_75"] = r.timestamp
            continue

        if lvl != prev_level:
            if lvl < prev_level:
                if lvl <= 0:
                    start_times["at_0"] = r.timestamp
                    start_times["at_25"] = None
                    start_times["at_50"] = None
                    start_times["at_75"] = None
                elif lvl <= 25:
                    start_times["at_25"] = r.timestamp
                    start_times["at_50"] = None
                    start_times["at_75"] = None
                elif lvl <= 50:
                    start_times["at_50"] = r.timestamp
                    start_times["at_75"] = None
            elif lvl > prev_level:
                if prev_level <= 0 and lvl >= 25:
                    if start_times["at_0"]:
                        d = (r.timestamp - start_times["at_0"]).total_seconds()
                        if 10 < d < 3600:
                            step_history["0_to_25"].append(d)
                    start_times["at_25"] = r.timestamp

                if prev_level <= 25 and lvl >= 50:
                    if start_times["at_25"]:
                        d = (r.timestamp - start_times["at_25"]).total_seconds()
                        if 10 < d < 3600:
                            step_history["25_to_50"].append(d)
                    start_times["at_50"] = r.timestamp

                if prev_level <= 50 and lvl >= 75:
                    if start_times["at_50"]:
                        d = (r.timestamp - start_times["at_50"]).total_seconds()
                        if 10 < d < 3600:
                            step_history["50_to_75"].append(d)
                    start_times["at_75"] = r.timestamp

                if prev_level >= 75 and lvl >= 100:
                    if start_times["at_75"]:
                        d = (r.timestamp - start_times["at_75"]).total_seconds()
                        if 120 <= d <= 1800:
                            step_history["75_to_100"].append(d)
                        start_times["at_75"] = None

            prev_level = lvl

    all_known_steps = []
    for k, vals in step_history.items():
        if vals:
            latest_dur = vals[-1]
            result["step_timings"][k] = {
                "seconds": latest_dur,
                "text": format_duration(latest_dur)
            }
            all_known_steps.append(latest_dur)
            if k == "75_to_100":
                result["timing_75_to_100_seconds"] = latest_dur
                result["timing_75_to_100_text"] = format_duration(latest_dur)

    if all(result["step_timings"][k]["seconds"] is not None for k in result["step_timings"]):
        total_s = sum(result["step_timings"][k]["seconds"] for k in result["step_timings"])
        result["total_fill_time_seconds"] = total_s
        result["total_fill_time_text"] = format_duration(total_s)

    if step_history["50_to_75"]:
        latest_step_reference = step_history["50_to_75"][-1]
        reference_source_label = f"Latest 50%->75% ({format_duration(latest_step_reference)})"
    elif step_history["75_to_100"]:
        latest_step_reference = step_history["75_to_100"][-1]
        reference_source_label = f"Last 75%->100% ({format_duration(latest_step_reference)})"
    elif step_history["25_to_50"]:
        latest_step_reference = step_history["25_to_50"][-1]
        reference_source_label = f"Latest 25%->50% ({format_duration(latest_step_reference)})"
    elif step_history["0_to_25"]:
        latest_step_reference = step_history["0_to_25"][-1]
        reference_source_label = f"Latest 0%->25% ({format_duration(latest_step_reference)})"
    else:
        latest_step_reference = 180.0
        reference_source_label = "Baseline default (3m)"

    result["latest_reference_seconds"] = latest_step_reference
    result["latest_reference_text"] = format_duration(latest_step_reference)
    result["reference_source_label"] = reference_source_label

    # Current level analysis (filter for readings with water_level != None)
    water_readings = [r for r in sorted_readings if r.water_level is not None]
    if water_readings:
        latest_reading = water_readings[-1]
        curr_lvl = int(round(latest_reading.water_level))
        curr_lvl_time = latest_reading.timestamp
        time_since_curr_lvl = (now - curr_lvl_time).total_seconds()
    else:
        curr_lvl = 0
        time_since_curr_lvl = 0

    active_75_start = start_times.get("at_75")
    time_spent_at_75 = (now - active_75_start).total_seconds() if active_75_start else 0

    # Get ACTUAL motor status reported by esp32motor.ino
    # SERVER DOES NOT ASSUME MOTOR STATUS!
    is_actively_filling = motor_info["motor_running"]
    servo_angle = motor_info["servo_angle"]

    if curr_lvl >= 100 and not is_actively_filling:
        result["is_filling"] = False
        result["motor_detected"] = False
        result["motor_status_text"] = f"Tank Full (100%) - Servo: {servo_angle}°"
        result["servo_angle"] = servo_angle
        result["eta_seconds"] = 0
        result["eta_text"] = "Tank Full (100%)"
        result["eta_target_time"] = "Full"
    elif is_actively_filling:
        result["is_filling"] = True
        result["motor_detected"] = True
        result["motor_status_text"] = f"Motor ON (Servo: {servo_angle}°)"
        result["servo_angle"] = servo_angle

        if curr_lvl == 75:
            step_75_100_est = latest_step_reference
            est_remaining = max(5, step_75_100_est - time_spent_at_75)
        elif curr_lvl == 50:
            est_remaining = max(5, (2 * latest_step_reference) - time_since_curr_lvl)
        elif curr_lvl == 25:
            est_remaining = max(5, (3 * latest_step_reference) - time_since_curr_lvl)
        else:
            est_remaining = max(10, (4 * latest_step_reference) - time_since_curr_lvl)

        result["eta_seconds"] = est_remaining
        result["eta_text"] = f"~{format_duration(est_remaining)} until 100%"

        target_time = now + datetime.timedelta(seconds=est_remaining)
        ist_offset = datetime.timezone(datetime.timedelta(hours=5, minutes=30))
        target_ist = target_time.astimezone(ist_offset)
        result["eta_target_time"] = target_ist.strftime("%I:%M:%S %p")
    else:
        result["is_filling"] = False
        result["motor_detected"] = False
        result["motor_status_text"] = f"Motor OFF (Servo: {servo_angle}°)"
        result["servo_angle"] = servo_angle
        result["eta_seconds"] = None
        result["eta_text"] = "Motor OFF"
        result["eta_target_time"] = "--"

    return result


def status(request):
    try:
        recent_devs = TelemetryReading.objects.order_by('-timestamp')[:50].values_list('device_id', flat=True)
        device_ids = list(dict.fromkeys(recent_devs))
    except Exception:
        device_ids = []

    default_devices = ["esp8266_device_01", "esp32motor"]
    for dev in default_devices:
        if dev not in device_ids:
            device_ids.append(dev)

    now = timezone.now()
    devices_data = {}
    for dev_id in device_ids:
        try:
            reading = TelemetryReading.objects.filter(device_id=dev_id).order_by('-timestamp').first()
        except Exception:
            reading = None

        if reading:
            seconds_ago = int((now - reading.timestamp).total_seconds())
            if seconds_ago < 0:
                seconds_ago = 0

            if seconds_ago < 10:
                last_seen_text = "Just now"
            elif seconds_ago < 60:
                last_seen_text = f"{seconds_ago}s ago"
            elif seconds_ago < 3600:
                last_seen_text = f"{seconds_ago // 60}m ago"
            elif seconds_ago < 86400:
                last_seen_text = f"{seconds_ago // 3600}h ago"
            else:
                last_seen_text = f"{seconds_ago // 86400}d ago"

            # Server listens for any time and lets board sync status/response on its own schedule
            connection_status = "online"

            devices_data[dev_id] = {
                "has_data": True,
                "device_id": reading.device_id,
                "temperature": reading.temperature,
                "humidity": reading.humidity,
                "water_level": reading.water_level,
                "probe_25": reading.probe_25,
                "probe_50": reading.probe_50,
                "probe_75": reading.probe_75,
                "probe_100": reading.probe_100,
                "motor_status": reading.motor_status,
                "motor_running": reading.motor_running,
                "servo_angle": reading.servo_angle,
                "timestamp": reading.timestamp.strftime('%Y-%m-%d %H:%M:%S'),
                "iso_timestamp": reading.timestamp.isoformat(),
                "seconds_ago": seconds_ago,
                "last_seen_text": last_seen_text,
                "connection_status": connection_status
            }
        else:
            devices_data[dev_id] = {
                "has_data": False,
                "device_id": dev_id,
                "temperature": None,
                "humidity": None,
                "water_level": None,
                "probe_25": False,
                "probe_50": False,
                "probe_75": False,
                "probe_100": False,
                "motor_status": "OFF",
                "motor_running": False,
                "servo_angle": 90,
                "timestamp": "Never",
                "iso_timestamp": None,
                "seconds_ago": None,
                "last_seen_text": "Never",
                "connection_status": "offline"
            }

    try:
        tank_readings = list(TelemetryReading.objects.order_by('-timestamp')[:200])
        fill_analytics = analyze_tank_timings(tank_readings, now=now)
    except Exception as e:
        print(f"Fill analytics calculation error: {e}")
        fill_analytics = analyze_tank_timings([], now=now)

    motor_info = get_esp32motor_status()

    try:
        latest_logs = list(DeviceLog.objects.order_by('-timestamp')[:15])
        latest_logs.reverse()
    except Exception:
        latest_logs = []

    logs_data = [
        {
            "device_id": log.device_id,
            "message": log.message,
            "timestamp": log.timestamp.strftime('%Y-%m-%d %H:%M:%S')
        }
        for log in latest_logs
    ]

    format_param = request.GET.get('format', '')
    accept_header = request.META.get('HTTP_ACCEPT', '')

    # Fetch latest water tank reading (specifically for water sensor boards, not motor controllers)
    try:
        tank_latest = TelemetryReading.objects.filter(water_level__isnull=False).order_by('-timestamp').first()
    except Exception:
        tank_latest = None

    active_tank_id = tank_latest.device_id if tank_latest else "esp8266_device_01"
    tank_device_data = devices_data.get(active_tank_id) or devices_data.get("esp8266_device_01")

    sync_interval_str = get_system_setting("sync_interval", "60")
    try:
        sync_interval = int(sync_interval_str)
    except ValueError:
        sync_interval = 60

    if 'text/html' in accept_header and format_param != 'json':
        context = {
            "devices": devices_data,
            "device_01": tank_device_data,
            "fill_analytics": fill_analytics,
            "motor_info": motor_info,
            "sync_interval": sync_interval,
            "has_data": tank_latest is not None,
            "logs": latest_logs
        }
        return render(request, 'api/status.html', context)

    return JsonResponse({
        "status": "success",
        "devices": devices_data,
        "fill_analytics": fill_analytics,
        "motor_info": motor_info,
        "sync_interval": sync_interval,
        "logs": logs_data,
        "device_id": active_tank_id,
        "water_level": tank_latest.water_level if tank_latest else None,
        "temperature": tank_latest.temperature if tank_latest else None,
        "humidity": tank_latest.humidity if tank_latest else None,
        "timestamp": tank_latest.timestamp.strftime('%Y-%m-%d %H:%M:%S') if tank_latest else None,
    })

