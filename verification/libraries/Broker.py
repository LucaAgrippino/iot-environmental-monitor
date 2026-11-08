"""Robot library: the bench MQTT broker (Docker Mosquitto with mutual TLS) and its log."""
import os
import subprocess
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
NAME = "bringup-mosquitto"
BROKER_DIR = os.path.join(ROOT, "local-test/mosquitto-broker")


def _docker(*args, timeout=60):
    return subprocess.run(["docker", *args], capture_output=True, text=True, timeout=timeout)


class Broker:
    ROBOT_LIBRARY_SCOPE = "SUITE"

    def start_broker(self):
        """Start the broker detached (a no-op when it already runs)."""
        if NAME in _docker("ps", "--format", "{{.Names}}").stdout.split():
            return
        r = _docker("run", "-d", "--rm", "--name", NAME, "-p", "8883:8883", "-v", "%s:/mosquitto/certs:ro" % BROKER_DIR,
                    "-v", "%s/mosquitto_tls.docker.conf:/mosquitto/config/mosquitto.conf:ro" % BROKER_DIR,
                    "eclipse-mosquitto:2", "mosquitto", "-c", "/mosquitto/config/mosquitto.conf", "-v")
        if r.returncode != 0:
            raise AssertionError("broker did not start: " + r.stderr)
        time.sleep(2)

    def stop_broker(self):
        _docker("stop", NAME, timeout=60)

    def broker_publishes_in_last(self, seconds, topic_part=""):
        """Number of PUBLISH lines of the broker log in the last `seconds` (optionally only a topic)."""
        r = _docker("logs", "--since", "%ds" % int(float(seconds)), NAME)
        out = r.stdout + r.stderr  # Mosquitto logs to stderr, which docker replays on stderr
        return sum(1 for l in out.split("\n") if "Received PUBLISH" in l and topic_part in l)

    def wait_for_broker_publish(self, timeout, topic_part=""):
        """Wait until a PUBLISH of the topic appears; returns the seconds waited."""
        start = time.time()
        while time.time() - start < float(timeout):
            if self.broker_publishes_in_last(int(time.time() - start) + 3, topic_part) > 0:
                return time.time() - start
            time.sleep(2)
        raise AssertionError("no PUBLISH %r within %s s" % (topic_part, timeout))
