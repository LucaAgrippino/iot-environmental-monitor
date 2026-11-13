*** Settings ***
Documentation     Closure cases: the requirements that had no case after waves 0 to 8 (docs/test/not-run-closure.md). Needs the
...               two boards, the bench broker and the Raspberry Pi (verification/run.sh 10 through ~/run-waves.sh). The
...               cases bounce the broker and the Gateway, so run them on their own.
Resource          ../resources/common.resource
Library           ../libraries/Broker.py
Library           ../libraries/Cloud.py
Library           ../libraries/PortCapture.py
Library           ../libraries/Rs485Probe.py
Library           Collections
Library           Process
Library           DateTime
Library           String
Force Tags        closure
Suite Setup       Prepare Closure
Suite Teardown    Restore Closure

*** Variables ***
${ORIG_TELE}      ${EMPTY}
${ORIG_HEALTH}    ${EMPTY}

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

Prepare Closure
    Create Directory    ${OUT}
    Start Broker
    Open GW Console
    Wait For Boot    60
    Ensure Gateway Online
    Send Command    config list    2.0
    ${t}=    Console Value    telemetry-interval-ms +(\\d+)
    ${h}=    Console Value    health-interval-ms +(\\d+)
    Set Suite Variable    ${ORIG_TELE}    ${t}
    Set Suite Variable    ${ORIG_HEALTH}    ${h}
    Start Collector
    Wait Until Collected    75

Restore Closure
    Run Keyword And Ignore Error    Cloud Config    {"telemetry_interval_ms":${ORIG_TELE},"health_interval_ms":${ORIG_HEALTH}}
    Stop Collector
    Close Console

GW Command
    [Arguments]    ${line}    ${expect}    ${wait}=2.0
    ${out}=    Command Should Answer    ${line}    ${expect}    wait=${wait}    tries=4
    RETURN    ${out}

GW Config Set And Apply
    [Arguments]    ${key}    ${value}
    Send Command    config set ${key} ${value}    2.0
    Send Command    config commit    2.0
    Send Command    y    4.0

Cloud Config
    [Arguments]    ${body}
    ${res}    ${secs}=    Time To Result    config    ${body}
    RETURN    ${res}

Newest Health
    [Documentation]    The newest health message collected, as a dictionary (fails when there is none).
    ${h}=    Messages On    health
    Should Not Be Empty    ${h}    no health message collected yet
    ${doc}=    Json Of    ${h}[-1]
    RETURN    ${doc}

Wait For Health Newer Than
    [Arguments]    ${count}    ${timeout}=60
    [Documentation]    Wait until more than ${count} health messages are collected; return the newest one.
    ${end}=    Evaluate    __import__('time').time() + ${timeout}
    WHILE    True
        ${h}=    Messages On    health
        ${n}=    Get Length    ${h}
        IF    ${n} > ${count}
            ${doc}=    Json Of    ${h}[-1]
            RETURN    ${doc}
        END
        ${now}=    Evaluate    __import__('time').time()
        IF    ${now} > ${end}    Fail    no new health message within ${timeout} s
        Sleep    2s
    END

Restart Collector
    [Documentation]    A broker restart drops the collector's subscriptions: start a new one (as TC-VER-CL-014 does).
    Stop Collector
    Start Collector
    Wait Until Collected    75

Health Count
    ${h}=    Messages On    health
    ${n}=    Get Length    ${h}
    RETURN    ${n}

Fast Health Period
    [Documentation]    Health period 10 s through the console. It applies at once; it reaches the flash only on an orderly restart
    ...    (ConfigService persists on request), so a hardware reset would bring the old value back.
    GW Config Set And Apply    health-interval-ms    10000
    Sleep    12s

Orderly Restart
    [Documentation]    `reboot` through the console: the pending configuration is persisted first, then the Gateway restarts.
    Send Command    reboot    2.0
    Sleep    5s
    Wait For Boot    90
    Ensure Gateway Online

Slow Health Period
    GW Config Set And Apply    health-interval-ms    ${ORIG_HEALTH}

Defaults Back And Persisted
    GW Config Set And Apply    health-interval-ms    ${ORIG_HEALTH}
    GW Config Set And Apply    ntp-interval-min    60
    Orderly Restart

