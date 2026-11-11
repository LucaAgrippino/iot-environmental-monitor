*** Settings ***
Documentation     Wave 8, batch 2: health metrics and period, self-test, a silent Field Device, a broker restart, the watchdog and
...               the RS-485 line (passive dongle). Run on the Raspberry Pi with both boards, the bench broker and the CH340 dongle
...               in parallel on the bus (verification/run.sh 8). UNRUN when written: derived from the firmware and the SRS.
Resource          ../resources/common.resource
Library           ../libraries/Broker.py
Library           ../libraries/Cloud.py
Library           ../libraries/Rs485Probe.py
Library           Collections
Library           Process
Force Tags        wave8
Suite Setup       Prepare Batch 2
Suite Teardown    Restore Batch 2

*** Keywords ***
Prepare Batch 2
    Create Directory    ${OUT}
    Start Broker
    Open GW Console
    Wait For Boot    60
    Ensure Gateway Online
    Start Collector

Restore Batch 2
    Run Keyword And Ignore Error    Release Board    FD
    Stop Collector
    Close Console

Ensure Gateway Online
    [Documentation]    The Wi-Fi module sometimes fails to join after a reset (known, WIFITASK-O2): reset again, up to 3 times.
    FOR    ${try}    IN RANGE    3
        ${ok}=    Run Keyword And Return Status    Wait For Mqtt Connected    150
        IF    ${ok}    RETURN
        Log    Gateway not online after 150 s (try ${try}), resetting it    WARN
        Reset Board    GW
        Sleep    3s
        Wait For Boot    60
    END
    Fail    the Gateway did not get online in three attempts

Restore Default Profile
    [Documentation]    The default profile of the registry (slave 1, map 1, vendor 0x1A45): never remove it, the poller needs it.
    Send Command    profiles add addr=1 map=1 vendor=0x1A45 desc=default    2.0
    Sleep    30s
    Send Command    modbus status    3.0

GW Command
    [Arguments]    ${line}    ${expect}    ${wait}=2.5
    ${out}=    Command Should Answer    ${line}    ${expect}    wait=${wait}    tries=4
    RETURN    ${out}

*** Test Cases ***
TC-VER-CL-012 The health message carries every metric the requirement lists
    [Tags]    TC-VER-CL-012    REQ-CC-010
    Wait Until Collected    ${{ 5 }}
    ${health}=    Messages On    health
    IF    not ${health}
        Wait Until Collected    330
        ${health}=    Messages On    health
    END
    Should Not Be Empty    ${health}    no health message was published
    ${doc}=    Json Of    ${health}[-1]
    @{needed}=    Create List    stack_free_words    arena_min_free_bytes    cpu_load_pct    wifi_rssi_dbm    mqtt_reconnect_count
    ...    mqtt_publishes_failed    modbus_timeouts    modbus_transactions_ok    uptime_s    mcu_temp_deci_c    buffer_entry_count
    FOR    ${key}    IN    @{needed}
        Run Keyword And Continue On Failure    Dictionary Should Contain Key    ${doc}    ${key}
    END
    ${keys}=    Evaluate    ' '.join(sorted(k for k in $doc if 'crc' in k))
    Should Not Be Empty    ${keys}    the requirement lists Modbus CRC counts, the health message has no CRC field (the counter exists in ModbusMaster)

TC-VER-CL-013 The health message is published every five minutes
    [Tags]    TC-VER-CL-013    REQ-NF-112
    Wait Until Collected    680
    ${iv}=    Intervals    health
    Log    health intervals ${iv}
    Should Not Be Empty    ${iv}    fewer than two health messages in 680 s
    FOR    ${x}    IN    @{iv}
        Should Be True    270 <= ${x} <= 330    health interval ${x} s is not 300 s +-10 %
    END

TC-VER-GW-016 The self-test checks the sensors and the links and reports each
    [Tags]    TC-VER-GW-016    REQ-DM-040
    GW Command    selftest    sensors    wait=8
    Console Should Contain    comms
    Console Should Contain    overall
    Console Should Not Contain    unknown command

TC-VER-GW-017 A failing diagnostic prints an error: the self-test with the Field Device silent
    [Tags]    TC-VER-GW-017    REQ-LI-170
    Hold Board In Reset    FD
    Sleep    20s
    GW Command    selftest    comms    wait=10
    Console Should Contain    FAIL
    [Teardown]    Release Board    FD

TC-VER-2B-005 A Field Device that stops answering is reported offline, and online again when it returns
    [Tags]    TC-VER-2B-005    REQ-NF-215
    Sleep    20s
    GW Command    modbus status    online
    Hold Board In Reset    FD
    Sleep    15s
    GW Command    modbus status    offline
    Release Board    FD
    Sleep    40s
    GW Command    modbus status    online
    [Teardown]    Release Board    FD

TC-VER-CL-014 After a broker restart the Gateway reconnects and publishes again
    [Tags]    TC-VER-CL-014    REQ-CC-060
    ${r}=    Run Process    sudo    -n    systemctl    restart    mosquitto    timeout=60
    Should Be Equal As Integers    ${r.rc}    0    ${r.stderr}
    Sleep    5s
    Wait For Mqtt Connected    240
    Start Collector
    Wait Until Collected    75
    ${tele}=    Messages On    telemetry
    Should Not Be Empty    ${tele}    no telemetry after the broker came back

TC-VER-PR-001 The function codes on the RS-485 line are within the supported set and the Gateway uses reads
    [Tags]    TC-VER-PR-001    REQ-MB-040    probe
    ${n}=    Capture Bus    12
    Should Be True    ${n} > 10    only ${n} frames in 12 s: is the dongle on the bus?
    ${codes}=    Function Codes Seen    1
    Log    function codes ${codes}
    Should Contain    ${codes}    ${4}
    FOR    ${c}    IN    @{codes}
        Should Contain    ${{ [1,2,3,4,5,6,15,16] }}    ${c}    function code ${c} is not one the requirement lists
    END

TC-VER-PR-002 With no slave the request is repeated every 200 ms (the response timeout)
    [Tags]    TC-VER-PR-002    REQ-MB-050    REQ-NF-105    probe
    Hold Board In Reset    FD
    Sleep    3s
    Capture Bus    8
    ${spacing}=    Dominant Request Spacing    1
    Log    median spacing of the repeated request ${spacing} s (response timeout 200 ms plus the turnaround)
    Should Be True    0.19 <= ${spacing} <= 0.25    the request is repeated every ${spacing} s, not about 200 ms
    [Teardown]    Release Board    FD

TC-VER-2B-006 A device whose identity does not match the profile is rejected, and accepted again when the profile is restored
    [Tags]    TC-VER-2B-006    REQ-MB-120    REQ-DM-101
    Sleep    30s
    GW Command    modbus status    online
    GW Command    profiles add addr=1 map=1 vendor=0x1234 desc=wrong-vendor    saved
    Sleep    25s
    Send Command    modbus status    3.0
    Console Should Not Contain    online
    [Teardown]    Restore Default Profile

