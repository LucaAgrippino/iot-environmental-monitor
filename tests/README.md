# Tests

Unit and integration tests for the IoT Environmental Monitoring Gateway, built with
[Ceedling](https://github.com/ThrowTheSwitch/Ceedling) (Unity + CMock). Tests run on the host
(no target hardware required) using GCC and the C11 standard.

## Running tests locally

```sh
cd tests
ceedling test:all
```

Ceedling discovers every file matching `tests/**/test_*.c`, compiles it against the corresponding
source under `firmware/`, and reports results via the `stdout_pretty_tests_report` plugin.
Generated mocks land in `tests/mocks/` (gitignored). Build artefacts land in `tests/build/`
(gitignored).
