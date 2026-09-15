/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026
 */

#define BARRIER_TEST_ENV

#include "server/Server.h"

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

TEST(TransferRoutingTests, routesSameSenderIdFromDifferentClients)
{
    TestEventQueue events;
    testing::NiceMock<MockConfig> config;
    testing::NiceMock<MockScreen> screen;
    testing::NiceMock<MockInputFilter> inputFilter;
    testing::NiceMock<TransferRecordingClient> primary;
    testing::NiceMock<TransferRecordingClient> firstSender;
    testing::NiceMock<TransferRecordingClient> secondSender;
    testing::NiceMock<TransferRecordingClient> receiver;
    ON_CALL(config, getInputFilter()).WillByDefault(testing::Return(&inputFilter));

    ServerArgs args;
    Server server(config, &primary, &screen, &events, args);
    server.setActive(&receiver);

    server.transferManifestReceived(&firstSender, 7, "first");
    server.transferManifestReceived(&secondSender, 7, "second");
    server.transferChunkReceived(&firstSender, 7, 0, 0, "a");
    server.transferChunkReceived(&secondSender, 7, 0, 0, "b");
	server.transferAcceptReceived(&receiver, receiver.manifestIds[0], true);
	server.transferAcceptReceived(&receiver, receiver.manifestIds[1], true);

    EXPECT_EQ(2u, receiver.manifestIds.size());
    EXPECT_EQ(2u, receiver.chunkIds.size());
	EXPECT_NE(receiver.manifestIds[0], receiver.manifestIds[1]);
	EXPECT_THAT(firstSender.acceptIds, testing::ElementsAre(7));
	EXPECT_THAT(secondSender.acceptIds, testing::ElementsAre(7));
}
