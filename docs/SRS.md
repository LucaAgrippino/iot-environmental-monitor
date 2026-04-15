# System Requirements Specification — IoT Environmental Monitoring Gateway

**Version:** 0.1 (draft)
**Date:** April 2026
**Status:** In progress

**Revision History:**

| Version | Date       | Changes              |
|---------|------------|----------------------|
| 0.1     | April 2026 | Initial draft        |

---

## 1. Introduction

### 1.1 Purpose

This document specifies the functional and non-functional requirements for the IoT Environmental Monitoring Gateway system. All requirements are testable and traceable to the project vision and use case descriptions.

### 1.2 Scope

The system consists of two nodes — a field device (STM32F469 Discovery) and a gateway (B-L475E-IOT01A) — communicating via Modbus RTU over RS-485. The gateway publishes data to AWS IoT Core via MQTT over TLS. See the [Vision Document](vision.md) for the full system concept.

### 1.3 Definitions and Abbreviations

| Term       | Definition                                                    |
|------------|---------------------------------------------------------------|
| Field device | STM32F469 Discovery board acting as Modbus RTU slave       |
| Gateway    | B-L475E-IOT01A board acting as Modbus RTU master and cloud bridge |
| SRS        | System Requirements Specification                            |
| TBD        | To Be Determined — flagged for resolution before HLD         |

### 1.4 Requirement Notation

- **Shall** — mandatory requirement. The system must satisfy this.
- **Should** — desired but not mandatory. Implemented if time permits.
- **[REQ-XX-NNN]** — unique requirement identifier. XX = functional area code, NNN = sequence number.

### 1.5 Functional Area Codes

| Code | Area                        |
|------|-----------------------------|
| SA   | Sensor Acquisition          |
| AM   | Alarm Management            |
| LD   | Local Display (LCD)         |
| LI   | Local Interface (CLI)       |
| MB   | Modbus Communication        |
| CC   | Cloud Communication         |
| TS   | Time Synchronisation        |
| DM   | Device Management           |
| BF   | Data Buffering              |
| NF   | Non-Functional              |

---

## 2. Functional Requirements

### 2.1 Sensor Acquisition [SA]

<!-- Traces to: UC-07 -->
<!-- preconditions: System is initialised -->
- [REQ-SA-000] The system shall read the polling interval configuration from flash during startup
- [REQ-SA-010] The system shall use default polling interval of 1 second if the configuration reading fails
- [REQ-SA-020] The system shall use default sensor minimum and maximum values if the configuration reading fails
- [REQ-SA-030] The system shall initialise the field node sensor simulation: temperature, humidity, and pressure with default parameters during startup.
- [REQ-SA-031] The system shall initialise the gateway node sensors: temperature, humidity, pressure, accelerometer, gyroscope and magnetometer with manufacturer default settings during startup
- [REQ-SA-040] The system shall log an error message if a sensor fails its initialisation
- [REQ-SA-050] The system shall use default filter parameters if configuration reading fails
- [REQ-SA-060] The system shall continue operation with available sensors if one or more sensors fail to initialise.

<!-- 1. System triggers a new sensor data acquisition at the configured polling interval -->
- [REQ-SA-070] The system shall read temperature, humidity, and pressure sensors on the field device at configurable polling interval
- [REQ-SA-071] The system shall read temperature, humidity, pressure, accelerometer, gyroscope and magnetometer sensors on the gateway device at configurable polling interval
- [REQ-SA-080] The system shall log the error code if the reading fails

<!-- 2. System gets data from sensor -->
- [REQ-SA-090] The system shall store the most recent [TBD] readings per sensor
- [REQ-SA-100] The system shall store a timestamp with each sensor measurement

<!-- 3. System processes sensor data -->
- [REQ-SA-110] The system shall apply the same processing pipeline to both periodic and on-demand sensor readings
- [REQ-SA-120] The system shall validate that the acquired value is within the configured sensor range
- [REQ-SA-130] The system shall clamp out-of-range readings to the nearest range boundary
- [REQ-SA-140] The system shall filter the sensor measurement using a low pass filter with parameters [TBD] 

<!-- 4. System updates Modbus registers and LCD display -->
- [REQ-SA-150] The system shall make processed sensor data available for Modbus register access and LCD display

<!-- E1 (step 2): if some sensors don't respond, use a default error value -->
- [REQ-SA-0E1] The system shall mark the sensor value as invalid if sensor reading fails
- [REQ-SA-160] The system shall publish sensor data marked as invalid to the cloud with an error flag

