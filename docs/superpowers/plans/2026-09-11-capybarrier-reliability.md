# CapyBarrier Reliability Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build reliable image clipboard sharing, resumable cross-device drag-and-drop, automatic reconnection, and a calm status-first GUI for CapyBarrier.

**Architecture:** Replace the legacy in-memory file transfer path with a capability-negotiated `TransferSession` that streams verified files to an isolated temporary directory. Keep input sharing on the existing connection, but schedule transfer work through the event queue so ACKs and reconnects cannot block input. Drive the GUI from explicit runtime states instead of log-text parsing.

**Tech Stack:** C++11, Qt Widgets, CMake, GoogleTest/GoogleMock, OpenSSL, native Windows/macOS drag-and-drop APIs.

**Spec:** `docs/superpowers/specs/2026-09-11-capybarrier-reliability-design.md`

## Global Constraints

- New CapyBarrier clients and servers require the new protocol major version; old Barrier nodes are rejected.
- The first drag-and-drop release supports Windows and macOS; Linux X11 and Wayland are subsequent releases.
- Transfers copy files and folders to the active target application, never move source files.
- Transfers resume after reconnect using verified file offsets and SHA-256 validation.
- TLS remains optional but the GUI must show a persistent warning when it is disabled.
- Do not add a third-party dependency for transfer framing, hashing, retry scheduling, or GUI styling.

---

### Task 1: Versioned capability handshake

**Files:**
- Modify: `src/lib/barrier/protocol_types.h`, `src/lib/barrier/protocol_types.cpp`
- Modify: `src/lib/client/Client.cpp`, `src/lib/client/ServerProxy.cpp`, `src/lib/server/ClientProxyUnknown.cpp`
- Create: `src/test/unittests/barrier/CapabilityTests.cpp`
- Modify: `src/test/integtests/net/NetworkTests.cpp`

**Interfaces:**
- Produces `Capabilities { bool clipboardImageV2; bool transferV2; bool transferResumeV2; }`.
- Produces `bool ProtocolUtil::writeCapabilities(IStream*, const Capabilities&)` and `bool ProtocolUtil::readCapabilities(IStream*, Capabilities&)`.
- Consumes the negotiated capability set before any image or transfer message is accepted.

- [ ] **Step 1: Write the failing protocol tests**

```cpp
TEST(CapabilityTests, rejectsOldProtocolMajorVersion) {
    EXPECT_FALSE(Capabilities::isCompatible(1, kProtocolMinorVersion));
}

TEST(CapabilityTests, roundTripsRequiredCapabilities) {
    Capabilities sent = { true, true, true };
    EXPECT_EQ(sent, roundTripCapabilities(sent));
}
```

- [ ] **Step 2: Run the focused test and verify failure**

Run: `ctest --test-dir build -R CapabilityTests --output-on-failure`

Expected: FAIL because `Capabilities` and its codec do not exist.

- [ ] **Step 3: Add the protocol major bump, `CAPS` message, and handshake gate**

```cpp
struct Capabilities {
    bool clipboardImageV2;
    bool transferV2;
    bool transferResumeV2;
    bool operator==(const Capabilities& other) const;
};
```

Reject handshakes whose major differs from `kProtocolMajorVersion`. Exchange `CAPS` after hello-back and close the stream if any required capability is false.

- [ ] **Step 4: Run unit and network handshake tests**

Run: `ctest --test-dir build -R 'CapabilityTests|NetworkTests' --output-on-failure`

Expected: PASS; an old major is rejected and matching new nodes negotiate all three capabilities.

- [ ] **Step 5: Commit**

```bash
git add src/lib/barrier src/lib/client src/lib/server src/test/unittests/barrier/CapabilityTests.cpp src/test/integtests/net/NetworkTests.cpp
git commit -m "feat: negotiate CapyBarrier capabilities"
```

### Task 2: Safe streaming transfer session

**Files:**
- Create: `src/lib/barrier/TransferManifest.h`, `src/lib/barrier/TransferManifest.cpp`
- Create: `src/lib/barrier/TransferSession.h`, `src/lib/barrier/TransferSession.cpp`
- Modify: `src/lib/barrier/CMakeLists.txt`, `src/lib/client/Client.h`, `src/lib/client/Client.cpp`, `src/lib/client/ServerProxy.cpp`, `src/lib/server/ClientProxy1_5.cpp`
- Create: `src/test/unittests/barrier/TransferManifestTests.cpp`, `src/test/unittests/barrier/TransferSessionTests.cpp`

