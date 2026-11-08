*** Settings ***
Documentation     Wave 4: the two boards together over RS-485 (link, polling, time push, link hysteresis, alarm to the cloud).
...               The Field Device runs the product image without the display (flash it with flash_fd gui=False).
...               "Unplugging the cable" is emulated by holding the Field Device in reset: it is as silent on the bus.
Resource          ../resources/common.resource
Library           ../libraries/Broker.py
Force Tags        wave4    two-boards
Suite Setup       Prepare Boards
Suite Teardown    Restore Boards

*** Variables ***
${ORIG_HIGH}      ${EMPTY}

*** Keywords ***
Prepare Boards
    Create Directory    ${OUT}
    Start Broker
    Release Board    FD
    Open FD Console
    Wait For Boot    45
    ${txt}=    Send Command    config list    2.0
    ${high}=    Console Value    temp-alarm-high: (-?\\d+)
    Set Suite Variable    ${ORIG_HIGH}    ${high}
    Open GW Console
    Wait For Boot    60

Restore Boards
    Release Board    FD
    Open FD Console
    Set FD Config    temp-alarm-high    ${ORIG_HIGH}
    Close Console

Set FD Config
    [Arguments]    ${key}    ${value}
    Send Command    config set ${key} ${value}    1.0
    Send Command    config commit    1.0
    Send Command    y    3.0

Modbus Counters
    ${out}=    Command Should Answer    modbus status    transactions    wait=2.5    tries=4
    ${ok}=    Console Value    ok (\\d+) transactions
    ${to}=    Console Value    timeouts (\\d+)
    RETURN    ${ok}    ${to}

Wait For Gateway Text
    [Arguments]    ${text}    ${seconds}
    Send Command    ${EMPTY}    0.2
    Console Should Contain    ${text}    ${seconds}

*** Test Cases ***
TC-VER-2B-001 The Gateway polls the Field Device over RS-485 and the link is up
    [Tags]    TC-VER-2B-001    REQ-MB-010
    ${ok1}    ${to1}=    Modbus Counters
    Sleep    5s
    ${ok2}    ${to2}=    Modbus Counters
    Command Should Answer    modbus status    online    wait=2.5
    Should Be True    ${ok2} - ${ok1} >= 5    only ${ok2} - ${ok1} transactions in 5 s
    Should Be Equal As Integers    ${to2}    ${to1}    timeouts grew on a healthy link
    Command Should Answer    status    Modbus    wait=2.5
    Console Should Not Contain    offline

TC-VER-2B-002 The Gateway writes its time to the Field Device when the link comes up
    [Tags]    TC-VER-2B-002    REQ-MB-020    REQ-TS-030    REQ-TS-060
    Hold Board In Reset    FD
    Send Command    ${EMPTY}    0.2
    Console Should Contain    node offline    15
    Open FD Console
    Release Board    FD
    ${seen}=    Evaluate    False
    FOR    ${i}    IN RANGE    60
        Send Command    ${EMPTY}    1.0
        ${text}=    Get Console Text
        ${seen}=    Evaluate    'CMD_SET_TIME applied' in $text
        Exit For Loop If    ${seen}
    END
    Open GW Console
    Should Be True    ${seen}    the Field Device never reported a time write after its reset

TC-VER-2B-003 A silent Field Device is declared offline and a returning one is declared up
    [Tags]    TC-VER-2B-003    REQ-NF-103    REQ-NF-104
    Hold Board In Reset    FD
    Send Command    ${EMPTY}    0.2
    Console Should Contain    node offline    15
    Command Should Answer    modbus status    offline    wait=2.5
    Release Board    FD
    Send Command    ${EMPTY}    0.2
    Console Should Contain    link up    40
    Sleep    3s
    Command Should Answer    modbus status    online    wait=2.5

TC-VER-2B-004 A Field Device alarm reaches the broker on the alarm topic
    [Tags]    TC-VER-2B-004    REQ-AM-030    REQ-CC-020    REQ-NF-207
    Open FD Console
    ${t}=    Read Temperature
    ${limit}=    Evaluate    int(float(${t}) * 100) - 150
    Set FD Config    temp-alarm-high    ${limit}
    Sleep    10s
    ${n}=    Broker Publishes In Last    30    alarm
    Set FD Config    temp-alarm-high    ${ORIG_HIGH}
    Open GW Console
    Should Be True    ${n} >= 1    no alarm publish reached the broker

*** Keywords ***
Read Temperature
    ${txt}=    Send Command    sensors    1.5
    ${t}=    Console Value    Temperature: (-?[\\d.]+) degC
    RETURN    ${t}
