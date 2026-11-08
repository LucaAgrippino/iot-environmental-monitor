*** Settings ***
Library     OperatingSystem
Library     String
Library     ../libraries/Boards.py
Library     ../libraries/ConsoleLib.py
Library     ../libraries/Screens.py

*** Variables ***
${FD_PORT}      /dev/serial/by-id/usb-STMicroelectronics_STM32_STLink_066BFF303435554157025015-if02
${GW_PORT}      /dev/serial/by-id/usb-STMicroelectronics_STM32_STLink_0670FF575157877567154553-if02
${OUT}          ${EXECDIR}/verification/build/captures

*** Keywords ***
Open FD Console
    Open Console    FD    ${FD_PORT}

Open GW Console
    Open Console    GW    ${GW_PORT}

Reset And Wait
    [Arguments]    ${board}    ${seconds}=45
    Reset Board    ${board}
    Sleep    2s
    Wait For Boot    ${seconds}