**Interfaces:**
- Produces `TransferManifest::parse(const std::string&, TransferManifest&)` and `bool TransferManifest::isSafeRelativePath(const std::string&)`.
- Produces `TransferSession::accept(const TransferManifest&, const std::string& tempRoot)` and `TransferSession::writeChunk(FileId, UInt64 offset, const void*, size_t)`.
- Produces `std::vector<ResumeOffset> TransferSession::verifiedOffsets() const` and `bool TransferSession::finalize()`.

**Wire contract:** Use `DTRM`, `DTRA`, `DTRC`, `DTRK`, `DTRF`, and `DTRX` exactly as defined in the design spec. Add the existing `ProtocolUtil` `%8i` network-order integer codec for `UInt64` offsets; keep variable manifest and offset-list fields as existing length-prefixed strings. A chunk is at most 1 MiB. Route v2 messages through the client proxy, server relay, and client server proxy; reject `DFTR`/`DDRG` after the v2 handshake.

- [ ] **Step 1: Write failing validation and resume tests**

```cpp
TEST(TransferManifestTests, rejectsTraversalAndAbsolutePaths) {
    EXPECT_FALSE(TransferManifest::isSafeRelativePath("../secret"));
    EXPECT_FALSE(TransferManifest::isSafeRelativePath("/etc/passwd"));
}

TEST(TransferSessionTests, resumesAtLastAcknowledgedOffset) {
    TransferSession session;
    session.writeChunk(1, 0, "abcd", 4);
    EXPECT_EQ(4u, session.verifiedOffsets().at(0).offset);
}
```

- [ ] **Step 2: Run tests and verify failure**

Run: `ctest --test-dir build -R 'TransferManifestTests|TransferSessionTests' --output-on-failure`

Expected: FAIL because the manifest and session types do not exist.

- [ ] **Step 3: Implement manifest parsing and disk-backed transfer state**

Serialize paths, kind, size, and SHA-256 in a length-prefixed manifest. Reject unsafe paths before opening a destination file. Write chunks only at the expected offset, ACK the durable offset, compute SHA-256 after the last chunk, and delete the session temporary directory on cancel, mismatch, or I/O error.

- [ ] **Step 4: Replace legacy in-memory `FileChunk` call sites**

Route new transfer messages to `TransferSession`; do not append file bytes to `Client::m_receivedFileData` or server-wide shared strings. Retain only the old code required for source compatibility until its callers are removed in this task.

- [ ] **Step 5: Run focused tests**

Run: `ctest --test-dir build -R 'TransferManifestTests|TransferSessionTests|NetworkTests' --output-on-failure`

Expected: PASS; invalid paths are rejected, interrupted files resume at the ACK offset, and SHA mismatch removes partial output.

- [ ] **Step 6: Commit**

```bash
git add src/lib/barrier src/lib/client src/lib/server src/test/unittests/barrier
git commit -m "feat: add resumable streaming transfers"
```

### Task 3: Image clipboard v2

**Files:**
- Modify: `src/lib/barrier/ClipboardChunk.h`, `src/lib/barrier/ClipboardChunk.cpp`
- Modify: `src/lib/platform/MSWindowsClipboardBitmapConverter.cpp`, `src/lib/platform/OSXClipboardBMPConverter.cpp`, `src/lib/platform/XWindowsClipboardBMPConverter.cpp`
- Modify: `src/test/unittests/barrier/ClipboardChunkTests.cpp`
- Create: `src/test/integtests/platform/ImageClipboardInteropTests.cpp`

**Interfaces:**
- Produces `ClipboardImagePayload { std::string png; UInt32 width; UInt32 height; }`.
- Produces `bool ClipboardChunk::assembleImage(IStream*, ClipboardImagePayload&)`.
- Consumes/produces canonical PNG between platform clipboard adapters.

- [ ] **Step 1: Write failing image tests**

```cpp
TEST(ClipboardChunkTests, rejectsOutOfOrderImageChunksWithoutReplacingClipboard) {
    EXPECT_EQ(kError, assembleImageChunks(dataBeforeStart));
}

TEST(ClipboardChunkTests, preservesPngPayload) {
    EXPECT_EQ(pngBytes, roundTripImage(pngBytes).png);
}
```

- [ ] **Step 2: Run focused test and verify failure**

Run: `ctest --test-dir build -R ClipboardChunkTests --output-on-failure`

Expected: FAIL because image-v2 assembly is absent.