<!-- Traces to: UC-14 -->
<!-- preconditions: System is initialised -->
<!-- 1. The Remote Operator request a sensor data reading -->
<!-- 2. System execute new data reading -->
- [REQ-SA-170] The system shall perform an additional sensor read upon receiving a remote read request
<!-- 3. System returns result -->
<!-- E1 (step 1): if the gateway is disconnected, show an command is not delivered to the gateway -->
<!-- E2 (step 3): if the gateway is disconnected, system cannot deliver result to AWS IoT Core; result is buffered -->

### 2.2 Alarm Management [AM]

<!-- Traces to: UC-08, UC-09, UC-03 -->

<!-- Traces to: UC-03 -->
<!-- Traces to: UC-08 -->
- [REQ-AM-000] The system shall compare sensor measurements with configured alarm thresholds
- [REQ-AM-010] The system shall clear existing alarm if the current measurement is within the configured alarm thresholds
- [REQ-AM-011] The system shall apply configurable hysteresis when clearing alarms to prevent flapping
<!-- Traces to: UC-09 -->
- [REQ-AM-020] The system shall trigger an alarm notification if the sensor measurement is out of range
- [REQ-AM-030] The system shall send alarm notification to AWS IoT Core
- [REQ-AM-040] The system shall include in each alarm notification: sensor ID, alarm type (high/low), measured value, threshold value, timestamp, and device ID

### 2.3 Local Display — LCD [LD]

<!-- Traces to: UC-01, UC-02, UC-03 -->
<!-- Traces to: UC-01 -->
- [REQ-LD-000] The system shall provide navigation between sensor readings, system status, system configuration, and alarm screens on the LCD
- [REQ-LD-010] The system shall display the most recent temperature, humidity, and pressure readings on the LCD
- [REQ-LD-020] The system shall display the timestamp of the most recent reading on the LCD
- [REQ-LD-030] The system shall display the measurement unit for each sensor reading on the LCD
- [REQ-LD-040] The system shall display an error indicator for any sensor whose reading is marked as invalid
- [REQ-LD-050] The system shall refresh the displayed sensor readings at the polling interval or upon receiving new data
- [REQ-LD-060] The system shall display a "waiting for data" message on the LCD until the first sensor reading is available
<!-- Traces to: UC-02 -->
- [REQ-LD-070] The system shall display a system status screen which lists: stack high watermarks, free heap, CPU load, WiFi RSSI, reconnection count, MQTT failure count, Modbus CRC/timeout/success counts, uptime, MCU temperature, buffer occupancy
<!-- Traces to: UC-03 -->
- [REQ-LD-080] The system shall display an alarm screen which lists all active alarms with their timestamp
- [REQ-LD-090] The system shall display a message on the alarm screen if no alarms are detected
<!-- Traces to: UC-15 -->
- [REQ-LD-100] The system shall display a configuration screen which lists these user configurable parameters: polling rate, alarm thresholds, display settings
- [REQ-LD-110] The system shall validate each input value against a configurable acceptable range
- [REQ-LD-120] The system shall request confirmation from the Field Technician before applying changes
- [REQ-LD-130] The system shall apply the change if a confirmation is received
- [REQ-LD-140] The system shall retain the previous configuration until all new parameters are successfully applied
- [REQ-LD-150] The system shall save the parameters to persistent memory when they are successfully applied 

- [REQ-LD-0E1] The system shall discard all the input parameters if no confirmation is received

### 2.4 Local Interface — CLI [LI]

<!-- Traces to: UC-04, UC-16 -->

<!-- Traces to: UC-04 -->
- [REQ-LI-000] The system shall provide a serial console interface accessible to the Field Technician. The connection parameters are:
    - Baud Rate: 115200
    - Data bits: 8
    - Parity bit: No Parity
    - Stop bits: 1
- [REQ-LI-010] The system shall receive a diagnostic command [command list TBD] and execute it
- [REQ-LI-020] The system shall display the result of the diagnostic 

<!-- Traces to: UC-16 -->
- [REQ-LI-030] The system shall have a provision menu
- [REQ-LI-040] The system shall receive the WIFI credentials 
- [REQ-LI-050] The system shall receive the cloud endpoint 
- [REQ-LI-060] The system shall receive the cloud certificates  
- [REQ-LI-070] The system shall receive the Modbus address 
- [REQ-LI-080] The system shall receive the serial port parameters 
- [REQ-LI-090] The system shall validate the format and the range of the input values
- [REQ-LI-100] The system shall request confirmation from the Field Technician before applying changes
- [REQ-LI-110] The system shall apply the change if a confirmation is received
- [REQ-LI-120] The system shall retain the previous configuration until all new parameters are successfully applied
- [REQ-LI-130] The system shall provide a board self-test command via the CLI 
  that verifies sensors, communication links, and flash memory
- [REQ-LI-140] The system shall report the self-test result as pass/fail 
  with details for each subsystem tested
