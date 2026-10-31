# WP4 merge plan (Gateway main and the fixes found on the board)

**Written:** 2026-10-31. **State of `main`:** `7328305` (merge of PR #115). **Open PRs:** #116 to #130.
**Rule:** the author never merges; Luca merges. This file only says in which order and what to watch.

## What is open

| PR | Base | What it carries |
|---|---|---|
| #116 | main | Docs: Gateway `main()` analysis (draft). Independent of everything else |
| #117 to #123 | main | The prerequisites of `main()`. All seven are already **inside #124** (checked as ancestors) |
| **#124** | main | Gateway `main()`, BootTask, application image build script |
| #125 | #124's branch | Drop the MQTT session after a failed publish or subscribe send (GM-O21) |
| #126 | #124's branch | Pace the store-and-forward replay (GM-O20) |
| #127 | #124's branch | Wi-Fi driver: wait for DRDY with NSS high, 1 s command-ready bound (GM-O22) |
| #128 | #126's branch | Boot offline, `connect_prepare` hook, device id from the CPU id (GM-O17/18/19). Contains #126 |
| #129 | #128's branch | Reset the Wi-Fi module when it stops answering. Contains #126, #127, #128 |
| #130 | #124's branch | `WifiTask` error codes in their own range (WIFITASK-D8) |

## The order

1. **#124.** Merging only #124 lands #117 to #123 with it; they should then show as merged by themselves
   (if one does not, close it as superseded). To review the pieces one by one instead, merge in this order:
   #119 (contains #117 and #118), #121, #120, #122, #123, then #124.
2. **Do not delete the `feature/phase-4-gw-main` branch** when merging #124. #125, #126, #127 and #130 are
   based on it, and GitHub closes a PR whose base branch is deleted (that is how PR #73 was closed).
3. **Retarget #125, #126, #127 and #130 to `main`.** CI does not start on a retarget alone: push a merge of
   `main` into each branch (no force-push).
4. **Merge #125, #127, #126**, in that order. They do not conflict with each other.
5. **Retarget #128 to `main`** and merge it once #126 is in. Do the same for #129 after #128. Each retarget
   needs the same CI-starting push.
6. **Merge #130 last.** It is the only PR that conflicts (see below).
7. **#116 at the end**, after its document is brought up to date: GM-O17 to O19 and O20 to O22 are done but
   still listed as open there.

## Rules

- **Merge commits, not squash.** #128 and #129 contain their predecessors' commits; a squash re-applies the same
  changes as new content and creates conflicts. The repository already uses merge commits ("Merge pull request #115").
- **Check before cleaning up.** Confirm `gh pr view <n> --json state` says `MERGED` before deleting a branch.
- **#125, #127 and #130 are independent of each other.** Only the position of #130 (last) matters.

## The one conflict (#130)

Merged after #129, #130 conflicts in two files, one spot each, because both PRs add to the same places:

- `docs/lld/middleware/wifi-task.md`: both add a row to the decision table (WIFITASK-D7 from #129, WIFITASK-D8 from #130). **Keep both rows.**
- `tests/gateway/middleware/wifi_task/test_wifi_task.c`: both append tests at the end of the file. **Keep both blocks.**
  Git attaches the closing brace of `test_WIFITASK_T30_...` to the wrong side: after resolving, check that T30 ends
  with `}` and that `test_WIFITASK_T31_...` starts after it.

Resolve it by merging `main` into #130's branch (no force-push), then run `ceedling test:test_wifi_task` (default
project): 31 tests expected.

## What was verified

Dry run in a scratch worktree, merging in the order above starting from #124's tip:

- #125, #127, #126, #128, #129: no conflicts. #130: exactly the two above.
- Suites on the merged result: default `wifi_task` 31 of 31, default `mqtt_client` 34 of 34,
  Gateway `wifi_driver` 38 of 38, Gateway `cloud_publisher` 107 of 107.
- Hardware: the board ran the stack of #125 to #129 together, and #130's firmware changes merged into that image.
  The exact merge order was not run on hardware.

**Not checked:** the full suites on the merged tree (the scratch worktree lacked the generated, git-ignored
`firmware_version.h`), and whether the merged tree links (the last build in the worktree printed nothing).
Run both full suites and `scripts/build-gateway-app.sh` once everything is in.

## Things only Luca can do

- **AWS IoT:** each board needs a Thing named by its CPU unique id (this board: `002d004f3442501020323848`) and a policy
  on `${iot:Connection.Thing.ThingName}`. The old `gw-bringup-001` Thing no longer matches the client id (#128),
  so a real AWS connection needs this first.
- **`.cproject`:** the CubeIDE project changes needed to build `Src/main.c` are listed in #124's description and are
  not made (the author never edits that file).
- **Board:** a stale 262-entry store-and-forward backlog (including 100 entries of 4000 bytes of `o`) is still in the
  board's flash.
- **History:** #129 contains a commit titled "wip: module reset recovery". Only squashing that one PR would hide it,
  which conflicts with the merge-commit rule above. Harmless if left.
