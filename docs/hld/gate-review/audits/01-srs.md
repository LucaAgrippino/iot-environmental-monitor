# Audit — docs/SRS.md
Auditor: Claude (gate-review chat 1)
Severity counts: 17B / 17F / 4D / 6C

---

## §1: Introduction — clean.

---

## §2.1: Sensor Acquisition [SA] — 5 defects

| ID | Severity | Description | Suggested fix |
|----|----------|-------------|---------------|
| SRS-001 | B | `[REQ-SA-090]` "store the most recent [TBD] readings per sensor" — TBD ring-buffer depth makes the requirement untestable. No rationale for deferral; §1.3 mandates TBD resolution before HLD. | Commit a concrete value with a cited source (memory budget calculation or HLD pre-allocation). |
| SRS-002 | B | `[REQ-SA-140]` "low pass filter with parameters [TBD]" — both the filter order and cut-off frequency are TBD. Untestable and has no deferral rationale. | State the parameter values and cite the source (signal-conditioning analysis). |
| SRS-003 | F | `[REQ-SA-080]` "The system shall log the error code if the reading fails" — "the reading" is ambiguous: sensor read? Modbus read? Configuration read? All three failure paths exist in §2.1. | Qualify the subject: "…if a sensor read operation fails." |
| SRS-004 | F | `[REQ-SA-160]` "The system shall publish sensor data marked as invalid to the cloud with an error flag" sits in the Sensor Acquisition section but its responsibility is cloud publication. Violates P6 (responsibility traces to component, not intent). | Relocate to §2.6 Cloud Communication; add a cross-reference in §2.1. |
| SRS-005 | F | `[REQ-SA-0E1]`, `[REQ-LD-0E1]`, `[REQ-LI-0E1]`, `[REQ-LI-0E2]`, `[REQ-LI-0E3]`, `[REQ-MB-0E1]`, `[REQ-TS-0E1]` — The "0E1" suffix violates the §1.4 notation `[REQ-XX-NNN]` where NNN is a numeric sequence. Consistent use of alphabetic "E" breaks the scheme. | Assign sequential numeric IDs (e.g., REQ-SA-180, REQ-SA-190) and use a comment annotation to mark them as exception paths. |

---

## §2.2: Alarm Management [AM] — 2 defects

| ID | Severity | Description | Suggested fix |
|----|----------|-------------|---------------|
| SRS-006 | B | `[REQ-AM-011]` "apply configurable hysteresis when clearing alarms" — no unit (°C? %RH? Pa?), no range, no default. A tester cannot verify this is correct. | State the hysteresis unit, the configurable range, and the default value; cite the source. |
| SRS-007 | C | Section opening comment `<!-- Traces to: UC-08, UC-09, UC-03 -->` lists UC-03, but no AM requirement traces to UC-03 in §6. | Remove UC-03 from the comment, or add the missing requirement and matrix row. |

---

## §2.3: Local Display — LCD [LD] — 5 defects

| ID | Severity | Description | Suggested fix |
|----|----------|-------------|---------------|
| SRS-008 | B | `[REQ-LD-050]` "refresh the displayed sensor readings at the polling rate" conflicts with `[REQ-NF-108]` "refresh rate of 5Hz for the field node LCD." Default polling rate is 1 Hz (REQ-NF-110); the LCD refresh rate is 5 Hz. Both use "refresh" to mean content update. Internal contradiction. | Disambiguate: REQ-LD-050 should specify the data-update cadence ("update displayed values when new data is available"); REQ-NF-108 should own the hardware frame rate. Cross-reference both. |
| SRS-009 | B | `[REQ-LD-210]` "The splash screen shall include…", `[REQ-LD-220]` "The splash screen progress bar shall update…", `[REQ-LD-230]` "The splash screen shall display…", `[REQ-LD-240]` "The splash screen shall display…" — subjects are "The splash screen" and "The splash screen progress bar," not "The system." Violates the §1.4 mandatory notation "The system shall [base verb]." | Rewrite all four with "The system shall" as the subject. |
| SRS-010 | C | `[REQ-LD-150]` ends with a stray "ù" character: "…when they are successfully applied ù". | Delete the character. |
| SRS-011 | C | `[REQ-LD-0E1]` is placed in the document after the splash screen group (REQ-LD-200–240), visually separating it from the configuration-confirmation flow it covers (REQ-LD-120–140). | Move REQ-LD-0E1 immediately after REQ-LD-140 where it belongs logically. |
| SRS-012 | C | `[REQ-LD-100]` lists "display settings" as configurable via the LCD configuration screen. UC-15 Remote Operator alternate flow does not include display settings, which is correct (Tier 2 vision §10). No conflict — cosmetic note only that the scope restriction is implicit; a comment linking it to Tier 2 would improve traceability. | Add an inline comment `<!-- Tier 2: LCD + remote; display settings LCD only per vision §10 -->`. |

