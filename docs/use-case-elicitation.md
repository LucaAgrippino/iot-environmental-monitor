# Use Case Elicitation — Working Document

---

## fn 1.1

The field device reads on-board sensors (temperature, humidity, pressure) at a configurable polling interval.
Sensor readings are processed and made available via Modbus registers for the gateway to read, and displayed on the local LCD.

**Who does what?**
the system does this: reads on-board sensors (temperature, humidity, pressure) at a configurable polling interval. It also processes sensor readings and make them available for the serial communication, for the gateway to read, and for displaying them in a proper manner.
the remote operator does this:
the field technician does this:

        use cases:
                    acquire sensor data
        actors:
                    Field Technician
                    Aws IoT
                    the system

---

## fn 1.2

The gateway device reads its own on-board sensors and also polls the field device. All sensor data is aggregated for cloud publishing.

**Who does what?**
the system does this: The gateway device reads its own on-board sensors and also polls the field device. And it aggregate all sensor data for cloud publishing.
the remote operator does this:
the field technician does this:

        use cases:
                    acquire data
        actors:
                    Aws IoT
                    the system

---

## fn 1.3

All sensor readings are timestamped using the local RTC.

**Who does what?**
the system does this: using local RTC to timestamp all sensor reading.
the remote operator does this:
the field technician does this:

        use cases:
        actors:

---

## fn 2.1

The system supports configurable alarm thresholds per sensor.
When a sensor value exceeds a threshold, an alarm is raised.
Alarms are:
- Displayed on the field device LCD (visual indicator, alarm details).
- Published to the cloud as alarm events with timestamps.
- Cleared automatically when the sensor value returns to the normal range (with hysteresis to prevent flapping).

Alarm thresholds are configurable both locally (LCD) and remotely (cloud command).

**Who does what?**
the system does this: supports configurable alarm thresholds per sensor. This configuration is done both locally (LCD) and remotely (cloud command). It check when a sensor value exceeds a threshold and raise an alarm with the current timestamp. This alarm event is consumed by both the device LCD and the cloud. When the sensor data return to the normal range, it clear automatically the alarm associated with this sensor.
the remote operator does this:
the field technician does this: see the sensor alarm on the LCD. Configure the sensor alarm thresholds.

        use cases:
                    configure operational parameters
        actors:
                    Field Technician
                    Aws IoT
                    the system

---

## fn 3.1

The gateway (master) polls the field device (slave) at a configurable interval.
The Modbus register map exposes:
- Sensor readings (input registers, read-only).
- Device status and alarm states (input registers, read-only).
- Configuration parameters (holding registers, read-write).
- Timestamp synchronisation (holding register, written by master to slave).

The register map design supports multiple field devices by Modbus address, even though only one is implemented.

**Who does what?**
the system does this: polls the field device (slave) at a configurable interval.
the remote operator does this:
the field technician does this:

        use cases:
        actors:

---

## fn 4.1

The gateway publishes telemetry and alarm data to AWS IoT Core via MQTT over TLS.
TLS is mandatory — all data in transit between the gateway and the cloud is encrypted.
The gateway authenticates to AWS IoT Core using X.509 client certificates.
Message payloads use JSON format, structured with versioned message schemas. The message abstraction layer in firmware is designed so the serialization format could be changed (e.g., to CBOR or Protocol Buffers) without affecting the layers above it.
The gateway subscribes to command topics to receive configuration changes and commands from the Remote Operator.

**Who does what?**
the system does this: The gateway publishes telemetry and alarm data to AWS IoT Core. The gateway subscribes to command topics to receive configuration changes and commands from the Remote Operator.
the remote operator does this: send remote command, send configuration.
the field technician does this:

        use cases:
                    publish data to cloud
                    receive data from cloud
        actors:
                    Aws IoT
                    Remote Operator
                    the system

---

## fn 5.5

The field device LCD provides a multi-screen monitoring interface:
- Sensor screen: Live sensor readings with units and timestamps.
- Alarm screen: Active alarms with severity and trigger time.
- Status screen: Device state, Modbus connection status, system health summary.
- Configuration screen (Tier 2): Basic operational settings (polling rate, alarm thresholds, display settings).

Navigation between screens uses the touchscreen. The LCD is a monitoring and basic configuration interface — it is not a full menu system.

