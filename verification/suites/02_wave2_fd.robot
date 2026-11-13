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

Collect Console After Reset
    Collect Console    0.2
    Reset Board    FD
    ${log}=    Collect Console    14
    RETURN    ${log}

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
    Sleep    8s    # let the splash finish: later cases capture the screens
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
    ...    The bar is the accent-coloured strip at y 268..278; it is longer in the later capture, or the splash is
    ...    already over and the tab bar shows.
    [Tags]    TC-VER-FD-013    REQ-LD-200    REQ-LD-210    REQ-LD-220    REQ-LD-230    REQ-LD-240
    ${early}=    Capture After Reset    ${OUT}/fd_splash_early.png    1700
    ${mid}=    Capture After Reset    ${OUT}/fd_splash_mid.png    2400
    ${done}=    Capture After Reset    ${OUT}/fd_splash_done.png    7000
    ${n1}=    Count Colour Pixels    ${early}    190    268    610    278    123,175,212
    ${n2}=    Count Colour Pixels    ${mid}    190    268    610    278    123,175,212
    ${tabs}=    Count Colour Pixels    ${mid}    0    0    800    4    123,175,212
    Log    bar pixels: early ${n1}, mid ${n2}, tab bar accent in mid ${tabs}
    Should Be True    ${n1} >= 20    no progress bar in the early capture
    Should Be True    ${n2} > ${n1} or ${tabs} > 50    the bar did not advance
    Region Should Have Text    ${early}    190    290    420    310    20
    Region Should Have Text    ${early}    24    440    500    456    20
    Region Should Contain Colour    ${done}    0    0    160    4    123,175,212    100

TC-VER-FD-014 Every reading on the Sensors screen shows its measurement unit
    [Tags]    TC-VER-FD-014    REQ-LD-030
    ${p}=    Show Screen    0    sensors_units
    # the unit sits right of the large value: degC, %RH and hPa slots of the three cards
    Region Should Have Text    ${p}    168    203    198    222    6
    Region Should Have Text    ${p}    424    203    458    222    6
    Region Should Have Text    ${p}    680    203    714    222    6

TC-VER-FD-015 The timestamp of the latest reading is shown and follows each new reading
    [Documentation]    The three cards show "VALID - hh:mm:ss" (the time of the reading) and the header shows the clock.
    ...    With the default 2 s polling the card timestamps change within a few seconds.
    [Tags]    TC-VER-FD-015    REQ-LD-020
    ${a}=    Show Screen    0    stamp_a
    Region Should Have Text    ${a}    53    292    180    306    30
    Region Should Have Text    ${a}    309    292    436    306    30
    Region Should Have Text    ${a}    565    292    692    306    30
    Sleep    6s
    ${b}=    Show Screen    0    stamp_b
    Run Keyword And Expect Error    *    Regions Should Match Golden    ${b}    ${a}    53    292    180    306    12    0.5

TC-VER-FD-016 The Trend screen redraws when a new history sample arrives
    [Documentation]    One history sample every 5 s: the plotted area differs between two captures 12 s apart.
    [Tags]    TC-VER-FD-016    REQ-LD-180
    ${a}=    Show Screen    1    trend_a
    Sleep    12s
    ${b}=    Show Screen    1    trend_b
    Run Keyword And Expect Error    *    Regions Should Match Golden    ${b}    ${a}    144    118    778    430    12    0.2

TC-VER-FD-017 The Trend screen shows the accumulating indicator right after a boot
    [Documentation]    The amber "ACCUMULATING m:ss / 5:00" badge (212,168,76) in the header of the Trend screen. The
    ...    design keeps it until the 5 min window is full, which is longer than the wording of REQ-LD-195 (until one valid
    ...    entry per channel); right after a boot both readings agree.
    [Tags]    TC-VER-FD-017    REQ-LD-195
    Reset And Wait    FD    30
    Sleep    6s
    ${p}=    Show Screen    1    trend_boot
    Region Should Contain Colour    ${p}    380    70    596    98    212,168,76    200

