# Audit — docs/hld/hld.md
Auditor: Claude (gate-review chat N)
Severity counts: 13B / 10F / 0D / 1C

## §1. Introduction [4 defects]
| ID | Severity | Description | Suggested fix |
|----|----------|-------------|---------------|
| hld-1 | B | §1.1 lists `domain-model.md` as a companion ("`domain-model.md` — entity catalogue and relationships."), but no such file exists in the project knowledge. §4 leans on it as the canonical source. Untestable cross-reference. | Either restore `domain-model.md` to the project, or move the entity catalogue into a dedicated §4 here and drop the companion pointer. |
| hld-2 | F | §1.1 companion list is incomplete: it cites only `vision.md`, `SRS.md`, `use-case-descriptions.md`, `domain-model.md`, `components.md`, `state-machines.md`. Missing: `task-breakdown.md`, `modbus-register-map.md`, `flash-partition-layout.md`, `sequence-diagrams.md`, `architecture-principles.md`, `diagram-colour-palette.md` — all referenced later. | Extend §1.1 to list every companion the body cites, in artefact order. |
| hld-3 | F | §1.2 "How to read" walks Sections 2–9 only ("Sections 8–9 explain the architectural patterns and hardware abstraction strategy that bind everything together."). Sections 10–14 (Sequence diagrams, Task design, Modbus register map, Flash partition layout, Decisions log) are not introduced anywhere in the reading guide. | Extend the §1.2 paragraph (or replace with a TOC) to cover §10–§14. |
| hld-4 | F | §1.3 artefact-status table row 5 is malformed: `\| 5 \| Data Flow \| Sequence Diagrams \| Complete \|` produces a 4-cell row in a 3-column table. Renders inconsistently. | Collapse to `\| 5 \| Sequence Diagrams \| Complete \|` (or `Data-Flow Sequence Diagrams`). |

## §(top of document — global) [2 defects]
| ID | Severity | Description | Suggested fix |
|----|----------|-------------|---------------|
| hld-5 | F | No table of contents anywhere. The audit's specialisation check explicitly tests "Section numbering matches the table of contents." There is nothing to match against, and the reader of a 14-section, 1000-line HLD has no map. | Insert a 14-row TOC under the front matter before §1. |
| hld-6 | F | No version history / change log section. Front matter shows only "**Version:** 0.6" with no record of the v0.1 → v0.6 trajectory or what each bump introduced; specialisation check explicitly requires this. | Add a §0 (or end-of-doc) "Revision history" table with one-line "what changed" per bump and the upcoming v1.0 entry. |

## §2. System context [1 defect]
| ID | Severity | Description | Suggested fix |
|----|----------|-------------|---------------|
| hld-7 | B | Actor list contradicts `vision.md` §4 and `use-case-descriptions.md`. HLD names "**Local Operator** … **Remote Operator** … **Field Technician** maintaining the device." Vision §4 names only **Field Technician** (the physically present person), **Remote Operator**, and **AWS IoT Core** (system actor). Every UC in `use-case-descriptions.md` uses `Field Technician` for the on-site role — there is no `Local Operator`. HLD also omits AWS IoT Core. | Restate as the three Vision actors (Field Technician, Remote Operator, AWS IoT Core), or — if a split is genuinely intended — push the change back through Vision §4 and the UC headers first. |

## §3. System architecture — physical topology: clean.

## §4. Domain model [1 defect]
| ID | Severity | Description | Suggested fix |
|----|----------|-------------|---------------|
| hld-8 | B | "The `domain-model.md` companion document explains each entity…" — file is not in project knowledge (see hld-1). The section's 13-entity claim and the polymorphic-by-composition / denormalisation rationale cannot be verified against any companion. | Same fix as hld-1. |

## §5. Component design — Field Device [2 defects]
| ID | Severity | Description | Suggested fix |
|----|----------|-------------|---------------|
| hld-9 | F | §5.4–§5.5 invoke "DIP" and "Metric Producer Pattern" as named patterns ("The DIP relationship…", "participate via the Metric Producer Pattern") before §8.3 and §8.4 define them. Reader without prerequisite knowledge has to scroll forward; violates the specialisation check "reads top-to-bottom without prerequisite knowledge". | Either forward-reference (`see §8.3 / §8.4`) at first use, or move the §8 pattern catalogue ahead of §5. |
| hld-10 | F | Component count "25 software components across four layers: Application, Middleware, Driver, and Hardware" — the four-layer model is asserted here for the first time and only formally defined in §8.1. Same prerequisite-knowledge issue. | Same fix as hld-9. |

