*** Settings ***
Documentation     Closure cases for the update path: images the Gateway must refuse before the cryptography, a download that
...               stalls, a second update command while one runs, and the authentication of the command channel. None of these
...               updates reaches the activation step, so the running firmware stays (1.0.1). Runs on the Raspberry Pi, next to the
...               broker and the Gateway's console (the Gateway's own log lines are part of the evidence), with a throw-away
...               signing key: the refusals tested here all come before the signature check, so the real OTA key never leaves
...               the PC. Start it with ~/run-ota-closure.sh. Ask the owner before running: it uploads to the Gateway.
Library           OperatingSystem
Library           Process
Library           ../libraries/Ota.py
Library           ../libraries/Cloud.py
Library           ../libraries/PortCapture.py
Force Tags        closure    ota
Test Setup        Settle And Log
Test Teardown     Stop Port Capture

*** Variables ***
${DEVICE}         002d004f3442501020323848
${PI}             192.168.0.209
${GW_PORT}        /dev/serial/by-id/usb-STMicroelectronics_STM32_STLink_0670FF575157877567154553-if02

*** Keywords ***
Settle And Log
    [Documentation]    Results the Gateway publishes late carry no job id: let them arrive before the next case starts.
    ${left}=    Settle Gateway    ${DEVICE}
    Log    results drained before this case: ${left}

Update Should Be Refused
    [Documentation]    The Gateway downloads the whole image (about 100 s at 3 KiB/s) and refuses it in the verify step, with its
    ...    reason on the result topic and an ERROR line on its own console. An image larger than a slot is refused at once.
    [Arguments]    ${mode}    ${job}    @{reasons}
    ${a}    ${b}=    Make Malformed Images    ${mode}    1.0.1    1.0.5    ${mode}
    Start Port Capture    ${GW_PORT}    460800
    ${r}=    Run Update    ${DEVICE}    ${a}    ${b}    ${job}    timeout=330
    ${why}=    Last Ota Reason
    Log    ${mode}: ${r}, reason ${why}
    Should Be True    '${r}' in ('failed', 'rejected')    the ${mode} image ended with '${r}'
    Should Contain    ${reasons}    ${why}    the ${mode} image was refused for '${why}', expected one of ${reasons}
    Port Capture Should Contain    OTA job ${job} failed: ${why}    15
    [Teardown]    Stop Port Capture

*** Test Cases ***
TC-VER-OTA-006 Images with a bad size, format or header are refused and the reason is reported
    [Documentation]    REQ-DM-060: the checks before the signature (header magic, declared header size, image larger than a slot).
    ...    REQ-DM-062: the Gateway reports the refusal with its reason (the log line is on the Gateway's own debug stream).
    [Tags]    TC-VER-OTA-006    REQ-DM-060    REQ-DM-062
    Update Should Be Refused    magic    3001    bad_header    bad_image
    Update Should Be Refused    header_size    3002    bad_header    bad_image
    Update Should Be Refused    oversize    3003    too_large    bad_header    bad_image

TC-VER-OTA-007 A download that stalls with the link up fails after the retries and the partial image is discarded
    [Documentation]    The server goes silent at 64 KB while the link stays up: three consecutive chunk timeouts end the update
    ...    (REQ-DM-052) with an error report (REQ-DM-053). The next update starts again at offset 0: nothing was kept.
    [Tags]    TC-VER-OTA-007    REQ-DM-052    REQ-DM-053
    ${a}    ${b}=    Make Images    stall    1.0.1    1.0.5
    Start Port Capture    ${GW_PORT}    460800
    ${r}=    Run Update    ${DEVICE}    ${a}    ${b}    3004    --silent-at 65536 --silent-seconds 240    timeout=240
    ${why}=    Last Ota Reason
    Should Be True    '${r}' in ('failed', 'rejected')    a stalled download ended with '${r}'
    Should Contain    ${{ ['stalled', 'download_failed'] }}    ${why}    the stalled download was reported as '${why}'
    Port Capture Should Contain    failure 3 of 3    15
    Port Capture Should Contain    OTA job 3004 failed: ${why}    15
    Stop Port Capture
    Settle And Log
    ${r2}=    Run Update    ${DEVICE}    ${a}    ${b}    3005    --silent-at 65536 --silent-seconds 240    timeout=240
    ${out}=    Last Ota Output
    ${first}=    Evaluate    int(re.findall(r'slot [AB] offset (\d+)', $out)[0])    modules=re
    Should Be Equal As Integers    ${first}    0    the second update resumed at offset ${first}: the partial image was kept

TC-VER-OTA-008 A second update command while one is running is refused as busy
    [Tags]    TC-VER-OTA-008    REQ-DM-054
    ${a}    ${b}=    Make Images    busy    1.0.1    1.0.5
    Start Update    ${DEVICE}    ${a}    ${b}    3006    --silent-at 65536 --silent-seconds 60
    Sleep    6s
    ${r}=    Run Update    ${DEVICE}    ${a}    ${b}    3007    timeout=150
    ${why}=    Last Ota Reason
    Finish Update    150
    Should Be Equal    ${r}    rejected    the second command ended with '${r}'
    Should Be Equal    ${why}    busy

TC-VER-OTA-009 The update command channel accepts only authenticated clients
    [Documentation]    The Gateway receives update commands on its own topic over the mutual-TLS broker connection; the broker
    ...    refuses a client without a certificate, so an unauthenticated source cannot publish a command (REQ-DM-056).
    [Tags]    TC-VER-OTA-009    REQ-DM-056
    ${refused}=    Broker Refuses A Client Without Certificate    ${PI}
    Should Be True    ${refused}    the broker accepted a client without a certificate
