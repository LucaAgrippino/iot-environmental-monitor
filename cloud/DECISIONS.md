# Decisions

Short, informal. Newest at the bottom.

1. **Self-hosted, AWS as the reference.** Each AWS service has a named replacement (README). A bridge to a real
   AWS account is an optional last step, not a dependency.
2. **In this repository, in `cloud/`.** The message formats are a contract with the firmware; keeping both in one
   place makes a mismatch visible in one diff.
3. **Python (FastAPI) and SQLite first.** Python is what the project's tools already use; SQLite has no server to
   run on a Pi with 1 GB. PostgreSQL later, to compare.
4. **A device simulator before the boards.** The two boards are in use on the bench; the simulator speaks the same
   schema, so the cloud side is developed and tested without them. It never replaces a test with the real device.
5. **Live updates with server-sent events.** One direction (server to browser) is all the dashboard needs, and it
   works through any proxy without extra libraries.
6. **Time of a message.** The device's timestamp is used only when the device says it is synchronised; otherwise
   the time of reception is stored (the firmware flags unsynchronised time on purpose, REQ-TS-040).
7. **Chunk requests of an OTA are not stored.** There is one per image chunk; the OTA job record keeps the
   progress instead.