- [ ] **Step 3: Implement canonical PNG framing and adapters**

Add image start/data/end framing with total size and sequence validation. Convert native screenshot bitmaps to PNG before send; publish valid PNG and native bitmap representations on receive. On decode or framing failure, keep the previously owned clipboard untouched.

- [ ] **Step 4: Run unit plus platform integration tests**

Run: `ctest --test-dir build -R 'ClipboardChunkTests|ImageClipboardInteropTests' --output-on-failure`

Expected: PASS; the same PNG survives a send/receive cycle and invalid input cannot clear a valid clipboard.

- [ ] **Step 5: Commit**

```bash
git add src/lib/barrier src/lib/platform src/test/unittests/barrier/ClipboardChunkTests.cpp src/test/integtests/platform/ImageClipboardInteropTests.cpp
git commit -m "feat: stabilize image clipboard transfer"
```

### Task 4: Single-owner automatic reconnection

**Files:**
- Modify: `src/lib/client/Client.h`, `src/lib/client/Client.cpp`
- Create: `src/test/unittests/client/ClientReconnectTests.cpp`
- Modify: `src/test/integtests/net/NetworkTests.cpp`

**Interfaces:**
- Produces `Client::scheduleReconnect()` and `Client::cancelReconnect()`.
- Produces `UInt32 Client::nextReconnectDelaySeconds() const`.
- Consumes disconnect, connect-failed, suspend, resume, and handshake-complete events.

- [ ] **Step 1: Write failing retry schedule tests**

```cpp
TEST(ClientReconnectTests, backsOffAndCapsAtThirtySeconds) {
    EXPECT_THAT(retryDelays(client, 7), ElementsAre(1, 2, 4, 8, 16, 30, 30));
}

TEST(ClientReconnectTests, resetsAfterHandshake) {
    client.handshakeComplete();
    EXPECT_EQ(1u, client.nextReconnectDelaySeconds());
}
```

- [ ] **Step 2: Run test and verify failure**

Run: `ctest --test-dir build -R ClientReconnectTests --output-on-failure`

Expected: FAIL because retry ownership and delays are not modeled.

- [ ] **Step 3: Implement one reconnect timer**

`scheduleReconnect()` is a no-op when a reconnect timer or active socket exists. It creates one one-shot timer, advances through `1,2,4,8,16,30`, and is cancelled before manual stop. Successful handshake resets the delay and starts a transfer resume negotiation only when an unfinished session exists.

- [ ] **Step 4: Run reconnect and network tests**

Run: `ctest --test-dir build -R 'ClientReconnectTests|NetworkTests' --output-on-failure`

Expected: PASS; no duplicate attempts occur and a dropped stream reconnects using the configured address.

- [ ] **Step 5: Commit**

```bash
git add src/lib/client src/test/unittests/client/ClientReconnectTests.cpp src/test/integtests/net/NetworkTests.cpp
git commit -m "feat: reconnect clients with bounded backoff"
```

### Task 5: Native Windows/macOS drop delivery

**Files:**
- Modify: `src/lib/platform/MSWindowsScreen.cpp`, `src/lib/platform/MSWindowsDropTarget.cpp`
- Modify: `src/lib/platform/OSXScreen.mm`, `src/lib/platform/OSXDragSimulator.mm`
- Modify: `src/lib/barrier/Screen.cpp`, `src/lib/client/Client.cpp`
- Create: `src/test/integtests/platform/DragDropTransferTests.cpp`

**Interfaces:**
- Consumes a finalized `TransferSession` directory and manifest.
- Produces `Screen::startDraggingFiles(DragFileList&)` only after transfer hash validation.
- Produces platform-native copy drag providers from completed temporary paths.

- [ ] **Step 1: Write failing completed-transfer delivery test**

```cpp
TEST(DragDropTransferTests, exposesOnlyVerifiedFilesToDropProvider) {
    EXPECT_FALSE(createDropProvider(incompleteSession).isValid());
    EXPECT_TRUE(createDropProvider(verifiedSession).isValid());
}
```

- [ ] **Step 2: Run test and verify failure**

Run: `ctest --test-dir build -R DragDropTransferTests --output-on-failure`

Expected: FAIL because native providers accept legacy transfer state.

- [ ] **Step 3: Bind verified session completion to native drag providers**

Use finalized temporary paths to populate `CF_HDROP` on Windows and promised-file URLs on macOS. Keep the drop provider alive until the target app consumes it, then remove the temporary directory. Preserve folders as directories and expose copy, not move, semantics.

