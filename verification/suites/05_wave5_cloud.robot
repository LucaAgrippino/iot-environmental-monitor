*** Settings ***
Documentation     Wave 5: the cloud side (payloads, topics, QoS, periods, alarms, TLS) as seen by an MQTT subscriber
...               on the bench broker. Both boards run; the Field Device image has no display.
Resource          ../resources/common.resource
Library           ../libraries/Broker.py
Library           ../libraries/Cloud.py
Library           Collections
Force Tags        wave5    cloud
Suite Setup       Prepare Cloud
Suite Teardown    Restore Cloud

*** Variables ***
${ORIG_HIGH}      ${EMPTY}
${GW_SERIAL_ID}   ${EMPTY}

*** Keywords ***
Prepare Cloud
    Create Directory    ${OUT}
    Start Broker
    Release Board    FD
    Open FD Console
    Wait For Boot    45
    Send Command    config list    2.0
    ${high}=    Console Value    temp-alarm-high: (-?\\d+)
    Set Suite Variable    ${ORIG_HIGH}    ${high}
    Start Collector

Restore Cloud
    Stop Collector
    Open FD Console
    Set FD Config    temp-alarm-high    ${ORIG_HIGH}
    Close Console

Set FD Config
    [Arguments]    ${key}    ${value}
    Send Command    config set ${key} ${value}    1.0
    Send Command    config commit    1.0
    Send Command    y    3.0

Read Temperature
    ${txt}=    Send Command    sensors    1.5
    ${t}=    Console Value    Temperature: (-?[\\d.]+) degC
    RETURN    ${t}

Raise Temperature Alarm
    [Documentation]    Set the temperature limit 1.5 degC under the reading; return the alarm messages seen and the limit.
    ${t}=    Read Temperature
    ${limit}=    Evaluate    int(float(${t}) * 100) - 150
    ${count}=    Messages On    alarms
    ${n0}=    Get Length    ${count}
    Set FD Config    temp-alarm-high    ${limit}
    Sleep    10s
    ${msgs}=    Messages On    alarms
    ${new}=    Evaluate    $msgs[$n0:]
    Set FD Config    temp-alarm-high    ${ORIG_HIGH}
    Sleep    10s
    RETURN    ${new}    ${limit}

*** Test Cases ***
TC-VER-CL-001 An alarm is published at QoS 1 as JSON
    [Tags]    TC-VER-CL-001    REQ-AM-030    REQ-CC-020    REQ-NF-207
    ${msgs}    ${limit}=    Raise Temperature Alarm
    Should Not Be Empty    ${msgs}    no alarm reached the subscriber
    Should Be Equal As Integers    ${msgs}[0][qos]    1
    ${doc}=    Json Of    ${msgs}[0]
    Dictionary Should Contain Key    ${doc}    timestamp

TC-VER-CL-004 An alarm carries the sensor, the type, the value, the threshold and the time
    [Tags]    TC-VER-CL-004    REQ-AM-040
    ${msgs}    ${limit}=    Raise Temperature Alarm
    Should Not Be Empty    ${msgs}    no alarm reached the subscriber
    ${doc}=    Json Of    ${msgs}[0]
    FOR    ${key}    IN    timestamp    sensor_id    alarm_type    measured_value_raw    threshold_value_raw
        Dictionary Should Contain Key    ${doc}    ${key}
    END
    Should Be Equal As Integers    ${doc}[threshold_value_raw]    ${limit}    the alarm carries threshold ${doc}[threshold_value_raw], the limit set was ${limit} (DEF-001)

TC-VER-CL-003 Telemetry and health are published on separate topics, as JSON, with a schema version and the serial, at their periods
    [Tags]    TC-VER-CL-003    REQ-CC-000    REQ-CC-070    REQ-CC-071    REQ-CC-080    REQ-CC-090    REQ-NF-111    REQ-NF-206    REQ-NF-216
    Wait Until Collected    340
    ${kinds}=    Topic Kinds
    Log    ${kinds}
    List Should Contain Value    ${kinds}    telemetry
    List Should Contain Value    ${kinds}    health
    ${tele}=    Messages On    telemetry
    ${health}=    Messages On    health
    Should Be True    len(${tele}) >= 4    only ${tele.__len__()} telemetry messages in 340 s
    Should Not Be Empty    ${health}
    FOR    ${m}    IN    @{tele}    @{health}
        ${doc}=    Json Of    ${m}
        Dictionary Should Contain Key    ${doc}    schema_version
        Dictionary Should Contain Key    ${doc}    device_id
        Should Match Regexp    ${m}[topic]    ^dt/iotmonitor/${doc}[device_id]/
    END
    FOR    ${m}    IN    @{tele}    @{health}
        Should Be Equal As Integers    ${m}[qos]    0
    END
    ${iv}=    Intervals    telemetry
    Log    telemetry intervals ${iv}
    FOR    ${x}    IN    @{iv}
        Should Be True    54 <= ${x} <= 66    telemetry interval ${x} s is not 60 s +-10 %
    END

TC-VER-CL-002 The broker refuses a client without a certificate
    [Tags]    TC-VER-CL-002    REQ-CC-050    REQ-NF-300    REQ-NF-301
    ${refused}=    Connection Without Certificate Is Refused
    Should Be True    ${refused}    the broker accepted a client with no certificate
    ${tele}=    Messages On    telemetry
    Should Not Be Empty    ${tele}    the Gateway has not published through the mutual-TLS session