TC-VER-FD-018 The sensor simulation is initialised at start-up and delivers temperature, humidity and pressure
    [Documentation]    The boot log has one line for the SensorService, not one per sensor: the three channels are
    ...    shown by the first `sensors` reading.
    [Tags]    TC-VER-FD-018    REQ-SA-030
    ${log}=    Collect Console After Reset
    Should Contain    ${log}    SensorService initialised
    Wait For Boot    30
    Sleep    4s
    ${txt}=    Send Command    sensors    1.5
    Should Match Regexp    ${txt}    Temperature: -?[\\d.]+ degC
    Should Match Regexp    ${txt}    Humidity\\s*: [\\d.]+
    Should Match Regexp    ${txt}    Pressure\\s*: [\\d.]+

TC-VER-FD-019 The display refreshes at 5 Hz
    [Documentation]    LcdUi counts its lcd_ui_tick() calls (the task runs every LCD_REFRESH_MS = 200 ms). The counter is read over SWD
    ...    twice, ten seconds apart, and the rate is the difference over the elapsed time.
    [Tags]    TC-VER-FD-019    REQ-NF-108
    ${addr}=    Fd Symbol    ui_tick_count_address
    ${c1}=    Read U32    FD    ${addr}
    ${t1}=    Evaluate    __import__('time').time()
    Sleep    10s
    ${c2}=    Read U32    FD    ${addr}
    ${t2}=    Evaluate    __import__('time').time()
    ${rate}=    Evaluate    (${c2} - ${c1}) / (${t2} - ${t1})
    Log    refresh rate ${rate} Hz (${c1} -> ${c2})
    Should Be True    4.7 <= ${rate} <= 5.3    the display ticks at ${rate} Hz, expected 5 Hz

TC-VER-FD-020 The green LED is off and the red LED is on when the system is in the Error state
    [Documentation]    REQ-LD-270. The Field Device has no sensor failure to provoke a fault on the bench, so the fault state is forced:
    ...    the lifecycle state HealthMonitor shows on the LEDs is written over SWD, and an alarm event (a temperature limit just
    ...    below the reading, as in TC-VER-FD-006) makes HealthMonitor recompute the LEDs. The board is reset afterwards.
    [Tags]    TC-VER-FD-020    REQ-LD-270
    ${state}=    Fd Symbol    hm_lifecycle_state_address
    ${faulted}=    Fd Symbol    lifecycle_faulted
    Write U8    FD    ${state}    ${faulted}
    ${t}=    Read Temperature
    ${limit}=    Evaluate    int(float(${t}) * 100) - 150
    Set Config    temp-alarm-high    ${limit}
    Sleep    6s
    # the green LED blinks in normal operation: it must be dark in every one of several samples, not just in one
    FOR    ${i}    IN RANGE    8
        LED Should Be    green    off
        LED Should Be    red    on
        Sleep    0.5s
    END
    [Teardown]    Run Keywords    Set Config    temp-alarm-high    ${ORIG_HIGH}
    ...    AND    Reset And Wait    FD    30

TC-VER-FD-021 The sensor polling cycle of the Field Device takes under 100 ms
    [Documentation]    REQ-NF-100 (Field Device). SensorTask keeps the longest cycle since start (microseconds, with any pre-emption by
    ...    other tasks included) in a global that is read over SWD after a few minutes of running.
    [Tags]    TC-VER-FD-021    REQ-NF-100
    ${addr}=    Fd Symbol    fd_sensor_cycle_us_max_address
    Sleep    30s
    ${us}=    Read U32    FD    ${addr}
    Log    longest sensor cycle ${us} us
    Should Be True    ${us} > 0    SensorTask has not completed a cycle
    Should Be True    ${us} < 100000    the longest sensor cycle took ${us} us, the limit is 100 ms
