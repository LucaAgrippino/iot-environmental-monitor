# IoT Environmental Monitoring Gateway

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
- **Hardware Abstraction:** Mixed HAL + register-level with clean driver interfaces
- **Coding Standard:** BARR-C:2018 (subset)
- **Testing:** Unity (ThrowTheSwitch.org)

## Project Structure

```
├── docs/                   Design documents (SRS, HLD, LLD)
├── firmware/
│   ├── field-device/       STM32F469 Discovery firmware
│   └── gateway/            B-L475E-IOT01A firmware
├── design/                 Visual Paradigm UML models
├── tests/                  Unit and integration tests
└── .github/workflows/      CI/CD (planned)
```

## Development Methodology

This project follows a **V-Model** lifecycle with model-based design:

1. System Requirements Specification (SRS)
2. High-Level Design / System Architecture (HLD)
3. Low-Level Design / Detailed Design (LLD)
4. Implementation
5. Unit Testing
6. Integration Testing
7. System Testing

## Documentation

- [System Requirements Specification](docs/SRS.md)
- [High-Level Design](docs/HLD.md)
- [Low-Level Design](docs/LLD.md)
- [Modbus Register Map](docs/modbus-register-map.md)

## Hardware

- [B-L475E-IOT01A User Manual](https://www.st.com/resource/en/user_manual/um2153-discovery-kit-for-iot-node-multi-channel-communication-with-stm32l4-stmicroelectronics.pdf)
- [STM32F469 Discovery User Manual](https://www.st.com/resource/en/user_manual/um1932-discovery-kit-with-stm32f469ni-mcu-stmicroelectronics.pdf)

## Licence

MIT