## §6. Component design — Gateway [4 defects]
| ID | Severity | Description | Suggested fix |
|----|----------|-------------|---------------|
| hld-11 | B | §6 intro: "decomposes into **32 software components: eleven application, seven middleware, fourteen drivers**." `components.md` §3 (Final component list, Gateway) lists **12 application** (incl. `DeviceProfileRegistry` and `StoreAndForward`), **8 middleware** (incl. `FirmwareStore`), **14 drivers** = **34 total**. Direct cross-document contradiction. | Update §6 intro to 34 / 12 / 8 / 14. |
| hld-12 | B | §6.1 enumerates eleven application components ("CloudPublisher, StoreAndForward, ModbusPoller, TimeService, UpdateService, … HealthMonitor, SensorService, AlarmService, ConsoleService, ConfigService … plus `LifecycleController`"). **`DeviceProfileRegistry` is missing** — yet §10.4 ¶1 explicitly introduces it as "a `DeviceProfileRegistry` Application component on the gateway," and `components.md` carries the full responsibility sentence. Stale text from before the §10.4 feedback was applied. | Add `DeviceProfileRegistry` to the §6.1 application enumeration; reflect its existence in §6.7 too (see hld-14). |
| hld-13 | B | §6.1 middleware paragraph lists eight middleware components (MqttClient, NtpClient, CircularFlashLog, FirmwareStore, ModbusMaster, plus Logger, TimeProvider, ConfigStore from FD). This directly contradicts §6 intro's "seven middleware" — internal inconsistency within the same section. | Reconcile both numbers to 8 (matches `components.md`). |
| hld-14 | B | §6.7 Configuration and persistence view: "Persistence flows through ConfigStore to QspiFlashDriver" — does not mention `DeviceProfileRegistry`, although `components.md` says profile persistence is delegated to `ConfigStore` and `flash-partition-layout.md` §5.2 sizes ConfigStore for "Operational config + `DeviceProfileRegistry` profiles". A view that claims to answer *"how do parameter changes propagate to flash?"* must include profiles. | Add `DeviceProfileRegistry` (via `IDeviceProfileManager` / `IDeviceProfileProvider`) to the §6.7 narrative. |

## §7. Behavioural design — state machines [1 defect]
| ID | Severity | Description | Suggested fix |
|----|----------|-------------|---------------|
| hld-15 | F | §7.6: "**BringingUpLCD replaces SelfChecking**". Not a substitution. Gateway Init runs `… → StartingMiddleware → SelfChecking` (step 5); FD Init runs `… → BringingUpSensors → BringingUpLCD → StartingMiddleware` (BringingUpLCD inserted as step 4, StartingMiddleware demoted to step 5, no SelfChecking at all). `state-machines.md` §Machine-5 Init table confirms this. | Reword: "FD Init adds `BringingUpLCD` between `BringingUpSensors` and `StartingMiddleware` and omits `SelfChecking` (no UC-17, no remote-restart self-check)." |

## §8. Architectural patterns: clean.

## §9. Hardware abstraction strategy [1 defect]
| ID | Severity | Description | Suggested fix |
|----|----------|-------------|---------------|
| hld-16 | F | §9 opens "Vision §9 establishes the portability stance: no STM32 HAL above the driver layer; CMSIS-only inside drivers." Vision §9 *also* states: "A virtual hardware layer built on top of the driver layer shall expose vendor-neutral interfaces to the middleware and application layers." HLD §9 never reconciles whether this "virtual hardware layer" is a separate layer above Driver (the four-layer model in §8.1 would then be wrong) or merely the drivers' upward `IXxx` interfaces. Specialisation check: "HAL-vs-CMSIS mixed strategy stated once authoritatively". | Add one paragraph in §9 stating that the "vendor-neutral interfaces" of Vision §9 are realised as the drivers' upward `IXxx` ports, not as a separate fifth layer (or, if a distinct layer is intended, document it explicitly and adjust §8.1). |

## §10. Sequence diagrams [1 defect]
| ID | Severity | Description | Suggested fix |
|----|----------|-------------|---------------|
| hld-17 | B | §10.4 opens: "Drawing the sequence diagrams surfaced four substantive issues that fed back into the structural design. **Each is recorded in §12.1.**" §12.1 of this document is the Modbus register map's "Purpose and scope" — no decision records there. The decisions log is §14. Broken internal cross-reference. | Replace "§12.1" with "§14". |

## §11. Task design [1 defect]
| ID | Severity | Description | Suggested fix |
|----|----------|-------------|---------------|
| hld-18 | B | §11.4: "Designing the task layout surfaced three decisions worth highlighting. **The full set is recorded in §13.**" §13 is Flash Partition Layout. The full set is in §14. Same class of error as hld-17. | Replace "§13" with "§14". |