- [REQ-LI-150] The system shall read and display the device serial number 
  (MCU unique ID) via the CLI
- [REQ-LI-160] The system shall store the most recent board self-test 
  result (date, pass/fail, per-subsystem details) in non-volatile storage
  
- [REQ-LI-0E1] The system shall display an error message if the diagnostic command fails
- [REQ-LI-0E2] The system shall display an error message with the problem if the input validation fails
- [REQ-LI-0E3] The system shall discard all the input parameters if no confirmation is received


### 2.5 Modbus Communication [MB]

<!-- Traces to: UC-07, UC-10, UC-13, UC-14, UC-19 -->
<!-- Traces to: UC-07 -->
- [REQ-MB-000] The system shall update Modbus register when a new sensor measurement is available
<!-- Traces to: UC-10 -->
- [REQ-MB-010] The system shall read sensor data from the field device via Modbus read input registers
<!-- Traces to: UC-13 -->
- [REQ-MB-020] The system shall write the updated time to the field node using Modbus holding register
<!-- Protocol parameters -->
- [REQ-MB-030] The system shall communicate via Modbus RTU at:
    - Baudrate: 9600 baud
    - Data bits: 8
    - Parity bit: no parity
    - Stop bits: 1
- [REQ-MB-040] The system shall support Modbus function codes: 
    - 03 (Read Holding Registers)
    - 04 (Read Input Registers) 
    - 06/16 (Write Single/Multiple Registers) 
<!-- See REQ-NF-105 -->
- [REQ-MB-050] The system shall timeout a Modbus request after 200 milliseconds with no response
<!-- See REQ-NF-103 -->
- [REQ-MB-060] The system shall retry a failed Modbus request up to 3 times before reporting a communication failure
- [REQ-MB-070] The system shall define a register map specifying the address, data type, and access mode for all exchanged data
<!-- Traces to: UC-19 -->
- [REQ-MB-080] The system shall receive remote commands from the gateway and write them to the appropriate Modbus holding registers on the field device.
- [REQ-MB-090] The system shall read the command execution result from the field device via Modbus and return it to the cloud.
- [REQ-MB-0E1] The system shall reject a remote command with an unrecognised format and log the error.


### 2.6 Cloud Communication [CC]

<!-- Traces to: UC-05, UC-06, UC-09, UC-10, UC-11, UC-12 -->
<!-- Traces to: UC-05 -->
- [REQ-CC-000] The system shall publish periodically to the cloud the most recent sensor measurements (temperature, humidity, pressure, accelerometer, gyroscope, magnetometer) with timestamps
<!-- Traces to: UC-06 -->
- [REQ-CC-010] The system shall publish to the cloud the most recent device health metrics: stack high watermarks, free heap, CPU load, WiFi RSSI, reconnection count, MQTT failure count, Modbus CRC/timeout/success counts, uptime, MCU temperature, buffer occupancy
<!-- Traces to: UC-09 -->
- [REQ-CC-020] The system shall publish to the cloud the alarm notification
<!-- Traces to: UC-10 -->
<!-- Traces to: UC-11 -->
<!-- Traces to: UC-12 -->
- [REQ-CC-030] The system shall publish sensor telemetry at a configurable interval, adjustable locally via CLI and remotely via cloud command.
- [REQ-CC-040] The system shall publish device health data at a configurable interval, adjustable locally via CLI and remotely via cloud command.
- [REQ-CC-050] The system shall connect to AWS IoT Core using MQTT over TLS with X.509 certificate authentication
- [REQ-CC-060] The system shall reconnect automatically if the MQTT connection is lost
- [REQ-CC-070] The system shall use JSON format for all MQTT payloads
- [REQ-CC-071] The system shall include a schema version identifier in all MQTT payloads.
- [REQ-CC-080] The system shall use separate MQTT topics for telemetry, alarms, and device health data
- [REQ-CC-090] The system shall include the device serial number 
  in all cloud health messages

### 2.7 Time Synchronisation [TS]

<!-- Traces to: UC-13 -->
- [REQ-TS-000] The system shall update the internal time during the boot and periodically every [TBD] thereafter
- [REQ-TS-010] The system shall synchronise time using NTP, with a configurable list of NTP servers
- [REQ-TS-020] The system shall update the RTC with the synchronised time
- [REQ-TS-030] The system shall write the current time to the field device via Modbus holding register on initial connection and at regular intervals.
- [REQ-TS-040] The system shall use uptime-based timestamps and flag data as "unsynchronised" if NTP synchronisation has not been completed.
- [REQ-TS-0E1] The system shall retry to get the time update as soon as the internet connection is restored


### 2.8 Device Management [DM]

