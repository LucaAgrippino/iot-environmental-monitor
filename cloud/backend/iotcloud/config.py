"""Settings, from environment variables (defaults suit a local run)."""
import os
from dataclasses import dataclass


@dataclass(frozen=True)
class Settings:
    db_path: str = os.environ.get("IOTCLOUD_DB", "iotcloud.sqlite3")
    mqtt_host: str = os.environ.get("IOTCLOUD_MQTT_HOST", "127.0.0.1")
    mqtt_port: int = int(os.environ.get("IOTCLOUD_MQTT_PORT", "1883"))
    # mutual TLS to the broker (the Gateway's own scheme); all three or none
    mqtt_cafile: str = os.environ.get("IOTCLOUD_MQTT_CA", "")
    mqtt_cert: str = os.environ.get("IOTCLOUD_MQTT_CERT", "")
    mqtt_key: str = os.environ.get("IOTCLOUD_MQTT_KEY", "")
    topic_root: str = os.environ.get("IOTCLOUD_TOPIC_ROOT", "iotmonitor")
    # a device counts as online while its last message is younger than this (telemetry is every 60 s)
    online_window_s: int = int(os.environ.get("IOTCLOUD_ONLINE_WINDOW_S", "150"))
    # firmware images (the S3 bucket of this system) and how long a command may stay unanswered
    # login: on unless told otherwise (run-local.sh turns it off for the localhost-only dev stack).
    # cookie_secure must be true behind HTTPS (the deployment sets it): the browser then never sends it over plain http
    auth_required: bool = os.environ.get("IOTCLOUD_AUTH", "on").lower() not in ("off", "0", "false")
    cookie_secure: bool = os.environ.get("IOTCLOUD_COOKIE_SECURE", "off").lower() in ("on", "1", "true")
    # the built web app (cloud/web/build.sh); served at / when the folder exists
    web_dir: str = os.environ.get("IOTCLOUD_WEB_DIR", os.path.join(os.path.dirname(__file__), "..", "..", "web", "dist"))
    image_dir: str = os.environ.get("IOTCLOUD_IMAGE_DIR", "images")
    command_timeout_s: int = int(os.environ.get("IOTCLOUD_COMMAND_TIMEOUT_S", "60"))


def load() -> Settings:
    return Settings()