## §12. Modbus register map [1 defect]
| ID | Severity | Description | Suggested fix |
|----|----------|-------------|---------------|
| hld-19 | B | §12.5–§12.7 cite decisions **D29, D30, D31, D32, D33, D34** ("scaled integers… *(D29)*", "Big-endian byte order, big-endian word order *(D30)*", "magic value `0xA5A5`… *(D32)*", etc.). Grep of all companion documents shows zero anchor for D29–D34 — they appear nowhere except as in-body citations in HLD §12. Reader cannot verify, reviewer cannot trace. Compare §13 (D35–D41 anchored in `flash-partition-layout.md` §10), §11 (D21–D28 anchored in `task-breakdown.md` §11), §10 (D13–D20 anchored in `sequence-diagrams.md`). | Add a Decision log to `modbus-register-map.md` (or insert D29–D34 entries into HLD §14.1 with the same numbering convention). |

## §13. Flash partition layout: clean.

## §14. Architectural decisions log [3 defects]
| ID | Severity | Description | Suggested fix |
|----|----------|-------------|---------------|
| hld-20 | B | Duplicate row in §14.1 "Decisions adopted" table: lines 968 and 969 are character-identical — "*Per-slave probe with profile-bound device-ID validation; fall-through to Running on failure \| Industry-standard deny-by-default; a slave that fails identity validation is excluded from the polling allowlist without blocking gateway boot. Supports the 5 s boot budget (REQ-NF-203).*" Decision-log integrity issue; suggests a copy-paste artefact during the §14 consolidation pass. | Delete the duplicate row. |
| hld-21 | B | §14.1 rows carry no `D##` anchors at all (the rationale columns paraphrase the decision text). The HLD body, in contrast, cites concrete numeric IDs (D13, D14, D15, D16, D17, D18, D21, D22, D27, D28, D29, D30, D31, D32, D33, D34, D35, D37, D38, D39, D40, D41) which point to numbered entries in the four companion decision logs. From the HLD reader's standpoint these refs are orphaned — `D14` is a string token, not a discoverable anchor. | Prefix every §14.1 row with its `D##` label so body citations resolve locally; or replace the body-prose `(D##)` notation with `({companion}.md decision DXX)`. Pick one and apply uniformly. |
| hld-22 | B | Specialisation check: "Decisions D1–D41 are each cited at least once in the body prose." Grep of HLD body (everything before §14) shows D13, D14, D15, D16, D17, D18, D21, D22, D27, D28, D29, D30, D31, D32, D33, D34, D35, D37, D38, D39, D40, D41 — **22 of 41**. Missing from any body citation: **D1, D2, D3, D4, D5, D6, D7, D8, D9, D10, D11, D12, D19, D20, D23, D24, D25, D26, D36** (19 decisions). Several of these are non-trivial (e.g. D6 "SD-06 decomposed into four sub-diagrams per P9"; D26 "Priority inheritance enabled on all mutexes"; D36 "Custom secondary bootloader on Gateway"). | Either cite each missing decision once at its natural point in the body (e.g. D36 in §13.5 Bootloader contract, D26 in §11 task design, D6 in §10.3) or scope the specialisation check down to "decisions material to the HLD body". |

## Summary

**Top three blockers.** (i) §6 enumerates the Gateway as 32 components (11/7/14) while `components.md` carries 34 (12/8/14); the missing piece is `DeviceProfileRegistry`, which the HLD itself introduces in §10.4 — three sections (§6 intro, §6.1, §6.7) were not retrofitted. (ii) §10.4 and §11.4 both point readers to wrong sections for the decision log (§12.1 and §13 instead of §14) — concrete, easily verifiable broken refs, and §14 itself has a verbatim duplicate row plus 19 orphaned decision IDs from the companion docs. (iii) §2's actor list ("Local Operator / Remote Operator / Field Technician") contradicts both `vision.md` §4 and every UC header in `use-case-descriptions.md` (`Field Technician` is the on-site role; AWS IoT Core is the system actor); this is the document's opening structural statement and the most visible cross-doc inconsistency.

**Overall posture.** The artefact's technical substance is sound — patterns, layering, state machines, partition layout, register map all hold up against their companions. The defects cluster in two specific classes: **decision-log hygiene** (§14 not numbered, 19 decisions orphaned, one row duplicated, two cross-references broken, D29–D34 unanchored anywhere) and **stale enumerations** (the §10.4 architectural feedback that introduced `DeviceProfileRegistry` was never propagated back into §6, §6.1, §6.7). Front matter (no TOC, no version history, incomplete companion list, broken §1.3 row) is fixable in an afternoon.

**Recommendation.** **Do not bump to v1.0 yet.** The 13 blockers above are mechanical to clear — a numbering pass on §14, a refresh of §6 against `components.md`, a search-and-replace on the two broken §-refs, a §2 actor rewrite against Vision §4 — but every one of them either contradicts a companion or breaks a link, and that is exactly what the v1.0 gate exists to catch. Estimated rework: half a day for the blockers, another half for the F-items. Re-audit on the corrected draft.