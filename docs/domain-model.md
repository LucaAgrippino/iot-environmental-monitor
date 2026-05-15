
# Introduction

This document defines the domain entities and relationships used throughout the HLD. It is the vocabulary for all subsequent design artefacts (component diagrams, sequence diagrams, Modbus register map, FreeRTOS task breakdown). The accompanying diagram is at docs/diagrams/domain-model.png.

# Step 3 — Domain Entities

## Core data entities

### Entity Device

- description: A physical or simulated node in the system, identified by a unique serial number, that holds configuration and produces data
- attributes:
  - serial_number: Identifier
  - operational_state: Enumeration (Normal, Degraded, Fault)
  - firmware_version: Version
  - time_sync_state: Enumeration (Synchronised, Unsynchronised, Unknown)

### Entity Sensor

- description: A named input source that produces readings of a specific measurement type and unit
- attributes:
  - id: Identifier
  - unit: Unit
  - range_min: Magnitude
  - range_max: Magnitude
  - type: Enumeration (Temperature, Humidity, Pressure, Accelerometer, Gyroscope, Magnetometer)

### Entity SensorReading

- description: A processed measurement value, or set of values, produced by a Sensor at a specific moment
- attributes:
  - timestamp: Timestamp
  - reading_id: Identifier
  - sync_state: Enumeration (Synchronised, Unsynchronised)
  - status: Enumeration (Valid, Clamped, Invalid)

### Entity MeasurementValue

- description: A value with a unit and an optional axis label that represents one component of a sensor reading
- attributes:
  - value: Magnitude
  - unit: Unit
  - axis_label: Enumeration (None, X, Y, Z)

### Entity AlarmThreshold

- description: A maximum or minimum value within which a sensor reading should remain
- attributes:
  - max_value: Magnitude
  - min_value: Magnitude
  - hysteresis: Magnitude

### Entity Alarm

- description: An event raised when a SensorReading crosses a configured AlarmThreshold
- attributes:
  - sensor_id: Identifier
  - alarm_type: Enumeration (HighThreshold, LowThreshold)
  - measured_value: Magnitude
  - threshold_value: Magnitude
  - status: Enumeration (Active, Cleared)
  - raised_at: Timestamp
  - cleared_at: Timestamp


## Configuration and commands

### Entity Configuration

- description: Holds all persistent operational and provisioning parameters for a Device
- attributes:
  - telemetry_interval: Duration
  - health_interval: Duration
  - wifi_credentials: Credential
  - cloud_endpoint: Endpoint
  - cloud_certificate: Certificate
  - modbus_address: Identifier
  - serial_port_params: SerialParameters
  - display_settings: DisplaySettings
  - polling_interval: Duration

### Entity Command

- description: Represents an instruction issued to the system that triggers an action and produces a result
- attributes:
  - payload: CommandPayload
  - received_at: Timestamp
  - result: Message
  - source: Enumeration (Local, Remote)
  - type: Enumeration (ReadRequest, RestartRequest, ConfigurationChange, ProvisioningUpdate, DiagnosticRequest)
  - tier: Enumeration (Tier1, Tier2, Tier3)
  - status: Enumeration (Received, Validated, Executed, Rejected, Failed)


## Communication artefacts

### Entity Telemetry

- description: Represents a bundled set of SensorReadings published to the cloud in a single MQTT message
- attributes:
  - telemetry_id: Identifier
  - device_serial_number: Identifier
  - timestamp: Timestamp
  - schema_version: Version

### Entity BufferedRecord

- description: Contains any outbound cloud message awaiting transmission: telemetry, alarms, or device health
- attributes:
  - record_id: Identifier
  - sequence_number: Count
  - buffered_at: Timestamp
  - status: Enumeration (Pending, Sent, Failed)
  - payload_type: Enumeration (Telemetry, Alarm, DeviceHealth)


## Diagnostic and lifecycle

### Entity DeviceHealthSnapshot

- description: Aggregate device status and diagnostic data
- attributes:
  - timestamp: Timestamp
  - uptime: Duration
  - free_heap: ByteSize
  - cpu_load: Percentage
  - wifi_rssi: SignalStrength
  - wifi_reconnection_count: Count
  - mqtt_failure_count: Count
  - modbus_crc_error_count: Count
  - modbus_timeout_count: Count
  - modbus_success_count: Count
  - mcu_temperature: Magnitude
  - buffer_occupancy: Percentage
  - stack_watermarks: Dictionary<TaskName, ByteSize>

### Entity LogEntry

- description: Represents a recorded system event, with severity, subsystem, and a human-readable message
- attributes:
  - timestamp: Timestamp
  - message: Message
  - severity: Enumeration (Info, Warning, Error, Fatal)
  - subsystem: Enumeration (SensorAcquisition, Modbus, WiFi, Cloud, Flash, System)

### Entity FirmwareImage

- description: Represents an executable firmware binary, versioned and cryptographically signed, that can be installed on a Device
- attributes:
  - version: Version
  - size: ByteSize
  - signature: CryptographicSignature
  - state: Enumeration (Pending, Verified, Installed, Active, Rolled-back)


