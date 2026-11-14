*** Settings ***
Documentation     Validation, Remote Operator use cases through the local cloud API (iotcloud on the Raspberry Pi stands in
...               for AWS IoT Core). Each test case is one use case scenario with the actor's goal as the pass criterion.
...               Needs VAL_PASS (the validator account password) in the environment and the boards running.
Library           ../libraries/ValCloud.py
Library           ../libraries/ValBoards.py
Library           Collections
Suite Setup       Start Session
Force Tags        validation

*** Keywords ***
Start Session
    ${status}=    Login
    Should Be Equal As Integers    ${status}    200
    Install Helpers

Restore Alarm Threshold
    Fd Set Config    temp-alarm-high    4200

*** Test Cases ***
UC-05 Operator sees the latest measurements with timestamps
    [Tags]    UC-05
    ${d}=    Device View
    ${l}=    Set Variable    ${d}[latest]
    Should Not Be Equal    ${l}    ${None}
    FOR    ${k}    IN    temperature_deci_c    humidity_pct    pressure_hpa    fd_temperature_deci_c    fd_humidity_pct    fd_pressure_hpa    ts
        Should Not Be Equal    ${l}[${k}]    ${None}    ${k} is missing
    END
    ${age}=    Evaluate    int(time.time()) - ${l}[ts]    modules=time
    Should Be True    ${age} < 150    the newest reading is ${age} s old, the device should be online

UC-05 The published payload carries the motion and magnetic sensors
    [Documentation]    The gateway publishes accelerometer, gyroscope and magnetometer (REQ-CC-000); the operator can get
    ...                them from the download of the raw telemetry.
    [Tags]    UC-05
    ${t}=    Now
    ${raw}=    Raw Telemetry    ${t - 600}
    ${last}=    Set Variable    ${raw}[-1]
    FOR    ${k}    IN    accel_x_mg    accel_y_mg    accel_z_mg    gyro_x_mdps    gyro_y_mdps    gyro_z_mdps    mag_x_mgauss    mag_y_mgauss    mag_z_mgauss
        Dictionary Should Contain Key    ${last}    ${k}
    END

UC-05 The API view presents accelerometer, gyroscope and magnetometer
    [Documentation]    UC-05 step 2 lists them. The device view and the web overview show temperature, humidity and
    ...                pressure only: finding F-2.
    [Tags]    UC-05    finding-F-2
    ${d}=    Device View
    ${l}=    Set Variable    ${d}[latest]
    ${keys}=    Evaluate    ' '.join(${l}.keys())
    Should Contain    ${keys}    accel    the device view has no accelerometer value

UC-06 Operator sees the device health
    [Tags]    UC-06
    ${d}=    Device View
    ${h}=    Set Variable    ${d}[health]
    FOR    ${k}    IN    uptime_s    lifecycle_state    sensor_fail_count    alarm_raise_count    fw_version    sync_state
        Dictionary Should Contain Key    ${h}    ${k}
    END
    Should Be Equal    ${h}[lifecycle_state]    operational
    ${age}=    Evaluate    int(time.time()) - ${h}[timestamp]    modules=time
    Should Be True    ${age} < 150

UC-10 Telemetry arrives at the configured period
    [Documentation]    Only the time since the Gateway's last start counts (a restart is a legitimate gap). A read request
    ...                (UC-14) adds a telemetry message, so a gap may be shorter than the period; it must never be longer.
    [Tags]    UC-10
    ${boot}=    Boot Time
    ${rows}=    History    telemetry    since=${boot + 30}
    ${n}=    Get Length    ${rows}
    Should Be True    ${n} >= 4    only ${n} telemetry messages since the last start (wait a few minutes after a restart)
    ${gaps}=    Evaluate    [b['ts'] - a['ts'] for a, b in zip(${rows}, ${rows}[1:])]
    ${bad}=    Evaluate    [g for g in ${gaps} if g > 70]
    Should Be Empty    ${bad}    gaps longer than the 60 s period: ${bad}

UC-11 Health data arrives regularly
    [Tags]    UC-11
    ${t}=    Now
    ${rows}=    History    health    since=${t - 300}
    ${n}=    Get Length    ${rows}
    Should Be True    ${n} >= 5    only ${n} health messages in 5 minutes

UC-08 UC-09 UC-12 An alarm on the field device reaches the operator and clears
    [Documentation]    The Field Technician's threshold is lowered below the reading (console), the alarm must appear in
    ...                the cloud (HIGH, source field_device) within 5 s, and clear (CLEAR) when the threshold is restored.
    [Tags]    UC-08    UC-09    UC-12
    [Teardown]    Restore Alarm Threshold
    ${t0}=    Now
    ${t0f}=    Evaluate    time.time()    modules=time
    Fd Set Config    temp-alarm-high    2000
    ${row}=    Wait For    alarms    ${t0 - 1}    alarm_type    HIGH    timeout=15
    ${t1f}=    Evaluate    time.time()    modules=time
    Should Be Equal    ${row}[source]    field_device
    Should Be Equal    ${row}[sensor_id]    temperature
    ${d}=    Device View
    Should Not Be Empty    ${d}[active_alarms]
    ${dt}=    Evaluate    round(${t1f} - ${t0f}, 1)
    Log    seconds from the commit command to the alarm in the cloud: ${dt}
    ${t2}=    Now
    Fd Set Config    temp-alarm-high    4200
    ${clear}=    Wait For    alarms    ${t2 - 1}    alarm_type    CLEAR    timeout=15
    Should Be Equal    ${clear}[sensor_id]    temperature
    ${d}=    Device View
    Should Be Empty    ${d}[active_alarms]

