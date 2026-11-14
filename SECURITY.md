# Security policy

This is a personal project (an IoT environmental monitor), not a product with a support commitment, but a security problem is welcome news.

**Reporting.** Please use GitHub's private vulnerability reporting (the Security tab of this repository, "Report a vulnerability") and do not open a public
issue with the details. I will answer as time allows.

**What is in scope.** The firmware of both boards, the OTA update path, the broker configuration and PKI tool, and the cloud service in this repository.
The review of 2026-11-14 and the state of each finding are in [docs/security-review.md](docs/security-review.md).

**Known limits.** The device's private key and Wi-Fi passphrase are stored obfuscated, not in secure hardware, and the chip's read-out protection is not
enabled (finding S-1); the update path does not protect against installing an older signed image unless the fix for S-4 is in your version.
