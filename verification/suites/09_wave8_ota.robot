*** Settings ***
Documentation     Wave 8, OTA: a valid update, a rollback, a wrong signature, a corrupted image and a download that stalls and resumes.
...               Runs on the PC against the Raspberry Pi bench broker with the Gateway (bootloader + product image, running 1.0.1)
...               online (verification/run.sh 9). It changes the firmware version of the Gateway: 1.0.1 -> 1.0.3 -> 1.0.4. UNRUN when written.
Library           OperatingSystem
Library           ../libraries/Ota.py
Force Tags        wave8    ota
Test Setup        Settle And Log

*** Variables ***
${DEVICE}         002d004f3442501020323848

*** Keywords ***
Settle And Log
    [Documentation]    Results the Gateway publishes late carry no job id: let them arrive before the next case starts.
    ${left}=    Settle Gateway    ${DEVICE}
    Log    results drained before this case: ${left}

*** Test Cases ***
TC-VER-OTA-001 An update whose build fails its self-check is rolled back to the running firmware
    [Tags]    TC-VER-OTA-001    REQ-DM-072    REQ-DM-073
    ${a}    ${b}=    Make Images    selfcheckfail    1.0.2    1.0.2
    ${r}=    Run Update    ${DEVICE}    ${a}    ${b}    2001
    Log    ${r}
    Should Be Equal    ${r}    rolled_back    ${{ $r }}

TC-VER-OTA-002 An image signed with another key is refused and the running firmware stays
    [Tags]    TC-VER-OTA-002    REQ-DM-080    REQ-NF-304
    ${a}    ${b}=    Make Images    wrongkey    1.0.1    1.0.5    wrong_key=True
    ${r}=    Run Update    ${DEVICE}    ${a}    ${b}    2002
    Should Be True    '${r}' in ('failed', 'rejected')    a wrongly signed image ended with '${r}'

TC-VER-OTA-003 An image with a damaged body fails validation and the running firmware stays
    [Tags]    TC-VER-OTA-003    REQ-DM-061
    ${a}    ${b}=    Make Images    damaged    1.0.1    1.0.5
    ${r}=    Run Update    ${DEVICE}    ${a}    ${b}    2003    --corrupt-body
    Should Be True    '${r}' in ('failed', 'rejected')    a damaged image ended with '${r}'

TC-VER-OTA-004 A valid signed update downloads over MQTT/TLS and the result is reported
    [Tags]    TC-VER-OTA-004    REQ-DM-050    REQ-DM-055
    ${a}    ${b}=    Make Images    valid    1.0.1    1.0.3
    ${r}=    Run Update    ${DEVICE}    ${a}    ${b}    2004
    Should Be Equal    ${r}    success

TC-VER-OTA-005 A download interrupted by a two-minute broker outage resumes where it stopped
    [Documentation]    The link is down (broker stopped), so the Gateway pauses and resumes at the saved offset (REQ-DM-051).
    ...    A server that is merely silent with the link up is a different case: it exhausts the retries by design (REQ-DM-052).
    [Tags]    TC-VER-OTA-005    REQ-DM-051
    ${a}    ${b}=    Make Images    resume    1.0.1    1.0.4
    ${r}=    Run Update With Broker Outage    ${DEVICE}    ${a}    ${b}    2005    110
    ${o}=    Last Ota Output
    Log    ${o}
    Should Be Equal    ${r}    success
