*** Settings ***
Documentation     Wave 8: the requirements the first seven waves left without a case. UNRUN when written (2026-11-10, no boards
...               in reach): every case here is derived from the firmware source and the method table and must be run once on
...               the bench (verification/run.sh 7) before its requirements can count as verified.
...               Needs the two boards and the bench broker (the Raspberry Pi). The RS-485 probe cases are in suite 08.
Resource          ../resources/common.resource
Library           ../libraries/Broker.py
Library           ../libraries/Cloud.py
Library           Collections
Force Tags        wave8
Suite Setup       Prepare Wave 8
Suite Teardown    Restore Wave 8

*** Variables ***
${ORIG_TELE}      ${EMPTY}
${ORIG_HEALTH}    ${EMPTY}
${PROFILE_ADDR}   17

*** Keywords ***
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

Prepare Wave 8
    Create Directory    ${OUT}
    Start Broker
    Open GW Console
    Wait For Boot    60
    Ensure Gateway Online
    Send Command    config list    2.0
    ${t}=    Console Value    telemetry-interval-ms (\\d+)
    ${h}=    Console Value    health-interval-ms (\\d+)
    Set Suite Variable    ${ORIG_TELE}    ${t}
    Set Suite Variable    ${ORIG_HEALTH}    ${h}
    Start Collector
    Wait Until Collected    75

Restore Wave 8
    Run Keyword And Ignore Error    Cloud Config    {"telemetry_interval_ms":${ORIG_TELE},"health_interval_ms":${ORIG_HEALTH}}
    Stop Collector
    Close Console

GW Command
    [Arguments]    ${line}    ${expect}    ${wait}=2.0
    ${out}=    Command Should Answer    ${line}    ${expect}    wait=${wait}    tries=4
    RETURN    ${out}

Cloud Config
    [Arguments]    ${body}
    ${res}    ${secs}=    Time To Result    config    ${body}
    RETURN    ${res}

*** Test Cases ***
# ---- 1. Gateway console and cloud commands (boards and broker) -----------------------------------------------------
TC-VER-GW-013 A device profile can be added, listed and removed
    [Tags]    TC-VER-GW-013    REQ-MB-110    REQ-DM-100
    GW Command    profiles add addr=${PROFILE_ADDR} map=1 vendor=0x1234 desc=verif    saved
    GW Command    profiles list    slave ${PROFILE_ADDR}
    Console Should Contain    0x1234
    GW Command    profiles remove ${PROFILE_ADDR}    removed
    Send Command    profiles list    2.0
    Console Should Not Contain    slave ${PROFILE_ADDR}

TC-VER-GW-014 A profile without its mandatory fields is refused
    [Tags]    TC-VER-GW-014    REQ-MB-111
    Send Command    profiles add addr=${PROFILE_ADDR} map=1    2.0
    Console Should Contain    [ERR]
    Send Command    profiles list    2.0
    Console Should Not Contain    slave ${PROFILE_ADDR}

TC-VER-CL-005 A parameter change by cloud command is applied, confirmed with the request id and takes effect
    [Tags]    TC-VER-CL-005    REQ-DM-000    REQ-DM-002    REQ-CC-030
    ${res}    ${secs}=    Time To Result    config    {"telemetry_interval_ms":20000,"req":501}
    Should Be Equal    ${res}[result]    applied
    Should Be Equal As Integers    ${res}[req]    501
    GW Command    config list    telemetry-interval-ms 20000
    Sleep    70s
    ${iv}=    Intervals    telemetry
    ${last}=    Set Variable    ${iv}[-1]
    Should Be True    18 <= ${last} <= 22    telemetry interval after the command is ${last} s, not 20 s
    Cloud Config    {"telemetry_interval_ms":${ORIG_TELE}}

