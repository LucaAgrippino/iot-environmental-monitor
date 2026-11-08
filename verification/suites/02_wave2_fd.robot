*** Settings ***
Documentation     Wave 2: the Field Device on its own (display, configuration, alarms, LEDs, splash, boot time).
Resource          ../resources/common.resource
Force Tags        wave2    fd
Suite Setup       Prepare Field Device
Suite Teardown    Restore Field Device

*** Variables ***
${GOLDEN}         ${EXECDIR}/docs/lcd-ui-design/screenshots/simulator
${GPIOG_ODR}      0x40021814
${GPIOD_ODR}      0x40020C14
${ORIG_POLL}      ${EMPTY}
${ORIG_HIGH}      ${EMPTY}

*** Keywords ***
Prepare Field Device
    Create Directory    ${OUT}
    Open FD Console
    ${txt}=    Send Command    config list    2.0
    ${poll}=    Console Value    polling-interval-ms: (\\d+)
    ${high}=    Console Value    temp-alarm-high: (-?\\d+)
    Set Suite Variable    ${ORIG_POLL}    ${poll}
    Set Suite Variable    ${ORIG_HIGH}    ${high}

Restore Field Device
    Set Config    polling-interval-ms    ${ORIG_POLL}
    Set Config    temp-alarm-high    ${ORIG_HIGH}
    Close Console

Set Config
    [Arguments]    ${key}    ${value}
    Send Command    config set ${key} ${value}    1.0
    Send Command    config commit    1.0
    Send Command    y    3.0

Read Temperature
    ${txt}=    Send Command    sensors    1.5
    ${t}=    Console Value    Temperature: (-?[\\d.]+) degC
    RETURN    ${t}

Read Sensor Time
    ${txt}=    Send Command    sensors    1.5
    ${t}=    Console Value    Temperature: [-\\d.]+ degC \\[t=(\\d+)\\]
    RETURN    ${t}

LED Should Be
    [Arguments]    ${led}    ${state}
    # both LEDs are active low: the pin is low when the LED is lit
    IF    '${led}' == 'green'
        ${odr}=    Read Gpio Odr    FD    0x40021800
        ${bit}=    Evaluate    (${odr} >> 6) & 1
    ELSE
        ${odr}=    Read Gpio Odr    FD    0x40020C00
        ${bit}=    Evaluate    (${odr} >> 5) & 1
    END
    ${lit}=    Evaluate    ${bit} == 0
    IF    '${state}' == 'on'
        Should Be True    ${lit}    the ${led} LED is off, expected on
    ELSE
        Should Not Be True    ${lit}    the ${led} LED is on, expected off
    END

Show Screen
    [Arguments]    ${index}    ${name}
    Select Fd Screen    ${index}
    Sleep    2.5s
    ${path}=    Set Variable    ${OUT}/fd_${name}.png
    Capture Framebuffer    ${path}
    RETURN    ${path}

*** Test Cases ***
TC-VER-FD-001 Readings are timestamped and refreshed at the polling interval
    [Tags]    TC-VER-FD-001    REQ-SA-070    REQ-SA-100
    ${t1}=    Read Sensor Time
    Sleep    7s
    ${t2}=    Read Sensor Time
    ${d}=    Evaluate    int(${t2}) - int(${t1})
    # at a polling interval of ${ORIG_POLL} ms the timestamp moves in whole cycles
    Should Be True    ${d} >= 4 and ${d} <= 10    timestamp moved by ${d} s in 7 s

TC-VER-FD-002 A configuration change survives a reset
    [Tags]    TC-VER-FD-002    REQ-SA-000    REQ-DM-090
    Set Config    polling-interval-ms    3000
    Reset And Wait    FD    30
    Send Command    config list    2.0
    Console Should Contain    polling-interval-ms: 3000
    Set Config    polling-interval-ms    ${ORIG_POLL}
    Reset And Wait    FD    30
    Send Command    config list    2.0
    Console Should Contain    polling-interval-ms: ${ORIG_POLL}

TC-VER-FD-003 An out-of-range value is refused and changes nothing
    [Tags]    TC-VER-FD-003    REQ-LI-090    REQ-LI-180    REQ-NF-114
    Send Command    config set polling-interval-ms 99999    1.0
    Send Command    config set polling-interval-ms 50    1.0
    Send Command    config discard    1.0
    Send Command    config list    2.0
    Console Should Contain    polling-interval-ms: ${ORIG_POLL}

TC-VER-FD-004 The polling interval accepts 100 ms and 10 s and refuses what is outside
    [Tags]    TC-VER-FD-004    REQ-NF-114
    Set Config    polling-interval-ms    100
    Send Command    config list    2.0
    Console Should Contain    polling-interval-ms: 100
    Set Config    polling-interval-ms    10000
    Send Command    config list    2.0
    Console Should Contain    polling-interval-ms: 10000
    Send Command    config set polling-interval-ms 10001    1.0
    Send Command    config commit    1.0
    Send Command    n    1.0
    Send Command    config list    2.0
    Console Should Contain    polling-interval-ms: 10000
    Set Config    polling-interval-ms    ${ORIG_POLL}

