# Local autonomous development loop

The Codex heartbeat is the scheduler. Each wake processes one backlog task; it must not start a second task while a branch, build, or test from the prior task is active.

1. Inspect `agent/state.json` when it exists, then read `agent/backlog.yaml`.
2. Promote a pending task to `ready` only when every dependency is `done`.
3. For the chosen task, follow its matching task in the implementation plan exactly.
4. Keep the change to one task. Run the focused test, then `agent/verify.sh`.
5. If verification passes, commit, fast-forward merge to `master`, push `master`, mark the task `done`, and start no further task in the same wake.
6. If verification fails, repair only the current task. After three failed attempts, mark it `blocked` with command output and continue with another independent task on the next wake.
7. A `local-runner-required` task cannot merge until each named evidence file is present at `agent/evidence/<name>.json` with `commit`, `command`, `passed: true`, and `recorded_at` fields.

The heartbeat is silent while no task becomes ready. It reports only a merge, a permanent block, a verification infrastructure failure, or a request for a missing local runner.