Telemetry Count
    ${t}=    Messages On    telemetry
    ${n}=    Get Length    ${t}
    RETURN    ${n}

Log Lines Should Be Well Formed
    [Arguments]    ${text}    ${board}
    ${levels}=    Evaluate    len(re.findall(r'\\[ ?(?:INFO|WARN|ERROR|DEBUG)\\]', $text))    modules=re
    ${full}=    Evaluate    len(re.findall(r'\\[ ?(?:INFO|WARN|ERROR|DEBUG)\\]\\[(?:\\d\\d:\\d\\d:\\d\\d|T\\+\\d+)\\]\\[[A-Za-z0-9_]+ *\\]', $text))    modules=re
    Log    ${board}: ${levels} log lines, ${full} with severity, time and module
    Should Be True    ${levels} >= 10    only ${levels} log lines from the ${board} after a reset
    Should Be True    ${full} >= ${levels} * 0.9    ${full} of ${levels} ${board} log lines have severity, time and module

Restore Field Device Address
    [Documentation]    Whatever happened, put the Field Device back to address 1 and remove the test profile.
    Close Console
    Run Keyword And Ignore Error    Open FD Console
    Run Keyword And Ignore Error    Wait For Boot    30
    Run Keyword And Ignore Error    Send Command    prov set modbus-addr 1    2.0
    Run Keyword And Ignore Error    Send Command    prov commit    2.0
    Run Keyword And Ignore Error    Send Command    y    4.0
    Reset Board    FD
    Sleep    20s
    Close Console
    Open GW Console
    Wait For Boot    60
    Run Keyword And Ignore Error    Send Command    profiles remove 5    2.0
    Sleep    30s
    GW Command    modbus status    online    wait=2.5

Bounce Broker
    ${r}=    Run Process    sudo    -n    systemctl    restart    mosquitto    timeout=60
    Should Be Equal As Integers    ${r.rc}    0    ${r.stderr}

*** Test Cases ***
# ---- 1. Time: the RTC, the retry, the period and the Field Device --------------------------------------------------
TC-VER-GW-016 The RTC keeps the time across a reset: the Gateway reports a synchronised time before it has the network
    [Documentation]    After a warm reset TimeProvider restores SYNCHRONISED from the RTC (DEF-009). The check is made on the first
    ...    answer of the console, which comes before the Wi-Fi join and the NTP query.
    [Tags]    TC-VER-GW-016    REQ-TS-020
    Reset Board    GW
    Sleep    1s
    Wait For Boot    60
    ${txt}=    Send Command    status    2.0
    Should Match Regexp    ${txt}    Time +synchronised
    Should Not Match Regexp    ${txt}    WiFi +connected    the network was already up: the time may come from NTP, not the RTC
    Ensure Gateway Online

TC-VER-CL-015 When the connection to the cloud returns the Gateway retries the time update at once
    [Documentation]    CloudPublisher triggers TimeService's retry when MQTT connects. The broker is restarted with the health period
    ...    at 10 s; the newest health message must carry a last_ntp_sync_epoch after the restart, not the hourly one.
    [Tags]    TC-VER-CL-015    REQ-TS-050
    Fast Health Period
    ${n}=    Health Count
    ${before}=    Wait For Health Newer Than    ${n}    90
    ${e0}=    Set Variable    ${before}[last_ntp_sync_epoch]
    ${restart}=    Evaluate    int(__import__('time').time())
    Bounce Broker
    Sleep    5s
    Wait For Mqtt Connected    240
    Restart Collector
    ${deadline}=    Evaluate    __import__('time').time() + 120
    ${e1}=    Set Variable    ${e0}
    WHILE    ${e1} <= ${e0}
        ${n}=    Health Count
        ${doc}=    Wait For Health Newer Than    ${n}    60
        ${e1}=    Set Variable    ${doc}[last_ntp_sync_epoch]
        ${now}=    Evaluate    __import__('time').time()
        IF    ${now} > ${deadline}    Fail    no NTP sync after the broker came back (epoch still ${e0})
    END
    Should Be True    ${e1} >= ${restart} - 5    the sync ${e1} is older than the restart ${restart}
    ${gap}=    Evaluate    ${e1} - ${restart}
    Log    NTP sync ${e1}, broker restarted at ${restart}: ${gap} s later
    [Teardown]    Run Keyword And Ignore Error    Slow Health Period