*(REQ-LD-200–240 absence from the traceability matrix is reported at §6.)*

---

## §2.4: Local Interface — CLI [LI] — 3 defects

| ID | Severity | Description | Suggested fix |
|----|----------|-------------|---------------|
| SRS-013 | F | `[REQ-LI-030]` "The system shall have a provision menu" — "have" asserts existence without defining an observable action or behaviour. Harder to test than a behavioural verb, and inconsistent with all other requirements that use action verbs ("provide," "display," "receive"). | Replace "have" with "provide": "The system shall provide a provisioning menu accessible via the CLI." |
| SRS-014 | D | `[REQ-LI-010]` "receive a diagnostic command [command list TBD]" — the command enumeration is still TBD. The requirement is partially testable but the scope is open. | Resolve the command list before LLD; document as a D-level item pending CLI design decision. |
| SRS-015 | C | `[REQ-LI-040]` "The system shall receive the WIFI credentials" — "WIFI" is inconsistent with "WiFi" used everywhere else in the document. | Replace "WIFI" with "WiFi." |

---

## §2.5: Modbus Communication [MB] — 4 defects

| ID | Severity | Description | Suggested fix |
|----|----------|-------------|---------------|
| SRS-016 | B | `[REQ-MB-060]` "retry a failed Modbus request up to 3 times" (1 original + 3 retries = 4 total transmissions) contradicts `[REQ-NF-103]` "declare Modbus communication failure after 3 consecutive unanswered requests" (failure after 3 total). The retry count is irreconcilable as written. | Decide the intended total transmission count (3 or 4), express the same number in both requirements, and have one cross-reference the other. |
| SRS-017 | B | `[REQ-MB-111]` "Each device profile shall specify: A device identifier…" — subject is "Each device profile," not "The system." Violates §1.4 mandatory notation. | Rewrite as: "The system shall require each device profile to specify: a device identifier, a device description, a Modbus slave address, and a register-map specification." |
| SRS-018 | F | `[REQ-MB-050]` (200 ms timeout) carries a one-way comment `<!-- See REQ-NF-105 -->` but REQ-NF-105 does not reference MB-050. The §5 "no duplicated values" specialisation check requires bidirectional cross-reference when the same value appears in two requirements. | Add `<!-- See REQ-MB-050 -->` to REQ-NF-105, or consolidate to one requirement and have the other reference it. |
| SRS-019 | F | `[REQ-MB-070]` "The system shall define a register map…" — "define" is a design-time obligation, not a runtime system behaviour. No test can verify the system "defines" a register map at runtime. | Replace "define" with "implement" or "maintain"; reference the register-map artefact as the normative source. |

---

## §2.6: Cloud Communication [CC] — 1 defect

| ID | Severity | Description | Suggested fix |
|----|----------|-------------|---------------|
| SRS-020 | B | `[REQ-CC-030]` "adjustable locally via CLI" and `[REQ-CC-040]` "adjustable locally via CLI" contradict vision §10 which classifies the telemetry publishing interval and health data publishing interval as **Tier 2 — Local LCD + remote**. CLI is Tier 1 (local serial only). Placing these intervals under CLI implies Tier 1, which is the wrong access tier per the cross-reference document. | Replace "locally via CLI" with "locally via the LCD configuration screen" in both requirements, consistent with vision §10 Tier 2 access. |

---

## §2.7: Time Synchronisation [TS] — 3 defects

