# Tests

[`non_hardware/`](non_hardware/) contains automated unit and component tests.
They do not need a Sidekiq card or link to the Sidekiq SDK. Its README has
the normal and sanitizer build and run commands.

[`hardware/`](hardware/) contains bounded validators and diagnostics that use
real cards. Some checks also need an RF path or external PPS source. Its README
lists the programs; the [integration test plan](hardware/INTEGRATION_TEST_PLAN.md)
covers the combined branch. Save run-specific results with the review or
release evidence.

Run test commands through [`run_logged.sh`](run_logged.sh) to save a dated log
in `logs/` and follow the current run through `out.txt`.
