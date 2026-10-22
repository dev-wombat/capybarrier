/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026
 */

#include "barrier/StreamChunker.h"
#include "barrier/TransferManifest.h"
#include "barrier/TransferSession.h"
#include "base/EventTypes.h"
#include "io/filesystem.h"
#include "test/global/gtest.h"
#include "test/global/TestEventQueue.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <future>
#include <iterator>
#include <mutex>

TEST(StreamChunkerTests, sendsChunksOnlyAfterTransferIsAccepted)
{
    StreamChunker::beginTransfer(42);
    StreamChunker::acceptTransfer(42, true);
    StreamChunker::resumeTransfer(42, 0, 0);
    EXPECT_TRUE(StreamChunker::waitForTransferAcceptance(42));
    StreamChunker::finishTransfer(42);
}

TEST(StreamChunkerTests, doesNotSendChunksWhenTransferIsRejected)
{
    StreamChunker::beginTransfer(43);
    StreamChunker::acceptTransfer(43, false);
    EXPECT_FALSE(StreamChunker::waitForTransferAcceptance(43));
    StreamChunker::finishTransfer(43);
}

TEST(StreamChunkerTests, resumesFromTheReceiverVerifiedOffset)
{
    StreamChunker::beginTransfer(44);
    StreamChunker::acceptTransfer(44, true);
    StreamChunker::resumeTransfer(44, 0, 1024);
    EXPECT_TRUE(StreamChunker::waitForTransferAcceptance(44));
    EXPECT_EQ(1024u, StreamChunker::transferOffset(44));
    StreamChunker::finishTransfer(44);
}

TEST(StreamChunkerTests, staleInitialAckCannotRewindResumeOffset)
{
    StreamChunker::beginTransfer(45);
    StreamChunker::acceptTransfer(45, true);
    StreamChunker::resumeTransfer(45, 0, 2);
    StreamChunker::resumeTransfer(45, 0, 0);
    EXPECT_EQ(2u, StreamChunker::transferOffset(45));
    StreamChunker::finishTransfer(45);
}

namespace {

class TransferQueue : public TestEventQueue {
public:
    TransferQueue() { forFile().fileChunkSending(); }

    ~TransferQueue()
    {
        for (const Event& event : queued) Event::deleteData(event);
    }

    void addEvent(const Event& event) override
    {
        std::lock_guard<std::mutex> lock(mutex);
        queued.push_back(event);
        changed.notify_all();
    }

    bool hasEvent(std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, timeout, [this] { return !queued.empty(); });
    }

    StreamChunker::TransferEvent take()
    {
        std::unique_lock<std::mutex> lock(mutex);
        if (!changed.wait_for(lock, std::chrono::seconds(2), [this] { return !queued.empty(); })) {
            throw std::runtime_error("sender produced no transfer event");
        }
        Event event = queued.front();
        queued.pop_front();
        StreamChunker::TransferEvent value =
            *static_cast<StreamChunker::TransferEvent*>(event.getDataObject());
        Event::deleteData(event);
        return value;
    }

private:
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<Event> queued;
};

class TransferSenderTests : public testing::Test {
protected:
    void SetUp() override
    {
        root = barrier::fs::canonical(barrier::fs::temp_directory_path()) / "capybarrier-sender-tests";
        barrier::fs::create_directories(root);
        path = root / "file.txt";
    }

    void start(const std::string& contents)
    {
        std::ofstream file;
        barrier::open_utf8_path(file, path, std::ios::binary | std::ios::out);
        file << contents;
        file.close();
        sender = std::async(std::launch::async, [this] {
            StreamChunker::sendTransferFile(path.string().c_str(), &events, this);
        });
        const auto manifest = events.take();
        ASSERT_EQ(StreamChunker::TransferEvent::kManifest, manifest.m_type);
        id = manifest.m_id;
        manifestText = manifest.m_data;
    }

    void TearDown() override
    {
        StreamChunker::acceptTransfer(id, false);
        if (sender.valid()) sender.get();
        StreamChunker::finishTransfer(id);
        barrier::fs::remove_all(root);
    }

    TransferQueue events;
    barrier::fs::path root, path;
    std::future<void> sender;
    std::uint64_t id = 0;
    std::string manifestText;
};