| ID | Severity | Description | Suggested fix |
|----|----------|-------------|---------------|
| SRS-021 | F | UC-13 Exception E2 "if the field node is disconnected, set an event to retry [time write] as soon as the field node is connected" is not covered by any TS requirement. `[REQ-TS-0E1]` only addresses internet loss, not Modbus loss during the field-node time-write step. | Add a requirement: "The system shall retry writing the synchronised time to the field device via Modbus as soon as Modbus connectivity to the field device is restored." |
| SRS-022 | D | `[REQ-TS-000]` "periodically every [TBD] thereafter" — the NTP re-synchronisation period is TBD. The same TBD appears in REQ-NF-210/211, both of which carry the note "determined by RTC drift measurement during integration testing." The deferral is legitimate but must be resolved no later than integration testing entry. | Retain as D; add the same rationale note to REQ-TS-000 for traceability consistency. |
| SRS-023 | D | `[REQ-NF-210]` "synchronise the RTC every [TBD]" and `[REQ-NF-211]` "synchronise the field node time every [TBD]" — both explicitly deferred to integration testing. Consistent with ASM-002 and the note is acceptable, but the open value makes both requirements untestable today. | Track as D; update both when integration test data is available; cross-reference REQ-TS-000. |

---

## §2.8: Device Management [DM] — 5 defects

| ID | Severity | Description | Suggested fix |
|----|----------|-------------|---------------|
| SRS-024 | F | `[REQ-DM-050]` "The system shall download the firmware image" — no source is specified. From where? Via what transport? The requirement is testable only if the source endpoint is known. | Qualify: "…shall download the firmware image from the authenticated cloud source via the existing MQTT/TLS channel." |
| SRS-025 | F | `[REQ-DM-052]` "delete the amount of downloaded firmware if after three retries, the firmware download still fails" — "delete the amount of downloaded firmware" is grammatically broken and does not state a clear action. The "three retries" value has no cited source. | Rewrite: "The system shall discard the partially downloaded firmware image if the download fails after [N] consecutive retries"; cite the source for N. |
| SRS-026 | C | `[REQ-DM-052]` appears before `[REQ-DM-051]` in the document, reversing the logical sequence (resume on reconnect → delete on exhausted retries). | Swap to place REQ-DM-051 before REQ-DM-052. |
| SRS-027 | F | `[REQ-DM-060]` "validate the firmware image" and `[REQ-DM-080]` "verify the cryptographic signature…before applying the update" — it is ambiguous whether DM-060 subsumes DM-080 (cryptographic verification is part of validation) or whether they are distinct steps (format/size check vs cryptographic check). Traceability matrix assigns different UCs (UC-18 vs UC-20), suggesting intentional separation, but the text does not make this explicit. | Add a parenthetical to DM-060: "…(size, format, and header integrity, excluding cryptographic verification — see REQ-DM-080)." |
| SRS-028 | F | `[REQ-DM-070]` "The system shall set the firmware partition as boot partition" — "the firmware partition" is ambiguous in the context of the dual-bank scheme mandated by REQ-DM-074. Which bank? | Qualify: "…set the newly written firmware partition (inactive bank) as the active boot partition." |

---

## §2.9: Data Buffering [BF] — 1 defect

| ID | Severity | Description | Suggested fix |
|----|----------|-------------|---------------|
| SRS-029 | D | `[REQ-BF-020]` "maximum capacity of [TBD] entries" — the buffer size is TBD. Vision §5.9 explicitly states "Buffer sizing is determined in the HLD," which grants legitimate deferral. | Track as D; the HLD must supply the concrete value before LLD proceeds. |

---

## §3.1: Performance — 5 defects

| ID | Severity | Description | Suggested fix |
|----|----------|-------------|---------------|
| SRS-030 | B | `[REQ-NF-112]` "publish the health data to AWS IoT Core each 10 minutes" contradicts vision §5.8 "once every 5 minutes." The two documents disagree on the health telemetry cadence. | Align SRS with vision (5 minutes) or issue a vision amendment; record which is authoritative. |
| SRS-031 | B | `[REQ-NF-114]` "use a polling rate within this range [TBD] Hz and [TBD] Hz" — both the lower and upper bounds are TBD. The requirement is entirely untestable. No rationale for deferral. | Determine and commit the bounds; cite the hardware or application source. |
| SRS-032 | B | `[REQ-NF-213]` "reach normal operational state within [TBD] seconds of power-on" — boot-time budget is TBD with no deferral rationale. This is determinable at design time from FreeRTOS startup analysis. | Derive the budget from the system initialisation sequence analysis; state an explicit value. |
| SRS-033 | F | `[REQ-NF-109]` "use a system watchdog of 10 seconds" — 10 s is a magic number with no cited source (e.g., worst-case task execution time analysis, longest legitimate blocking operation). | Add a source comment: cite the analysis or the rationale for choosing 10 s. |
| SRS-034 | F | `[REQ-NF-111]` "publish the sensor measurements to AWS IoT Core each 60 seconds" is stated as absolute, but `[REQ-CC-030]` declares the interval configurable. NF-111 must be explicitly qualified as the default value or it contradicts CC-030. | Add "(default)" to NF-111 and cross-reference REQ-CC-030. |