**Who does what?**
the system does this: receive data from the local interface, update the local interface.
the remote operator does this:
the field technician does this: send data from the local interface, read data from the local interface.

        use cases:
                    view sensor reading
                    view active alarm
                    view system status
                    configure operational parameters
        actors:
                    Field Technician
                    the system

---

## fn 5.6

Both boards provide a serial CLI console over a dedicated UART. The console serves two purposes:
- Provisioning (Tier 1): Initial device setup — WiFi credentials, cloud endpoint and certificates, Modbus address, serial port parameters. These settings are stored in non-volatile flash and persist across reboots.
- Diagnostics: Runtime inspection — view logs, check connectivity, inspect Modbus frame statistics, view FreeRTOS task status and stack usage, view system health metrics and error counters.

The CLI is a system feature, not a development-only tool. It is designed to be used by a field technician during installation and troubleshooting.

**Who does what?**
the system does this: provide a serial CLI console for provisioning and diagnostics.
the remote operator does this:
the field technician does this: during installation and troubleshooting.

        use cases:
                    provisioning device
                    view device diagnostics
        actors:
                    Field Technician
                    the system

---

## fn 5.7

The Remote Operator can send commands to the system via the cloud:
- Change polling rate (Tier 2 — operational, safe to change remotely).
- Change alarm thresholds (Tier 2 — operational).
- Request immediate sensor reading (Tier 3 — command, executed with acknowledgement).
- Restart device (Tier 3 — command, potentially disruptive, requires confirmation mechanism).

Commands are received by the gateway via MQTT subscription. Commands targeting the field device are forwarded via Modbus register writes.

**Who does what?**
the system does this: receive commands by the gateway. Commands targeting the field device are forwarded accordingly.
the remote operator does this: can send commands to the system via the cloud.
the field technician does this:

        use cases:
                    send remote commands Tier2
                    send remote commands Tier3
        actors:
                    Remote Operator
                    the system

---

## fn 5.8

The system publishes device health telemetry alongside sensor data, on a slower cadence (e.g., once every 5 minutes vs. sensor data at the configured polling interval). Health telemetry includes:
- Resource usage: FreeRTOS stack high watermarks per task, free heap (at boot and current), CPU load estimate.
- Connectivity metrics: WiFi RSSI (signal strength), WiFi reconnection count, MQTT publish failure count.
- Protocol reliability: Modbus CRC error count, Modbus timeout count, Modbus successful transaction count.
- System status: Uptime since last boot, MCU internal temperature, store-and-forward buffer occupancy (percentage full).

These metrics are:
- Published to the cloud as a separate "device health" message type.
- Available via the CLI console for local inspection by a field technician.
- Summarised on the LCD status screen.
- Error counters are cumulative since last boot and reset on restart.

**Who does what?**
the system does this: publishes device health telemetry alongside sensor data, on a slower cadence (e.g., once every 5 minutes vs. sensor data at the configured polling interval).
the remote operator does this: read the device health message.
the field technician does this: read the device health message.

        use cases:
                    monitor device healt
                    receive healt data
        actors:
                    Remote Operator
                    Aws IoT
                    the system

---

## fn 5.9

Both boards use external Quad-SPI flash for persistent data:
- Configuration persistence: All provisioning and operational settings survive power cycles and reboots.
- Store-and-forward buffer (gateway only): When WiFi connectivity is lost, the gateway buffers telemetry data in flash. When connectivity is restored, buffered data is published to the cloud in chronological order. If the buffer is full, the oldest data is overwritten (circular buffer strategy). Buffer sizing is determined in the HLD.

**Who does what?**
the system does this: both nodes use external Quad-SPI flash for configuration persistence. The gateway, when the WiFi connectivity is lost, buffer telemetry data in flash. When the connectivity is restored, buffered data is published to the cloud in chronological order.
the remote operator does this:
the field technician does this:

        use cases:
        actors:

---

## fn 5.10

The system includes a secure bootloader that verifies firmware integrity before execution. The bootloader:
- Validates the firmware image signature (cryptographic verification) before jumping to the application.
- Supports a dual-bank or A/B partition scheme to allow safe firmware rollback if an update fails.
- Provides an OTA (Over-The-Air) update mechanism: the gateway receives firmware images via the cloud, verifies them, and writes them to the inactive flash partition.

