### UC-01: View Sensor Readings

**Actor:** Field Technician
**Preconditions:** 
- Field node is powered on, and initialised
**Postconditions:** 
- Field Technician has current sensor readings screen displayed on LCD

**Main Flow (Field Technician — local):**
1. Field Technician navigates to measure visualization screen on LCD.
2. System displays sensor measures on LCD.

**Exceptions:**
- E1: (step 2) if the sensor is in error, displays only the past data if available and a description of the sensor's errors.


### UC-02: View System Status

**Actor:** Field Technician
**Preconditions:** 
- Field node is powered on, and initialised
**Postconditions:** 
- Field Technician has current system status screen displayed on LCD

**Main Flow (Field Technician — local):**
1. Field Technician navigates to system status screen on LCD.
2. System displays resource usage, connectivity metrics, protocol reliability, and system status on LCD.

**Exceptions:**
- E1: (step 2) if the system is in error, the system will display a error message on LCD


### UC-03: View Active Alarms

**Actor:** Field Technician
**Preconditions:** 
- Field node is powered on, and initialised
**Postconditions:** 
- Field Technician has current active alarms screen displayed on LCD

**Main Flow (Field Technician — local):**
1. Field Technician navigates to alarms screen on LCD.
2. System displays active alarms, or indicates no alarms are active

**Exceptions:**


### UC-04: View Device Diagnostics

**Actor:** Field Technician
**Preconditions:** 
- Field node is powered on, and initialised
**Postconditions:** 
- Field Technician has the device diagnostic screen displayed on the console

**Main Flow (Field Technician — local):**
1. Field Technician connect to system CLI.
2. Field Technician send command for a device diagnostic 
3. System displays selected diagnostic on the console

**Exceptions:**
- E1: (step 3) if a diagnostic isn't available, the system will display a error message


### UC-05: View Telemetry Data

**Actor:** Remote Operator
**Preconditions:**
- Gateway is publishing telemetry to AWS IoT Core

**Postconditions:**
- Remote Operator has access to current sensor readings via the cloud

**Main Flow:**
1. Remote Operator accesses telemetry data via AWS IoT Core.
2. System presents the most recently published sensor readings 
   (temperature, humidity, pressure) with timestamps.

**Exceptions:**
- E1 (step 1): Gateway is offline — most recent published data 
  is available but may be stale. Timestamps indicate data age.
  

### UC-06: Monitor Device Health

**Actor:** Remote Operator
**Preconditions:**
- Gateway is publishing device health to AWS IoT Core

**Postconditions:**
- Remote Operator has access to current device health data via the cloud

**Main Flow:**
1. Remote Operator accesses device health data via AWS IoT Core.
2. System presents the most recently published device health.

**Exceptions:**
- E1 (step 1): Gateway is offline — most recent published data 
  is available but may be stale. Timestamps indicate data age.


### UC-07: Acquire Sensor Data

**Actor:** System
**Preconditions:**
- System is powered on, and initialised

**Postconditions:**
- system has new sensor data.

**Main Flow:**
1. System triggers a new sensor data acquisition at the configured polling interval
2. System get data from sensor
3. System process sensor data «include: Evaluate Sensor Alarms»
4. System updates Modbus registers and LCD display.

**Exceptions:**
- E1 (step 2): if some sensors don't respond, use a default error value


### UC-08: Evaluate Sensor Alarms

**Actor:** System
**Preconditions:**
- System is powered on, and initialised

**Postconditions:**
- Sensor data has been evaluated against alarm thresholds; alarm raised or cleared as appropriate.

**Main Flow:**
1. System receives new sensor data.
2. System compares value against configured thresholds.
3a. Value within range — no action (or clear existing alarm if applicable).
3b. Value outside range — system raises alarm and updates LCD alarm indicator.

**Exceptions:**


### UC-09: Receive Alarm Notification

**Actor:** Remote Operator
**Preconditions:**
- System is powered on, initialised and connected to internet

**Postconditions:**
- Remote Operator received an alarm notification

**Main Flow:**
1. System trigger an alarm notification
2. System send alarm notification over internet to Aws IoT

**Exceptions:**
- E1 (step 2): if the system is disconnected, buffer data in non-volatile storage; publish in chronological order when connectivity is restored.

### UC-10: Receive Sensor Telemetry

**Actor:** Aws IoT
**Preconditions:**
- System is powered on, initialised and connected to internet

**Postconditions:**
- AWS IoT Core has received current sensor telemetry.

**Main Flow:**
1. System trigger a new remote data update event
2. System send sensors data to Aws Iot

**Exceptions:**
- E1 (step 2): if the system is disconnected, buffer data in non-volatile storage; publish in chronological order when connectivity is restored.


### UC-11: Receive Device Health Data

**Actor:** Aws IoT
**Preconditions:**
- System is powered on, initialised and connected to internet

**Postconditions:**
- Aws Iot received device health data

**Main Flow:**
1. System trigger a new device health data update event
2. System send device health data to Aws Iot

**Exceptions:**
- E1 (step 2): if the system is disconnected, buffer data in non-volatile storage; publish in chronological order when connectivity is restored.

### UC-12: Receive Alarm Events

**Actor:** Aws IoT
**Preconditions:**
- System is powered on, initialised and connected to internet

**Postconditions:**
- Aws Iot received alarm event