---

## §3.2: Reliability — 3 defects

| ID | Severity | Description | Suggested fix |
|----|----------|-------------|---------------|
| SRS-035 | B | `[REQ-NF-212]` "use uptime-based timestamps and flag the data as 'unsynchronised' if the RTC is not synchronised" is substantively identical to `[REQ-TS-040]` "use uptime-based timestamps and flag data as 'unsynchronised' if NTP synchronisation has not been completed." The triggering condition is worded slightly differently (RTC vs NTP state) but the operational outcome is the same. The "no duplicated values" specialisation check applies; this is a full duplicate of a functional requirement in a non-functional section. | Retain one requirement (preferred: REQ-TS-040 in the TS functional section). Replace REQ-NF-212 with a cross-reference: "See REQ-TS-040." |
| SRS-036 | F | `[REQ-NF-202]` "5 seconds after a watchdog reset" and `[REQ-NF-203]` "5 seconds after a normal reset" use the same value for different conditions with no cross-reference between them. The "no duplicated values" specialisation check requires one to reference the other, or a single shared value to be defined once. | Either consolidate to a single requirement covering all reset types, or add a cross-reference note in each. |
| SRS-037 | F | `[REQ-NF-209]` "attempt to reconnect at a frequency of 1Hz if a disconnection is detected" — 1 Hz is a magic number with no cited source; it is also ambiguous as to scope (WiFi layer? MQTT layer? Both?). | State whether this applies to WiFi reconnection, MQTT reconnection, or both, and cite the source for 1 Hz. |

---

## §3.3: Security — 2 defects

| ID | Severity | Description | Suggested fix |
|----|----------|-------------|---------------|
| SRS-038 | B | `[REQ-NF-307]` "require confirmation before executing Tier 3 commands" — vision §5.7 distinguishes: **restart** "requires a confirmation mechanism" but **immediate sensor reading** requires only "acknowledgement" (post-execution response), not pre-execution confirmation. NF-307 blanket-applies confirmation to all Tier 3, which is a cross-document conflict. | Restrict NF-307 to Tier 3 commands flagged as potentially disruptive (restart). Add a separate requirement for immediate-read acknowledgement that uses "post-execution response." |
| SRS-039 | F | `[REQ-NF-302]` "not store provisioning credentials in plaintext" — "provisioning credentials" is undefined. It could mean WiFi passphrase only, or all Tier 1 parameters (including certificates and Modbus address). Without a definition, a tester cannot determine the scope. | Define "provisioning credentials" in §1.3 or replace with an explicit enumeration: "WiFi passphrase and X.509 private key." |

---

## §3.4: Memory and Resource Constraints — 2 defects

| ID | Severity | Description | Suggested fix |
|----|----------|-------------|---------------|
| SRS-040 | B | `[REQ-NF-406]` "no more than 3 MB of non-volatile storage for diagnostic logs **per node**" and `[REQ-NF-407]` "no more than 4 MB of non-volatile storage for sensor measurements **per node**" — vision §5.9 states "Store-and-forward buffer (gateway only)." The field device has no store-and-forward buffer and does not need a measurement allocation. Applying "per node" to both boards contradicts the vision and would incorrectly charge 4 MB of measurement storage against the field device's 16 MB QSPI. | Remove "per node" and state separate limits per board; assign the measurement buffer to the gateway only. |
| SRS-041 | D | Gateway QSPI (8 MB): REQ-NF-406 (3 MB logs) + REQ-NF-407 (4 MB measurements) = 7 MB, leaving 1 MB for configuration, X.509 certificates, and any OTA staging partition. Whether this fits is not verified in the SRS. Not a blocker at this stage but must be resolved before LLD. | Add a capacity allocation summary (flash partition layout) to the HLD; verify the sum does not exceed 8 MB. |

---

## §3.5: Maintainability — 1 defect

