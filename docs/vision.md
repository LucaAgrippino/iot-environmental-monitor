# Project Vision — IoT Environmental Monitoring Gateway

**Version:** 1.4  
**Date:** April 2026  
**Status:** Approved (updated scope)

**Revision History:**

| Version | Date       | Changes                                                                 |
|---------|------------|-------------------------------------------------------------------------|
| 1.0     | April 2026 | Initial version from stakeholder elicitation                            |
| 1.1     | April 2026 | Added TLS, RTC/NTP, flash storage, observability, OTA/secure boot scope |
| 1.2     | April 2026 | Added portability requirement                                           |
| 1.3     | April 2026 | Added Signal conditioning requirement                   |
| 1.4     | April 2026 | Corrected field device sensor description: simulated sensors behind driver abstraction |

---

## 1. Purpose

This document captures the system concept, stakeholder needs, and scope boundaries for the IoT Environmental Monitoring Gateway project. It serves as the authoritative input for all downstream design documents (Use Case Diagram, SRS, HLD, LLD).

All design decisions must trace back to this document. If a requirement cannot be justified by a statement in this vision, it should be questioned.


Note: This project has no external customer. The problem statement and stakeholder needs were derived from the author's professional experience with industrial IoT systems. Domain knowledge substitutes for formal customer elicitation.

AI tools (Claude, Anthropic) were used as a mentoring and review aid throughout the project. All technical decisions and implementation are the author's own.

---

## 2. Problem Statement

Industrial facilities need to monitor environmental conditions (temperature, humidity, atmospheric pressure) at remote locations. Current pain points include:

- Sensor data is only available locally — no one sees it until a technician visits the site.
- When sensors fail or readings go out of range, there is no timely notification.
- There is no historical record of environmental data for trend analysis or compliance.
- Configuration changes require a physical visit to each device.
- There is no visibility into device health or reliability issues until a device fails completely.

---

## 3. System Concept

A two-node embedded system where a **field device** collects sensor data and communicates with a **gateway** via Modbus RTU. The gateway aggregates data and publishes it to the cloud via WiFi/MQTT over TLS.

### Node 1 — Field Device (Modbus RTU Slave)