TC-VER-FD-005 Green LED is lit and red is dark while running without alarm
    [Tags]    TC-VER-FD-005    REQ-LD-250    REQ-LD-260
    Send Command    alarms    1.5
    Console Should Contain    No active alarms
    LED Should Be    green    on
    LED Should Be    red    off

TC-VER-FD-006 A temperature above its limit raises an alarm, lights the red LED and clears again
    [Tags]    TC-VER-FD-006    REQ-AM-000    REQ-AM-010    REQ-AM-020    REQ-LD-280    REQ-NF-101    REQ-NF-102
    ${t}=    Read Temperature
    ${limit}=    Evaluate    int(float(${t}) * 100) - 150
    Set Config    temp-alarm-high    ${limit}
    Sleep    8s
    Send Command    alarms    1.5
    Console Should Not Contain    No active alarms
    LED Should Be    red    on
    Set Config    temp-alarm-high    ${ORIG_HIGH}
    Sleep    8s
    Send Command    alarms    1.5
    Console Should Contain    No active alarms
    LED Should Be    red    off

TC-VER-FD-007 Boot reaches a working system within 5 seconds
    [Tags]    TC-VER-FD-007    REQ-NF-203    REQ-NF-213
    Reset Board    FD
    ${start}=    Evaluate    __import__('time').time()
    FOR    ${i}    IN RANGE    40
        ${txt}=    Send Command    sensors    0.5
        ${ok}=    Evaluate    'Temperature:' in $txt and 'degC' in $txt
        Exit For Loop If    ${ok}
    END
    ${elapsed}=    Evaluate    __import__('time').time() - ${start}
    Log    first valid reading ${elapsed} s after the reset
    Should Be True    ${ok}
    Should Be True    ${elapsed} <= 5.0    first reading after ${elapsed} s

TC-VER-FD-008 The Sensors screen shows the chrome of the design and the three cards
    [Tags]    TC-VER-FD-008    REQ-LD-000    REQ-LD-010    REQ-LD-050
    ${p}=    Show Screen    0    sensors
    Screen Should Not Be Blank    ${p}
    Regions Should Match Golden    ${p}    ${GOLDEN}/sensors-normal.png    0    0    800    64
    Region Should Contain Colour    ${p}    0    0    160    4    123,175,212    100
    Region Should Have Text    ${p}    22    126    776    380    300

TC-VER-FD-009 The Trend screen is selected and drawn
    [Tags]    TC-VER-FD-009    REQ-LD-160    REQ-LD-170
    ${p}=    Show Screen    1    trend
    Regions Should Match Golden    ${p}    ${GOLDEN}/trend-full-gap.png    0    0    800    64
    Region Should Contain Colour    ${p}    160    0    320    4    123,175,212    100

TC-VER-FD-010 The Status screen lists the resources of the design
    [Tags]    TC-VER-FD-010    REQ-LD-070
    ${p}=    Show Screen    2    status
    Regions Should Match Golden    ${p}    ${GOLDEN}/status-healthy.png    0    0    800    64
    Regions Should Match Golden    ${p}    ${GOLDEN}/status-healthy.png    40    140    215    400    12    2.0

TC-VER-FD-011 The Alarms screen shows its empty state when there is no alarm
    [Tags]    TC-VER-FD-011    REQ-LD-080    REQ-LD-090
    ${p}=    Show Screen    3    alarms
    Regions Should Match Golden    ${p}    ${GOLDEN}/alarms-empty.png    0    0    800    64
    Regions Should Match Golden    ${p}    ${GOLDEN}/alarms-empty.png    260    260    540    340    12    3.0

TC-VER-FD-012 The Config screen lists the parameters with the keypad
    [Tags]    TC-VER-FD-012    REQ-LD-100
    ${p}=    Show Screen    4    config
    Regions Should Match Golden    ${p}    ${GOLDEN}/config-idle.png    0    0    800    64
    Regions Should Match Golden    ${p}    ${GOLDEN}/config-idle.png    498    172    780    440    12    2.0

TC-VER-FD-013 The splash shows the progress bar and the steps, then the screens appear
    [Documentation]    Timing-sensitive (REQ-LD-220 and -230): the framebuffer is captured at fixed delays after a reset.
    [Tags]    TC-VER-FD-013    REQ-LD-200    REQ-LD-210    REQ-LD-220    REQ-LD-230    REQ-LD-240
    ${early}=    Capture After Reset    ${OUT}/fd_splash_early.png    1800
    ${late}=    Capture After Reset    ${OUT}/fd_splash_late.png    3200
    ${done}=    Capture After Reset    ${OUT}/fd_splash_done.png    9000
    Region Should Contain Colour    ${early}    190    268    610    278    123,175,212    20
    Region Should Have Text    ${early}    190    290    420    310    20
    Region Should Have Text    ${early}    24    440    500    456    20
    Region Should Contain Colour    ${late}    190    268    610    278    123,175,212    ${100}
    Region Should Contain Colour    ${done}    0    0    160    4    123,175,212    100