| ID | Severity | Description | Suggested fix |
|----|----------|-------------|---------------|
| SRS-042 | B | `[REQ-NF-502]` "**All source code** shall conform to the project's BARR-C:2018 subset coding standard" — the subject is "All source code," not "The system." Violates the §1.4 mandatory notation "The system shall [base verb]." | Rewrite: "The system shall be implemented in conformance with the project's BARR-C:2018 subset coding standard." |

---

## §4: Constraints — 1 defect

| ID | Severity | Description | Suggested fix |
|----|----------|-------------|---------------|
| SRS-043 | B | `[CON-004]` "single data frame to 256 bytes (253 bytes of application data plus 3 bytes of overhead)" — factually incorrect. Modbus RTU frame structure: 1 byte slave address + 1 byte function code + up to **252 bytes** data + 2 bytes CRC = 256 bytes. Overhead is **4 bytes**, leaving **252 bytes** of application data. The stated 253+3 split is wrong on both counts. | Correct to: "253 bytes total data field per Modbus RTU frame (1 address + 1 function + 252 data bytes + 2 CRC = 256 bytes maximum frame)." — or better, cite the Modbus Application Protocol Specification v1.1b3, §4.1. |

---

## §5: Assumptions — 1 defect

| ID | Severity | Description | Suggested fix |
|----|----------|-------------|---------------|
| SRS-044 | C | `[ASM-002]` "REQ-TS-000 through REQ-TS-030 cannot be satisfied" — the TS section extends through REQ-TS-040 and REQ-TS-0E1, both of which would also be unsatisfiable without NTP. The referenced range is incomplete. | Extend to "REQ-TS-000 through REQ-TS-040 and REQ-TS-0E1." |

---

## §6: Traceability Matrix — 2 defects

| ID | Severity | Description | Suggested fix |
|----|----------|-------------|---------------|
| SRS-045 | B | `[REQ-LD-200]`, `[REQ-LD-210]`, `[REQ-LD-220]`, `[REQ-LD-230]`, `[REQ-LD-240]` — all five splash-screen requirements are absent from the §6 traceability matrix. No use-case or vision-section trace exists for any of them. Every requirement must trace to at least one UC or vision section per the §1.1 stated policy ("All requirements are testable and traceable"). | Add all five rows to the matrix; trace to vision §5.5 (LCD multi-screen interface) and UC-01 or equivalent. |
| SRS-046 | F | No functional section (§2.1 through §2.9) or non-functional section (§3.1 through §3.5) states a requirement count. The specialisation check "each functional section's stated count matches the actual number of 'shall' statements in that section" cannot be applied because no counts are stated. The matrix has no total row. | Add a count annotation to each section heading (e.g., "### 2.1 Sensor Acquisition [SA] — 21 requirements") and add a TOTAL row to §6. |

---

## Summary

**Top 3 blockers:**

1. **SRS-016 (MB-060 vs NF-103)** — The retry-count semantics are contradictory: "retry 3 times" gives 4 total transmissions before failure is declared; "3 consecutive unanswered requests" gives 3. These drive the Modbus error-detection behaviour and will produce conflicting implementations in the gateway task and the metrics subsystem.

2. **SRS-020 (CC-030/040 access tier)** — Publishing intervals are specified as adjustable "via CLI" (Tier 1 — local serial) but vision §10 places them in Tier 2 (LCD + remote). The conflict determines which UI component owns these parameters, which cascades into the configuration architecture for both boards.

3. **SRS-030 (NF-112 vs vision §5.8)** — The health-data cadence is stated as 10 minutes in the SRS and 5 minutes in the vision. Both documents are baselined; one must be amended before the HLD can specify the health-publisher task timing.

**Overall posture:** The SRS is structurally sound — requirements are well-grouped by component responsibility, the use of functional-area codes is consistent, and the constraint/assumption sections are unusually thorough. However, it carries **17 blockers** that collectively prevent it from being called correct and complete at this gate. Five of these are cross-document contradictions with the baselined vision; six are notation violations that obscure testability; the Modbus retry conflict and the health-data cadence discrepancy would produce directly divergent LLD artefacts. The unresolved [TBD] values in SA, NF-Performance, and NF-Reliability mean a tester cannot write verification criteria for those items today.

**Recommendation: Do not advance to Phase 3 (HLD bump).** Resolve all 17 B-level defects, then re-submit for a targeted re-review before LLD starts.