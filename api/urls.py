from django.urls import path
from .views.status import status
from .views.telemetry import telemetry
from .views.command import send_command

urlpatterns = [
    path("status/", status),
    path("telemetry/", telemetry),
    path("command/", send_command),
]