**Main Flow:**
1. System trigger a new alarm update event
2. System send alarm update event to Aws Iot

**Exceptions:**
- E1 (step 2): if the system is disconnected, buffer data in non-volatile storage; publish in chronological order when connectivity is restored.


### UC-13: Synchronise Time

**Actor:** System
**Preconditions:**
- System is powered on, initialised and connected to internet

**Postconditions:**
- System has time synchronized

**Main Flow:**
1. The system trigger an time sync event upon boot and periodically thereafter.
2. System get a time update from NTP
3. System set this value to the internal timer
4. System write this value to the field node using Modbus holding register.

**Exceptions:**
- E1 (step 2): if the system is disconnected, set an event to retry as soon the system is connected
- E2 (step 4): if the field node is disconnected, set an event to retry as soon the field node is connected


### UC-14: Request Sensor Reading

**Actor:** Remote Operator
**Preconditions:**
- System is powered on, initialised and connected to internet

**Postconditions:**
- Fresh sensor reading has been published to AWS IoT Core.

**Main Flow:**
1. The Remote Operator request a sensor data reading
2. System execute new data reading
3. System returns result.

**Exceptions:**
- E1 (step 1): if the gateway is disconnected, show an command is not delivered to the gateway
- E2 (step 3): if the gateway is disconnected, system cannot deliver result to AWS IoT Core; result is buffered.


### UC-15: Configure Operational Parameters

**Actor:** Field Technician, Remote Operator
**Preconditions:**
- The Field Technician: System is powered on and initialised 
- The Remote Operator: System is powered on, initialised and connected to internet

**Postconditions:**
- The Field Technician has configured operational parameters
- The Remote Operator has configured operational parameters

**Main Flow - Field Technician:**
1. The Field Technician is on operational parameter setting on the LCD
2. The Field Technician configure operational parameters: polling rate, alarm thresholds, and display settings.
3. System check the input value and ask for confirmation
4. The Field Technician confirm
5. The system update the operational parameters and save the parameter values.

**Alternative Flow - Remote Operator:**
1. The Remote Operator configure new operational parameter: polling rate, and alarm thresholds
2. The system receive the request
3. System check the input value 
4. The system update the operational parameters and save the parameter values.
5. The system send a response to the Aws Iot

**Exceptions:**
- E1 (main flow step 5): if the system is unable to update operational parameter, the previous value should be used
- E2 (alternate flow 2): if the gateway is disconnected, system raise an error
- E3 (alternate flow 3): if the system is unable to update operational parameter, the previous value should be used


### UC-16: Provision Device

**Actor:** Field Technician
**Preconditions:**
- System is powered on and initialised

**Postconditions:**
- The Field Technician has provisioned the device

**Main Flow**
1. The Field Technician connect to system console
2. The Field Technician set WiFi credentials or cloud endpoint or Modbus address or serial port parameters or cloud certificates
3. System check the input value and ask for confirmation
4. The Field Technician confirm
5. The system update the provision parameters and save these values.

**Exceptions:**
- E1 (step 5): if the system is unable to update some parameter, the previous value should be used and an error should be raised


### UC-17: Request Restart Device

**Actor:** Remote Operator
**Preconditions:**
- System is powered on, initialised and connected to internet

**Postconditions:**
- The system has been restarted

**Main Flow**
1. The Remote Operator request a device restart
2. The gateway receive the command
3. The system acknowledges receipt and requests confirmation
4. The Remote Operator confirms
5. The system executes restart


**Exceptions:**
- E1 (step 2): if the gateway is not connected, raise an connection error.


### UC-18: Deploy Firmware Update

**Actor:** Remote Operator
**Preconditions:**
- System is powered on, initialised and connected to internet

**Postconditions:**
- The device has a new firmware version.
- Previous firmware is retained for rollback.

**Main Flow**
1. The Remote Operator request a firmware update
2. The gateway receive the command
3. The system download the firmware image
4. The system validate the firmware image «include: Verify Firmware Integrity»
4a. The firmware image is valid
4a1. The system set the firmware partition as boot partition
4a2. The system reboots
4a3. The new firmware performs a self-check
4a4. The system send an ack to the Aws Iot
4b1. The firmware image is not valid
4b2. The firmware image is deleted
4b3. The system send a nack to the Aws Iot

**Exceptions:**
- E1 (step 1): if the gateway is not connected, raise an connection error.
- E2 (step 3): if the connection is lost, save the firmware data and continue to download when the connection come back.


### UC-19: Forward Remote Commands

**Actor:** Aws Iot 
**Preconditions:**
- System is powered on, initialised and connected to internet

**Postconditions:**
- The Aws Iot has a forwarded a remote command

**Main Flow**
1. AWS IoT Core delivers a remote command to the gateway via MQTT.
2. Gateway validates the command.
3. Gateway executes the command or routes it to the field device.
4. Gateway sends result back to AWS IoT Core.

**Exceptions:**
- E1 (step 1): if the gateway is not connected, raise an connection error.


### UC-20: Verify Firmware Integrity

**Actor:** the system
**Preconditions:**
- System is powered on, and initialised

**Postconditions:**
- The system has verified the firmware integrity

**Main Flow**
1. System calculates cryptographic signature of the firmware image.
2. System compares against expected signature.
3a. Match — verification passes.
3b. Mismatch — verification fails, system reports error.

**Exceptions:**