TC-VER-CL-006 An invalid, unknown or local-only parameter is refused with a reason and nothing changes
    [Tags]    TC-VER-CL-006    REQ-DM-001    REQ-NF-306
    ${bad}=    Cloud Config    {"polling_interval_ms":99999,"req":502}
    Should Be Equal    ${bad}[result]    rejected
    Should Be Equal    ${bad}[reason]    invalid_value
    ${unk}=    Cloud Config    {"no_such_key":1}
    Should Be Equal    ${unk}[reason]    unknown_key
    ${loc}=    Cloud Config    {"mqtt_port":1883}
    Should Be Equal    ${loc}[result]    rejected
    Should Be Equal    ${loc}[reason]    not_remote
    ${junk}=    Cloud Config    {"polling_interval_ms":
    Should Be Equal    ${junk}[reason]    bad_json
    GW Command    config list    polling-interval-ms 1000

TC-VER-CL-007 The health period can be changed by cloud command
    [Tags]    TC-VER-CL-007    REQ-CC-040
    ${res}=    Cloud Config    {"health_interval_ms":60000}
    Should Be Equal    ${res}[result]    applied
    GW Command    config list    health-interval-ms 60000
    Cloud Config    {"health_interval_ms":${ORIG_HEALTH}}

TC-VER-CL-008 A remote read is accepted and acknowledged (latency logged; REQ-NF-107 is about the start of the command, not measurable from the reply)
    [Tags]    TC-VER-CL-008    REQ-SA-170    REQ-NF-308
    ${res}    ${secs}=    Time To Result    control    {"command":"read","req":503}
    Should Be Equal    ${res}[event]    control
    Should Be Equal    ${res}[result]    accepted
    Should Be Equal As Integers    ${res}[req]    503
    Log    acknowledgement after ${secs} s    WARN
    Should Be True    ${secs} < 2.0    the acknowledgement took ${secs} s

TC-VER-CL-009 An unknown or malformed control command is rejected with a reason
    [Tags]    TC-VER-CL-009    REQ-NF-308
    ${a}    ${s}=    Time To Result    control    {"command":"selfdestruct"}
    Should Be Equal    ${a}[result]    rejected
    Should Be Equal    ${a}[reason]    unknown_command
    ${b}    ${s}=    Time To Result    control    not json
    Should Be Equal    ${b}[reason]    bad_format

TC-VER-CL-010 A restart without confirmation does nothing and the restart is cancelled
    [Tags]    TC-VER-CL-010    REQ-DM-020    REQ-NF-307
    Send Command    status    2.5
    ${u0}=    Uptime Seconds
    ${res}    ${secs}=    Time To Result    control    {"command":"restart","req":504}
    Should Be Equal    ${res}[result]    accepted
    Sleep    40s
    Send Command    status    2.5
    ${u1}=    Uptime Seconds
    Should Be True    ${u1} > ${u0}    the Gateway restarted without a confirmation
    Cloud Config    {"telemetry_interval_ms":${ORIG_TELE}}

TC-VER-CL-011 A confirmed restart restarts the Gateway, which comes back and publishes again
    [Tags]    TC-VER-CL-011    REQ-DM-010
    ${seen}=    Seconds Collected
    Time To Result    control    {"command":"restart","req":505}
    Publish Command    control    {"command":"restart_confirm","req":506}
    Sleep    20s
    Ensure Gateway Online
    Sleep    75s
    ${tele}=    Messages On    telemetry    since=${seen}
    Should Not Be Empty    ${tele}    no telemetry after the restart
    Send Command    status    2.5
    ${up}=    Uptime Seconds
    Should Be True    ${up} < 400    the Gateway has been up ${up} s: it did not restart

TC-VER-GW-015 The time is synchronised from NTP shortly after boot (read from the health message; the console status line is DEF-009)
    [Tags]    TC-VER-GW-015    REQ-TS-000    REQ-TS-010
    Reset And Wait    GW    60
    Ensure Gateway Online
    Start Collector
    FOR    ${i}    IN RANGE    40
        Sleep    10s
        ${now}=    Messages On    health
        IF    len(${now}) > 0    BREAK
    END
    Should Not Be Empty    ${now}    no health message within 400 s of the restart
    ${doc}=    Json Of    ${now}[0]
    Should Be True    ${doc}[last_ntp_sync_epoch] > 1700000000    the Gateway reports no NTP synchronisation
    Should Be Equal As Integers    ${doc}[ntp_sync_fail_count]    0
    ${boot}=    Evaluate    ${doc}[timestamp] - ${doc}[uptime_s]
    Should Be True    ${doc}[last_ntp_sync_epoch] - ${boot} < 180    the first NTP sync came ${doc}[last_ntp_sync_epoch] - ${boot} s after boot

