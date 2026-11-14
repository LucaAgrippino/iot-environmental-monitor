*** Settings ***
Documentation     Validation, Remote Operator use cases in a real browser (Playwright, headless Chromium) on the local
...               cloud web app. Needs VAL_PASS. Run it when the API suite is not running: both drive the same device.
Library           ../libraries/ValWeb.py
Library           ../libraries/ValCloud.py
Suite Setup       Open Browser
Suite Teardown    Close Browser
Force Tags        validation    web

*** Test Cases ***
Web-01 Nobody sees the devices without signing in
    [Tags]    UC-05
    ${s}=    Api Status Without Login    /devices
    Should Be Equal As Integers    ${s}    401
    New Visitor
    Go    /
    Text Visible    Sign in to see the devices.
    Screenshot    web-01-login

Web-02 A wrong password is refused with a message
    [Tags]    UC-05
    New Visitor
    Go    /
    Sign In    validator    not-the-password
    Text Visible    Sign in to see the devices.
    ${body}=    Body Text
    Should Not Contain    ${body}    Sign out

Web-03 The operator signs in and sees the device online with its readings
    [Tags]    UC-05    UC-06
    New Visitor
    Go    /
    Sign In As Validator
    Text Visible    Sign out
    Text Visible    online
    ${vals}=    Big Values
    Length Should Be    ${vals}    4
    Should Not Contain    ${vals}[0]    -
    Screenshot    web-03-overview

Web-04 The overview shows the health table
    [Tags]    UC-06
    Text Visible    Health
    Text Visible    uptime_s
    Text Visible    lifecycle_state

Web-05 The overview presents the motion and magnetic sensors
    [Documentation]    UC-05 step 2 lists accelerometer, gyroscope and magnetometer: finding F-2.
    [Tags]    UC-05    finding-F-2
    ${body}=    Body Text
    ${low}=    Evaluate    $body.lower()
    Should Contain Any    ${low}    accel    gyro    magnet

Web-06 The charts draw the history
    [Tags]    UC-05
    Open Tab    Charts
    Text Visible    readings
    ${ok}=    Chart Has Data    Temperature
    Should Be True    ${ok}
    ${ok}=    Chart Has Data    Pressure
    Should Be True    ${ok}
    Screenshot    web-06-charts

Web-07 The alarm history lists the alarms with their source
    [Tags]    UC-09    UC-12
    Open Tab    Alarms
    Text Visible    Alarm history
    ${rows}=    Table Rows    Alarm history
    ${n}=    Get Length    ${rows}
    Should Be True    ${n} >= 1
    Screenshot    web-07-alarms

Web-08 The operator sends Read now and sees the answer
    [Tags]    UC-14
    Open Tab    Commands
    Click Button    Read now
    Wait For Row Containing    Commands sent    read    timeout=20000
    Wait For Row Containing    Commands sent    accepted    timeout=20000
    Screenshot    web-08-commands

Web-09 Restart needs a separate confirmation on screen
    [Documentation]    UC-17: pressing Restart only asks; nothing restarts until the confirm button is pressed.
    [Tags]    UC-17
    Click Button    Restart…
    Text Visible    Confirm restart
    Screenshot    web-09-restart-confirm
    # leave the 30 s window to close: a second restart request inside it would count as the confirmation (finding F-5)
    Sleep    35s

Web-10 The operator signs out
    [Tags]    UC-05
    Click Button    Sign out
    Text Visible    Sign in to see the devices.
