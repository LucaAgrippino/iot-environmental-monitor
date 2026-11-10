*** Settings ***
Documentation     Wave 8: the requirements the first seven waves left without a case. UNRUN when written (2026-11-10, no boards
...               in reach): every case here is derived from the firmware source and the method table and must be run once on
...               the bench (verification/run.sh 7) before its requirements can count as verified.
...               Section 1 needs the two boards and the bench broker. Section 2 also needs the Raspberry Pi with the RS-485
...               probe (tag `probe`, run them with --include probe on the Pi, Gateway unplugged where the case says so).
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
Prepare Wave 8
    Create Directory    ${OUT}
    Start Broker
    Open GW Console
    Wait For Boot    60
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
    [Tags]    TC-VER-GW-013    REQ-MB-110
    GW Command    profiles add addr=${PROFILE_ADDR} map=1 vendor=0x1234 desc=verif    saved
    GW Command    profiles list    slave ${PROFILE_ADDR}
    Console Should Contain    0x1234
    GW Command    profiles remove ${PROFILE_ADDR}    removed
    Send Command    profiles list    2.0
    Console Should Not Contain    slave ${PROFILE_ADDR}

TC-VER-GW-014 A profile without its mandatory fields is refused
    [Tags]    TC-VER-GW-014    REQ-MB-111
    Send Command    profiles add addr=${PROFILE_ADDR} map=1    2.0
    Console Should Contain    usage
    Send Command    profiles list    2.0
    Console Should Not Contain    slave ${PROFILE_ADDR}

TC-VER-GW-015 The time is synchronised after boot and shown
    [Tags]    TC-VER-GW-015    REQ-TS-000    REQ-TS-010
    Reset And Wait    GW    60
    Sleep    40s
    GW Command    status    synchronised

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

TC-VER-CL-008 A remote read is accepted, answered and acknowledged within 500 ms
    [Tags]    TC-VER-CL-008    REQ-SA-170    REQ-NF-107    REQ-NF-308
    ${res}    ${secs}=    Time To Result    control    {"command":"read","req":503}
    Should Be Equal    ${res}[event]    control
    Should Be Equal    ${res}[result]    accepted
    Should Be Equal As Integers    ${res}[req]    503
    Should Be True    ${secs} < 0.5    the acknowledgement took ${secs} s

TC-VER-CL-009 An unknown or malformed control command is rejected with a reason
    [Tags]    TC-VER-CL-009    REQ-NF-308
    ${a}    ${s}=    Time To Result    control    {"command":"selfdestruct"}
    Should Be Equal    ${a}[result]    rejected
    Should Be Equal    ${a}[reason]    unknown_command
    ${b}    ${s}=    Time To Result    control    not json
    Should Be Equal    ${b}[reason]    bad_format

TC-VER-CL-010 A restart without confirmation does nothing and the restart is cancelled
    [Tags]    TC-VER-CL-010    REQ-DM-020    REQ-NF-307
    Send Command    uptime    1.5
    ${u0}=    Console Value    (\\d+) ?s
    ${res}    ${secs}=    Time To Result    control    {"command":"restart","req":504}
    Should Be Equal    ${res}[result]    accepted
    Sleep    40s
    Send Command    uptime    1.5
    ${u1}=    Console Value    (\\d+) ?s
    Should Be True    ${u1} > ${u0}    the Gateway restarted without a confirmation
    Cloud Config    {"telemetry_interval_ms":${ORIG_TELE}}

TC-VER-CL-011 A confirmed restart restarts the Gateway, which comes back and publishes again
    [Tags]    TC-VER-CL-011    REQ-DM-010    REQ-DM-030
    ${seen}=    Seconds Collected
    Time To Result    control    {"command":"restart","req":505}
    Time To Result    control    {"command":"restart_confirm","req":506}
    Sleep    20s
    Close Console
    Open GW Console
    Wait For Boot    60
    Sleep    60s
    ${tele}=    Messages On    telemetry    since=${seen}
    Should Not Be Empty    ${tele}    no telemetry after the restart
    GW Command    uptime    uptime
    Send Command    status    2.0
    Console Should Contain    Operational

# ---- 2. Needs the Raspberry Pi RS-485 probe --------------------------------------------------------------------------
TC-VER-PR-001 PROBE The function codes on the line are the ones the requirement lists
    [Tags]    TC-VER-PR-001    REQ-MB-040    probe
    Skip    Not written: run `rpi_rs485_probe.py listen` with the Gateway connected; the Gateway sends FC03 (limits) and FC04 (readings), the Field Device answers with the same function code. Needs a Rs485Probe library.

TC-VER-PR-002 PROBE With no slave the request times out after 200 ms and is sent four times
    [Tags]    TC-VER-PR-002    REQ-MB-050    REQ-MB-060    REQ-NF-105    probe
    Skip    Not written: hold the Field Device in reset (Hold Board In Reset FD), listen on the line, expect 4 identical requests about 200 ms apart per transaction. Needs a Rs485Probe library.

TC-VER-PR-003 PROBE The default polling is one frame burst per second on the line
    [Tags]    TC-VER-PR-003    REQ-NF-110    REQ-NF-100    probe
    Skip    Not written: count request bursts in 30 s of `listen` at the default polling interval. Needs a Rs485Probe library.
