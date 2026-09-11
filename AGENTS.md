# Autonomous development policy

This repository permits an autonomous local agent to implement the tasks in `docs/superpowers/plans/2026-09-11-capybarrier-reliability.md`.

## Loop

1. Read `agent/backlog.yaml` and choose the first `ready` task whose dependencies are `done`.
2. Create `agent/<task-id>` from current `master`.
3. Write a failing focused test, implement the smallest change, then run that focused test.
4. Run `agent/verify.sh` before committing.
5. Commit with one task-scoped conventional message, fast-forward merge into local `master`, and push `master` to `origin`.
6. Update only `agent/state.json`; it is runtime state and intentionally ignored by Git.

## Authority and boundaries

- Push and merge are authorized after `agent/verify.sh` succeeds. Never force-push, rewrite history, delete remote branches, tags, releases, or issues.
- Work only on declared backlog tasks and their direct tests/docs. Do not add dependencies without a task requirement.
- Do not print, commit, or copy credentials. Use credential helpers or environment-provided authentication only.
- Do not merge a task marked `local-runner-required` until its required local runner evidence exists in `agent/evidence/` and its referenced test command passed.
- On three consecutive failed implementation attempts for one task, record the commands and failures in `agent/state.json`, mark that task `blocked`, then continue with an independent ready task. Stop only when no ready task remains.
- Never disable, weaken, skip, or rewrite tests to obtain a passing result.

## Required gate

`agent/verify.sh` must exit 0 and `git diff --check` must be clean immediately before every merge. Its results belong in the commit message body or `agent/state.json`.