<!-- Traces to: UC-15, UC-16, UC-17, UC-18, UC-20 -->
<!-- Traces to: UC-15 -->
- [REQ-DM-000] The system shall accept operational parameter changes from the Remote Operator via cloud command
- [REQ-DM-001] The system shall validate remote parameter changes before applying them.
- [REQ-DM-002] The system shall send a confirmation or rejection response to the cloud after processing a remote parameter change.
<!-- Traces to: UC-17 -->
- [REQ-DM-010] The system shall execute remote restart command
- [REQ-DM-020] The system shall request confirmation from the Remote Operator before executing a restart
- [REQ-DM-021] The system shall cancel the restart if the Remote Operator declines confirmation
- [REQ-DM-030] The system shall report restart success after reboot and self-check
- [REQ-DM-040] The system shall complete a self-check after restart, verifying sensor initialisations and communication links
<!-- Traces to: UC-18 -->
- [REQ-DM-050] The system shall download the firmware image
- [REQ-DM-051] The system shall resume a firmware download from the point of interruption if the connection is restored
- [REQ-DM-052] The system shall delete the amount of downloaded firmware if after three retries, the firmware download still fails
- [REQ-DM-053] The system shall log an error if the firmware image download fails
- [REQ-DM-054] The system shall reject a new update command if one is in progress
- [REQ-DM-055] The system shall report the update result to the cloud
- [REQ-DM-056] The system shall only accept firmware update commands from an authenticated source
- [REQ-DM-060] The system shall validate the firmware image
- [REQ-DM-061] The system shall discard the firmware update process and restore the current firmware if the firmware validation fails
- [REQ-DM-062] The system shall log an error if the firmware image validation fails
- [REQ-DM-070] The system shall set the firmware partition as boot partition
- [REQ-DM-071] The system shall trigger a self-check validation on the new firmware
- [REQ-DM-072] The system shall rollback the previous firmware image if the self-check fails 
- [REQ-DM-073] The system shall retain the current firmware image until the success of the self-check validation 
- [REQ-DM-074] The system shall maintain a dual-bank partition scheme to enable firmware rollback
<!-- Traces to: UC-20 -->
- [REQ-DM-080] The system shall verify the cryptographic signature of the firmware image before applying the update
- [REQ-DM-090] The system shall persist all configuration changes to non-volatile storage


### 2.9 Data Buffering [BF]

<!-- Traces to: UC-10, UC-11, UC-12 (exception flows) -->
- [REQ-BF-000] The system shall buffer all outbound cloud messages (telemetry, alarms, device health) in non-volatile storage when internet connectivity is lost
- [REQ-BF-010] The system shall publish buffered messages in chronological order when connectivity is restored
- [REQ-BF-020] The system shall discard the oldest buffered messages when the buffer reaches its maximum capacity of [TBD] entries


---

## 3. Non-Functional Requirements [NF]

### 3.1 Performance
- [REQ-NF-100] The system shall complete a full sensor polling cycle (read all sensors, timestamp, queue data) within 100 ms
- [REQ-NF-101] The system shall detect an alarm condition within one polling cycle of the threshold being exceeded
- [REQ-NF-102] The system shall clear an alarm condition within one polling cycle of the measurement being in the range
- [REQ-NF-103] The system shall declare Modbus communication failure after 3 consecutive unanswered requests
- [REQ-NF-104] The system shall declare Modbus communication restored after 3 consecutive successful responses
- [REQ-NF-105] The system shall consider a Modbus request failed if no response is received within 200 ms of transmission 
- [REQ-NF-106] The system shall queue an MQTT message for publication within 200 ms of data being ready
- [REQ-NF-107] The system shall begin executing a received remote command within 500 ms of receipt
- [REQ-NF-108] The system shall use a refresh rate of 5Hz for the field node LCD
- [REQ-NF-109] The system shall use a system watchdog of 10 seconds
- [REQ-NF-110] The system shall poll the field node at default polling frequency of 1Hz
- [REQ-NF-111] The system shall publish the sensor measurements to AWS IoT Core each 60 seconds
- [REQ-NF-112] The system shall publish the health data to AWS IoT Core each 10 minutes
- [REQ-NF-113] The system shall publish the alarm notification to AWS IoT Core within 500 ms since their detection
- [REQ-NF-114] The system shall use a polling frequency within this range [TBD] Hz and [TBD] Hz

