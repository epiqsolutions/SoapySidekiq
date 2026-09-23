# Tests

Automated tests that do not require a Sidekiq card live in
[`non_hardware_tests`](../non_hardware_tests/).

Legacy programs that exercise real hardware have been collected in
[`hardware`](hardware/). They are currently manual validation and stress
programs rather than a uniform automated suite. Their existing behavior was
preserved during the directory reorganization so hardware-test cleanup can be
reviewed separately from the user-facing example cleanup.
