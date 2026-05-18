# LLD Per-Companion Methodology

**Version:** 1.1
**Date:** May 2026
**Status:** Baselined

---

## 1. Introduction

### 1.1 Purpose

This document defines the repeatable process used to derive every LLD companion from the HLD baseline. It applies uniformly to drivers, middleware, and application components.

The methodology has two goals. First, every companion is produced the same way, so reviewers know what to expect and what to check. Second, every line in every companion traces back to an HLD or SRS source, so the LLD inherits the discipline of the HLD gate review rather than starting over.

This methodology is not a template. The template — the standard sections every companion document contains — is in §4. The methodology is the *process* that fills the template.

### 1.2 Scope

The methodology applies to any HLD component that requires implementation-ready specification before code is written. A component is "ready for code" when its companion has passed gate review per §5.

The methodology does not apply to:

- Configuration data (Modbus register map, flash partition layout) — these are HLD artefacts already.
- Linker scripts, startup code, and toolchain configuration — captured separately during the implementation phase.
- Build-system files — `CMakeLists.txt`, makefiles, project metadata.

### 1.3 Relationship to the LLD master

`lld.md` §3 fixes the cross-cutting conventions that every companion inherits — coding standard, error-code convention, OOP-in-C idioms, threading annotations, memory rules, file layout. This methodology assumes those are settled and does not restate them. Where a methodology step refers to "the conventions", it means `lld.md` §3.

---

## 2. Inputs

Before any companion derivation begins, the following sources are available and must be consulted:

| Source | Role |
|---|---|
| `components.md` | The component's row: responsibilities, PROVIDES interfaces, USES dependencies, traceability to SRS/UC. **Primary input.** |
| `hld.md` §5 / §6 | Prose context for the component within its layer and node. |
| `hld.md` §9 | Hardware abstraction stance — drivers only. |
| `sequence-diagrams.md` | Every diagram in which the component appears as a lifeline. |
| `state-machines.md` | Any state machine the component owns or participates in. |
| `task-breakdown.md` | The FreeRTOS task that hosts the component; IPC primitives. |
| `modbus-register-map.md` / `flash-partition-layout.md` | Where the component touches the register map or a flash partition. |
| `architecture-principles.md` | The principles cited in the rationale section. |
| `SRS.md` | The requirements traced from the component. |
| Board user manual (`um1932…pdf` or `um2153…pdf`) | Drivers only — peripheral wiring, pin assignments, on-board components. |
| Reference manual (RM0386 / RM0351) | Drivers only — register-level definitions. Fetched as needed; not in project knowledge. |

If a source contradicts another, the conflict is escalated and resolved before drafting continues. The companion is not the place to fix HLD defects silently.

---

## 3. The eight steps

The methodology proceeds in eight steps. Each step has a defined input, a defined output, and a validation rule. The steps are sequential — later steps depend on earlier outputs — but iteration between adjacent steps is permitted (e.g., Step 5 may surface an API gap that sends drafting back to Step 2).

### Step 1 — Source extraction

**Input:** The full set of sources from §2.

**Action:** Locate the component in `components.md` and capture:

- Layer (Application / Middleware / Driver).
- Responsibilities, verbatim.
- PROVIDES interfaces, each with its full name and consumers.
- USES dependencies, downward only.
- Traceability: every SRS requirement and use case the component traces to.

Then for each PROVIDES interface, list every consumer mentioned anywhere in the HLD. For each USES dependency, confirm the providing component's existence in `components.md`. For drivers, identify the relevant hardware peripheral(s) and the reference manual section(s) covering them.

Locate every sequence diagram in which the component appears as a lifeline. Locate every state machine the component owns or participates in. Locate the hosting task and the IPC primitives used.

**Output:** Section §1 of the companion ("Sources") — a structured summary of all the above, with explicit references.

**Validation:** Every later section must cite at least one Source entry. The Sources section is the contract: nothing in the companion exists without a citation. P6 (responsibility traces to requirements) is the gate here — if a Source citation cannot be produced, the companion has invented work.

### Step 2 — Public API

