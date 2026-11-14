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
# a 1.0.2 built with --fail-selfcheck and signed with the real key on the PC (scripts/build-release.sh), copied to the Pi
${FAIL_A}         %{HOME}/rel-fail/1.0.2/gateway_slot_a.ota
${FAIL_B}         %{HOME}/rel-fail/1.0.2/gateway_slot_b.ota
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
    ${r}=    Run Update    ${DEVICE}    ${a}    ${b}    ${job}    timeout=630
    ${why}=    Last Ota Reason
    Log    ${mode}: ${r}, reason ${why}
    Should Be True    '${r}' in ('failed', 'rejected')    the ${mode} image ended with '${r}'
    Should Contain    ${reasons}    ${why}    the ${mode} image was refused for '${why}', expected one of ${reasons}
    # the error line: 'OTA job N failed: reason' after a download, a rejection line for a refused command
    Port Capture Should Contain    OTA job ${job}    15
    Port Capture Should Contain    ${why}    15
    [Teardown]    Stop Port Capture

*** Test Cases ***
TC-VER-OTA-006 An image with a bad header magic is refused and the reason is reported
    [Documentation]    REQ-DM-060: the Gateway downloads the image and refuses it in the verify step (bad_header). REQ-DM-062: the
    ...    refusal is on the result topic and an ERROR line on the Gateway's own console.
    [Tags]    TC-VER-OTA-006    REQ-DM-060    REQ-DM-062
    Update Should Be Refused    magic    3001    bad_header    bad_image

TC-VER-OTA-010 An image whose declared header size is wrong is refused and the reason is reported
    [Tags]    TC-VER-OTA-010    REQ-DM-060    REQ-DM-062
    Update Should Be Refused    header_size    3002    bad_header    bad_image

TC-VER-OTA-011 An image larger than a slot is refused at once and the reason is reported
    [Documentation]    The declared size exceeds a slot (480 KB): the command itself is refused (bad_command), before any download.
    [Tags]    TC-VER-OTA-011    REQ-DM-060    REQ-DM-062
    Update Should Be Refused    oversize    3003    bad_command    too_large    bad_header    bad_image

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
    ${first}=    Evaluate    int(re.findall(r'slot [AB] offset (\\d+)', $out)[0])    modules=re
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

TC-VER-OTA-012 A failed self-check rolls back and the previous firmware is operational again within 10 s
    [Documentation]    REQ-NF-204 and REQ-DM-071. A 1.0.2 built to fail its self-check is installed; its trial boot waits for the verdict (the
    ...    self-check budget is 60 s), fails, and the Gateway rolls back. The time from "self-check failed: rolling back" to the
    ...    previous firmware's "Operational" is taken from the Gateway's own console. The firmware is 1.0.1 again afterwards.
    [Tags]    TC-VER-OTA-012    REQ-NF-204    REQ-DM-071
    Start Port Capture    ${GW_PORT}    460800
    ${r}=    Run Update    ${DEVICE}    ${FAIL_A}    ${FAIL_B}    4001    timeout=900
    Should Be Equal    ${r}    rolled_back
    Sleep    45s
    Port Capture Should Contain    self-check failed: rolling back    5
    ${failed}=    Port Capture Time Of    self-check failed: rolling back
    ${ok}=    Port Capture Time Of    ] Operational    ${failed}
    ${gap}=    Evaluate    ${ok} - ${failed}
    Log    rollback: self-check failed -> previous firmware operational in ${gap} s
    Should Be True    0 < ${gap} <= 10    the previous firmware was operational ${gap} s after the failed self-check, the limit is 10 s
    [Teardown]    Stop Port Capture
