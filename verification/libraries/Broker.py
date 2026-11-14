"""Robot library: the bench MQTT broker (Docker Mosquitto with mutual TLS) and its log."""
import os
import socket
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
        """Start the broker detached (a no-op when it already runs).
        VERIFY_BROKER=system: the broker is the system Mosquitto service (the Raspberry Pi); it is only checked."""
        if os.environ.get("VERIFY_BROKER") == "system":
            if subprocess.run(["systemctl", "is-active", "--quiet", "mosquitto"]).returncode != 0:
                subprocess.run(["sudo", "-n", "systemctl", "start", "mosquitto"], check=True)
                time.sleep(2)
            return
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

    def plain_listener_records(self, port, seconds):
        """Listen on a TCP port for `seconds` instead of the broker, accept every connection and note the first bytes the client
        sends (as hex). Each client is answered with a plain MQTT CONNACK (20 02 00 00), which a client that insists on TLS must
        not take for a connection. Returns the list of hex strings (empty when nobody connected)."""
        import time
        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind(("0.0.0.0", int(port)))
        srv.listen(5)
        srv.settimeout(1.0)
        firsts = []
        end = time.time() + float(seconds)
        try:
            while time.time() < end:
                try:
                    conn, _ = srv.accept()
                except socket.timeout:
                    continue
                conn.settimeout(3.0)
                try:
                    data = conn.recv(16)
                    firsts.append(data.hex())
                    conn.sendall(bytes([0x20, 0x02, 0x00, 0x00]))
                    time.sleep(0.3)
                except (socket.timeout, OSError):
                    firsts.append("")
                finally:
                    conn.close()
        finally:
            srv.close()
        return firsts

    def plain_listener_attempt_times(self, port, seconds, ignore_addresses="127.0.0.1"):
        """Like plain_listener_records, but returns the time (seconds since the epoch) at which each connection attempt arrived,
        leaving out the clients whose address is in the comma-separated `ignore_addresses` (the collector and the cloud service run
        on the same machine as the broker and reconnect to it too)."""
        import time
        ignore = {a.strip() for a in str(ignore_addresses).split(",") if a.strip()}
        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind(("0.0.0.0", int(port)))
        srv.listen(5)
        srv.settimeout(0.5)
        times = []
        end = time.time() + float(seconds)
        try:
            while time.time() < end:
                try:
                    conn, peer = srv.accept()
                except socket.timeout:
                    continue
                if peer[0] not in ignore:
                    times.append(time.time())
                conn.close()
        finally:
            srv.close()
        return times