### 3.2 Reliability
- [REQ-NF-200] The system shall continue local sensor acquisition and alarm evaluation when internet connectivity is lost
- [REQ-NF-201] The system shall recover Modbus communication automatically after a transient bus error without requiring a restart
- [REQ-NF-202] The system shall restart and resume normal operation within 5 seconds after a watchdog reset
- [REQ-NF-203] The system shall restart and resume normal operation within 5 seconds after a normal reset
- [REQ-NF-204] The system shall roll back to the previous firmware and resume normal operation within 10 seconds if a firmware update fails post-installation
- [REQ-NF-205] The system shall recover from a sensor failure by reinitialising the failed sensor  
<!-- Rationale: fire-and-forget; data is buffered locally by REQ-BF-000 -->
- [REQ-NF-206] The system shall use MQTT QoS 0 for publishing sensor telemetry
<!-- Rationale: at-least-once delivery for safety-critical events -->
- [REQ-NF-207] The system shall use MQTT QoS 1 for publishing alarm notifications
- [REQ-NF-208] The system shall substitute a defined error indicator value for any sensor that fails to provide a reading
- [REQ-NF-209] The system shall attempt to reconnect at a frequency of 1Hz if a disconnection is detected
- [REQ-NF-210] The system shall synchronise the RTC every [TBD] [TBD — determined by RTC drift measurement during integration testing]
- [REQ-NF-211] The system shall synchronise the field node time every [TBD] [TBD — determined by RTC drift measurement during integration testing]
- [REQ-NF-212] The system shall use uptime-based timestamps and flag the data as "unsynchronised" if the RTC is not synchronised
- [REQ-NF-213] The system shall reach normal operational state within [TBD] seconds of power-on
- [REQ-NF-214] The system shall recover to a known-good state if power is lost during a flash write operation (configuration save, buffer write, or firmware update)
- [REQ-NF-215] The system shall report a "node offline" status to the AWS IoT Core when no Modbus response is received for 3 consecutive polls

### 3.3 Security
- [REQ-NF-300] The system shall encrypt all communication with AWS IoT Core using TLS 1.2 or higher
- [REQ-NF-301] The system shall authenticate to AWS IoT Core using X.509 client certificates
- [REQ-NF-302] The system shall not store provisioning credentials in plaintext
- [REQ-NF-303] The system shall restrict CLI access to physical serial connections only
- [REQ-NF-304] The system shall cryptographically verify firmware images before installation
- [REQ-NF-305] The system shall reject any unencrypted connection to cloud services
- [REQ-NF-306] The system shall restrict Tier 1 configuration parameters to local serial access only
- [REQ-NF-307] The system shall require confirmation before executing Tier 3 commands

### 3.4 Memory and Resource Constraints
- [REQ-NF-400] The system shall operate within the 128 KB SRAM of the B-L475E-IOT01A
- [REQ-NF-401] The system shall operate within the 1 MB FLASH of the B-L475E-IOT01A
- [REQ-NF-402] The system shall operate within the 8 MB QUAD-SPI FLASH of the B-L475E-IOT01A
- [REQ-NF-403] The system shall operate within the 320+4 KB SRAM of the STM32F469 Discovery 
- [REQ-NF-404] The system shall operate within the 2 MB FLASH of the STM32F469 Discovery
- [REQ-NF-405] The system shall operate within the 16 MB QUAD-SPI FLASH of the STM32F469 Discovery 
- [REQ-NF-406] The system shall allocate no more than 3 MB of non-volatile storage for diagnostic logs per node
- [REQ-NF-407] The system shall allocate no more than 4 MB of non-volatile storage for sensor measurements per node
- [REQ-NF-408] The system shall not use dynamic memory allocation (malloc/free) after initialisation completes

### 3.5 Maintainability
- [REQ-NF-500] The system shall log diagnostic events with severity level, timestamp, and source module identifier
- [REQ-NF-501] The system shall enforce a layered architecture where no module above the driver layer imports vendor-specific headers
- [REQ-NF-502] All source code shall conform to the project's BARR-C:2018 subset coding standard
- [REQ-NF-503] The system shall report its firmware version via the CLI and in cloud health messages
- [REQ-NF-504] The system shall provide a diagnostic output channel for runtime tracing and debugging
- [REQ-NF-505] The system shall include Doxygen-compatible documentation comments for all public API functions
- [REQ-NF-506] The system shall expose vendor-neutral interfaces to the middleware and application layers

---

## 4. Constraints