# Step 4 — Relationships

## Core data domain

### Device — Sensor
- Device --aggregation-- Sensor    1 : 3..6

### Device — Configuration
- Device --composition-- Configuration    1 : 1

### Sensor — SensorReading
- Sensor --association-- SensorReading    1 : 0..*    (produced by)

### SensorReading — MeasurementValue
- SensorReading --composition-- MeasurementValue    1 : 1..3    (reading is composed of 1 scalar or 3 axes)

### Sensor — AlarmThreshold
- Sensor --association-- AlarmThreshold    1 : 0..1    (applies to)

### Configuration — AlarmThreshold
- Configuration --composition-- AlarmThreshold    1 : 0..*    (configures)

### SensorReading — Alarm
- SensorReading --- Alarm    not related    (denormalised into Alarm attributes)

### AlarmThreshold — Alarm
- AlarmThreshold --- Alarm    not related    (denormalised into Alarm attributes)

### Device — Alarm
- Device --aggregation-- Alarm    1 : 0..*    (raised by)


## Communication

### Telemetry — SensorReading
- Telemetry --aggregation-- SensorReading    1 : 1..*    (contains)

### Device — Telemetry
- Device --aggregation-- Telemetry    1 : 0..*    (produced by)

### BufferedRecord — Telemetry
- BufferedRecord --composition-- Telemetry    1 : 0..1    (invariant: exactly one payload)

### BufferedRecord — Alarm
- BufferedRecord --composition-- Alarm    1 : 0..1

### BufferedRecord — DeviceHealthSnapshot
- BufferedRecord --composition-- DeviceHealthSnapshot    1 : 0..1

### Device — BufferedRecord
- Device --aggregation-- BufferedRecord    1 : 0..*    (buffered by)


## Commands and diagnostics

### Device — Command
- Device --aggregation-- Command    1 : 0..*    (received by)

### Command — Configuration
- Command --- Configuration    not related    (behaviour, not structure)

### Device — DeviceHealthSnapshot
- Device --aggregation-- DeviceHealthSnapshot    1 : 0..*    (produced by)

### Device — LogEntry
- Device --aggregation-- LogEntry    1 : 0..*    (logged by)

### Device — FirmwareImage
- Device --aggregation-- FirmwareImage    1 : 1..3    (active / rollback / pending)


# Modelling Decisions

This section documents the rationale for modelling choices that are not self-evident from the diagram alone. Each decision reflects a deliberate trade-off; alternatives were considered and rejected for the reasons given.

---

### SensorReading is composed of MeasurementValues rather than generalised into scalar and vector subtypes

The gateway reads six sensors: temperature, humidity, and pressure produce scalar values, while the accelerometer, gyroscope, and magnetometer each produce three-axis vector values (REQ-SA-071). Two modelling approaches were considered:

1. Generalise Sensor into ScalarSensor and VectorSensor, with corresponding reading subtypes.
2. Model a single SensorReading composed of one or more MeasurementValue instances, where scalar readings contain one value and vector readings contain three with axis labels.

Approach 2 is adopted. The structural difference between scalar and vector readings is expressible as cardinality (one vs three MeasurementValues) rather than as a type hierarchy. Preferring composition over generalisation when a variation is a cardinality statement keeps the model simpler, avoids propagating inheritance down into the C implementation, and extends naturally to future sensor shapes (for example two-axis or spectral) without requiring new subtypes.

---

### Alarm is denormalised from SensorReading and AlarmThreshold

An Alarm is raised when a SensorReading crosses an AlarmThreshold (REQ-AM-000, REQ-AM-020). A relational model would give Alarm foreign-key references to both. Instead, Alarm carries measured_value, threshold_value, sensor_id, and raised_at as its own attributes (REQ-AM-040).

This denormalisation is deliberate. SensorReadings are bounded in number (REQ-SA-090 retains only the most recent N per sensor) and older readings are discarded. AlarmThresholds are mutable (Tier 2 configuration, REQ-DM-000). If Alarm held references instead of copied values, the originating reading might no longer exist when the Alarm is inspected, and the threshold value might have changed since the alarm was raised. Capturing the values at the moment of the event preserves historical accuracy.

The denormalisation appears in the diagram as two explicit "not related" decisions: Alarm is not structurally linked to SensorReading or to AlarmThreshold.

---

### Command is a single entity with a source attribute rather than separate LocalCommand and RemoteCommand

Commands originate from two paths: local (Field Technician via CLI or LCD) and remote (Remote Operator via cloud, REQ-DM-000, REQ-NF-306). The two paths cross different trust boundaries and carry different tier capabilities (Vision §10).

Separating them into LocalCommand and RemoteCommand subtypes was considered and rejected. The two would share identical attributes and an identical lifecycle (Received → Validated → Executed → Rejected | Failed) — their only difference is where they originate. When the sole distinction between candidate subtypes is a value, the correct model is a single entity with a discriminator attribute.

