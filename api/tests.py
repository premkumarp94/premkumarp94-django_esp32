import json
from django.test import TestCase, Client
from api.models import TelemetryReading, DeviceCommand, DeviceLog, SystemSetting, get_system_setting, set_system_setting


class TelemetryAndMotorTestCase(TestCase):
    def setUp(self):
        self.client = Client()

    def test_sync_interval_persistence_and_response(self):
        # 1. Default interval should be 60
        self.assertEqual(get_system_setting("sync_interval", "60"), "60")

        # 2. Update interval via command endpoint
        resp = self.client.post(
            '/api/command/',
            data=json.dumps({"device_id": "esp8266_device_01", "command": "INTERVAL:30"}),
            content_type='application/json'
        )
        self.assertEqual(resp.status_code, 200)
        self.assertEqual(get_system_setting("sync_interval"), "30")

        # 3. Telemetry endpoint should return updated sync interval
        telemetry_resp = self.client.post(
            '/api/telemetry/',
            data=json.dumps({"id": "esp8266_device_01", "sensor values": {"water_level": 50}}),
            content_type='application/json'
        )
        self.assertEqual(telemetry_resp.status_code, 200)
        data = telemetry_resp.json()
        self.assertEqual(data.get("interval_seconds"), 30)
        self.assertEqual(data.get("sync_interval"), 30)

    def test_auto_shutoff_at_100_percent_water_level(self):
        # 1. Report motor is running from esp32motor
        self.client.post(
            '/api/telemetry/',
            data=json.dumps({"id": "esp32motor", "motor_status": "ON", "motor_running": True, "servo_angle": 45}),
            content_type='application/json'
        )

        # 2. ESP8266 reports 100% water level
        resp_tank = self.client.post(
            '/api/telemetry/',
            data=json.dumps({"id": "esp8266_device_01", "sensor values": {"water_level": 100, "probe_100": True}}),
            content_type='application/json'
        )
        self.assertEqual(resp_tank.status_code, 200)

        # Check that MOTOR_OFF command was queued in DB
        pending_off = DeviceCommand.objects.filter(device_id="esp32motor", command="MOTOR_OFF").exists()
        self.assertTrue(pending_off)

        # 3. Next telemetry request from esp32motor should deliver MOTOR_OFF command
        resp_motor = self.client.post(
            '/api/telemetry/',
            data=json.dumps({"id": "esp32motor", "motor_status": "ON", "motor_running": True}),
            content_type='application/json'
        )
        self.assertEqual(resp_motor.status_code, 200)
        data_motor = resp_motor.json()
        self.assertEqual(data_motor.get("command"), "MOTOR_OFF")

    def test_hold_sec_and_angles_persistence(self):
        # 1. Post hold duration command (e.g. 2.5 seconds)
        resp = self.client.post(
            '/api/command/',
            data=json.dumps({"device_id": "esp32motor", "command": "SET_HOLD_SEC:2.5"}),
            content_type='application/json'
        )
        self.assertEqual(resp.status_code, 200)
        self.assertEqual(get_system_setting("motor_hold_ms"), "2500")

        # 2. Status API should return hold_sec == 2.5
        status_resp = self.client.get('/api/status/?format=json')
        self.assertEqual(status_resp.status_code, 200)
        status_data = status_resp.json()
        self.assertEqual(status_data["motor_info"]["hold_sec"], 2.5)

        # 3. Telemetry from esp32motor with hold_ms=3000 should update system setting
        self.client.post(
            '/api/telemetry/',
            data=json.dumps({"id": "esp32motor", "hold_ms": 3000, "angle_on": 40, "angle_off": 140}),
            content_type='application/json'
        )
        self.assertEqual(get_system_setting("motor_hold_ms"), "3000")
        self.assertEqual(get_system_setting("motor_angle_on"), "40")
        self.assertEqual(get_system_setting("motor_angle_off"), "140")

    def test_server_calculated_sync_probe_and_online(self):
        # 1. Post telemetry with water level probe reading
        self.client.post(
            '/api/telemetry/',
            data=json.dumps({
                "id": "esp8266_device_01",
                "sensor values": {
                    "water_level": 75,
                    "probe_75": True
                }
            }),
            content_type='application/json'
        )

        status_resp = self.client.get('/api/status/?format=json')
        dev_data = status_resp.json()["devices"]["esp8266_device_01"]
        self.assertEqual(dev_data["probe_read_text"], "Just now")
        self.assertEqual(dev_data["last_seen_text"], "Just now")

        # 2. Post 10s routine ping WITHOUT probe values
        self.client.post(
            '/api/telemetry/',
            data=json.dumps({
                "id": "esp8266_device_01",
                "message": "none"
            }),
            content_type='application/json'
        )

        status_resp2 = self.client.get('/api/status/?format=json')
        dev_data2 = status_resp2.json()["devices"]["esp8266_device_01"]
        self.assertEqual(dev_data2["last_seen_text"], "Just now")
        self.assertEqual(dev_data2["water_level"], 75.0)
        self.assertIsNotNone(dev_data2["probe_read_ago_sec"])

    def test_max_run_min_setting_and_auto_shutoff(self):
        # 1. Default max run min should be 30
        self.assertEqual(get_system_setting("motor_max_run_min", "30"), "30")

        # 2. Update max_run_min to 15 minutes via command
        resp = self.client.post(
            '/api/command/',
            data=json.dumps({"device_id": "esp32motor", "command": "SET_MAX_RUN_MIN:15"}),
            content_type='application/json'
        )
        self.assertEqual(resp.status_code, 200)
        self.assertEqual(get_system_setting("motor_max_run_min"), "15")

        status_resp = self.client.get('/api/status/?format=json')
        self.assertEqual(status_resp.json()["motor_info"]["max_run_min"], 15)

    def test_last_motor_off_reason_persistence(self):
        # 1. Post MOTOR_OFF command
        self.client.post(
            '/api/command/',
            data=json.dumps({"device_id": "esp32motor", "command": "MOTOR_OFF"}),
            content_type='application/json'
        )
        self.assertIn("Turn OFF requested", get_system_setting("last_motor_off_reason"))

        # 2. Telemetry with explicit auto-shutoff message from esp32motor
        self.client.post(
            '/api/telemetry/',
            data=json.dumps({
                "id": "esp32motor",
                "motor_running": False,
                "message": "[AUTO-SHUTOFF] Reached maximum run time limit (30 min)"
            }),
            content_type='application/json'
        )
        self.assertIn("maximum run time limit", get_system_setting("last_motor_off_reason"))

        status_resp = self.client.get('/api/status/?format=json')
        self.assertIn("maximum run time limit", status_resp.json()["motor_info"]["last_off_reason"])