- **Board:** STM32F469 Discovery (Cortex-M4, 4" touchscreen LCD)
- **Role:** Simulates an industrial field device (sensor node)
- **Sensors:** Simulated temperature, humidity, and pressure sensors (software simulation module behind the same driver interface used for real hardware sensors)
- **Local interface:** LCD touchscreen for monitoring sensor readings, viewing system status and alarms, and performing basic operational configuration
- **Serial interface:** CLI console for commissioning/provisioning and diagnostics
- **Communication:** Modbus RTU slave over UART
- **Time:** RTC maintained on-board, synchronised from the gateway via Modbus on initial connection and periodically thereafter
- **Non-volatile storage:** External Quad-SPI flash for configuration persistence

### Node 2 — IoT Gateway (Modbus RTU Master + Cloud)

- **Board:** B-L475E-IOT01A (Discovery Kit for IoT Node, Cortex-M4)
- **Role:** Gateway that polls field devices and forwards data to the cloud
- **Sensors:** On-board sensors (temperature, humidity, pressure, accelerometer, gyroscope, magnetometer)
- **Serial interface:** CLI console for commissioning/provisioning and diagnostics
- **Communication:** Modbus RTU master over UART (to field device), WiFi/MQTT over TLS (to AWS IoT Core)
- **Time:** RTC synchronised via NTP over WiFi on boot and periodically thereafter; provides authoritative time to field devices via Modbus
- **Non-volatile storage:** External Quad-SPI NOR flash (64 Mbit Macronix MX25R6435F) for configuration persistence and store-and-forward telemetry buffer

### Physical Connection

RS-485 (A/B differential pair + GND) between the two boards for Modbus RTU.
UART-to-RS-485 conversion handled by a MAX485/MAX3485 module on each board.

---

## 4. Stakeholders and Users

### Field Technician

A person physically present at the device installation site. They:

- View live sensor readings, system status, and active alarms on the field device LCD.
- Perform basic operational configuration via the LCD (polling rate, alarm thresholds).
- Commission and provision both devices via serial console (WiFi credentials, cloud endpoint, Modbus address, serial port parameters).
- Run diagnostic commands via the serial console (view logs, check connectivity, inspect Modbus statistics, view system health metrics).

### Remote Operator

A person accessing the system remotely through a cloud dashboard. They:

- Monitor telemetry data (sensor readings, trends) through the cloud dashboard.
- Monitor device health and reliability metrics through the cloud dashboard.
- Receive alarm notifications when sensor values exceed configured thresholds.
- Send operational commands: change polling rate, set alarm thresholds, request an immediate sensor reading, restart a device.

### AWS IoT Core (System Actor)

The cloud platform that:

- Receives sensor telemetry and device health data via MQTT over TLS.
- Receives alarm events.
- Forwards configuration changes and commands from the Remote Operator to the gateway.

---

## 5. Functional Areas

### 5.1. Sensor Data Acquisition

The field device reads sensors (temperature, humidity, pressure) at a configurable polling interval. Sensor readings are processed and made available via Modbus registers for the gateway to read, and displayed on the local LCD.

The gateway reads its own on-board sensors and also polls the field device via Modbus RTU. All sensor data is aggregated for cloud publishing.

All sensor readings are timestamped using the local RTC.

The system applies signal conditioning (filtering, range validation) to raw sensor data before use in alarm evaluation, display, and cloud publishing.

#### 5.1.1 Field Device Sensor Implementation

The field device does not have on-board environmental sensors. Sensor data is generated by a simulation module that produces realistic, time-varying values with configurable noise and fault injection. The simulation module implements the same driver interface as a real I2C sensor driver, demonstrating that the abstraction layer decouples hardware from application logic. Replacing the simulation with physical sensors requires changes only in the driver layer.

### 5.2. Alarm Management

The system supports configurable alarm thresholds per sensor. When a sensor value exceeds a threshold, an alarm is raised. Alarms are:

- Displayed on the field device LCD (visual indicator, alarm details).
- Published to the cloud as alarm events with timestamps.
- Cleared automatically when the sensor value returns to the normal range (with hysteresis to prevent flapping).

Alarm thresholds are configurable both locally (LCD) and remotely (cloud command).

### 5.3. Modbus RTU Communication

The gateway (master) polls the field device (slave) at a configurable interval. The Modbus register map exposes:

- Sensor readings (input registers, read-only).
- Device status and alarm states (input registers, read-only).
- Configuration parameters (holding registers, read-write).
- Timestamp synchronisation (holding register, written by master to slave).

The register map design supports multiple field devices by Modbus address, even though only one is implemented.

### 5.4. Cloud Connectivity

The gateway publishes telemetry and alarm data to AWS IoT Core via MQTT over TLS. TLS is mandatory — all data in transit between the gateway and the cloud is encrypted. The gateway authenticates to AWS IoT Core using X.509 client certificates.

Message payloads use JSON format, structured with versioned message schemas. The message abstraction layer in firmware is designed so the serialization format could be changed (e.g., to CBOR or Protocol Buffers) without affecting the layers above it.

The gateway subscribes to command topics to receive configuration changes and commands from the Remote Operator.

### 5.5. LCD Display (Field Device)

The field device LCD provides a multi-screen monitoring interface:

- **Sensor screen:** Live sensor readings with units and timestamps.
- **Alarm screen:** Active alarms with severity and trigger time.
- **Status screen:** Device state, Modbus connection status, system health summary.
- **Configuration screen (Tier 2):** Basic operational settings (polling rate, alarm thresholds, display settings).

Navigation between screens uses the touchscreen. The LCD is a monitoring and basic configuration interface — it is not a full menu system.

### 5.6. Diagnostic Console (CLI)

Both boards provide a serial CLI console over a dedicated UART. The console serves two purposes:

- **Provisioning (Tier 1):** Initial device setup — WiFi credentials, cloud endpoint and certificates, Modbus address, serial port parameters. These settings are stored in non-volatile flash and persist across reboots.
- **Diagnostics:** Runtime inspection — view logs, check connectivity, inspect Modbus frame statistics, view FreeRTOS task status and stack usage, view system health metrics and error counters.

The CLI is a system feature, not a development-only tool. It is designed to be used by a field technician during installation and troubleshooting.

### 5.7. Remote Commands

The Remote Operator can send commands to the system via the cloud:

- **Change polling rate** (Tier 2 — operational, safe to change remotely).
- **Change alarm thresholds** (Tier 2 — operational).
- **Request immediate sensor reading** (Tier 3 — command, executed with acknowledgement).
- **Restart device** (Tier 3 — command, potentially disruptive, requires confirmation mechanism).

Commands are received by the gateway via MQTT subscription. Commands targeting the field device are forwarded via Modbus register writes.

### 5.8. Observability and Device Health

The system publishes device health telemetry alongside sensor data, on a slower cadence (e.g., once every 5 minutes vs. sensor data at the configured polling interval). Health telemetry includes:

- **Resource usage:** FreeRTOS stack high watermarks per task, free heap (at boot and current), CPU load estimate.
- **Connectivity metrics:** WiFi RSSI (signal strength), WiFi reconnection count, MQTT publish failure count.
- **Protocol reliability:** Modbus CRC error count, Modbus timeout count, Modbus successful transaction count.
- **System status:** Uptime since last boot, MCU internal temperature, store-and-forward buffer occupancy (percentage full).

These metrics are:

- Published to the cloud as a separate "device health" message type.
- Available via the CLI console for local inspection by a field technician.
- Summarised on the LCD status screen.

Error counters are cumulative since last boot and reset on restart.

### 5.9. Non-Volatile Storage

Both boards use external Quad-SPI flash for persistent data:

- **Configuration persistence:** All provisioning and operational settings survive power cycles and reboots.
- **Store-and-forward buffer (gateway only):** When WiFi connectivity is lost, the gateway buffers telemetry data in flash. When connectivity is restored, buffered data is published to the cloud in chronological order. If the buffer is full, the oldest data is overwritten (circular buffer strategy). Buffer sizing is determined in the HLD.

### 5.10. Secure Bootloader and OTA Updates

The system includes a secure bootloader that verifies firmware integrity before execution. The bootloader:

- Validates the firmware image signature (cryptographic verification) before jumping to the application.
- Supports a dual-bank or A/B partition scheme to allow safe firmware rollback if an update fails.
- Provides an OTA (Over-The-Air) update mechanism: the gateway receives firmware images via the cloud, verifies them, and writes them to the inactive flash partition.

Note: This feature is fully designed in the HLD and LLD. Implementation is progressive — the bootloader and signature verification are implemented first; the OTA transport mechanism is implemented as time allows. Even a partial implementation with complete design documentation demonstrates the required competence.

---

## 6. Multi-Device Architecture

The Modbus register map and gateway polling logic are designed to support multiple field devices, each with a unique Modbus slave address. The gateway's data structures, cloud message formats, and polling loop support N devices.

For this project, only one field device is implemented. Adding a second device should require:

- Connecting another board to the UART bus.
- Assigning a unique Modbus address.
- Updating a device table in the gateway configuration (no code changes to the polling or publishing logic).

This approach demonstrates scalable design without the implementation burden of managing multiple physical devices.

---

## 7. Connectivity and Reliability

- All communication between the gateway and AWS IoT Core uses MQTT over TLS with X.509 certificate-based authentication. Unencrypted cloud communication is not supported.
- The gateway must handle WiFi outages gracefully using a store-and-forward buffer backed by non-volatile flash.
- Modbus communication failures between gateway and field device must be detected, counted (for observability), and reported (via LCD and cloud).
- The system must recover automatically from transient communication failures without manual intervention.
- No data loss is acceptable during short connectivity outages (buffer sizing TBD in HLD).

---

## 8. Time Synchronisation

- The gateway synchronises its RTC via NTP over WiFi on boot and periodically thereafter.
- The gateway writes the current timestamp to the field device via a Modbus holding register on initial connection and at regular intervals.
- All sensor readings, alarms, and log entries are timestamped using the local RTC.
- If the RTC has not been synchronised (e.g., first boot with no connectivity), the system uses uptime-based timestamps and flags the data as "unsynchronised."

---

## 9. Portability

The firmware architecture shall be designed for vendor portability. Hardware dependencies shall be confined to the driver layer, which accesses peripherals directly via CMSIS device headers. A virtual hardware layer built on top of the driver layer shall expose vendor-neutral interfaces to the middleware and application layers. Porting to a different MCU vendor shall require rewriting only the driver layer.

---

## 10. Configuration Tiers

| Tier | Scope         | Access              | Examples                                              | Rationale                                        |
|------|---------------|----------------------|-------------------------------------------------------|--------------------------------------------------|
| Tier 1 | Provisioning | Local serial only   | WiFi credentials, cloud certificates, Modbus address  | Changing remotely could make device unreachable   |
| Tier 2 | Operational  | Local LCD + remote   | Polling rate, alarm thresholds, display settings, telemetry publishing interval, health data publishing interval | Safe to change remotely, no connectivity risk     |
| Tier 3 | Commands     | Remote with safeguards | Restart, immediate read                             | Potentially disruptive, needs confirmation        |

---

## 11. Development Practices

These are not system features but engineering practices applied throughout the project:

- **CI/CD:** GitHub Actions workflow for automated cross-compilation on every push. Static analysis (cppcheck) integrated into the build pipeline. Build status badge displayed on the repository README.
- **Coding standard:** BARR-C:2018 subset with OOP-in-C patterns.
- **Testing:** Unity framework for host-based unit tests. Integration tests on target hardware.
- **Version control:** Git with conventional commit messages. Feature branches merged via pull requests.

---

## 12. Out of Scope (for initial release)

- BLE connectivity
- Data logging to SD card
- Multiple physical field devices (designed for, not implemented)
- Full graphical menu system on the LCD
- Cloud dashboard implementation (AWS IoT Core is the boundary — what happens beyond it is out of scope)

---

## 13. Success Criteria

The project is considered successful when:

1. The field device reads sensor data, timestamps it, and displays it on the LCD.
2. The gateway polls the field device via Modbus RTU and receives sensor data.
3. The gateway synchronises time to the field device via Modbus.
4. The gateway publishes telemetry to AWS IoT Core via MQTT over TLS.
5. Alarm thresholds can be configured and alarms are generated, displayed, and published.
6. Remote commands are received from the cloud and executed.
7. The system recovers gracefully from WiFi and Modbus communication failures.
8. The store-and-forward buffer persists data in flash during WiFi outages.
9. Device health metrics are published to the cloud and accessible via CLI.
10. A diagnostic CLI console is available on both boards for provisioning and troubleshooting.
11. The secure bootloader verifies firmware integrity before execution.
12. CI/CD pipeline builds and analyses the firmware on every commit.
13. Configuration persists across power cycles.
14. No vendor-specific header is imported above the driver layer.

---

*This document is the single source of truth for project scope. Any feature not described here requires a vision update before design work begins.*