The trust-boundary distinction is already captured on the Deployment Diagram. The tier invariant (Tier 1 commands must have source = Local) is expressible as a constraint on the single Command entity. Duplicating the local/remote distinction in the domain model would redundantly encode information that lives on the deployment view.

---

### Telemetry is not a polymorphic container of cloud payloads

Telemetry specifically represents a bundle of SensorReadings published to the cloud as sensor telemetry (REQ-CC-000, REQ-CC-030). Alarms (REQ-CC-020) and DeviceHealthSnapshots (REQ-CC-010) are published as separate message types on separate MQTT topics (REQ-CC-080).

Modelling Telemetry as a polymorphic container of all three payload types would misrepresent the system. The polymorphism applies one layer up, at the buffering layer.

---

### BufferedRecord uses three composition relationships with an invariant, not generalisation

REQ-BF-000 requires that all outbound cloud messages (telemetry, alarms, device health) be buffered in non-volatile storage when internet connectivity is lost. BufferedRecord is therefore polymorphic across three payload types.

Three options were considered:

1. Generalise the three payload types into a common CloudPayload supertype.
2. Inline all three payload shapes into BufferedRecord with nullable attribute groups.
3. Model three separate composition relationships from BufferedRecord, each with multiplicity 0..1, plus an invariant constraining exactly one to be non-null.

Option 3 is adopted. Option 1 introduces a generalisation used only once in the model, adding hierarchy for a single relationship. Option 2 collapses three distinct entities into one wide entity, losing their independent structure and semantics. Option 3 preserves each payload type's identity while accurately reflecting the one-of-three constraint. The constraint is expressed as a UML note attached to BufferedRecord, as UML does not express xor-relationships natively.

The payload_type attribute on BufferedRecord indicates which of the three composition links is populated for a given record.

---

### SimulatedSensor is not a domain entity

Vision §5.1.1 commits the architecture to a driver abstraction in which simulated sensors implement the same driver interface as real I²C sensor drivers. The simulation is confined to the driver layer and is invisible above it.

Introducing a SimulatedSensor entity in the domain model would contradict this architectural decision by leaking an implementation concern (how sensor data is produced) into the domain layer. The domain reasons about Sensors and SensorReadings without reference to how those readings originate. The simulation module is properly represented in the Component Diagram as a driver-layer component implementing the Sensor Driver interface.

---

### LCD and Serial CLI are not domain entities

The Field Technician interacts with the device locally through two interfaces: the LCD touchscreen (field node) and the Serial CLI (both nodes). Modelling these as domain entities or as subtypes of a local-interface abstraction was considered and rejected.

Both interfaces consume and produce the same domain concepts: Sensors, SensorReadings, Alarms, Configurations, Commands. At the domain level, the mechanism of interaction is irrelevant — what flows through each interface is what matters, and those flows are already modelled. The interfaces themselves are presentation and input mechanisms, properly represented in the Component Diagram.

---

### Time is modelled as a value type, not as an entity

Timestamps appear on many entities (SensorReading, Alarm, LogEntry, Telemetry, BufferedRecord, DeviceHealthSnapshot, Command). Introducing Time as a first-class entity was considered and rejected.

Entities have identity independent of their attribute values and have a lifecycle. Value types have no independent identity — two timestamps holding the same value are the same value, and they exist only as properties of the entities that carry them. By this test, Timestamp is a value type.

The system does reason about time synchronisation state, which is captured as attributes on the relevant entities: Device.time_sync_state for the current RTC sync state, and SensorReading.sync_state to flag whether a specific reading was taken while the RTC was synchronised (REQ-NF-212, REQ-TS-040).

---

### Configuration is a single entity covering all tiers

Vision §10 defines three configuration tiers (provisioning, operational, commands). Configuration is modelled as a single entity holding all persistent parameters across Tiers 1 and 2, rather than as separate ProvisioningConfiguration and OperationalConfiguration entities.

Tier is an access-control concern (which interface is allowed to change which parameter), not a structural concern. All configuration parameters share the same persistence mechanism (REQ-DM-090), the same storage location (non-volatile flash), and the same lifecycle (loaded at boot, modified by authorised commands, persisted on change). Splitting them into separate entities would create a structural distinction where only a policy distinction exists.

Tier 3 is not configuration at all — it comprises commands (restart, immediate read), which are modelled as Command instances.

---

### Identifiers are used as references between entities

Several entities carry identifier attributes that reference other entities: SensorReading.reading_id is its own identifier, while Alarm.sensor_id, Telemetry.device_serial_number, and similar attributes are references to other entities. These appear alongside formal relationships on the diagram.

This duplication is deliberate. The relationships on the diagram carry multiplicity and lifecycle information; the identifier attributes carry the reference itself. Together they document both the structural connection and the way it is realised in implementation. When an entity's name is used as an identifier on another (for example device_serial_number matching Device.serial_number), naming is kept consistent to avoid vocabulary drift.