TC-VER-CL-016 The RTC is synchronised every hour by default and the period can be set from the console
    [Documentation]    Default 60 min = 3600 s. Set to 1 min, reset (the period is read at start-up), and the successive sync
    ...    times in the health message are about 60 s apart. The health period is 10 s for the run.
    [Tags]    TC-VER-CL-016    REQ-NF-210
    ${txt}=    Send Command    config list    2.5
    Should Match Regexp    ${txt}    ntp-interval-min\\s+60\\b
    GW Config Set And Apply    ntp-interval-min    1
    ${txt}=    Send Command    config list    2.5
    Should Match Regexp    ${txt}    ntp-interval-min\\s+1\\b
    GW Config Set And Apply    health-interval-ms    10000
    Orderly Restart
    ${seen}=    Create List
    ${end}=    Evaluate    __import__('time').time() + 270
    WHILE    True
        ${n}=    Health Count
        ${doc}=    Wait For Health Newer Than    ${n}    60
        ${e}=    Set Variable    ${doc}[last_ntp_sync_epoch]
        ${known}=    Evaluate    ${e} in ${seen}
        IF    not ${known} and ${e} > 0    Append To List    ${seen}    ${e}
        ${now}=    Evaluate    __import__('time').time()
        IF    ${now} > ${end}    BREAK
    END
    Log    distinct sync times ${seen}
    ${count}=    Get Length    ${seen}
    Should Be True    ${count} >= 3    only ${count} syncs in 270 s at a 1 min period
    FOR    ${i}    IN RANGE    1    ${count}
        ${d}=    Evaluate    $seen[$i] - $seen[$i - 1]
        Should Be True    50 <= ${d} <= 75    the syncs are ${d} s apart, expected about 60 s
    END
    [Teardown]    Run Keyword And Ignore Error    Defaults Back And Persisted

TC-VER-CL-017 The Field Device gets the time after the Gateway's NTP sync
    [Documentation]    The broker restart makes the Gateway sync (REQ-TS-050) and TimeService pushes the new time to the
    ...    Field Device, whose console logs "CMD_SET_TIME applied".
    [Tags]    TC-VER-CL-017    REQ-NF-211
    Start Port Capture    ${FD_PORT}    115200
    Bounce Broker
    Sleep    5s
    Wait For Mqtt Connected    240
    Port Capture Should Contain    CMD_SET_TIME applied    90
    Restart Collector
    [Teardown]    Stop Port Capture

# ---- 2. Remote commands to the Field Device and the Modbus retries (RS-485 dongle on the bus) -----------------------
TC-VER-CL-018 A remote command is written to the Field Device's command register and the answer is returned to the cloud
    [Documentation]    "fd_reset_metrics" -> FC06 on 0x0201 (CMD_RESET_METRICS). The regular poll and the time push use function
    ...    codes 3, 4 and 16 only, so an FC06 frame on the line, answered by the Field Device, is the command; the result
    ...    message carries the Field Device's answer (REQ-MB-080, REQ-MB-090).
    [Tags]    TC-VER-CL-018    REQ-MB-080    REQ-MB-090    probe
    Start Capture Bus    12
    Sleep    2s
    ${res}    ${secs}=    Time To Result    control    {"command":"fd_reset_metrics","req":601}    8
    Wait Capture Bus
    Should Be Equal    ${res}[command]    fd_reset_metrics
    Should Be Equal As Integers    ${res}[req]    601
    Should Be Equal    ${res}[result]    accepted
    ${codes}=    Function Codes Seen    1
    Log    function codes on the line ${codes}; answer after ${secs} s
    Should Contain    ${codes}    ${6}    no FC06 frame on the line: the command was not written to the Field Device

