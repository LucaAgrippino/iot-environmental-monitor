# Bug Log — WifiDriver

No bug was intentionally planted in this module for interview-prep purposes
(see `exercise.md` instead for that). One real defect was found and fixed
during development, before the first Ceedling run — documented here because
it is a realistic, findable-with-a-debugger class of bug.

## Response buffer parsed against its full capacity instead of the bytes actually received

**File:** firmware/gateway/drivers/wifi_driver/wifi_driver.c
**Line:** `wifi_get_rssi()` / `wifi_recv()` (draft versions, before the fix below)
**Category:** wrong bound / stale-data read

**What the code did (draft):**
`prv_at_command()` filled `inst->at_buf` with only as many bytes as the
module actually sent, but the earliest draft of `wifi_get_rssi()` and
`wifi_recv()` passed the full `WIFI_AT_BUF_SIZE` (512) to the downstream
parser (`prv_parse_rssi()`) and to the `"\r\nOK\r\n"`-stripping logic in
`wifi_recv()`, instead of the true received length.

**What it should do:**
Only the bytes actually written by the current AT transaction are valid;
anything past that is leftover content from a previous, possibly longer,
response (`at_buf` is reused across every call for both TX and RX). Parsing
against the full buffer risks matching a stale marker from an earlier
transaction, and in `wifi_recv()` it looked for the trailing `"\r\nOK\r\n"`
at the very end of the 512-byte buffer instead of right after the real
payload — silently corrupting the returned payload length on any response
shorter than the buffer.

**Correct fix:**

    /* before */
    static wifi_err_t prv_at_command(struct wifi_inst *inst, const uint8_t *cmd,
                                      size_t cmd_len, char *resp_buf, size_t resp_buf_len);
    ...
    const wifi_err_t parse_err = prv_parse_rssi(handle->at_buf, WIFI_AT_BUF_SIZE, &handle->rssi_dbm);

    /* after */
    static wifi_err_t prv_at_command(struct wifi_inst *inst, const uint8_t *cmd,
                                      size_t cmd_len, char *resp_buf, size_t resp_buf_len,
                                      size_t *out_resp_len);
    ...
    size_t resp_len = 0u;
    const wifi_err_t err = prv_at_command(handle, at_rssi, sizeof(at_rssi) - 1u, handle->at_buf,
                                           WIFI_AT_BUF_SIZE, &resp_len);
    const wifi_err_t parse_err = prv_parse_rssi(handle->at_buf, resp_len, &handle->rssi_dbm);

**How to find it with a debugger:**
Set a breakpoint at the top of `wifi_get_rssi()`. Step into `prv_at_command()`
and watch `received` in `prv_recv_words()` — note it's e.g. 16 bytes for a
short RSSI response. Step back out and watch what length is passed to
`prv_parse_rssi()`: in the buggy version it's `WIFI_AT_BUF_SIZE` (512), not
16. Inspect `handle->at_buf[16..511]` — it holds whatever the previous AT
command's response left behind (e.g. the longer `AT+GMR` firmware-version
string from `wifi_create()`), which a coincidentally-matching substring could
cause `prv_parse_rssi()` or the `wifi_recv()` OK-marker search to misfire on.

**Why it would have passed CI unnoticed:**
Ceedling/cppcheck have no notion of "logical length vs. buffer capacity" —
the code is memory-safe (never reads past `at_buf`'s actual allocation) and
compiles cleanly. Only a unit test that scripts *two different-length*
responses back-to-back (e.g. `wifi_create()`'s longer `AT+GMR` reply followed
by a short `AT+WRSSI` reply) would have caught the stale-data risk. This
implementation was caught by design review before any test was written,
which is why none of the 18 committed unit tests exercise the buggy path —
worth adding as a regression test if this module is revisited.
