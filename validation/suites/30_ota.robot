*** Settings ***
Documentation     Validation, UC-18 (deploy a firmware update) and UC-20 (verify firmware integrity), through the cloud API, on the real Gateway.
...               Order matters and is the safe one: the bad images first (the Gateway refuses them and keeps its firmware), then the real
...               update, then the update whose self-check fails (the bootloader must roll back). The images are made beforehand:
...               ~/rel/1.1.4 (good, signed with the release key), ~/ota-validation/tampered (one body byte flipped after signing),
...               ~/ota-validation/wrongkey (signed with a throw-away key), ~/rel-fail/1.1.5 (self-check forced to fail, release key).
...               The Gateway must run 1.1.3 when the suite starts. Needs VAL_PASS. Ask the owner before running: it updates the Gateway.
Library           ../libraries/ValCloud.py
Library           ../libraries/ValBoards.py
Library           Collections
Suite Setup       Start Session
Force Tags        validation    ota

*** Variables ***
${REL}            %{HOME}/rel/1.1.4
${BAD}            %{HOME}/ota-validation
${FAILREL}        %{HOME}/rel-fail/1.1.5
${GW_SWD}         openocd -f interface/stlink-dap.cfg -c "adapter serial 0670FF575157877567154553" -c "transport select dapdirect_swd" -f target/stm32l4x.cfg

*** Keywords ***
Start Session
    ${status}=    Login
    Should Be Equal As Integers    ${status}    200
    Install Helpers

Upload Pair
    [Arguments]    ${name}    ${dir}
    Delete Image    ${name}-a
    Delete Image    ${name}-b
    ${a}=    Upload Image    ${name}-a    ${dir}/gateway_slot_a.ota
    ${b}=    Upload Image    ${name}-b    ${dir}/gateway_slot_b.ota
    Should Be Equal As Integers    ${a}    201
    Should Be Equal As Integers    ${b}    201

Run Update Job
    [Documentation]    Start a job and poll it until it ends. With ${interrupt_at} > 0 the broker is stopped for ${interrupt_s} seconds
    ...    once that many bytes were served (UC-18 E2). Returns the final job row.
    [Arguments]    ${a}    ${b}    ${timeout}=900    ${interrupt_at}=${0}    ${interrupt_s}=${25}
    Clear Active Jobs
    ${status}    ${job}=    Start Ota    ${a}    ${b}
    Should Be Equal As Integers    ${status}    202    ${job}
    ${interrupted}=    Set Variable    ${False}
    ${end}=    Evaluate    time.time() + ${timeout}    modules=time
    ${row}=    Set Variable    ${job}
    FOR    ${i}    IN RANGE    1000
        ${row}=    Ota Job    ${job}[id]
        Log    job ${row}[id]: ${row}[state], ${row}[sent_bytes] of ${row}[size_b] bytes served
        Exit For Loop If    '${row}[state]' not in ('sent', 'running')
        ${now}=    Evaluate    time.time()    modules=time
        Run Keyword If    ${now} > ${end}    Fail    the job is still ${row}[state] after ${timeout} s: ${row}
        ${cut}=    Evaluate    ${interrupt_at} > 0 and not ${interrupted} and ${row}[sent_bytes] >= ${interrupt_at}
        IF    ${cut}
            Log    interrupting the broker for ${interrupt_s} s at ${row}[sent_bytes] bytes    WARN
            Pi Shell    sudo systemctl stop mosquitto; sleep ${interrupt_s}; sudo systemctl start mosquitto    timeout=${{ ${interrupt_s} + 60 }}
            ${interrupted}=    Set Variable    ${True}
        END
        Sleep    5s
    END
    Log    final job row: ${row}
    RETURN    ${row}

Clear Active Jobs
    [Documentation]    A job left running by an earlier case would make every new one fail with 'already in progress'.
    ${ids}=    Active Ota Jobs
    FOR    ${id}    IN    @{ids}
        Log    cancelling the stale job ${id}    WARN
        Cancel Ota    ${id}
    END

