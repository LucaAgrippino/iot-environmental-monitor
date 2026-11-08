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
8. **Accounts are made on the machine, not over HTTP.** There is no sign-up and no password reset, so there is
   nothing to attack there; `iotcloud-admin` on the Pi creates users. Two roles, decided by the HTTP method: a
   viewer reads, an admin also changes things.
9. **Own sessions, not a login library.** A random token in an HttpOnly, SameSite=Strict cookie, stored hashed;
   passwords with scrypt from the standard library. About 100 lines, no extra dependency. Cognito's counterpart is
   a user pool; this is the same idea at the size of one house.
10. **Caddy for HTTPS, with its own certificate authority.** A public CA cannot issue for a name that exists only
    on the home network. The price is importing one root certificate in each browser, once.
11. **The deployment does not touch the broker.** The Pi's Mosquitto belongs to the bench; the cloud connects to it
    as a client. The system's own CA, access list and broker installer are in `broker/`, installed on their own.
12. **Identity from the certificate, not from the client id.** The access list uses the certificate's common name
    (`%u`): a client chooses its MQTT client id itself, so keying permissions on it lets a device claim another's.
13. **The backend keeps a persistent session.** An alarm sent while the backend restarts waits in the broker and is
    delivered when it returns (checked against a real broker in `broker/tests`).