TC-VER-CL-019 A command for a Field Device that does not answer is rejected and reported (UC-19 exception E3)
    [Tags]    TC-VER-CL-019    REQ-MB-090
    Hold Board In Reset    FD
    Sleep    3s
    ${res}    ${secs}=    Time To Result    control    {"command":"fd_ack_alarm","req":602}    12
    Should Be Equal    ${res}[result]    rejected
    Should Be Equal    ${res}[reason]    field_device_unreachable
    Log    rejection after ${secs} s
    [Teardown]    Release Board    FD

TC-VER-PR-004 A failed Modbus transaction is sent four times: the request and three retries
    [Documentation]    With the Field Device held in reset and the poll period at 1 s, the repeated request of each poll cycle
    ...    is a group of identical frames about 200 ms apart (the response timeout), separated from the next cycle.
    [Tags]    TC-VER-PR-004    REQ-MB-060    probe
    GW Config Set And Apply    modbus-poll-period-ms    1000
    Hold Board In Reset    FD
    Sleep    4s
    Capture Bus    14
    # retries are about 0.2 s apart, the next poll cycle about 0.4 s after the last retry: 0.3 s separates the two
    ${groups}=    Retry Groups    1    4    0.3
    Log    groups (count, median interval) ${groups}
    Should Not Be Empty    ${groups}    no repeated request seen on the line
    ${counts}=    Evaluate    [g[0] for g in ${groups}]
    ${biggest}=    Evaluate    max(${counts})
    ${fours}=    Evaluate    ${counts}.count(4)
    Should Be Equal As Integers    ${biggest}    4    the largest group is ${biggest} identical requests, expected 4 (request + 3 retries)
    Should Be True    ${fours} >= 3    only ${fours} complete groups of 4 in 14 s: ${counts}
    [Teardown]    Run Keywords    Release Board    FD
    ...    AND    Run Keyword And Ignore Error    GW Config Set And Apply    modbus-poll-period-ms    200

TC-VER-CL-020 A device profile can be added and removed from the cloud
    [Documentation]    REQ-DM-100 (UC-15): the same registry the console writes, through the /control commands profile_add and
    ...    profile_remove. The console (UC-16) path is TC-VER-GW-013. A slave address that is no real Field Device is used,
    ...    and the default profile (slave 1) is never touched: the poller needs it.
    [Tags]    TC-VER-CL-020    REQ-DM-100
    ${res}    ${secs}=    Time To Result    control    {"command":"profile_add","slave":17,"map":1,"vendor":4660,"desc":"verif","req":701}    8
    Should Be Equal    ${res}[result]    accepted
    ${txt}=    Send Command    profiles list    2.5
    Should Match Regexp    ${txt}    (?m)^\\s*slave\\s+17\\s+map\\s+1\\s+vendor\\s+0x1234
    ${bad}    ${secs}=    Time To Result    control    {"command":"profile_add","slave":17,"map":1,"req":702}    8
    Should Be Equal    ${bad}[result]    rejected
    Should Be Equal    ${bad}[reason]    bad_format
    ${gone}    ${secs}=    Time To Result    control    {"command":"profile_remove","slave":17,"req":703}    8
    Should Be Equal    ${gone}[result]    accepted
    ${txt}=    Send Command    profiles list    2.5
    Should Not Match Regexp    ${txt}    (?m)^\\s*slave\\s+17\\s+map
    ${again}    ${secs}=    Time To Result    control    {"command":"profile_remove","slave":17,"req":704}    8
    Should Be Equal    ${again}[reason]    not_found
    [Teardown]    Run Keyword And Ignore Error    Send Command    profiles remove 17    2.0

# ---- 3. Log format, the Field Device register, the Modbus address ---------------------------------------------------
TC-VER-GW-017 Every log line carries a severity, a time and the module on both boards
    [Documentation]    A reset of each board is recorded on its own console stream; every log line has the shape
    ...    [ LEVEL][hh:mm:ss][Module ] text (the time is T+seconds until the RTC is usable). A line whose prefix is cut by the
    ...    Gateway's status repaint is tolerated up to 10 %.
    [Tags]    TC-VER-GW-017    REQ-NF-500
    Close Console
    Start Port Capture    ${GW_PORT}    460800
    Reset Board    GW
    Sleep    35s
    ${gw}=    Get Port Capture
    Stop Port Capture
    Start Port Capture    ${FD_PORT}    115200
    Reset Board    FD
    Sleep    20s
    ${fd}=    Get Port Capture
    Stop Port Capture
    Log Lines Should Be Well Formed    ${gw}    Gateway
    Log Lines Should Be Well Formed    ${fd}    Field Device
    Open GW Console
    Wait For Boot    90
    Ensure Gateway Online

