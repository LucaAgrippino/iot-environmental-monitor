"""Robot library: an MQTT subscriber on the bench broker (mutual TLS, operator certificate) that records every message."""
import json
import os
import ssl
import time

import paho.mqtt.client as mqtt

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
CERTS = os.path.join(ROOT, "local-test/mosquitto-broker")


class Cloud:
    ROBOT_LIBRARY_SCOPE = "SUITE"

    def __init__(self):
        self.client = None
        self.messages = []   # (monotonic time, topic, qos, payload bytes)
        self.t0 = 0.0

    def start_collector(self):
        """Subscribe to everything at QoS 1, so the delivered QoS shows the QoS the device published with."""
        self.stop_collector()
        self.messages = []
        c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="verification-collector")
        c.tls_set(os.path.join(CERTS, "ca.crt"), os.path.join(CERTS, "operator.crt"), os.path.join(CERTS, "operator.key"),
                  tls_version=ssl.PROTOCOL_TLSv1_2)
        c.tls_insecure_set(True)
        c.on_message = lambda cl, u, m: self.messages.append((time.monotonic(), m.topic, m.qos, bytes(m.payload)))
        c.connect("127.0.0.1", 8883)
        c.subscribe("#", 1)
        c.loop_start()
        self.client = c
        self.t0 = time.monotonic()

    def stop_collector(self):
        if self.client is not None:
            self.client.loop_stop()
            self.client.disconnect()
            self.client = None

    def seconds_collected(self):
        return time.monotonic() - self.t0

    def wait_until_collected(self, seconds):
        """Sleep until the collector has run for `seconds` in total."""
        left = float(seconds) - self.seconds_collected()
        if left > 0:
            time.sleep(left)

    def messages_on(self, kind, since=0.0):
        """Messages whose topic ends with /kind, as dicts {t, topic, qos, payload (bytes)}, oldest first."""
        return [dict(t=t - self.t0, topic=tp, qos=q, payload=p) for (t, tp, q, p) in self.messages
                if tp.endswith("/" + kind) and t - self.t0 >= float(since)]

    def topic_kinds(self):
        return sorted({tp.rsplit("/", 1)[-1] for (_, tp, _, _) in self.messages if tp.startswith("dt/")})

    def json_of(self, message):
        """Parse a payload; raises AssertionError when it is not JSON."""
        try:
            return json.loads(message["payload"])
        except ValueError as e:
            raise AssertionError("payload on %s is not JSON: %s" % (message["topic"], e))

    def intervals(self, kind):
        ts = [m["t"] for m in self.messages_on(kind)]
        return [round(b - a, 1) for a, b in zip(ts, ts[1:])]

    def connection_without_certificate_is_refused(self):
        """A client that presents no certificate must not be accepted by the broker."""
        c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="verification-nocert")
        c.tls_set(os.path.join(CERTS, "ca.crt"), tls_version=ssl.PROTOCOL_TLSv1_2)
        c.tls_insecure_set(True)
        try:
            c.connect("127.0.0.1", 8883)
            c.loop_start()
            time.sleep(3)
            ok = c.is_connected()
            c.loop_stop()
            c.disconnect()
            return not ok
        except (ssl.SSLError, ConnectionError, OSError):
            return True
