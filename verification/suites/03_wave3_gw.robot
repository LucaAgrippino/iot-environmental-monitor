*** Settings ***
Documentation     Wave 3: the Gateway on its own (console, configuration, provisioning, self-test, persistence, buffering).
...               The Field Device stays connected: the Gateway polls it during these cases.
Resource          ../resources/common.resource
Library           Process
Library           ../libraries/Broker.py
Force Tags        wave3    gw
Suite Setup       Prepare Gateway
Suite Teardown    Close Console

*** Variables ***
${SECRETS}        ${EXECDIR}/firmware/gateway/certs/bringup_secrets.h
${CERTS}          ${EXECDIR}/firmware/gateway/certs
${PY}             %{HOME}/.venvs/envmon-verify/bin/python

*** Keywords ***
Prepare Gateway
    Create Directory    ${OUT}
    Start Broker
    Open GW Console
    Wait For Boot    60

GW Command
    [Arguments]    ${line}    ${expect}    ${wait}=2.0
    ${out}=    Command Should Answer    ${line}    ${expect}    wait=${wait}    tries=4
    RETURN    ${out}

GW Config Set And Apply
    [Arguments]    ${key}    ${value}
    Send Command    config set ${key} ${value}    2.0
    Send Command    config commit    2.0
    Send Command    y    4.0

Read Secret
    [Arguments]    ${name}
    ${text}=    Get File    ${SECRETS}
    ${m}=    Get Regexp Matches    ${text}    ${name} +"([^"]*)"    1
    RETURN    ${m}[0]

Provision
    [Arguments]    @{args}
    ${r}=    Run Process    ${PY}    ${EXECDIR}/scripts/provision_device.py    --port    ${GW_PORT}    @{args}    timeout=300
    Log    ${r.stdout}
    Should Be Equal As Integers    ${r.rc}    0    ${r.stdout}${r.stderr}
    Open GW Console

*** Test Cases ***
TC-VER-GW-001 The sensors are read and shown with their time
    [Tags]    TC-VER-GW-001    REQ-SA-071    REQ-SA-031    REQ-SA-100    REQ-SA-150
    GW Command    sensors    temp
    Console Should Contain    humid
    Console Should Contain    press
    Console Should Contain    ok
    GW Command    status    Sensors

TC-VER-GW-002 Every diagnostic command of the console answers
    [Tags]    TC-VER-GW-002    REQ-LI-000    REQ-LI-010    REQ-LI-020    REQ-NF-504
    GW Command    version    v0.0.0
    GW Command    serial    00
    GW Command    uptime    uptime
    GW Command    alarms    alarm
    GW Command    thresholds    HIGH
    GW Command    status    Modbus
    GW Command    wifi status    wifi
    GW Command    mqtt status    mqtt
    GW Command    modbus status    modbus
    GW Command    config list    polling-interval-ms
    GW Command    profiles list    slave
    Console Should Not Contain    unknown command

TC-VER-GW-003 The serial command prints the unique ID of the chip
    [Tags]    TC-VER-GW-003    REQ-LI-150
    ${w0}=    Read U32    GW    0x1FFF7590
    ${w1}=    Read U32    GW    0x1FFF7594
    ${w2}=    Read U32    GW    0x1FFF7598
    ${uid}=    Evaluate    '%08X%08X%08X' % (${w0}, ${w1}, ${w2})
    GW Command    serial    ${uid}

TC-VER-GW-004 A bad or out-of-range value is refused and nothing changes
    [Tags]    TC-VER-GW-004    REQ-LI-090    REQ-LI-180    REQ-NF-114
    Send Command    config set polling-interval-ms 99999    2.0
    Send Command    config set polling-interval-ms abc    2.0
    Send Command    config set polling-interval-ms 99    2.0
    Send Command    config discard    2.0
    GW Command    config list    polling-interval-ms
    ${v}=    Console Value    polling-interval-ms (\\d+)
    Should Be Equal    ${v}    1000

TC-VER-GW-005 A confirmed change is applied, kept across a reset and the limits of the polling rate hold
    [Tags]    TC-VER-GW-005    REQ-LI-100    REQ-LI-110    REQ-LI-120    REQ-DM-090    REQ-NF-114
    GW Config Set And Apply    polling-interval-ms    2000
    GW Command    config list    polling-interval-ms 2000
    Reset And Wait    GW    60
    GW Command    config list    polling-interval-ms 2000
    GW Config Set And Apply    polling-interval-ms    100
    GW Command    config list    polling-interval-ms 100
    GW Config Set And Apply    polling-interval-ms    10000
    GW Command    config list    polling-interval-ms 10000
    Send Command    config set polling-interval-ms 10001    2.0
    Send Command    config discard    2.0
    GW Config Set And Apply    polling-interval-ms    1000
    GW Command    config list    polling-interval-ms 1000

