#include "client/Client.h"

#include "barrier/ClientArgs.h"
#include "net/NetworkAddress.h"
#include "net/TCPSocketFactory.h"
#include "test/global/TestEventQueue.h"
#include "test/global/gmock.h"
#include "test/mock/barrier/MockScreen.h"

TEST(ClientReconnectTests, backsOffAndCapsAtThirtySeconds)
{
    TestEventQueue events;
    testing::NiceMock<MockScreen> screen;
    ON_CALL(screen, getEventTarget()).WillByDefault(testing::Return(&screen));
    ClientArgs args;
    Client client(&events, "test", NetworkAddress("127.0.0.1", 24800),
                  new TCPSocketFactory(&events, NULL), &screen, args);
    std::vector<UInt32> delays;

    for (int i = 0; i < 7; ++i) {
        delays.push_back(client.nextReconnectDelaySeconds());
        client.scheduleReconnect();
        client.cancelReconnect();
    }

    EXPECT_THAT(delays, testing::ElementsAre(1, 2, 4, 8, 16, 30, 30));
}

TEST(ClientReconnectTests, resetsAfterHandshake)
{
    TestEventQueue events;
    testing::NiceMock<MockScreen> screen;
    ON_CALL(screen, getEventTarget()).WillByDefault(testing::Return(&screen));
    ClientArgs args;
    Client client(&events, "test", NetworkAddress("127.0.0.1", 24800),
                  new TCPSocketFactory(&events, NULL), &screen, args);

    client.scheduleReconnect();
    client.handshakeComplete();

    EXPECT_EQ(1u, client.nextReconnectDelaySeconds());
}

TEST(ClientReconnectTests, doesNotStackReconnectTimers)
{
    TestEventQueue events;
    testing::NiceMock<MockScreen> screen;
    ON_CALL(screen, getEventTarget()).WillByDefault(testing::Return(&screen));
    ClientArgs args;
    Client client(&events, "test", NetworkAddress("127.0.0.1", 24800),
                  new TCPSocketFactory(&events, NULL), &screen, args);

    client.scheduleReconnect();
    client.scheduleReconnect();

    EXPECT_EQ(2u, client.nextReconnectDelaySeconds());
}
