# IoT Environmental Monitoring Gateway

[![CI](https://github.com/LucaAgrippino/iot-environmental-monitor/actions/workflows/ci.yml/badge.svg)](https://github.com/LucaAgrippino/iot-environmental-monitor/actions/workflows/ci.yml)

**Project status:** Phase 4 (Implementation) in progress. HLD baseline at `hld-v1.0`. LLD baseline at `lld-v1.0`.

A dual-node embedded system demonstrating industrial IoT architecture: a field device collects sensor data and communicates with a gateway via **Modbus RTU**, which then publishes to the cloud via **WiFi/MQTT**.

## System Overview

| Node | Board | Role |
|------|-------|------|
| Field Device | STM32F469 Discovery | Sensor acquisition, LCD display, Modbus RTU slave |
| IoT Gateway | B-L475E-IOT01A | Modbus RTU master, data aggregation, MQTT to AWS IoT Core |

## Technology Stack

- **Language:** C (OOP-in-C patterns)
- **RTOS:** FreeRTOS
- **Protocols:** Modbus RTU (UART), MQTT (WiFi)
- **Cloud:** AWS IoT Core
- **Hardware Abstraction:** CMSIS register-level drivers; no STM32 HAL above the driver layer
- **Coding Standard:** BARR-C:2018 (subset — see [docs/coding-standard.md](docs/coding-standard.md))
- **Testing:** Unity + CMock via Ceedling

## Layer Scope and Priority

The implementation covers seven functional layers. Layers 1–5 are non-negotiable; Layers 6–7 are good-to-have.

| Layer | Name | Priority |
|-------|------|----------|
| 1 | Sensor Acquisition | Non-negotiable |
| 2 | Processing / State Machines | Non-negotiable |
| 3 | Modbus RTU | Non-negotiable |
| 4 | LCD | Non-negotiable |
| 5 | WiFi / MQTT | Non-negotiable |
| 6 | Bootloader OTA | Good-to-have |
| 7 | CI/CD extensions beyond bootstrap | Good-to-have |

If timeline pressure requires cuts, the cut order is: Layer 7 extras first, then Layer 6, then
Layer-5 polish (TLS cert rotation, advanced exception flows). The functional core of Layers 1–5
ships regardless.

## Project Structure

```file system
├── docs/                   Design documents (SRS, HLD, LLD, coding standard)
├── firmware/
│   ├── field-device/       STM32F469 Discovery firmware
│   ├── gateway/            B-L475E-IOT01A firmware
│   └── shared/             Code shared between both targets
├── tests/                  Unit and integration tests (Ceedling / Unity / CMock)
├── tools/                  Developer scripts (traceability checker, etc.)
├── design/                 Visual Paradigm UML models
└── .github/workflows/      CI/CD
```

## Development Environment

| Tool | Version |
|------|---------|
| STM32CubeIDE | 2.1.1 |
| STM32CubeMX | 6.14.x |
| Host OS | Windows |

## Build Instructions

See `docs/build-instructions.md` (to be added when CubeIDE projects land).

## Development Methodology

This project follows a **V-Model** lifecycle with model-based design:

1. System Requirements Specification (SRS)
2. High-Level Design / System Architecture (HLD)
3. Low-Level Design / Detailed Design (LLD)
4. Implementation ← *current phase*
5. Unit Testing
6. Integration Testing
7. System Testing

## Documentation

- [System Requirements Specification](docs/SRS.md)
- [High-Level Design](docs/hld/hld.md)
- [Low-Level Design](docs/lld/lld.md)
- [Coding Standard](docs/coding-standard.md)

## Hardware

- [B-L475E-IOT01A User Manual](https://www.st.com/resource/en/user_manual/um2153-discovery-kit-for-iot-node-multi-channel-communication-with-stm32l4-stmicroelectronics.pdf)
- [STM32F469 Discovery User Manual](https://www.st.com/resource/en/user_manual/um1932-discovery-kit-with-stm32f469ni-mcu-stmicroelectronics.pdf)

## Licence

MIT