TEST_F(TransferSenderTests, waitsForInitialOffsetAfterAcceptance)
{
    start("abcd");
    StreamChunker::acceptTransfer(id, true);
    EXPECT_FALSE(events.hasEvent(std::chrono::milliseconds(100)));
    StreamChunker::resumeTransfer(id, 0, 2);
    const auto chunk = events.take();
    EXPECT_EQ(StreamChunker::TransferEvent::kChunk, chunk.m_type);
    EXPECT_EQ(2u, chunk.m_offset);
    EXPECT_EQ("cd", chunk.m_data);
    StreamChunker::resumeTransfer(id, 0, 4);
    const auto finished = events.take();
    EXPECT_EQ(StreamChunker::TransferEvent::kFinished, finished.m_type);
    EXPECT_TRUE(finished.m_success);
}

TEST_F(TransferSenderTests, waitsForEachChunkAckBeforeQueueingMoreData)
{
    const std::string contents(65537, 'a');
    start(contents);
    StreamChunker::acceptTransfer(id, true);
    StreamChunker::resumeTransfer(id, 0, 0);
    std::string received;
    const std::size_t sizes[] = {32768, 32768, 1};
    for (const auto size : sizes) {
        const auto chunk = events.take();
        EXPECT_EQ(StreamChunker::TransferEvent::kChunk, chunk.m_type);
        EXPECT_EQ(received.size(), chunk.m_offset);
        EXPECT_EQ(size, chunk.m_data.size());
        received += chunk.m_data;
        EXPECT_FALSE(events.hasEvent(std::chrono::milliseconds(100)));
        StreamChunker::resumeTransfer(id, 0, received.size());
    }
    EXPECT_EQ(contents, received);
    const auto finished = events.take();
    EXPECT_EQ(StreamChunker::TransferEvent::kFinished, finished.m_type);
    EXPECT_TRUE(finished.m_success);
}

TEST_F(TransferSenderTests, rejectsResumeOffsetBeyondFileSize)
{
    start("abcd");
    StreamChunker::acceptTransfer(id, true);
    StreamChunker::resumeTransfer(id, 0, 5);
    const auto finished = events.take();
    EXPECT_EQ(StreamChunker::TransferEvent::kFinished, finished.m_type);
    EXPECT_FALSE(finished.m_success);
}

TEST_F(TransferSenderTests, rejectsFileTruncatedAfterManifestWasSent)
{
    start("abcd");
    std::ofstream truncate;
    barrier::open_utf8_path(truncate, path, std::ios::binary | std::ios::out | std::ios::trunc);
    truncate.close();
    StreamChunker::acceptTransfer(id, true);
    StreamChunker::resumeTransfer(id, 0, 0);
    const auto finished = events.take();
    EXPECT_EQ(StreamChunker::TransferEvent::kFinished, finished.m_type);
    EXPECT_FALSE(finished.m_success);
}

TEST_F(TransferSenderTests, stopsWithoutQueueingMoreChunksWhenReceiverRejects)
{
    start(std::string(65537, 'a'));
    StreamChunker::acceptTransfer(id, true);
    StreamChunker::resumeTransfer(id, 0, 0);
    EXPECT_EQ(StreamChunker::TransferEvent::kChunk, events.take().m_type);
    StreamChunker::acceptTransfer(id, false);
    const auto finished = events.take();
    EXPECT_EQ(StreamChunker::TransferEvent::kFinished, finished.m_type);
    EXPECT_FALSE(finished.m_success);
}

TEST_F(TransferSenderTests, resumedChunksCompleteRealReceiverHashAndDelivery)
{
    start("abcd");
    TransferManifest manifest;
    ASSERT_TRUE(TransferManifest::parse(manifestText, manifest));
    TransferSession receiver;
    ASSERT_TRUE(receiver.accept(manifest, (root / "receiver").string()));
    ASSERT_TRUE(receiver.writeChunk(0, 0, "ab"));
    StreamChunker::acceptTransfer(id, true);
    StreamChunker::resumeTransfer(id, 0, receiver.verifiedOffsets()[0].offset);
    const auto chunk = events.take();
    ASSERT_EQ(StreamChunker::TransferEvent::kChunk, chunk.m_type);
    EXPECT_EQ(2u, chunk.m_offset);
    ASSERT_TRUE(receiver.writeChunk(chunk.m_entry, chunk.m_offset, chunk.m_data));
    StreamChunker::resumeTransfer(id, 0, receiver.verifiedOffsets()[0].offset);
    const auto finished = events.take();
    ASSERT_EQ(StreamChunker::TransferEvent::kFinished, finished.m_type);
    ASSERT_TRUE(finished.m_success);
    ASSERT_TRUE(receiver.finalize());
    ASSERT_TRUE(receiver.copyTo((root / "delivered").string()));
    std::ifstream file;
    barrier::open_utf8_path(file, root / "delivered/file.txt", std::ios::binary | std::ios::in);
    EXPECT_EQ("abcd", std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()));
}

} // namespace
