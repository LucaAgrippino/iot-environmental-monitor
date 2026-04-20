### UC-01: View Sensor Readings

**Actor:** Field Technician

**Preconditions:** 
- Field node is powered on, and initialised

**Postconditions:** 
- Field Technician has current sensor measurements screen displayed on LCD

**Main Flow (Field Technician — local):**
1. Field Technician navigates to measurement visualization screen on LCD.
2. System displays sensor measurement on LCD.

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
- E1: (step 2) if the system is in error, the system will display an error message on LCD


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
1. Field Technician connects to system CLI.
2. Field Technician sends command for a device diagnostic 
3. System displays selected diagnostic on the console

**Exceptions:**
- E1: (step 2) if the command fails, the system will display an error message
- E2: (step 3) if a diagnostic isn't available, the system will display an error message


### UC-05: View Telemetry Data

**Actor:** Remote Operator

**Preconditions:**
- Gateway is publishing telemetry to AWS IoT Core

**Postconditions:**
- Remote Operator has access to current sensor measurement via the cloud

**Main Flow:**
1. Remote Operator accesses telemetry data via AWS IoT Core.
2. System presents the most recently published sensor measurement 
   (temperature, humidity, pressure, accelerometer, gyroscope, and magnetometer) with timestamps.

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
- System has new sensor data.

**Main Flow - Field Node:**
1. System triggers a new sensor data acquisition at the configured polling interval
2. System gets data from sensor
3. System processes sensor data «include: Evaluate Sensor Alarms»
4. System updates Modbus registers and LCD display.

**Main Flow - Gateway Node:**
1. System triggers a new sensor data acquisition at the configured polling interval
2. System gets data from sensor
3. System processes sensor data «include: Evaluate Sensor Alarms»

**Exceptions:**
- E1 (step 2): if some sensors don't respond on time, use a default error value and log the error event


### UC-08: Evaluate Sensor Alarms

**Actor:** System

**Preconditions:**
- System is powered on, and initialised

**Postconditions:**
- Sensor data has been evaluated against alarm thresholds; alarm raised or cleared as appropriate

**Main Flow:**
1. System receives new sensor data
2. System compares value against configured thresholds
3. Value within range — no action, clear existing alarm if applicable, applying hysteresis
4. Value outside range — system raises alarm and updates LCD alarm indicator and publishes alarm event to the cloud

**Exceptions:**


### UC-09: Receive Alarm Notification

**Actor:** Remote Operator

**Preconditions:**
- System is powered on, initialised and connected to internet

**Postconditions:**
- Remote Operator received an alarm notification

**Main Flow:**
1. System triggers an alarm notification
2. System sends alarm notification over internet to AWS IoT Core

**Exceptions:**
- E1 (step 2): if the system is disconnected, buffer data in non-volatile storage; publish in chronological order when connectivity is restored.

### UC-10: Receive Sensor Telemetry

**Actor:** AWS IoT Core

**Preconditions:**
- System is powered on, initialised and connected to internet

**Postconditions:**
- AWS IoT Core has received current sensor telemetry.

**Main Flow:**
1. System triggers a new remote data update event
2. System sends sensor data to AWS IoT Core

**Exceptions:**
- E1 (step 2): if the system is disconnected, buffer data in non-volatile storage; publish in chronological order when connectivity is restored.


### UC-11: Receive Device Health Data

**Actor:** AWS IoT Core

**Preconditions:**
- System is powered on, initialised and connected to internet

**Postconditions:**
- AWS IoT Core received device health data

**Main Flow:**
1. System triggers a new device health data update event
2. System sends device health data to AWS IoT Core

**Exceptions:**
- E1 (step 2): if the system is disconnected, buffer data in non-volatile storage; publish in chronological order when connectivity is restored.

### UC-12: Receive Alarm Events

**Actor:** AWS IoT Core

**Preconditions:**
- System is powered on, initialised and connected to internet

**Postconditions:**
- AWS IoT Core received alarm event

**Main Flow:**
1. System triggers a new alarm update event
2. System sends alarm update event to AWS IoT Core

**Exceptions:**
- E1 (step 2): if the system is disconnected, buffer data in non-volatile storage; publish in chronological order when connectivity is restored.


### UC-13: Synchronise Time

**Actor:** System

**Preconditions:**
- System is powered on, initialised and connected to internet

**Postconditions:**
- System has time synchronized

**Main Flow:**
1. The system triggers a time sync event upon boot and periodically thereafter.
2. System gets a time update from NTP
3. System sets this value to the internal timer
4. System writes this value to the field node using Modbus holding register.

**Exceptions:**
- E1 (step 2): if the system is disconnected, set an event to retry as soon the system is connected
- E2 (step 4): if the field node is disconnected, set an event to retry as soon the field node is connected


### UC-14: Request Sensor Reading

**Actor:** Remote Operator

