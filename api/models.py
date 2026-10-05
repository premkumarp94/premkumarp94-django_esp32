from django.db import models

class TelemetryReading(models.Model):
    device_id = models.CharField(max_length=100, db_index=True)
    temperature = models.FloatField(null=True, blank=True)
    humidity = models.FloatField(null=True, blank=True)
    water_level = models.FloatField(null=True, blank=True)
    probe_25 = models.BooleanField(null=True, blank=True)
    probe_50 = models.BooleanField(null=True, blank=True)
    probe_75 = models.BooleanField(null=True, blank=True)
    probe_100 = models.BooleanField(null=True, blank=True)
    motor_status = models.CharField(max_length=20, null=True, blank=True)
    motor_running = models.BooleanField(null=True, blank=True)
    servo_angle = models.IntegerField(null=True, blank=True)
    probe_read_ago_sec = models.IntegerField(null=True, blank=True)
    timestamp = models.DateTimeField(auto_now_add=True, db_index=True)

    class Meta:
        ordering = ['-timestamp']
        indexes = [
            models.Index(fields=['device_id', '-timestamp']),
            models.Index(fields=['-timestamp']),
        ]

    def __str__(self):
        return f"{self.device_id} - Water: {self.water_level}%, Temp: {self.temperature}°C at {self.timestamp}"


class DeviceCommand(models.Model):
    device_id = models.CharField(max_length=100, db_index=True)
    command = models.CharField(max_length=100)
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


class SystemSetting(models.Model):
    key = models.CharField(max_length=50, unique=True, db_index=True)
    value = models.CharField(max_length=255)
    updated_at = models.DateTimeField(auto_now=True)

    class Meta:
        ordering = ['key']

    def __str__(self):
        return f"{self.key} = {self.value}"


def get_system_setting(key, default="60"):
    try:
        setting = SystemSetting.objects.filter(key=key).first()
        return setting.value if setting else default
    except Exception:
        return default


def set_system_setting(key, value):
    try:
        SystemSetting.objects.update_or_create(key=key, defaults={"value": str(value)})
    except Exception as e:
        print(f"Error saving system setting {key}: {e}")

