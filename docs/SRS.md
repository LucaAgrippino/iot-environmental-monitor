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


<!-- Traces to: UC-14 -->
<!-- preconditions: System is initialised -->
<!-- 1. The Remote Operator request a sensor data reading -->
<!-- 2. System execute new data reading -->
- [REQ-SA-160] The system shall perform an additional sensor read upon receiving a remote read request
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
- [REQ-MB-050] The system shall timeout a Modbus request after [TBD] milliseconds with no response
- [REQ-MB-060] The system shall retry a failed Modbus request up to [TBD] times before reporting a communication failure
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

### 3.2 Reliability

### 3.3 Security

### 3.4 Memory and Resource Constraints

### 3.5 Maintainability

---

## 4. Constraints

<!-- Hardware constraints, protocol constraints, third-party limitations -->

---

## 5. Assumptions

<!-- Assumptions that, if wrong, would invalidate requirements -->

---

## 6. Traceability Matrix

| Requirement | Use Case(s)       | Vision Section |
|-------------|--------------------|----------------|

---

*All requirements trace to the [Vision Document](vision.md) and [Use Case Descriptions](use-case-descriptions.md).*