- [CON-001] The gateway WiFi module (ISM43362-M3G-L44) communicates with the host MCU via SPI using AT commands. All TCP/IP and TLS operations are handled by the module's internal stack, not by the application firmware.
- [CON-002] The ISM43362 WiFi module limits RF output power to 9 dBm to comply with FCC/IC/CE requirements. WiFi range is constrained accordingly.
- [CON-003] The STM32F469 Discovery board has no on-board environmental sensors. All field device sensor data is produced by a software simulation module.
- [CON-004] The Modbus RTU protocol limits a single data frame to 256 bytes (253 bytes of application data plus 3 bytes of overhead).
- [CON-005] The Modbus RTU bus operates as single-master. Only the gateway may initiate transactions; the field device responds only when polled.
- [CON-006] X.509 client certificates and private keys shall be stored in a dedicated flash partition, separate from configuration and telemetry buffer storage.
- [CON-007] AWS IoT Core limits MQTT message payload size to 128 KB per publish. All telemetry, alarm, and health messages must fit within this limit.
- [CON-008] AWS IoT Core requires MQTT over TLS with X.509 certificate authentication. No alternative authentication method is supported by the system.
- [CON-009] The Quad-SPI NOR flash (MX25R6435F on the gateway) has a rated write endurance of 100,000 cycles per sector. Flash write patterns (buffering, configuration, logs) must account for wear levelling.
- [CON-010] Both STM32 MCUs operate at 3.3 V logic levels. All external modules (RS-485 transceivers, sensors) must be 3.3 V compatible.
- [CON-011] The system uses FreeRTOS as its real-time operating system. All timing, scheduling, and inter-task communication constraints are governed by FreeRTOS capabilities.

---

## 5. Assumptions

- [ASM-001] The field device sensor simulation module generates realistic noise and fault conditions sufficient to exercise the signal conditioning pipeline. If the simulation produced perfect data, the filtering and range validation requirements would not be testable on the field device.
- [ASM-002] The gateway has access to at least one NTP server via the internet on boot or within a reasonable period after boot. If NTP is permanently unreachable, time synchronisation requirements (REQ-TS-000 through REQ-TS-030) cannot be satisfied.
- [ASM-003] The RS-485 bus has exactly one master (the gateway). No other Modbus master device is present on the bus. If a second master were introduced, bus contention would violate the single-master constraint (CON-005).
- [ASM-004] The combined SRAM requirements of FreeRTOS kernel, TLS/MQTT stack, application tasks, and communication buffers fit within 128 KB on the gateway (B-L475E-IOT01A). If TLS stack memory requirements exceed estimates, the system may require a reduced task set or optimised buffer allocation.
- [ASM-005] Power supply to both boards is stable under normal operating conditions. The power failure recovery requirement (REQ-NF-214) covers transient power loss, not sustained operation on degraded power.
- [ASM-006] The ISM43362 WiFi module firmware (version C3.5.2.3.BETA9) is pre-loaded and functional. The system does not update or manage the WiFi module's internal firmware.
- [ASM-007] AWS IoT Core is available and operational. The system handles temporary cloud unavailability via store-and-forward buffering but does not handle permanent cloud service decommissioning.

---

## 6. Traceability Matrix

