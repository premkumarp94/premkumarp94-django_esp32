from django.db import models

class TelemetryReading(models.Model):
    device_id = models.CharField(max_length=100, db_index=True)
    temperature = models.FloatField(null=True, blank=True)
    humidity = models.FloatField(null=True, blank=True)
    water_level = models.FloatField(null=True, blank=True)
    motor_status = models.CharField(max_length=20, default="stopped")
    timestamp = models.DateTimeField(auto_now_add=True, db_index=True)

    class Meta:
        ordering = ['-timestamp']
        indexes = [
            models.Index(fields=['device_id', '-timestamp']),
            models.Index(fields=['-timestamp']),
        ]

    def __str__(self):
        return f"{self.device_id} - Water: {self.water_level}%, Temp: {self.temperature}°C, Motor: {self.motor_status} at {self.timestamp}"


class DeviceCommand(models.Model):
    device_id = models.CharField(max_length=100, db_index=True)
    command = models.CharField(max_length=100)  # e.g., "start_motor", "stop_motor"
    timestamp = models.DateTimeField(auto_now_add=True, db_index=True)
    is_executed = models.BooleanField(default=False, db_index=True)

    class Meta:
        ordering = ['timestamp']
        indexes = [
            models.Index(fields=['device_id', 'is_executed']),
        ]

    def __str__(self):
        return f"{self.device_id}: {self.command} (Executed: {self.is_executed})"


class DeviceLog(models.Model):
    device_id = models.CharField(max_length=100, db_index=True)
    message = models.TextField()
    timestamp = models.DateTimeField(auto_now_add=True, db_index=True)

    class Meta:
        ordering = ['-timestamp']
        indexes = [
            models.Index(fields=['-timestamp']),
        ]

    def __str__(self):
        return f"{self.device_id} - {self.message} at {self.timestamp}"



