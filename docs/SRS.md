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
- [REQ-SA-001] The system shall use default polling interval of 1 Hz if the configuration reading fails
- [REQ-SA-002] The system shall use default sensor minimum and maximum values if the configuration reading fails
- [REQ-SA-003] The system shall initialise temperature, humidity, and pressure sensors with manufacturer default settings during startup
- [REQ-SA-004] The system shall log an error message if a sensor fails its initialisation
- [REQ-SA-005] The system shall use default filter parameters if configuration reading fails
- [REQ-SA-006] The system shall continue operation with available sensors if one or more sensors fail to initialise.

<!-- 1. System triggers a new sensor data acquisition at the configured polling interval -->
- [REQ-SA-010] The system shall read temperature, humidity, and pressure sensors on the field device at configurable polling interval
- [REQ-SA-011] The system shall log the error code if the reading fails

<!-- 2. System get data from sensor -->
- [REQ-SA-020] The system shall store the most recent [TBD] readings per sensor
- [REQ-SA-021] The system shall store a timestamp with each sensor measurement

<!-- 3. System process sensor data -->
- [REQ-SA-030] The system shall apply the same processing pipeline to both periodic and on-demand sensor readings
- [REQ-SA-031] The system shall validate that the acquired value is within the configured sensor range
- [REQ-SA-032] The system shall clamp out-of-range readings to the nearest range boundary
- [REQ-SA-033] The system shall filter the sensor measurement using a low pass filter with parameters [TBD] 

<!-- 4. System updates Modbus registers and LCD display -->
- [REQ-SA-040] The system shall make processed sensor data available for Modbus register access and LCD display

<!-- E1 (step 2): if some sensors don't respond, use a default error value -->
- [REQ-SA-0E1] The system shall mark the sensor value as invalid if sensor reading fails


<!-- Traces to: UC-14 -->
<!-- preconditions: System is initialised -->
<!-- 1. The Remote Operator request a sensor data reading -->
<!-- 2. System execute new data reading -->
- [REQ-SA-070] The system shall perform an additional sensor read upon receiving a remote read request
<!-- 3. System returns result -->
<!-- E1 (step 1): if the gateway is disconnected, show an command is not delivered to the gateway -->
<!-- E2 (step 3): if the gateway is disconnected, system cannot deliver result to AWS IoT Core; result is buffered -->

### 2.2 Alarm Management [AM]

<!-- Traces to: UC-08, UC-09, UC-03 -->

### 2.3 Local Display — LCD [LD]

<!-- Traces to: UC-01, UC-02, UC-03 -->

### 2.4 Local Interface — CLI [LI]

<!-- Traces to: UC-04, UC-16 -->

### 2.5 Modbus Communication [MB]

<!-- Traces to: UC-07, UC-10, UC-13, UC-14, UC-19 -->

### 2.6 Cloud Communication [CC]

<!-- Traces to: UC-05, UC-06, UC-09, UC-10, UC-11, UC-12 -->

### 2.7 Time Synchronisation [TS]

<!-- Traces to: UC-13 -->

### 2.8 Device Management [DM]

<!-- Traces to: UC-15, UC-16, UC-17, UC-18, UC-20 -->

### 2.9 Data Buffering [BF]

<!-- Traces to: UC-10, UC-11, UC-12 (exception flows) -->

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
| REQ-SA-001  | UC-07              | §3, §12.1     |
| ...         | ...                | ...            |

---

*All requirements trace to the [Vision Document](vision.md) and [Use Case Descriptions](use-case-descriptions.md).*