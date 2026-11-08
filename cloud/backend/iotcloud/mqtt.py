"""The MQTT connection of the cloud side: subscribes to everything the devices send and hands it to ingest."""
import logging
import ssl

import paho.mqtt.client as mqtt

from . import ingest, ota
from .config import Settings

log = logging.getLogger("iotcloud.mqtt")


def make_client(settings: Settings, db, hub, notifier=None, client_id: str = "iotcloud") -> mqtt.Client:
    # a persistent session: what the devices send while this process restarts waits in the broker (QoS 1)
    c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=client_id, clean_session=False)
    if settings.mqtt_cafile:
        c.tls_set(ca_certs=settings.mqtt_cafile, certfile=settings.mqtt_cert or None, keyfile=settings.mqtt_key or None,
                  cert_reqs=ssl.CERT_REQUIRED)

    def on_connect(client, userdata, flags, reason, properties=None):
        log.info("connected to the broker: %s", reason)
        # QoS 1 for everything: alarms and results must not be lost while this process restarts
        client.subscribe([(f"dt/{settings.topic_root}/+/{s}", 1) for s in ("telemetry", "health", "alarms", "ota/result",
                                                                          "ota/request", "config/result", "control/result")])

    def on_message(client, userdata, msg):
        try:
            event = ingest.handle(db, settings.topic_root, msg.topic, msg.payload)
            if event and event["kind"] == "ota_request":
                job = ota.on_request(db, client.publish, settings.image_dir, settings.topic_root, event["device"], msg.payload)
                event = {**event, "job": job["id"], "sent_bytes": job["sent_bytes"], "size": job["size_b"]} if job else None
            if event:
                hub.publish(event)
                if notifier:
                    notifier.submit(event)
        except Exception:  # one bad message must not stop the stream
            log.exception("could not store %s", msg.topic)

    c.on_connect = on_connect
    c.on_message = on_message
    c.reconnect_delay_set(min_delay=1, max_delay=30)
    return c


def start(settings: Settings, db, hub, notifier=None) -> mqtt.Client:
    c = make_client(settings, db, hub, notifier)
    c.connect_async(settings.mqtt_host, settings.mqtt_port, keepalive=30)
    c.loop_start()
    return c
