/**
 * @file wifi_at_commands.h
 * @brief Inventek IWIN AT command codes used by WifiDriver (Gateway).
 *
 * Every command is `<CODE>[=<value>]<CR>` — the ISM43362's IWIN command
 * set has no Hayes "AT+" attention prefix (companion §3.3, WIFI-D12).
 * Verified against the ISM43362-M3G-L44 IWIN AT Command Set User Manual
 * (DOC-UM-20035-4.1) and the IWIN quick reference
 * (DOC-esWiFi_AT_Command_20041.1.20).
 *
 * `?` (Print Help) is intentionally not defined here: the quick
 * reference's Help Commands table marks it "Not available in SPI
 * firmware." WifiDriver uses `I?` alone as its post-reset liveness and
 * firmware-version check (see wifi_create() in wifi_driver.c).
 */

#ifndef WIFI_AT_COMMANDS_H
#define WIFI_AT_COMMANDS_H

/** Module info: PID, FW rev, API rev, stack rev, RTOS rev, CPU clk, product name (User Manual
 * §4.11.1). */
#define WIFI_AT_INFO "I?"

/* Network join (User Manual §4.6) */
#define WIFI_AT_SET_SSID "C1"
#define WIFI_AT_SET_PASSPHRASE "C2"
#define WIFI_AT_SET_SECURITY "C3"
#define WIFI_AT_SET_DHCP "C4"
#define WIFI_AT_JOIN "C0"
#define WIFI_AT_DISCONNECT "CD"
#define WIFI_AT_GET_RSSI "CR"

/* Transport / sockets (User Manual §4.13-4.15) */
#define WIFI_AT_SET_SOCKET "P0"
#define WIFI_AT_SET_PROTOCOL "P1"
#define WIFI_AT_SET_REMOTE_HOST "P3"
#define WIFI_AT_SET_REMOTE_PORT "P4"
#define WIFI_AT_SET_CLIENT "P6"
#define WIFI_AT_SET_RECV_PACKET_SIZE "R1"
#define WIFI_AT_SET_RECV_TIMEOUT "R2" /**< Read transport timeout, ms (User Manual §4.14). */
#define WIFI_AT_RECV_DATA "R0"
#define WIFI_AT_SEND_DATA "S3"

/* Fixed command values */
/** C3= security type (IWIN: "WPA + WPA2" / "WPA2 Mixed"). */
#define WIFI_SECURITY_WPA2_MIXED "4"
#define WIFI_DHCP_ENABLE "1" /**< C4= DHCP enable. */

/* Response framing (User Manual §1.4.2; datasheet §10.2.1). The error
 * marker deliberately has no trailing "\r\n": real error responses carry
 * a description right after the word, e.g. "\r\nERROR: Unknown Error\r\n"
 * (confirmed on hardware, and matches the "MT" example in the quick
 * reference doc) — matching only "\r\nERROR" catches both that and the
 * bare "\r\nERROR\r\n" form. */
#define WIFI_RESP_OK_MARKER "\r\nOK\r\n"
#define WIFI_RESP_ERROR_MARKER "\r\nERROR"

/* Required module firmware (UM2153 §7.11.3) — substring-matched against
 * the WIFI_AT_INFO response's FW Revision field. */
#define WIFI_FIRMWARE_VERSION "C3.5.2.3"

#endif /* WIFI_AT_COMMANDS_H */