| Requirement | Use Case(s) | Vision Section |
|---|---|---|
| REQ-SA-000 | UC-07 | §5.1 |
| REQ-SA-010 | UC-07 | §5.1 |
| REQ-SA-020 | UC-07 | §5.1 |
| REQ-SA-030 | UC-07 | §5.1, §5.1.1 |
| REQ-SA-031 | UC-07 | §5.1 |
| REQ-SA-040 | UC-07 | §5.1 |
| REQ-SA-050 | UC-07 | §5.1 |
| REQ-SA-060 | UC-07 | §5.1 |
| REQ-SA-070 | UC-07 | §5.1 |
| REQ-SA-071 | UC-07 | §5.1 |
| REQ-SA-080 | UC-07 | §5.1 |
| REQ-SA-090 | UC-07 | §5.1 |
| REQ-SA-100 | UC-07 | §5.1, §8 |
| REQ-SA-110 | UC-07, UC-14 | §5.1 |
| REQ-SA-120 | UC-07 | §5.1 |
| REQ-SA-130 | UC-07 | §5.1 |
| REQ-SA-140 | UC-07 | §5.1 |
| REQ-SA-150 | UC-07 | §5.1, §5.3 |
| REQ-SA-0E1 | UC-07 | §5.1 |
| REQ-SA-160 | UC-07 | §5.1 |
| REQ-SA-170 | UC-14 | §5.7 |
| REQ-AM-000 | UC-08 | §5.2 |
| REQ-AM-010 | UC-08 | §5.2 |
| REQ-AM-011 | UC-08 | §5.2 |
| REQ-AM-020 | UC-08, UC-09 | §5.2 |
| REQ-AM-030 | UC-09 | §5.2, §5.4 |
| REQ-AM-040 | UC-09 | §5.2 |
| REQ-LD-000 | UC-01, UC-02, UC-03, UC-15 | §5.5 |
| REQ-LD-010 | UC-01 | §5.5 |
| REQ-LD-020 | UC-01 | §5.5 |
| REQ-LD-030 | UC-01 | §5.5 |
| REQ-LD-040 | UC-01 | §5.5 |
| REQ-LD-050 | UC-01 | §5.5 |
| REQ-LD-060 | UC-01 | §5.5 |
| REQ-LD-070 | UC-02 | §5.5, §5.8 |
| REQ-LD-080 | UC-03 | §5.5 |
| REQ-LD-090 | UC-03 | §5.5 |
| REQ-LD-100 | UC-15 | §5.5, §10 |
| REQ-LD-110 | UC-15 | §5.5 |
| REQ-LD-120 | UC-15 | §5.5 |
| REQ-LD-130 | UC-15 | §5.5 |
| REQ-LD-140 | UC-15 | §5.5 |
| REQ-LD-150 | UC-15 | §5.5, §5.9 |
| REQ-LD-0E1 | UC-15 | §5.5 |
| REQ-LI-000 | UC-04 | §5.6 |
| REQ-LI-010 | UC-04 | §5.6 |
| REQ-LI-020 | UC-04 | §5.6 |
| REQ-LI-030 | UC-16 | §5.6 |
| REQ-LI-040 | UC-16 | §5.6 |
| REQ-LI-050 | UC-16 | §5.6 |
| REQ-LI-060 | UC-16 | §5.6 |
| REQ-LI-070 | UC-16 | §5.6 |
| REQ-LI-080 | UC-16 | §5.6 |
| REQ-LI-090 | UC-16 | §5.6 |
| REQ-LI-100 | UC-16 | §5.6 |
| REQ-LI-110 | UC-16 | §5.6 |
| REQ-LI-120 | UC-16 | §5.6 |
| REQ-LI-130 | UC-04 | §5.6 |
| REQ-LI-140 | UC-04 | §5.6 |
| REQ-LI-150 | UC-04 | §5.6 |
| REQ-LI-160 | UC-04 | §5.6, §5.9 |
| REQ-LI-0E1 | UC-04 | §5.6 |
| REQ-LI-0E2 | UC-16 | §5.6 |
| REQ-LI-0E3 | UC-16 | §5.6 |
| REQ-MB-000 | UC-07 | §5.3 |
| REQ-MB-010 | UC-10 | §5.3 |
| REQ-MB-020 | UC-13 | §5.3, §8 |
| REQ-MB-030 | UC-07, UC-10, UC-13 | §5.3 |
| REQ-MB-040 | UC-07, UC-10, UC-13, UC-19 | §5.3 |
| REQ-MB-050 | UC-07, UC-10 | §5.3 |
| REQ-MB-060 | UC-07, UC-10 | §5.3 |
| REQ-MB-070 | UC-07, UC-10, UC-13, UC-19 | §5.3 |
| REQ-MB-080 | UC-19 | §5.3, §5.7 |
| REQ-MB-090 | UC-19 | §5.3, §5.7 |
| REQ-MB-0E1 | UC-19 | §5.3 |
| REQ-CC-000 | UC-05, UC-10 | §5.4 |
| REQ-CC-010 | UC-06, UC-11 | §5.4, §5.8 |
| REQ-CC-020 | UC-09, UC-12 | §5.4, §5.2 |
| REQ-CC-030 | UC-10 | §5.4, §10 |
| REQ-CC-040 | UC-11 | §5.4, §5.8, §10 |
| REQ-CC-050 | UC-10, UC-11, UC-12 | §5.4, §7 |
| REQ-CC-060 | UC-10, UC-11, UC-12 | §5.4, §7 |
| REQ-CC-070 | UC-10, UC-11, UC-12 | §5.4 |
| REQ-CC-071 | UC-10, UC-11, UC-12 | §5.4 |
| REQ-CC-080 | UC-10, UC-11, UC-12 | §5.4 |
| REQ-CC-090 | UC-06 | §5.6, §5.8 |
| REQ-TS-000 | UC-13 | §8 |
| REQ-TS-010 | UC-13 | §8 |
| REQ-TS-020 | UC-13 | §8 |
| REQ-TS-030 | UC-13 | §8 |
| REQ-TS-040 | UC-13 | §8 |
| REQ-TS-0E1 | UC-13 | §8 |
| REQ-DM-000 | UC-15 | §5.7, §10 |
| REQ-DM-001 | UC-15 | §5.7 |
| REQ-DM-002 | UC-15 | §5.7 |
| REQ-DM-010 | UC-17 | §5.7 |
| REQ-DM-020 | UC-17 | §5.7, §10 |
| REQ-DM-021 | UC-17 | §5.7 |
| REQ-DM-030 | UC-17 | §5.7 |
| REQ-DM-040 | UC-17 | §5.7 |
| REQ-DM-050 | UC-18 | §5.10 |
| REQ-DM-051 | UC-18 | §5.10 |
| REQ-DM-052 | UC-18 | §5.10 |
| REQ-DM-053 | UC-18 | §5.10 |
| REQ-DM-054 | UC-18 | §5.10 |
| REQ-DM-055 | UC-18 | §5.10 |
| REQ-DM-056 | UC-18 | §5.10 |
| REQ-DM-060 | UC-18, UC-20 | §5.10 |
| REQ-DM-061 | UC-18 | §5.10 |
| REQ-DM-062 | UC-18 | §5.10 |
| REQ-DM-070 | UC-18 | §5.10 |
| REQ-DM-071 | UC-18 | §5.10 |
| REQ-DM-072 | UC-18 | §5.10 |
| REQ-DM-073 | UC-18 | §5.10 |
| REQ-DM-074 | UC-18 | §5.10 |
| REQ-DM-080 | UC-20 | §5.10 |
| REQ-DM-090 | UC-15, UC-16 | §5.9 |
| REQ-BF-000 | UC-10, UC-11, UC-12 | §5.9, §7 |
| REQ-BF-010 | UC-10, UC-11, UC-12 | §5.9, §7 |
| REQ-BF-020 | UC-10, UC-11, UC-12 | §5.9, §7 |
| REQ-NF-100 | UC-07 | §5.1 |
| REQ-NF-101 | UC-08 | §5.2 |
| REQ-NF-102 | UC-08 | §5.2 |
| REQ-NF-103 | UC-07, UC-10 | §5.3, §7 |
| REQ-NF-104 | UC-07, UC-10 | §5.3, §7 |
| REQ-NF-105 | UC-07, UC-10 | §5.3 |
| REQ-NF-106 | UC-10, UC-11, UC-12 | §5.4 |
| REQ-NF-107 | UC-19 | §5.7 |
| REQ-NF-108 | UC-01 | §5.5 |
| REQ-NF-109 | — | §7 |
| REQ-NF-110 | UC-07 | §5.1 |
| REQ-NF-111 | UC-10 | §5.4 |
| REQ-NF-112 | UC-11 | §5.8 |
| REQ-NF-113 | UC-09, UC-12 | §5.2, §5.4 |
| REQ-NF-114 | UC-07 | §5.1, §10 |
| REQ-NF-200 | UC-07, UC-08 | §7 |
| REQ-NF-201 | UC-07, UC-10 | §7 |
| REQ-NF-202 | — | §7 |
| REQ-NF-203 | UC-17 | §7 |
| REQ-NF-204 | UC-18 | §5.10 |
| REQ-NF-205 | UC-07 | §5.1 |
| REQ-NF-206 | UC-10 | §5.4 |
| REQ-NF-207 | UC-09, UC-12 | §5.2, §5.4 |
| REQ-NF-208 | UC-07 | §5.1 |
| REQ-NF-209 | UC-10, UC-11, UC-12 | §7 |
| REQ-NF-210 | UC-13 | §8 |
| REQ-NF-211 | UC-13 | §8 |
| REQ-NF-212 | UC-13 | §8 |
| REQ-NF-213 | — | §7 |
| REQ-NF-214 | — | §5.9, §7 |
| REQ-NF-215 | UC-07, UC-10 | §5.8, §7 |
| REQ-NF-300 | UC-10, UC-11, UC-12 | §5.4, §7 |
| REQ-NF-301 | UC-10, UC-11, UC-12 | §5.4, §7 |
| REQ-NF-302 | UC-16 | §5.6 |
| REQ-NF-303 | UC-04, UC-16 | §5.6 |
| REQ-NF-304 | UC-18, UC-20 | §5.10 |
| REQ-NF-305 | UC-10, UC-11, UC-12 | §5.4, §7 |
| REQ-NF-306 | UC-16 | §10 |
| REQ-NF-307 | UC-17 | §10 |
| REQ-NF-400 | — | §3 |
| REQ-NF-401 | — | §3 |
| REQ-NF-402 | — | §3 |
| REQ-NF-403 | — | §3 |
| REQ-NF-404 | — | §3 |
| REQ-NF-405 | — | §3 |
| REQ-NF-406 | — | §5.9 |
| REQ-NF-407 | — | §5.9 |
| REQ-NF-408 | — | §11 |
| REQ-NF-500 | UC-04 | §5.6, §5.8 |
| REQ-NF-501 | — | §9 |
| REQ-NF-502 | — | §11 |
| REQ-NF-503 | UC-04, UC-06 | §5.6, §5.8 |
| REQ-NF-504 | UC-04 | §5.6 |
| REQ-NF-505 | — | §11 |
| REQ-NF-506 | — | §9 |

---

*All requirements trace to the [Vision Document](vision.md) and [Use Case Descriptions](use-case-descriptions.md).*
