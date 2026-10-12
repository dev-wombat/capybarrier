/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026
 */

#define BARRIER_TEST_ENV

#include "client/Client.h"
#include "server/Server.h"
#include "server/ClientListener.h"
#include "server/ClientProxy.h"
#include "net/SocketMultiplexer.h"
#include "net/TCPSocketFactory.h"
#include "base/TMethodEventJob.h"
#include "test/global/TestEventQueue.h"
#include "test/global/gtest.h"
#include "test/mock/barrier/MockScreen.h"
#include "test/mock/server/MockConfig.h"
#include "test/mock/server/MockInputFilter.h"
#include "test/mock/server/MockPrimaryClient.h"

#include <functional>
#include <exception>
#include <vector>

namespace {

class ResumeSender : public MockPrimaryClient {
public:
    void transferAcceptSending(std::uint64_t, bool accepted) override
    {
        onAcceptance(accepted);
    }

    void transferResumeSending(std::uint64_t, UInt32, std::uint64_t offset) override
    {
        onOffset(offset);
    }

    std::function<void(std::uint64_t)> onOffset;
    std::function<void(bool)> onAcceptance;
};

class TransferRoutingNetworkTests : public testing::Test {
public:
    void connected(const Event&, void*)
    {
        auto* peer = listener->getNextClient();
        ASSERT_NE(nullptr, peer);
        server->adoptClient(peer);
        server->setActive(peer);
        ++connections;
        std::string manifest =
            "F 8:note.txt 4 64:88d4266fd4e6338d13b845fcf289579d209c897823b9217da3e161936f031589\n";
        if (connections == 2 && !replacementManifest.empty()) manifest = replacementManifest;
        server->transferManifestReceived(&sender, 7001, manifest);
    }

    void disconnected(const Event&, void*)
    {
        if (beforeReconnect) beforeReconnect();
    }

    void runScenario();

    void offsetReceived(std::uint64_t offset)
    {
        offsets.push_back(offset);
        if (connections == 1 && offset == 0) {
            server->transferChunkReceived(&sender, 7001, 0, 0, "ab");
        }
        else if (connections == 1 && offset == 2) {
            server->setActive(&primary);
            receiver->disconnect(nullptr);
        }
        else if (connections == 2 && offset == 2) {
            server->transferChunkReceived(&sender, 7001, 0, 2, "cd");
        }
        else {
            server->transferCancelReceived(&sender, 7001);
            server->setActive(&primary);
            events.raiseQuitEvent();
        }
    }

    TestEventQueue events;
    testing::NiceMock<MockPrimaryClient> primary;
    testing::NiceMock<ResumeSender> sender;
    Server* server = nullptr;
    Client* receiver = nullptr;
    ClientListener* listener = nullptr;
    unsigned connections = 0;
    std::vector<std::uint64_t> offsets;
    std::vector<bool> acceptances;
    std::string replacementManifest;
    std::function<void()> beforeReconnect;
};

void TransferRoutingNetworkTests::runScenario()
{
    NetworkAddress address("127.0.0.1", 24804);
    address.resolve();
    SocketMultiplexer serverSockets, clientSockets;
    ClientListener listening(address, new TCPSocketFactory(&events, &serverSockets),
                             &events, ConnectionSecurityLevel::PLAINTEXT);
    listener = &listening;
    testing::NiceMock<MockScreen> serverScreen, clientScreen;
    testing::NiceMock<MockConfig> config;
    testing::NiceMock<MockInputFilter> inputFilter;
    ON_CALL(config, isScreen(testing::_)).WillByDefault(testing::Return(true));
    ON_CALL(config, getInputFilter()).WillByDefault(testing::Return(&inputFilter));
    ON_CALL(clientScreen, getShape(testing::_, testing::_, testing::_, testing::_))
        .WillByDefault(testing::Invoke([](SInt32& x, SInt32& y, SInt32& w, SInt32& h) {
            x = y = 0;
            w = h = 1;
        }));
    ServerArgs serverArgs;
    Server routing(config, &primary, &serverScreen, &events, serverArgs);
    server = &routing;
    listening.setServer(server);
    ClientArgs clientArgs;
    clientArgs.m_enableCrypto = false;
    clientArgs.m_restartable = true;
    Client receiving(&events, "receiver", address,
                     new TCPSocketFactory(&events, &clientSockets), &clientScreen, clientArgs);
    receiver = &receiving;
    sender.onOffset = [this](std::uint64_t offset) { offsetReceived(offset); };
    sender.onAcceptance = [this](bool accepted) {
        acceptances.push_back(accepted);
        if (!accepted) {
            server->setActive(&primary);
            events.raiseQuitEvent();
        }
    };
    events.adoptHandler(events.forClientListener().connected(), listener,
        new TMethodEventJob<TransferRoutingNetworkTests>(this, &TransferRoutingNetworkTests::connected));
    events.adoptHandler(events.forServer().disconnected(), server,
        new TMethodEventJob<TransferRoutingNetworkTests>(this, &TransferRoutingNetworkTests::disconnected));
    receiving.connect();
    events.initQuitTimeout(10);
    std::exception_ptr failure;
    try { events.loop(); }
    catch (...) { failure = std::current_exception(); }
    routing.setActive(&primary);
    events.cleanupQuitTimeout();
    events.removeHandler(events.forClientListener().connected(), listener);
    events.removeHandler(events.forServer().disconnected(), server);
    if (failure) std::rethrow_exception(failure);

    EXPECT_EQ(2u, connections);
}

TEST_F(TransferRoutingNetworkTests, repeatedManifestResumesAfterTcpReceiverReconnect)
{
    beforeReconnect = [this] {
        server->transferChunkReceived(&sender, 7001, 0, 2, "cd");
        server->transferManifestReceived(&sender, 7001, "offline retry");
    };
    runScenario();
    EXPECT_THAT(acceptances, testing::ElementsAre(true, true));
    EXPECT_THAT(offsets, testing::ElementsAre(0, 2, 2, 4));
}

TEST_F(TransferRoutingNetworkTests, changedManifestIsRejectedAfterTcpReceiverReconnect)
{
    replacementManifest =
        "F 8:note.txt 4 64:0000000000000000000000000000000000000000000000000000000000000000\n";
    runScenario();
    EXPECT_THAT(acceptances, testing::ElementsAre(true, false));
    EXPECT_THAT(offsets, testing::ElementsAre(0, 2));
}

TEST_F(TransferRoutingNetworkTests, cancellationWhileReceiverIsOfflineDiscardsTheRoute)
{
    beforeReconnect = [this] { server->transferCancelReceived(&sender, 7001); };
    runScenario();
    EXPECT_THAT(acceptances, testing::ElementsAre(true, true));
    EXPECT_THAT(offsets, testing::ElementsAre(0, 2, 0));
}

TEST_F(TransferRoutingNetworkTests, failureWhileReceiverIsOfflineDiscardsTheRoute)
{
    beforeReconnect = [this] { server->transferFinishedReceived(&sender, 7001, false); };
    runScenario();
    EXPECT_THAT(acceptances, testing::ElementsAre(true, true));
    EXPECT_THAT(offsets, testing::ElementsAre(0, 2, 0));
}

} // namespace
