*** Settings ***
Documentation     Wave 0: the checks that need no board. The host tests are re-run at the baseline and credited to the
...               requirements verified at unit level; the memory, symbol and tool checks are the analysis and inspection items.
Library           OperatingSystem
Library           ../libraries/Baseline.py
Force Tags        wave0
Suite Setup       Create Directory    ${EXECDIR}/verification/build/captures

*** Variables ***
${CLANG_FORMAT}    %{HOME}/.venvs/envmon-verify/bin/clang-format

*** Test Cases ***
TC-VER-U-001 Host tests of the default project pass
    [Tags]    TC-VER-U-001    unit-credit
    ${tags}=    Unit Level Requirements
    Set Tags    @{tags}
    ${tested}    ${passed}    ${failed}=    Run Host Tests    default
    Log    default project: ${tested} tested, ${passed} passed, ${failed} failed
    Should Be Equal As Integers    ${failed}    0
    Should Be True    ${tested} > 800

TC-VER-U-002 Host tests of the Gateway project pass
    [Tags]    TC-VER-U-002    unit-credit
    ${tested}    ${passed}    ${failed}=    Run Host Tests    gateway
    Log    gateway project: ${tested} tested, ${passed} passed, ${failed} failed
    Should Be Equal As Integers    ${failed}    0
    Should Be True    ${tested} > 1500

TC-VER-NF-001 Gateway image fits the RAM and the flash of the chip
    [Tags]    TC-VER-NF-001    REQ-NF-400    REQ-NF-401
    ${u}=    Build Usage    GW
    Log    ${u}
    Should Be True    ${u}[ram] <= ${u}[ram_limit]
    Should Be True    ${u}[sram2] <= ${u}[sram2_limit]
    Should Be True    ${u}[flash] <= ${u}[flash_limit]
    Should Be Equal As Integers    ${u}[flash_limit]    1048576

TC-VER-NF-002 Field Device image fits the RAM and the flash of the chip
    [Tags]    TC-VER-NF-002    REQ-NF-403    REQ-NF-404
    ${u}=    Build Usage    FD
    Log    ${u}
    Should Be True    ${u}[ram] <= ${u}[ram_limit]
    Should Be True    ${u}[flash] <= ${u}[flash_limit]
    Should Be Equal As Integers    ${u}[flash_limit]    2097152

TC-VER-NF-003 Gateway QSPI partitions fit the device and the log and buffer limits
    [Tags]    TC-VER-NF-003    REQ-NF-402    REQ-NF-406    REQ-NF-407
    ${p}=    Partition Sizes    GW
    Log    ${p}
    ${total}=    Evaluate    sum($p.values())
    Should Be True    ${total} <= 8*1024*1024
    ${cfl}=    Set Variable    ${p}[CircularFlashLog]
    Should Be True    ${cfl} <= 3*1024*1024
    Should Be True    ${cfl} <= 4*1024*1024

TC-VER-NF-004 Field Device QSPI partitions fit the device
    [Tags]    TC-VER-NF-004    REQ-NF-405
    ${p}=    Partition Sizes    FD
    ${total}=    Evaluate    sum($p.values())
    Should Be True    ${total} <= 16*1024*1024

TC-VER-NF-005 No project object calls malloc or free
    [Tags]    TC-VER-NF-005    REQ-NF-408
    FOR    ${board}    IN    GW    FD
        ${bad}=    Objects Calling Malloc    ${board}
        Should Be Empty    ${bad}    ${board}: ${bad}
    END

TC-VER-NF-006 HAL and BSP headers are included only by the LCD driver
    [Tags]    TC-VER-NF-006    REQ-NF-501    REQ-NF-506
    ${rc}    ${out}=    Run Tool    bash scripts/check-hal-encapsulation.sh
    Should Be Equal As Integers    ${rc}    0    ${out}

TC-VER-NF-007 Formatting and static analysis are clean (tool part of the style review)
    [Documentation]    Evidence for REQ-NF-502 only: the BARR-C review by reading is still to be done.
    [Tags]    TC-VER-NF-007    REQ-NF-502    evidence-partial
    ${rc}    ${out}=    Run Tool    set -o pipefail; git ls-files 'firmware/*.c' 'firmware/*.h' | grep -E '/(drivers|middleware|application|integration-tests|bootloader|boot-stub)/|^firmware/shared/' | grep -v firmware_version.h | xargs ${CLANG_FORMAT} --dry-run --Werror 2>&1 | tail -3
    Should Be Equal As Integers    ${rc}    0    ${out}
    ${rc2}    ${out2}=    Run Tool    cppcheck --enable=style,warning,performance -I firmware/shared --suppressions-list=cppcheck-suppressions.txt --suppress=unmatchedSuppression --inline-suppr --error-exitcode=1 -q firmware/gateway/application firmware/gateway/middleware/cert_store firmware/field-device/application firmware/field-device/drivers/adc
    Should Be Equal As Integers    ${rc2}    0    ${out2}

TC-VER-NF-008 Build configurations have exactly one main
    [Tags]    TC-VER-NF-008    REQ-NF-501
    ${rc}    ${out}=    Run Tool    python3 scripts/check-build-exclusions.py --config Debug --config Release firmware/gateway && python3 scripts/check-build-exclusions.py --config Debug --config Release firmware/field-device
    Should Be Equal As Integers    ${rc}    0    ${out}
