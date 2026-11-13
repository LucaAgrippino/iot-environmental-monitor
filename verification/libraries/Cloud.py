"""Robot library: an MQTT subscriber on the bench broker (mutual TLS, operator certificate) that records every message."""
import json
import os
import ssl
import time

import paho.mqtt.client as mqtt

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
# VERIFY_CERTS: a directory with ca.crt, operator.crt, operator.key (the Raspberry Pi keeps them outside the repository)
CERTS = os.environ.get("VERIFY_CERTS", os.path.join(ROOT, "local-test/mosquitto-broker"))


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
        c.connect(os.environ.get("VERIFY_BROKER_HOST", "127.0.0.1"), 8883)
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

    def broker_refuses_a_client_without_certificate(self, host="127.0.0.1"):
        """The broker at `host` listens and refuses a TLS client that presents no certificate (mutual authentication).
        The TCP connection must succeed first, so a dead broker cannot pass for a refusal. Returns True when refused."""
        import socket
        socket.create_connection((host, 8883), 5).close()
        c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="verification-nocert-host")
        c.tls_set(os.path.join(CERTS, "ca.crt"), tls_version=ssl.PROTOCOL_TLSv1_2)
        c.tls_insecure_set(True)
        try:
            c.connect(host, 8883)
            c.loop_start()
            time.sleep(3)
            ok = c.is_connected()
            c.loop_stop()
            c.disconnect()
            return not ok
        except (ssl.SSLError, ConnectionError, OSError):
            return True

    # ---- commands to the device (cloud to device) -------------------------------------------
    def device_id(self):
        """The Gateway's device id, taken from the first telemetry message collected."""
        for m in self.messages_on("telemetry"):
            return self.json_of(m)["device_id"]
        raise AssertionError("no telemetry collected yet: the device id is unknown")

    def publish_command(self, kind, body):
        """Publish a command (kind: config, control) to cmd/iotmonitor/<device>/<kind> at QoS 1; `body` is a JSON text.
        Returns the monotonic count of messages seen before the send, to pass to `wait_for_result`."""
        topic = "cmd/iotmonitor/%s/%s" % (self.device_id(), kind)
        n0 = len(self.messages)
        info = self.client.publish(topic, body, qos=1)
        info.wait_for_publish(10)
        return n0

    def wait_for_result(self, kind, since_count, timeout=10.0):
        """Wait for a message on .../<kind>/result recorded after `since_count`; returns it parsed. kind: config, control."""
        end = time.monotonic() + float(timeout)
        while time.monotonic() < end:
            for (t, tp, q, p) in self.messages[int(since_count):]:
                if tp.endswith("/%s/result" % kind):
                    return json.loads(p)
            time.sleep(0.1)
        raise AssertionError("no %s result within %s s" % (kind, timeout))

    def time_to_result(self, kind, body, timeout=10.0):
        """Send a command and return (result dict, seconds from the send to the result arriving)."""
        t0 = time.monotonic()
        n0 = self.publish_command(kind, body)
        res = self.wait_for_result(kind, n0, timeout)
        for (t, tp, q, p) in self.messages[int(n0):]:
            if tp.endswith("/%s/result" % kind):
                return res, t - t0
        return res, time.monotonic() - t0