Wait For Firmware
    [Arguments]    ${version}    ${timeout}=420
    ${end}=    Evaluate    time.time() + ${timeout}    modules=time
    ${seen}=    Set Variable    ${None}
    FOR    ${i}    IN RANGE    500
        ${seen}=    Run Keyword And Return Status    Firmware Is    ${version}
        Exit For Loop If    ${seen}
        ${now}=    Evaluate    time.time()    modules=time
        Run Keyword If    ${now} > ${end}    Fail    the Gateway did not report firmware ${version} within ${timeout} s
        Sleep    10s
    END

Firmware Is
    [Arguments]    ${version}
    ${d}=    Device View
    Should Be Equal    ${d}[fw_version]    ${version}
    Should Be True    ${d}[online]

*** Test Cases ***
UC-20 A tampered image is refused and the running firmware stays
    [Documentation]    The image was signed correctly and then one byte of its body was flipped: the integrity check must fail
    ...    (UC-20 rainy day), the image is dropped and a nack is sent (UC-18 rainy day).
    [Tags]    UC-20    UC-18
    ${before}=    Firmware Version
    Should Be Equal    ${before}    1.1.3
    Upload Pair    val-tamper    ${BAD}/tampered
    ${row}=    Run Update Job    val-tamper-a    val-tamper-b
    Log    the Gateway's answer: ${row}[detail]
    Should Not Be Equal    ${row}[state]    success
    Should Be True    '${row}[state]' in ('failed', 'rejected')    the tampered image ended as ${row}[state]
    Sleep    20s
    Firmware Is    1.1.3

UC-20 An image signed with another key is refused and the running firmware stays
    [Documentation]    A well-formed image whose signature does not verify with the key built into the Gateway.
    [Tags]    UC-20    UC-18
    Upload Pair    val-wrongkey    ${BAD}/wrongkey
    ${row}=    Run Update Job    val-wrongkey-a    val-wrongkey-b
    Log    the Gateway's answer: ${row}[detail]
    Should Be True    '${row}[state]' in ('failed', 'rejected')    the wrong-key image ended as ${row}[state]
    Sleep    20s
    Firmware Is    1.1.3

UC-18 A valid update is downloaded through a lost connection and the new firmware runs
    [Documentation]    Main flow with E2: the broker is stopped for 25 s in the middle of the download; the download must continue
    ...    when the connection is back. Then verification, activation, reboot, self-check and the ack (success).
    [Tags]    UC-18
    Upload Pair    val-114    ${REL}
    ${row}=    Run Update Job    val-114-a    val-114-b    timeout=1200    interrupt_at=${100000}
    Log    the Gateway's answer: ${row}[detail]
    Should Be Equal    ${row}[state]    success
    Wait For Firmware    1.1.4

UC-18 E3 A new firmware whose self-check fails is rolled back to the previous one
    [Documentation]    1.1.5 is built with the trial self-check forced to fail: after the reboot the bootloader must return to 1.1.4.
    [Tags]    UC-18
    Upload Pair    val-115fail    ${FAILREL}
    ${row}=    Run Update Job    val-115fail-a    val-115fail-b    timeout=1200
    Log    the Gateway's answer: ${row}[detail]
    Should Be True    '${row}[state]' in ('rolled_back', 'failed')    the failing firmware ended as ${row}[state]
    Wait For Firmware    1.1.4

UC-18 E1 An update for a Gateway that is not connected is refused
    [Documentation]    The Gateway's CPU is halted for 170 s, longer than the online window of 150 s, with the broker up. The API
    ...    should say that the device is offline instead of starting a job that nobody will run.
    [Tags]    UC-18    E1
    [Teardown]    Pi Shell    ${GW_SWD} -c init -c "reset run" -c exit
    Pi Shell    ${GW_SWD} -c init -c "reset halt" -c exit
    Clear Active Jobs
    Sleep    170s
    ${status}    ${body}=    Start Ota    val-114-a    val-114-b
    Log    API answer: ${status} ${body}
    Run Keyword If    ${status} == 202    Cancel Ota    ${body}[id]
    Should Be True    ${status} in (409, 503)    the API accepted the update (${status}) although the Gateway has not reported for 170 s: ${body}
    Should Contain    ${body}[detail]    offline    the refusal does not say that the device is offline: ${body}