Note: This feature is fully designed in the HLD and LLD. Implementation is progressive — the bootloader and signature verification are implemented first; the OTA transport mechanism is implemented as time allows. Even a partial implementation with complete design documentation demonstrates the required competence.

**Who does what?**
the system does this: includes a secure bootloader that verifies firmware integrity before execution. Provides an OTA (Over-The-Air) update mechanism.
the remote operator does this:
the field technician does this:

        use cases:
                    check firmware integrity
                    OTA updates: gateway update, field device update; These are include of the OTA updates use case.
        actors:
                    Aws IoT
                    the system

---

## fn 8.1

The gateway synchronises its RTC via NTP over WiFi on boot and periodically thereafter.

**Who does what?**
the system does this: synchronises its time.
the remote operator does this:
the field technician does this:

        use cases:
                    synchronize time
        actors:
                    Aws IoT (should be the NTP?)
                    the system

---

## fn 8.2

The gateway writes the current timestamp to the field device via a Modbus holding register on initial connection and at regular intervals.

**Who does what?**
the system does this: The gateway synchronises the field device.
the remote operator does this:
the field technician does this:

        use cases:
        actors:

---

## fn 8.3

All sensor readings, alarms, and log entries are timestamped using the local RTC.

**Who does what?**
the system does this: timestamp all sensor reading, alarms and log entries.
the remote operator does this:
the field technician does this:

        use cases:
        actors:

---

## fn 8.4

If the RTC has not been synchronised (e.g., first boot with no connectivity), the system uses uptime-based timestamps and flags the data as "unsynchronised."

**Who does what?**
the system does this: recovery from missing remote time synchronization.
the remote operator does this:
the field technician does this:

        use cases:
        actors:


---

## uc Field Technician
A person physically present at the device installation site. They:
    View live sensor readings, system status, and active alarms on the field device LCD.
    Perform basic operational configuration via the LCD (polling rate, alarm thresholds).
    Commission and provision both devices via serial console (WiFi credentials, cloud endpoint, Modbus address, serial port parameters).
    Run diagnostic commands via the serial console (view logs, check connectivity, inspect Modbus statistics, view system health metrics).

**Who does what?**
the system does this:
the remote operator does this:
the field technician does this:

        use cases:
                view sensor reading
                view sysrtem status
                view active alarm
                configure operational parameters
                provision device
                view device diagnostics
        actors:
            the field technician
            the system

---

## uc Remote Operator
A person accessing the system remotely through a cloud dashboard. They:
    Monitor telemetry data (sensor readings, trends) through the cloud dashboard.
    Monitor device health and reliability metrics through the cloud dashboard.
    Receive alarm notifications when sensor values exceed configured thresholds.
    Send operational commands: change polling rate, set alarm thresholds, request an immediate sensor reading, restart a device.

**Who does what?**
the system does this:
the remote operator does this:
the field technician does this:

        use cases:
                view telemetry data
                monitor device health
                receive active alarm
                configure operational parameters
                request sensor reading
                request restart device
        actors:
            the remote operator
            the system
            the Aws Iot


---

## uc AWS IoT Core (System Actor)
The cloud platform that:
    Receives sensor telemetry and device health data via MQTT over TLS.
    Receives alarm events.
    Forwards configuration changes and commands from the Remote Operator to the gateway.

**Who does what?**
the system does this:
the remote operator does this:
the field technician does this:

        use cases:
                receive sensor telemetry
                receive device healt data
                receive alarm events
                foward remote commands
        actors:
            the remote operator
            the system
            the Aws Iot

---

## uc System

**Who does what?**
the system does this:
the remote operator does this:
the field technician does this:

        use cases:
                    acquire sensor data
                    synchronise time
                    verify firmware integrity
        actors:


---

# uc Field Technician
- view sensor reading
- view system status
- view active alarm
- configure operational parameters
- provision device
- view device diagnostics

# uc Remote Operator
- view telemetry data
- monitor device health
- receive alarm notification
- configure operational parameters
- request sensor reading
- request restart device
- deploy firmware update

# uc Aws Iot
- receive sensor telemetry
- receive device health data
- receive alarm events
- forward remote commands

# uc System
- acquire sensor data
- synchronise time
- verify firmware integrity
- evaluate sensor alarm