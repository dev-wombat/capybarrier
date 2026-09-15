# Transfer v2 Recovery Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the abandoned in-memory file-transfer path with one routed, disk-backed v2 transfer flow that can resume safely.

**Architecture:** A `TransferSession` is owned by the receiving endpoint; proxies only decode/encode v2 protocol messages and the server relays them to the currently active peer. The legacy `FileChunk` path remains untouched until the new path has an end-to-end integration test; it is then rejected for protocol-major 2 peers.

**Tech Stack:** C++11, CMake, GoogleTest, OpenSSL SHA-256, existing `ProtocolUtil` and event queue.

**Spec:** `docs/superpowers/specs/2026-09-11-capybarrier-reliability-design.md`

## Recovery rules

- Do not merge the untracked `agent/streaming-transfer` files as-is; they have local tests but no protocol route.
- Start a new clean `agent/streaming-transfer-v2` worktree from `master`; keep the old worktree read-only until its useful tests are re-created test-first.
- One `transferId` has exactly one receiver-owned `TransferSession`; the server never stores payload bytes.
- `DTRM` and `DTRA` select/resume the session; `DTRC` and `DTRK` carry data/ACK; `DTRF` finalizes; `DTRX` cleans up.
- No merge without `agent/verify.sh`, clean `git diff --check`, and a passing focused protocol-routing test.

---

### Task 1: Re-establish a clean, tested session core

**Files:** create `src/lib/barrier/TransferManifest.{h,cpp}`, `src/lib/barrier/TransferSession.{h,cpp}`; create `src/test/unittests/barrier/TransferSessionTests.cpp`.

- [ ] Write failing tests for unsafe paths, ordered durable offsets, and deletion after a SHA-256 mismatch.
- [ ] Run `./build-agent/bin/unittests --gtest_filter='TransferManifestTests.*:TransferSessionTests.*'`; confirm the missing types cause failure.
- [ ] Implement only parsing, temporary-directory writes, offset validation, SHA-256 finalization, and cleanup.
- [ ] Re-run the focused tests; commit `feat: add disk-backed transfer session`.

### Task 2: Add v2 protocol codec and message contract

**Files:** modify `src/lib/barrier/protocol_types.{h,cpp}`, `src/lib/barrier/ProtocolUtil.{h,cpp}`; extend `src/test/unittests/barrier/TransferSessionTests.cpp`.

- [ ] Write a failing round-trip test for a network-order `std::uint64_t` offset and literal v2 message layouts.
- [ ] Run its focused filter; confirm `%8i` is unsupported.
- [ ] Add `%8i` plus `DTRM`, `DTRA`, `DTRC`, `DTRK`, `DTRF`, and `DTRX`; enforce the 1 MiB chunk limit at decode.
- [ ] Re-run the focused tests; commit `feat: define transfer v2 protocol`.

### Task 3: Route transfer messages without server payload ownership

**Files:** modify `src/lib/client/{Client,ServerProxy}.{h,cpp}`, `src/lib/server/{BaseClientProxy,ClientProxy1_6,Server}.{h,cpp}`; create `src/test/integtests/net/TransferRoutingTests.cpp`.

- [ ] Write a failing integration test showing `DTRM` reaches the active peer, `DTRA` returns to the sender, and `DTRC` is relayed without a server byte buffer.
- [ ] Run `ctest --test-dir build-agent -R TransferRoutingTests --output-on-failure`; confirm it fails before routing exists.
- [ ] Add proxy methods that forward message fields only; store `TransferSession` in the receiving `Client`/`Server` endpoint, not in `Server::m_receivedFileData` or `Client::m_receivedFileData`.
- [ ] Verify the test, `agent/verify.sh`, and `git diff --check`; commit `feat: route resumable transfers`.

### Task 4: Resume and retire the v2 legacy branch

**Files:** same as Task 3; modify `agent/backlog.yaml` and `agent/state.json` only after verification.

- [ ] Write a failing reconnect test: a repeated `DTRM` with identical manifest hash yields the durable offsets; a changed hash yields `DTRX` and cleanup.
- [ ] Implement only that resume branch, then run focused tests and `agent/verify.sh`.
- [ ] Fast-forward merge, push `master`, mark `streaming-transfer` done, and remove the abandoned untracked worktree only after the new branch is merged.