**Input:** The PROVIDES interfaces and responsibilities from Step 1.

**Action:** Specify the C header for the component. Decide first whether each interface is a *vtable* (multiple implementations expected) or a *direct API* (single implementation). The vtable case applies when `components.md` shows the same interface implemented by more than one component, or when the Vision document mandates implementation variants (e.g., sensor drivers on the Field Device are simulated; on the Gateway they are real). Otherwise the interface is a direct C API on the opaque handle.

For each function in the API:

- Choose a name. Module prefix, snake_case, verb-first.
- Choose a return type. Always `<module>_status_t` if the function can fail; `void` only if failure is impossible by construction.
- Choose parameters. Input first, output last, opaque handle as the first parameter for instance methods.
- Annotate the threading context (one of the four categories in `lld.md` §3.4).
- Write the Doxygen block.

The init/deinit lifecycle (per `lld.md` §3.3) is mandatory. The storage strategy — static singleton vs caller-buffer — is documented here and applied consistently.

**Output:** Section §2 of the companion ("Public API") — the full header content, including the opaque type declaration, the status enum, every public function with its Doxygen block, and any vtable struct definition.

**Validation:** Every PROVIDES interface from Step 1 maps to one or more functions in §2. Every function has a threading annotation. Every Doxygen block has `@brief`, `@param` for each parameter, and `@return`. P3 (ISP) applies — if the API mixes read and write concerns across distinct consumers, split it before continuing. P10 (naming) applies — interface names start with `I`, components are PascalCase, files are kebab-case.

**Dependency conformance.** The header's `#include` list and the types named in the public API must stay within the dependencies declared in `components.md` `USES (downward)` for this component. If the natural API requires a type from outside that list (e.g., a `TaskHandle_t` when `USES` is only `CMSIS`), either (a) replace it with a primitive abstraction the consumer wires up — a function-pointer callback is the usual answer — or (b) escalate the `USES` line in `components.md` as an HLD change before continuing. **Never silently widen the dependency footprint at the LLD layer.**

### Step 3 — Internal design

**Input:** The public API from Step 2 and the USES dependencies from Step 1.

**Action:** Specify the private struct that backs the opaque type. List every field with its type and its purpose. If the module holds any of:

- An internal state machine — define the state enum, the event enum, and the transition rule (table or function description).
- Synchronisation primitives — name each mutex/semaphore/queue handle, declare which functions take it, and state the locking order if multiple are held simultaneously.
- Buffers — name each buffer, give its size, and state whether storage is static within the module or caller-provided.
- Function-static state used at init time — flag it; it is a singleton constraint.

For each public function in Step 2, describe its internal flow in prose — enough that another engineer could implement it without consulting another document, but without writing the code itself.

Add a "Principles applied" subsection citing every P1–P10 principle that influenced the design, with one sentence each. This is the section that defends the design in interview review and at gate.

**Output:** Section §3 of the companion ("Internal design") — private struct, synchronisation strategy, per-function internal flow, principles cited.

**Validation:** Every function from Step 2 has a corresponding internal flow description. The principles section names at least P1 (every component obeys directional layering) and any others that materially shaped the design. If no principle applies beyond P1, the component is probably too thin and may not warrant a companion.

### Step 4 — Hardware contract (drivers only)

**Input:** The peripheral identification from Step 1, the board user manual, the reference manual.

**Action:** Specify, with citations, exactly which hardware the driver touches:

- Peripheral instance(s) used — by name (e.g., `GPIOA`, `USART1`).
- Register set — every register the driver reads or writes, with its reference manual section number.
- Clock tree dependency — which RCC enable bit (e.g., `RCC->AHB2ENR.GPIOAEN`).
- Pin assignments — board user manual table cited (e.g., UM2153 Table 6).
- Alternate-function configuration — for pin-using peripherals.
- NVIC — vector name, priority assignment if interrupts are used.
- DMA — stream/channel/request if DMA is used.

CMSIS macros only. The driver source file `#include`s the device header (`stm32f469xx.h` or `stm32l475xx.h`) and accesses peripherals via the structures defined there. No `stm32xxx_hal_*.h` headers appear in driver source.