TC-VER-2B-007 The Field Device register follows its measurement: the Gateway's value equals the Field Device's own
    [Documentation]    REQ-MB-000: right after a telemetry message the Field Device console and the message agree (the message
    ...    carries what the Gateway last read from the Field Device's input registers).
    [Tags]    TC-VER-2B-007    REQ-MB-000
    ${count}=    Telemetry Count
    ${end}=    Evaluate    __import__('time').time() + 100
    WHILE    True
        ${now}=    Telemetry Count
        IF    ${now} > ${count}    BREAK
        ${t}=    Evaluate    __import__('time').time()
        IF    ${t} > ${end}    Fail    no telemetry message within 100 s
        Sleep    2s
    END
    ${tele}=    Messages On    telemetry
    ${doc}=    Json Of    ${tele}[-1]
    ${fd}=    Set Variable    ${doc}[field_device]
    Close Console
    Open FD Console
    Wait For Boot    30
    ${txt}=    Send Command    sensors    2.0
    ${temp}=    Console Value    Temperature\\s*: (-?[\\d.]+) degC
    ${hum}=    Console Value    Humidity\\s*: ([\\d.]+) %RH
    ${press}=    Console Value    Pressure\\s*: ([\\d.]+) hPa
    Log    Field Device console ${temp} degC ${hum} % ${press} hPa; Gateway message ${fd}
    Should Be True    ${fd}[online]
    Should Be True    abs(${temp} * 10 - ${fd}[temperature_deci_c]) <= 8    temperature ${temp} against ${fd}[temperature_deci_c] deci-degC
    Should Be True    abs(${hum} - ${fd}[humidity_pct]) <= 4    humidity ${hum} against ${fd}[humidity_pct]
    Should Be True    abs(${press} - ${fd}[pressure_hpa]) <= 4    pressure ${press} against ${fd}[pressure_hpa]
    [Teardown]    Run Keywords    Close Console    AND    Open GW Console    AND    Wait For Boot    60

TC-VER-2B-008 The Modbus address is received on the console, persisted, and takes effect on the line
    [Documentation]    REQ-LI-070. Field Device: `prov set modbus-addr 5`, commit, reset: it answers at 5 and no longer at 1 (the
    ...    passive dongle sees the replies). Gateway: the same key is staged and accepted (discarded, so the poller is
    ...    untouched). The Field Device is put back to address 1 afterwards.
    [Tags]    TC-VER-2B-008    REQ-LI-070    probe
    Close Console
    Open FD Console
    Wait For Boot    30
    Send Command    prov set modbus-addr 5    2.0
    Console Should Contain    staged
    Send Command    prov commit    2.0
    Send Command    y    4.0
    ${txt}=    Send Command    config list    2.5
    Should Match Regexp    ${txt}    modbus-slave-addr:\\s+5\\b
    Reset Board    FD
    Sleep    20s
    Close Console
    Open GW Console
    Wait For Boot    60
    Send Command    prov set modbus-addr 3    2.0
    Console Should Contain    staged
    Send Command    prov discard    2.0
    Capture Bus    8
    ${at_1}=    Reply Count    1
    Should Be Equal As Integers    ${at_1}    0    the Field Device still answers at address 1
    GW Command    profiles add addr=5 map=1 vendor=0x1A45 desc=fd-at-5    saved
    Sleep    15s
    Capture Bus    8
    ${at_5}=    Reply Count    5
    Log    replies at address 5: ${at_5}
    Should Be True    ${at_5} > 0    the Field Device does not answer at address 5
    [Teardown]    Restore Field Device Address


TC-VER-CL-021 The Gateway never talks to the cloud without TLS
    [Documentation]    REQ-NF-305. The broker is stopped and a recording listener takes its port. Every attempt of the Gateway starts
    ...    with a TLS handshake (record type 0x16, version 0x03), never with a plaintext MQTT CONNECT (0x10), and the plaintext
    ...    CONNACK the listener sends back does not make the Gateway report a connection.
    [Tags]    TC-VER-CL-021    REQ-NF-305
    ${r}=    Run Process    sudo    -n    systemctl    stop    mosquitto    timeout=60
    Should Be Equal As Integers    ${r.rc}    0    ${r.stderr}
    Sleep    3s
    ${firsts}=    Plain Listener Records    8883    150
    Log    first bytes of each connection attempt: ${firsts}
    Should Not Be Empty    ${firsts}    the Gateway made no connection attempt in 150 s
    FOR    ${hex}    IN    @{firsts}
        Should Start With    ${hex}    1603    an attempt began with ${hex}, not a TLS handshake
    END
    ${txt}=    Send Command    mqtt status    2.5
    Should Not Match Regexp    ${txt}    (?i)connected\\s+\\d    the Gateway reports an MQTT connection to a plaintext peer
    [Teardown]    Run Keywords    Run Process    sudo    -n    systemctl    start    mosquitto    timeout=60
    ...    AND    Sleep    5s
    ...    AND    Restart Collector
    ...    AND    Ensure Gateway Online

TC-VER-CL-022 Readings that are not valid are published as null, not as old values
    [Documentation]    REQ-SA-160. The Field Device is held in reset so none of its readings can be valid; a remote read makes the Gateway
    ...    publish at once. The Field Device block says online:false and every reading is null. (The Gateway's own channels use
    ...    the same encoding; host tests CP-T22 and CP-T28.)
    [Tags]    TC-VER-CL-022    REQ-SA-160
    Hold Board In Reset    FD
    Sleep    20s
    ${seen}=    Seconds Collected
    ${res}    ${secs}=    Time To Result    control    {"command":"read","req":801}    10
    Should Be Equal    ${res}[result]    accepted
    Sleep    2s
    ${tele}=    Messages On    telemetry    since=${seen}
    Should Not Be Empty    ${tele}    no telemetry after the remote read
    ${doc}=    Json Of    ${tele}[-1]
    Log    ${doc}[field_device]
    Should Be Equal    ${doc}[field_device][online]    ${False}
    Should Be Equal    ${doc}[field_device][temperature_deci_c]    ${None}
    Should Be Equal    ${doc}[field_device][humidity_pct]    ${None}
    Should Be Equal    ${doc}[field_device][pressure_hpa]    ${None}
    [Teardown]    Release Board    FD

TC-VER-CL-023 A reading is in the MQTT queue within 100 ms of the request, and the Gateway's sensor cycle takes under 100 ms
    [Documentation]    REQ-NF-106 and REQ-NF-100 (Gateway), on the board's own clock. A remote read makes SensorTask run one cycle and
    ...    CloudPublisher queue the telemetry; CloudPublisher logs the time between the request and the queued message. The longest
    ...    sensor cycle since start is also read over SWD (milliseconds, with pre-emption included).
    [Tags]    TC-VER-CL-023    REQ-NF-106    REQ-NF-100
    Send Command    ${EMPTY}    0.5
    ${res}    ${secs}=    Time To Result    control    {"command":"read","req":802}    10
    Should Be Equal    ${res}[result]    accepted
    Send Command    ${EMPTY}    3.0
    ${ms}=    Console Value    read: telemetry queued (\\d+) ms after the request
    Log    request to queued telemetry: ${ms} ms
    Should Be True    ${ms} <= 100    ${ms} ms from the request to the queued message (a cycle may take 100 ms, a queued message 200 ms)
    ${addr}=    Fd Symbol    gw_sensor_cycle_ms_max_address
    ${cycle}=    Read U32    GW    ${addr}
    Log    longest Gateway sensor cycle ${cycle} ms
    Should Be True    ${cycle} > 0    SensorTask has not completed a cycle
    Should Be True    ${cycle} < 100    the longest sensor cycle took ${cycle} ms, the limit is 100 ms
