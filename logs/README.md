# Local test logs

Run tests through `tests/run_logged.sh LABEL COMMAND [ARGS...]`. Each run gets
its own UTC-stamped `.log` file here, including its command, tested Git commit,
output, and exit status. The root `out.txt` symlink permanently points to
`logs/current.log`; each run appends to that regular file while also retaining
its own history file. `tail -F out.txt` follows the active test. Log files and the symlink are
local evidence and are ignored by Git; summarize durable results in
`tests/hardware/INTEGRATION_VALIDATION.md`.