**Output:** Section §4 of the companion ("Hardware contract") — a peripheral-by-peripheral specification with citations.

**Validation:** Every register touched in §3's internal flows is named in §4, with its reference manual section. Every pin used is cited to the board user manual. P9 (multi-view) does not apply here, but consistency does: if the driver targets both boards, the table makes the board-specific differences explicit.

This step is skipped for middleware and application companions, which contain no register access by construction.

### Step 5 — Sequence integration

**Input:** The sequence diagrams from Step 1, the public API from Step 2.

**Action:** For each HLD sequence diagram in which the component appears, build a message-to-function map. The format is a table with three columns: sequence diagram ID, message label (verbatim from the diagram), and target function (from §2).

If a message has no target function, one of three things has happened:

1. The function was missed in Step 2 — return to Step 2 and add it.
2. The sequence diagram is wrong — escalate as an HLD defect; do not silently invent.
3. The mapping is indirect (the message represents an asynchronous event delivered through a queue, not a direct call) — record the IPC primitive instead of a function name, but record something.

**Output:** Section §5 of the companion ("Sequence integration") — one table per sequence diagram in which the component appears.

**Validation:** Every message arrow targeting the component's lifeline appears in some row of some table in §5. No exceptions, no "implicit" messages. This step is where the LLD verifies that the API as designed is sufficient for the runtime behaviour the HLD specified.

### Step 6 — Error and fault behaviour

**Input:** The public API from Step 2, the internal design from Step 3.

**Action:** Specify the failure model:

- The status enum — full definition with one sentence per value.
- For each public function, the failure modes that can produce a non-OK return — what causes them, and which status value surfaces them.
- Retry policy — if the module retries internally (e.g., Modbus master), state the parameters and where they come from.
- Behaviour on downstream failure — if a USES dependency fails, does the module retry, escalate, quiesce, or pass through? State the choice and the rationale.
- Observability — which stats are exposed via the `IXxxStats` interface (per the Metric Producer Pattern in `hld.md` §8.4), what is logged, and at which log level.

**Output:** Section §6 of the companion ("Error and fault behaviour") — status enum, per-function failure modes, retry policy if any, observability hooks.

**Validation:** Every non-OK status value is produced by at least one documented failure mode. Every public function that can fail (per its threading annotation and Doxygen) has at least one failure mode listed.

### Step 7 — Unit-test plan

**Input:** The public API from Step 2, the failure modes from Step 6.

**Action:** Specify the Unity test file:

- Test file name and location (mirrors the firmware path under `tests/`).
- Mocks required — which downstream symbols need stubbing, with the strategy (link-time substitution, function pointer injection, or compile-time `#ifdef`).
- Test cases — at minimum, for each public API function, one happy-path case and one error-path case. List each test case with its name (`test_<module>_<function>_<scenario>`) and the assertion it verifies.
- Host-side build strategy — how the module compiles on the host (typically with CMSIS register definitions stubbed out).
- Coverage target — what is realistic; what is not (e.g., interrupt-handler entry points cannot be reached from a host test).

**Output:** Section §7 of the companion ("Unit-test plan") — file name, mock strategy, test-case list, coverage target.

**Validation:** Every public API function appears in at least two test cases (happy plus error). Every error mode from §6 is exercised by at least one test case.

### Step 8 — Open items

**Input:** Anything surfaced during Steps 1–7 that cannot be resolved within this companion.

**Action:** Record each item with a clear resolution path. Categories:

- **Integration-time measurement** — values that can only be known on hardware (WCET, stack high water mark, timing margin). Tracked in the companion and reflected in `lld.md` §5 if it resolves an inherited TBD.
- **Cross-component coupling** — anything that depends on a later companion's decision (e.g., the GPIO companion may flag interrupt priority levels that the NVIC-using drivers will need to honour).
- **HLD ambiguity** — a defect found in the HLD that did not warrant blocking the companion but should be tracked. Escalated separately.

If the companion resolves any of the three inherited TBDs from `lld.md` §5 (O1/O2/O3), update `lld.md` §5 in the same commit that lands the companion.

