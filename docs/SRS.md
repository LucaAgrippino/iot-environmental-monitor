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

<!-- Traces to: UC-07, UC-14 -->

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