**Preconditions:**
- System is powered on, initialised and connected to internet

**Postconditions:**
- Fresh sensor measurement has been published to AWS IoT Core.

**Main Flow:**
1. The Remote Operator requests a sensor measurement
2. System executes new measurement
3. System returns result.

**Exceptions:**
- E1 (step 1): if the gateway is disconnected, show that the command is not delivered to the gateway
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
2. The Field Technician configures operational parameters: polling rate, alarm thresholds, and display settings.
3. System checks the input value and asks for confirmation
4. The Field Technician confirms
5. The system updates the operational parameters and saves the parameter values.

**Alternative Flow - Remote Operator:**
1. The Remote Operator configures new operational parameter: polling rate, and alarm thresholds
2. The system receives the request
3. System validates the input values
4. The system updates the operational parameters and saves the parameter values.
5. The system sends a response to the AWS IoT Core

**Exceptions:**
- E1 (main flow step 5): if the system is unable to update operational parameter, the previous value should be used
- E2 (alternate flow 2): if the gateway is disconnected, system raises an error
- E3 (alternate flow 3): if the system is unable to update operational parameter, the previous value should be used
- E4 (main flow 4): Field Technician declines — system discards changes, retains current values


### UC-16: Provision Device

**Actor:** Field Technician

**Preconditions:**
- System is powered on and initialised

**Postconditions:**
- The Field Technician has provisioned the device

**Main Flow**
1. The Field Technician connects to system console
2. The Field Technician sets WiFi credentials or cloud endpoint or Modbus address or serial port parameters or cloud certificates
3. System checks the input value and asks for confirmation
4. The Field Technician confirms
5. The system updates the provision parameters and saves these values.

**Exceptions:**
- E1 (step 5): if the system is unable to update some parameter, the previous value should be used and an error should be raised
- E2 (step  4): Field Technician declines — system discards changes, retains current values

### UC-17: Request Restart Device

**Actor:** Remote Operator

**Preconditions:**
- System is powered on, initialised and connected to internet

**Postconditions:**
- The gateway has been restarted

**Main Flow**
1. The Remote Operator requests a device restart
2. The gateway receives the command
3. The system acknowledges receipt and requests confirmation
4. The Remote Operator confirms
5. The system executes restart


**Exceptions:**
- E1 (step 2): if the gateway is not connected, raise a connection error.
- E2 (step  4): system cancels the restart if the remote operator declines


### UC-18: Deploy Firmware Update

**Actor:** Remote Operator

**Preconditions:**
- System is powered on, initialised and connected to internet

**Postconditions:**
- The device has a new firmware version.
- Previous firmware is retained for rollback.

**Main Flow**

1. The Remote Operator requests a firmware update
2. The gateway receives the command
3. The system downloads the firmware image
4. The system validates the firmware image «include: Verify Firmware Integrity»

**Sunny Day Flow**

5. The firmware image is valid
6. The system sets the firmware partition as boot partition
7. The system reboots
8. The new firmware performs a self-check
9. The system sends an ack to the AWS IoT Core

**Rainy Day Flow**

5. The firmware image is not valid
6. The firmware image is deleted
7. The system sends a nack to the AWS IoT Core

**Exceptions:**
- E1 (step 1): if the gateway is not connected, raise a connection error.
- E2 (step 3): if the connection is lost, save the firmware data and continue to download when the connection is restored
- E3 (sunny day flow step 8): Self-check fails after reboot — system rolls back to previous firmware
- E4 (rainy day flow step 6): Flash write fails during deletion — system logs error and retains current firmware


### UC-19: Forward Remote Commands

**Actor:** AWS IoT Core 

**Preconditions:**
- System is powered on, initialised and connected to internet

**Postconditions:**
- The AWS IoT Core has forwarded a remote command

**Main Flow**
1. AWS IoT Core delivers a remote command to the gateway via MQTT.
2. Gateway validates the command.
3. Gateway executes the command or routes it to the field device.
4. Gateway sends result back to AWS IoT Core.

**Exceptions:**
- E1 (step 1): If the gateway is not connected, raise a connection error.
- E2 (step 2): Invalid or unrecognised command format — gateway rejects and logs error
- E3 (step 3): Field device unreachable via Modbus — gateway reports failure to cloud
- E4 (step 4): Gateway disconnected from cloud — result is buffered


### UC-20: Verify Firmware Integrity

**Actor:** The System

**Preconditions:**
- System is powered on, and initialised

**Postconditions:**
- The system has verified the firmware integrity

**Main Flow**
1. System calculates cryptographic signature of the firmware image.
2. System compares against expected signature.

**Sunny Day Flow**

3. Match — verification passes.

**Rainy Day Flow**

3. Mismatch — verification fails, system reports error.

**Exceptions:**
- E1 (step 1): If the cryptographic calculation fails due to a read error, the system logs the error and rejects the firmware image