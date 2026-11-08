*** Settings ***
Documentation     Wave 1: the smoke test, the gate of every later run. Both boards boot to a working console.
Resource          ../resources/common.resource
Force Tags        wave1    smoke
Suite Teardown    Close Console

*** Test Cases ***
TC-VER-SM-001 Field Device boots and answers on its console
    [Tags]    TC-VER-SM-001    REQ-LI-000    REQ-LI-010    REQ-NF-503
    Open FD Console
    Reset And Wait    FD    30
    ${out}=    Send Command    version    1.5
    Console Should Contain    v0.0.0
    ${out}=    Send Command    help    1.5
    Console Should Contain    status

TC-VER-SM-002 Gateway boots and answers on its console
    [Tags]    TC-VER-SM-002    REQ-LI-000    REQ-LI-010    REQ-NF-503
    Open GW Console
    Reset And Wait    GW    60
    Command Should Answer    version    v0.0.0    wait=2.0    tries=4
    Command Should Answer    help    status    wait=2.0    tries=4
