*** Settings ***
Documentation     Wave 6: endurance. Both boards, the broker and the cloud collector run for ${SOAK_SECONDS} s (default 8 h).
...               Every 60 s the library reads both consoles. Pass: no reset of either board, a clean bus, MQTT without failures.
...               Short dry run:  verification/run.sh --variable SOAK_SECONDS:300 6
Resource          ../resources/common.resource
Library           ../libraries/Broker.py
Library           ../libraries/Cloud.py
Library           ../libraries/Soak.py
Force Tags        wave6    endurance
Suite Setup       Prepare Endurance
Suite Teardown    Stop Collector

*** Variables ***
${SOAK_SECONDS}   28800
${SAMPLE_EVERY}   60

*** Keywords ***
Prepare Endurance
    Create Directory    ${OUT}
    Start Broker
    Start Collector

*** Test Cases ***
TC-VER-EN-001 Both boards run without a reset, the bus stays clean and the cloud keeps receiving
    [Tags]    TC-VER-EN-001    REQ-CC-000    REQ-NF-111
    ${csv}=    Set Variable    ${OUT}/soak-${SOAK_SECONDS}s.csv
    Run Soak    ${SOAK_SECONDS}    ${SAMPLE_EVERY}    ${csv}
    ${gw}    ${fd}=    Soak Resets
    ${s}=    Soak Summary
    Log    ${s}
    ${tele}=    Messages On    telemetry
    Log    ${tele.__len__()} telemetry messages
    Should Be Equal As Integers    ${gw}    0    the Gateway reset ${gw} times
    Should Be Equal As Integers    ${fd}    0    the Field Device reset ${fd} times
    Should Be True    ${s}[gw_timeouts] <= 0.01 * ${s}[gw_ok]    ${s}[gw_timeouts] Modbus timeouts against ${s}[gw_ok] good transactions
    Should Be True    ${s}[fd_crc] <= 0.01 * ${s}[fd_frames]    ${s}[fd_crc] CRC errors in ${s}[fd_frames] frames
    Should Be Equal As Integers    ${s}[mqtt_failed]    0    ${s}[mqtt_failed] MQTT publishes failed
    ${expected}=    Evaluate    int(${SOAK_SECONDS} / 60 * 0.9)
    Should Be True    len(${tele}) >= ${expected}    ${tele.__len__()} telemetry messages, expected at least ${expected}