- [ ] **Step 4: Run automated tests and manual app matrix**

Run: `ctest --test-dir build -R DragDropTransferTests --output-on-failure`

Expected: PASS. Then perform `DND-01`, `XFER-01`, `XFER-02`, and `XFER-03` from the acceptance document on Windows and macOS.

- [ ] **Step 5: Commit**

```bash
git add src/lib/platform src/lib/barrier/Screen.cpp src/lib/client src/test/integtests/platform/DragDropTransferTests.cpp
git commit -m "feat: deliver verified files through native drops"
```

### Task 6: Calm, status-first Qt GUI

**Files:**
- Modify: `src/gui/src/MainWindow.h`, `src/gui/src/MainWindow.cpp`, `src/gui/src/MainWindowBase.ui`
- Modify: `src/gui/src/ServerConfigDialogBase.ui`, `src/gui/res/Barrier.qrc`
- Create: `src/test/guitests/src/MainWindowStateTests.cpp`, `src/test/guitests/src/MainWindowStateTests.h`

**Interfaces:**
- Produces `MainWindow::setConnectionState(ConnectionState)` and `MainWindow::setTransferStatus(const TransferStatus&)`.
- `ConnectionState` values: `Disconnected`, `Connecting`, `Connected`, `Reconnecting`, `Transferring`, `Error`.
- `TransferStatus` contains `label`, `completedBytes`, `totalBytes`, `resuming`, and `failureReason`.

- [ ] **Step 1: Write failing GUI state tests**

```cpp
TEST_F(MainWindowStateTests, reconnectingShowsNextActionAndKeepsStopEnabled) {
    window.setConnectionState(ConnectionState::Reconnecting);
    EXPECT_EQ("다시 연결하는 중", window.statusText());
    EXPECT_TRUE(window.stopButton()->isEnabled());
}

TEST_F(MainWindowStateTests, insecureConnectionShowsWarning) {
    window.setConnectionState(ConnectionState::Connected);
    window.setCryptoEnabled(false);
    EXPECT_TRUE(window.insecureWarning()->isVisible());
}
```

- [ ] **Step 2: Run GUI test and verify failure**

Run: `ctest --test-dir build -R MainWindowStateTests --output-on-failure`

Expected: FAIL because explicit state and status widgets do not exist.

- [ ] **Step 3: Implement the state panel and explicit state transitions**

Replace log-text-driven connection inference with calls from IPC/process state handlers. Add a compact status panel above existing setup controls, with connection summary, security line, clipboard line, transfer line, progress bar only while transferring, and `기기 배치`/`전송 보기` secondary actions. Keep configuration and log dialogs intact.

- [ ] **Step 4: Run GUI tests and keyboard-accessibility check**

Run: `ctest --test-dir build -R MainWindowStateTests --output-on-failure`

Expected: PASS. Manually complete `GUI-01`, `GUI-02`, and `GUI-03` in the acceptance document in both light and dark OS themes.

- [ ] **Step 5: Commit**

```bash
git add src/gui/src src/gui/res src/test/guitests/src/MainWindowStateTests.cpp src/test/guitests/src/MainWindowStateTests.h
git commit -m "feat: add calm connection status UI"
```

### Task 7: Full regression and release evidence

**Files:**
- Modify: `docs/superpowers/verification/2026-09-11-capybarrier-acceptance.md`
- Add: `doc/newsfragments/<issue>.feature.md`

**Interfaces:**
- Consumes all automated results and the completed manual QA matrix.
- Produces a release-ready verification record with every P0 item marked Pass.

- [ ] **Step 1: Build every supported local target**

Run: `cmake --build build --parallel`

Expected: PASS with no compiler or linker error.

- [ ] **Step 2: Run the full automated suite**

Run: `ctest --test-dir build --output-on-failure`

Expected: PASS with all new unit, integration, and GUI tests included.

- [ ] **Step 3: Complete the manual matrix**

Run every P0 row in `docs/superpowers/verification/2026-09-11-capybarrier-acceptance.md`; enter OS pair, application, TLS state, result, and log location in its QA table.

- [ ] **Step 4: Add release note**

Describe image clipboard compatibility, resumable drag-and-drop, reconnection, and the new status-first GUI in the project newsfragment format.

- [ ] **Step 5: Commit**

```bash
git add docs/superpowers/verification doc/newsfragments
git commit -m "docs: record CapyBarrier reliability verification"
```