UC-14 A read request produces a fresh measurement
    [Tags]    UC-14
    ${t}=    Now
    ${status}    ${row}=    Send    control    ${{ {"command": "read"} }}
    Should Be Equal As Integers    ${status}    202
    ${done}=    Command Result    ${row}[id]    timeout=20
    Should Be Equal    ${done}[status]    accepted
    ${tel}=    Wait For Telemetry After    ${t}    timeout=30
    Should Be True    ${tel}[recv_ts] >= ${t}

UC-15 Operator changes a setting, the device applies it and keeps it valid
    [Tags]    UC-15
    [Teardown]    Send    config    ${{ {"telemetry_interval_ms": 60000} }}
    ${status}    ${row}=    Send    config    ${{ {"telemetry_interval_ms": 30000} }}
    Should Be Equal As Integers    ${status}    202
    ${done}=    Command Result    ${row}[id]    timeout=20
    Should Be Equal    ${done}[status]    applied
    ${t}=    Now
    Sleep    80s
    ${rows}=    History    telemetry    since=${t}
    ${gaps}=    Evaluate    [b['ts'] - a['ts'] for a, b in zip(${rows}, ${rows}[1:])]
    Should Not Be Empty    ${gaps}
    Should Be True    all(25 <= g <= 36 for g in ${gaps})    gaps after the change: ${gaps}

UC-15 E3 A value the device refuses is reported as rejected
    [Tags]    UC-15
    ${status}    ${row}=    Send    config    ${{ {"telemetry_interval_ms": 10} }}
    Should Be Equal As Integers    ${status}    202
    ${done}=    Command Result    ${row}[id]    timeout=20
    Should Be Equal    ${done}[status]    rejected
    Should Be Equal    ${done}[reason]    invalid_value

UC-15 E3 A setting the device does not know is reported as rejected
    [Documentation]    The device answers unknown_key without the request id, so the cloud cannot match the answer:
    ...                the command stays pending and then shows timeout. Finding F-4.
    [Tags]    UC-15    finding-F-4
    ${status}    ${row}=    Send    config    ${{ {"no_such_setting": 1} }}
    ${done}=    Command Result    ${row}[id]    timeout=20
    Should Be Equal    ${done}[status]    rejected

UC-19 E2 The cloud refuses an unknown command and the gateway ignores a bad one
    [Tags]    UC-19
    ${status}    ${body}=    Send    control    ${{ {"command": "format_disk"} }}
    Should Be Equal As Integers    ${status}    400
    ${status}    ${row}=    Send    config    ${{ {"telemetry_interval_ms": "soon"} }}
    Should Be Equal As Integers    ${status}    400

UC-19 A command for the field device is routed to it
    [Tags]    UC-19
    ${status}    ${row}=    Send    control    ${{ {"command": "fd_ack_alarm"} }}
    Should Be Equal As Integers    ${status}    202
    ${done}=    Command Result    ${row}[id]    timeout=20
    Should Be Equal    ${done}[status]    accepted

UC-17 E2 A restart that is not confirmed is cancelled
    [Tags]    UC-17
    ${t}=    Now
    ${status}    ${row}=    Send    control    ${{ {"command": "restart"} }}
    ${done}=    Command Result    ${row}[id]    timeout=20
    Should Be Equal    ${done}[status]    accepted
    Sleep    45s
    ${rows}=    History    health    since=${t}
    ${ups}=    Evaluate    [r['uptime_s'] for r in ${rows}]
    Should Be True    all(b > a for a, b in zip(${ups}, ${ups}[1:]))    the uptime restarted: ${ups}

UC-17 E3 A late confirmation and a repeated restart are refused (F-5)
    [Documentation]    Needs Gateway firmware with the F-5 fix. Nothing may reboot in this test.
    [Tags]    UC-17    F-5
    ${status}    ${row}=    Send    control    ${{ {"command": "restart_confirm"} }}
    ${done}=    Command Result    ${row}[id]    timeout=20
    Should Be Equal    ${done}[status]    rejected    a confirmation with no restart pending was ${done}[status]
    ${status}    ${row}=    Send    control    ${{ {"command": "restart"} }}
    ${done}=    Command Result    ${row}[id]    timeout=20
    Should Be Equal    ${done}[status]    accepted
    ${status}    ${row}=    Send    control    ${{ {"command": "restart"} }}
    ${done}=    Command Result    ${row}[id]    timeout=20
    Should Be Equal    ${done}[status]    rejected    a repeated restart was ${done}[status]
    # the pending restart times out after 30 s: leave the Gateway as it was found
    Sleep    35s
    ${d}=    Device View
    Should Be Equal    ${d}[health][lifecycle_state]    operational
    Should Be True    ${d}[health][uptime_s] > 60    the gateway rebooted: uptime ${d}[health][uptime_s] s

UC-17 The confirmed restart reboots the gateway and it reports again
    [Tags]    UC-17
    ${t}=    Now
    ${status}    ${row}=    Send    control    ${{ {"command": "restart"} }}
    Sleep    2s
    ${status}    ${row}=    Send    control    ${{ {"command": "restart_confirm"} }}
    # no answer is expected to the confirmation: the Gateway restarts before it can publish one
    ${secs}=    Wait Until Offline Then Online    online_timeout=180
    Log    back online and publishing after ${secs} s
    ${d}=    Device View
    ${h}=    Set Variable    ${d}[health]
    Should Be True    ${h}[uptime_s] < 200    uptime ${h}[uptime_s] s: the gateway did not restart
    Should Be Equal    ${h}[lifecycle_state]    operational
