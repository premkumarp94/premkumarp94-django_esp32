import datetime
from django.utils import timezone
from django.http import JsonResponse
from django.shortcuts import render
from api.models import TelemetryReading, DeviceLog

def status(request):
    try:
        # Fetch device IDs from top 50 recent readings
        recent_devs = TelemetryReading.objects.order_by('-timestamp')[:50].values_list('device_id', flat=True)
        device_ids = list(dict.fromkeys(recent_devs))
    except Exception:
        device_ids = []
    
    # Ensure default tank monitor device is present
    default_devices = ["esp8266_device_01"]
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

            # Connection health
            if seconds_ago <= 15:
                connection_status = "online"
            elif seconds_ago <= 60:
                connection_status = "idle"
            else:
                connection_status = "offline"

            devices_data[dev_id] = {
                "has_data": True,
                "device_id": reading.device_id,
                "temperature": reading.temperature,
                "humidity": reading.humidity,
                "water_level": reading.water_level,
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
                "timestamp": "Never",
                "iso_timestamp": None,
                "seconds_ago": None,
                "last_seen_text": "Never",
                "connection_status": "offline"
            }

    # Fetch recent logs across devices
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

    try:
        overall_latest = TelemetryReading.objects.order_by('-timestamp').first()
    except Exception:
        overall_latest = None

    if 'text/html' in accept_header and format_param != 'json':
        context = {
            "devices": devices_data,
            "device_01": devices_data.get("esp8266_device_01"),
            "has_data": overall_latest is not None,
            "logs": latest_logs
        }
        return render(request, 'api/status.html', context)

    # Return JSON response for AJAX requests
    return JsonResponse({
        "status": "success",
        "devices": devices_data,
        "logs": logs_data,
        "device_id": overall_latest.device_id if overall_latest else "esp8266_device_01",
        "water_level": overall_latest.water_level if overall_latest else None,
        "temperature": overall_latest.temperature if overall_latest else None,
        "humidity": overall_latest.humidity if overall_latest else None,
        "timestamp": overall_latest.timestamp.strftime('%Y-%m-%d %H:%M:%S') if overall_latest else None,
    })
