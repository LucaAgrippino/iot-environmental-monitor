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
        ${d}=    Evaluate    ${seen}[${i}] - ${seen}[${i}-1]
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
    Should Match Regexp    ${txt}    17\\s.*0x1234
    ${bad}    ${secs}=    Time To Result    control    {"command":"profile_add","slave":17,"map":1,"req":702}    8
    Should Be Equal    ${bad}[result]    rejected
    Should Be Equal    ${bad}[reason]    bad_format
    ${gone}    ${secs}=    Time To Result    control    {"command":"profile_remove","slave":17,"req":703}    8
    Should Be Equal    ${gone}[result]    accepted
    ${txt}=    Send Command    profiles list    2.5
    Should Not Match Regexp    ${txt}    slave\\s+17\\b
    ${again}    ${secs}=    Time To Result    control    {"command":"profile_remove","slave":17,"req":704}    8
    Should Be Equal    ${again}[reason]    not_found
    [Teardown]    Run Keyword And Ignore Error    Send Command    profiles remove 17    2.0
