/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026
 */

#define BARRIER_TEST_ENV

#include "server/Server.h"
#include "barrier/StreamChunker.h"

#include "test/global/gtest.h"
#include "test/global/TestEventQueue.h"
#include "test/mock/barrier/MockScreen.h"
#include "test/mock/server/MockConfig.h"
#include "test/mock/server/MockPrimaryClient.h"
#include "test/mock/server/MockInputFilter.h"

class TransferRecordingClient : public MockPrimaryClient
{
public:
    void transferManifestSending(std::uint64_t id, const std::string&) override
    {
        manifestIds.push_back(id);
    }

    void transferChunkSending(std::uint64_t id, UInt32, std::uint64_t, const std::string&) override
    {
        chunkIds.push_back(id);
    }

    void transferAcceptSending(std::uint64_t id, bool) override
    {
        acceptIds.push_back(id);
    }

    std::vector<std::uint64_t> manifestIds;
    std::vector<std::uint64_t> chunkIds;
    std::vector<std::uint64_t> acceptIds;
};

class TransferRoutingTests : public testing::Test {
protected:
    void SetUp() override
    {
        ON_CALL(config, getInputFilter()).WillByDefault(testing::Return(&inputFilter));
        ON_CALL(config, isScreen(testing::_)).WillByDefault(testing::Return(true));
    }

    TestEventQueue events;
    testing::NiceMock<MockConfig> config;
    testing::NiceMock<MockScreen> screen;
    testing::NiceMock<MockInputFilter> inputFilter;
    testing::NiceMock<TransferRecordingClient> primary, firstSender, secondSender, receiver;
    ServerArgs args;
};

TEST_F(TransferRoutingTests, routesSameSenderIdFromDifferentClients)
{
    Server server(config, &primary, &screen, &events, args);
    server.setActive(&receiver);

    server.transferManifestReceived(&firstSender, 7, "first");
    server.transferManifestReceived(&secondSender, 7, "second");
    server.transferChunkReceived(&firstSender, 7, 0, 0, "a");
    server.transferChunkReceived(&secondSender, 7, 0, 0, "b");
	ASSERT_EQ(2u, receiver.manifestIds.size());
	server.transferAcceptReceived(&receiver, receiver.manifestIds[0], true);
	server.transferAcceptReceived(&receiver, receiver.manifestIds[1], true);

    EXPECT_EQ(2u, receiver.manifestIds.size());
    EXPECT_EQ(2u, receiver.chunkIds.size());
	EXPECT_NE(receiver.manifestIds[0], receiver.manifestIds[1]);
	EXPECT_THAT(firstSender.acceptIds, testing::ElementsAre(7));
	EXPECT_THAT(secondSender.acceptIds, testing::ElementsAre(7));
}

TEST_F(TransferRoutingTests, repeatsManifestToOriginalReceiverAfterActiveScreenChanges)
{
    Server server(config, &primary, &screen, &events, args);
    server.setActive(&receiver);
    server.transferManifestReceived(&firstSender, 7, "manifest");
    ASSERT_EQ(1u, receiver.manifestIds.size());
    const auto receiverId = receiver.manifestIds[0];
    server.setActive(&firstSender);
    server.transferManifestReceived(&firstSender, 7, "manifest");
    EXPECT_THAT(receiver.manifestIds, testing::ElementsAre(receiverId, receiverId));
    EXPECT_TRUE(firstSender.manifestIds.empty());
}

TEST_F(TransferRoutingTests, freshTransferAfterServerRestartUsesNewReceiverId)
{
    {
        Server server(config, &primary, &screen, &events, args);
        server.setActive(&receiver);
        server.transferManifestReceived(&firstSender, 7, "partial transfer");
    }
    {
        Server server(config, &primary, &screen, &events, args);
        server.setActive(&receiver);
        server.transferManifestReceived(&firstSender, 7, "new transfer");
    }
    ASSERT_EQ(2u, receiver.manifestIds.size());
    EXPECT_NE(receiver.manifestIds[0], receiver.manifestIds[1]);
}

TEST_F(TransferRoutingTests, receiverFailureStopsLocalSenderWaitingForAck)
{
    Server server(config, &primary, &screen, &events, args);
    server.setActive(&receiver);
    StreamChunker::beginTransfer(42);
    server.transferManifestReceived(&primary, 42, "manifest");
    ASSERT_EQ(1u, receiver.manifestIds.size());
    const auto receiverId = receiver.manifestIds[0];
    server.transferAcceptReceived(&receiver, receiverId, true);
    server.transferResumeReceived(&receiver, receiverId, 0, 0);
    server.transferFinishedReceived(&receiver, receiverId, false);
    EXPECT_FALSE(StreamChunker::waitForTransferAcceptance(42));
    StreamChunker::finishTransfer(42);
}

TEST_F(TransferRoutingTests, receiverCancellationStopsLocalSenderWaitingForAck)
{
    Server server(config, &primary, &screen, &events, args);
    server.setActive(&receiver);
    StreamChunker::beginTransfer(42);
    server.transferManifestReceived(&primary, 42, "manifest");
    ASSERT_EQ(1u, receiver.manifestIds.size());
    const auto receiverId = receiver.manifestIds[0];
    server.transferAcceptReceived(&receiver, receiverId, true);
    server.transferResumeReceived(&receiver, receiverId, 0, 0);
    server.transferCancelReceived(&receiver, receiverId);
    EXPECT_FALSE(StreamChunker::waitForTransferAcceptance(42));
    StreamChunker::finishTransfer(42);
}