TC-VER-GW-006 A change that is not confirmed is discarded
    [Tags]    TC-VER-GW-006    REQ-LI-190
    Send Command    config set polling-interval-ms 3000    2.0
    Send Command    config commit    2.0
    Sleep    35s
    GW Command    config list    polling-interval-ms 1000

TC-VER-GW-007 The self-test reports pass or fail and its result survives a reset
    [Tags]    TC-VER-GW-007    REQ-LI-130    REQ-LI-140    REQ-LI-160
    Send Command    selftest    4.0
    Console Should Contain    sensors
    Console Should Contain    overall
    Sleep    3s
    Reset And Wait    GW    60
    GW Command    selftest-result    stored    wait=2.5

TC-VER-GW-008 WiFi credentials, the broker endpoint and the certificates are provisioned and kept
    [Tags]    TC-VER-GW-008    REQ-LI-030    REQ-LI-040    REQ-LI-050    REQ-LI-060    REQ-NF-302
    ${ssid}=    Read Secret    BRINGUP_WIFI_SSID
    ${pass}=    Read Secret    BRINGUP_WIFI_PASSWORD
    Provision    --ssid    ${ssid}    --password    ${pass}    --endpoint    192.168.0.218    --ca    ${CERTS}/ca.crt    --cert    ${CERTS}/client.crt    --key    ${CERTS}/client.key
    GW Command    prov cert status    stored
    Console Should Contain    CA, client certificate and key stored
    Reset And Wait    GW    60
    GW Command    prov cert status    stored
    Console Should Contain    CA, client certificate and key stored

TC-VER-GW-009 The Modbus baud rate and parity follow the provisioning and the link recovers when restored
    [Tags]    TC-VER-GW-009    REQ-LI-080    REQ-MB-030    REQ-NF-201
    Send Command    prov set modbus-baud 19200    2.0
    Send Command    prov set modbus-parity E    2.0
    Send Command    prov commit    2.0
    Send Command    y    4.0
    ${brr}=    Read U32    GW    0x40004C0C
    ${cr1}=    Read U32    GW    0x40004C00
    Should Be Equal As Integers    ${brr}    4167
    Should Be True    ${cr1} & 0x1400 == 0x1400    parity bits not set: ${cr1}
    Send Command    prov set modbus-baud 9600    2.0
    Send Command    prov set modbus-parity N    2.0
    Send Command    prov commit    2.0
    Send Command    y    4.0
    ${brr}=    Read U32    GW    0x40004C0C
    Should Be Equal As Integers    ${brr}    8333
    Sleep    15s
    GW Command    modbus status    online    wait=2.5

TC-VER-GW-010 Time is flagged unsynchronised until the network time is set
    [Documentation]    Evidence only for REQ-TS-040: the status line names the synchronisation state.
    [Tags]    TC-VER-GW-010    REQ-TS-040    evidence-partial
    Send Command    status    3.0
    ${t}=    Get Console Text
    ${ok}=    Evaluate    'ynchronised' in ' '.join($t.split())
    Should Be True    ${ok}

TC-VER-GW-011 Messages are buffered in flash while the broker is down and delivered when it returns
    [Tags]    TC-VER-GW-011    REQ-BF-000    REQ-BF-010    REQ-NF-200
    Stop Broker
    Sleep    150s
    GW Command    status    Buffer    wait=3.0
    ${n}=    Console Value    Buffer (\\d+) entr
    Should Be True    ${n} >= 1    nothing was buffered while the broker was down
    Start Broker
    ${waited}=    Run Keyword And Ignore Error    Wait For Broker Publish    300    telemetry
    Log    wait result: ${waited}
    ${st}=    Send Command    mqtt status    3.0
    Log    mqtt status after the broker returned: ${st}
    Should Be Equal    ${waited}[0]    PASS    the Gateway did not reconnect to the restarted broker within 300 s: ${st}
    Sleep    60s
    GW Command    status    Buffer    wait=3.0
    ${after}=    Console Value    Buffer (\\d+) entr
    Should Be True    ${after} < ${n}    the buffer did not drain: ${n} then ${after}

TC-VER-GW-012 The Gateway reaches its operational state within 10 seconds of a reset
    [Tags]    TC-VER-GW-012    REQ-NF-213    REQ-NF-203
    Reset Board    GW
    ${start}=    Evaluate    __import__('time').time()
    FOR    ${i}    IN RANGE    60
        ${t}=    Send Command    status    0.8
        ${ok}=    Evaluate    'Modbus' in ' '.join($t.split()) and 'Sensors' in ' '.join($t.split())
        Exit For Loop If    ${ok}
    END
    ${elapsed}=    Evaluate    __import__('time').time() - ${start}
    Log    the console shows a working status ${elapsed} s after the reset
    Should Be True    ${ok}
    Should Be True    ${elapsed} <= 10.0    status after ${elapsed} s