**Output:** Section §8 of the companion ("Open items") — table of open items with resolution paths.

**Validation:** Open items have a named owner and a target resolution milestone. Items without either are either resolved before close-out or escalated.

---

## 4. Standard companion structure

Every companion document has the following top-level structure. Section numbers are stable; subsection structure within each is at the author's discretion.

```
# <Component name> — LLD Companion

**Version:** <semver>
**Date:** <month year>
**Status:** Draft | Reviewed | Baselined
**HLD anchor:** <component name in components.md, layer, board(s)>

## 1. Sources
## 2. Public API
## 3. Internal design
## 4. Hardware contract            (drivers only — omit for middleware/application)
## 5. Sequence integration
## 6. Error and fault behaviour
## 7. Unit-test plan
## 8. Open items
```

The file is written in British English, mirrors the HLD prose-over-bullets convention, and uses fenced code blocks for any C declarations.

---

## 5. Gate criteria

A companion is ready for baseline when every item in this checklist holds. Luca runs the checklist before declaring `v1.0`; Claude runs it during review.

**Sources (§1)**
- Component located in `components.md`; responsibilities, PROVIDES, USES, traceability captured verbatim.
- Every sequence diagram and state machine in which the component appears is enumerated.

**Public API (§2)**
- Every PROVIDES interface from §1 is realised in the API.
- Every function has a threading annotation from `lld.md` §3.4.
- Every function has a Doxygen block with `@brief`, `@param`, `@return`.
- Naming follows `lld.md` §3.1 and P10.
- The vtable-vs-direct-API choice is justified per interface.
- The header's `#include` list and API types stay within `components.md` `USES (downward)`. Any wider dependency was either replaced by an abstraction (e.g., callback) or escalated as an HLD change first.

**Internal design (§3)**
- The private struct is defined.
- Synchronisation primitives are named and the locking order is stated where relevant.
- Every public function has an internal flow description.
- The "Principles applied" subsection names every P1–P10 principle that influenced the design.

**Hardware contract (§4 — drivers only)**
- Every register read or written in §3 is named and cited to the reference manual.
- Every pin used is cited to the board user manual.
- Clock tree and NVIC dependencies are explicit.
- No HAL headers appear.

**Sequence integration (§5)**
- Every message arrow targeting the component's lifeline in any HLD sequence diagram is in a table row.
- No "implicit" or "obvious" messages — every one is mapped.

**Error and fault behaviour (§6)**
- The status enum is fully defined.
- Every non-OK value has at least one producing failure mode documented.
- Retry policy, downstream failure policy, and observability hooks are stated.

**Unit-test plan (§7)**
- Every public API function has ≥ 1 happy-path and ≥ 1 error-path test case listed.
- Every failure mode in §6 is exercised by at least one test case.
- Mock strategy is specified.

**Open items (§8)**
- Each open item has a named resolution path and target milestone.
- Any resolved inherited TBDs are reflected in `lld.md` §5 in the same commit.

**Cross-cutting**
- British spelling.
- No content invented without a Sources citation.
- The companion does not restate HLD content; it refines.

---

## 6. Iteration and escalation

The eight steps are sequential but not rigid. Iteration between adjacent steps is normal — Step 5 commonly sends the author back to Step 2 to add a missed function; Step 6 commonly refines Step 3's principles section as failure modes clarify the design.

Escalation, by contrast, is rare and explicit. An HLD defect surfaced during companion derivation is escalated by:

1. Halting the companion at the affected step.
2. Filing the defect against the relevant HLD document with severity (Blocker / Fix-now / Defer / Cosmetic).
3. Resolving the defect on its own branch and PR, with the HLD bumped if material.
4. Resuming the companion against the corrected HLD.

The companion never silently absorbs the cost of an HLD defect. The HLD baseline is preserved; the LLD inherits a clean source.

---

*This document is the LLD methodology. It supersedes any ad-hoc derivation step taken during a companion draft. The methodology may itself be amended; amendments are reviewed and version-bumped like any other deliverable.*
