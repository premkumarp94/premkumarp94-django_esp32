from django.http import JsonResponse
from django.shortcuts import render
from api.models import TelemetryReading, DeviceLog

def status(request):
    # Fetch distinct device IDs from DB
    device_ids = list(TelemetryReading.objects.values_list('device_id', flat=True).distinct())
    
    # Ensure standard simulator devices exist in dictionary
    default_devices = ["esp32_device_01", "esp32_gear_motor_02"]
    for dev in default_devices:
        if dev not in device_ids:
            device_ids.append(dev)
            
    devices_data = {}
    for dev_id in device_ids:
        reading = TelemetryReading.objects.filter(device_id=dev_id).order_by('-timestamp').first()
        if reading:
            devices_data[dev_id] = {
                "has_data": True,
                "device_id": reading.device_id,
                "temperature": reading.temperature,
                "humidity": reading.humidity,
                "water_level": reading.water_level,
                "motor_status": reading.motor_status,
                "timestamp": reading.timestamp.strftime('%Y-%m-%d %H:%M:%S')
            }
        else:
            devices_data[dev_id] = {
                "has_data": False,
                "device_id": dev_id,
                "temperature": None,
                "humidity": None,
                "water_level": None,
                "motor_status": "stopped",
                "timestamp": "Never"
            }

    # Fetch recent logs across devices
    latest_logs = list(DeviceLog.objects.order_by('-timestamp')[:15])
    latest_logs.reverse()
    
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

    overall_latest = TelemetryReading.objects.order_by('-timestamp').first()

    if 'text/html' in accept_header and format_param != 'json':
        context = {
            "devices": devices_data,
            "device_01": devices_data.get("esp32_device_01"),
            "device_02": devices_data.get("esp32_gear_motor_02"),
            "has_data": overall_latest is not None,
            "logs": latest_logs
        }
        return render(request, 'api/status.html', context)

    # Return JSON response for AJAX requests
    return JsonResponse({
        "status": "success",
        "devices": devices_data,
        "logs": logs_data,
        "device_id": overall_latest.device_id if overall_latest else "esp32_device_01",
        "water_level": overall_latest.water_level if overall_latest else None,
        "temperature": overall_latest.temperature if overall_latest else None,
        "humidity": overall_latest.humidity if overall_latest else None,
        "motor_status": overall_latest.motor_status if overall_latest else "stopped",
        "timestamp": overall_latest.timestamp.strftime('%Y-%m-%d %H:%M:%S') if overall_latest else None,
    })
