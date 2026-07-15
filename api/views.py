from django.http import JsonResponse

def status(request):
    return JsonResponse({
        "message": "Hello ESP32!"
    })