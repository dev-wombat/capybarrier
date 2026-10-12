/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026
 */

#include "client/Client.h"
#include "client/ServerProxy.h"
#include "barrier/ProtocolUtil.h"
#include "barrier/protocol_types.h"
#include "net/TCPSocketFactory.h"
#include "io/filesystem.h"
#include "test/global/TestEventQueue.h"
#include "test/global/gtest.h"
#include "test/mock/barrier/MockScreen.h"
#include "test/mock/io/MockStream.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <set>

namespace {

class WireStream : public testing::NiceMock<MockStream> {
public:
    UInt32 read(void* data, UInt32 size) override
    {
        const auto count = static_cast<UInt32>(std::min<std::size_t>(size, input.size()));
        std::memcpy(data, input.data(), count);
        input.erase(0, count);
        return count;
    }

    void write(const void* data, UInt32 size) override
    {
        output.emplace_back(static_cast<const char*>(data), size);
    }

    void* getEventTarget() const override { return const_cast<WireStream*>(this); }

    std::string input;
    std::vector<std::string> output;
};

class ReceivingProxy : public ServerProxy {
public:
    using ServerProxy::ServerProxy;
    using ServerProxy::parseMessage;
};

class ServerProxyTransferTests : public testing::Test {
protected:
    ServerProxyTransferTests() :
        client(&events, "receiver", NetworkAddress("127.0.0.1", 24800),
               new TCPSocketFactory(&events, nullptr), &screen, args),
        proxy(new ReceivingProxy(&client, &stream, &events)) { }

    void TearDown() override
    {
        WireStream packet;
        ProtocolUtil::writef(&packet, kMsgDTransferCancel, id);
        receive(packet.output[0]);
    }

    void receive(const std::string& packet)
    {
        stream.input = packet.substr(4);
        proxy->parseMessage(reinterpret_cast<const UInt8*>(packet.data()));
        if (!stream.output.empty() && stream.output.back() == "CNOP") stream.output.pop_back();
    }

    void manifest()
    {
        WireStream packet;
        const std::string text =
            "F 8:note.txt 4 64:88d4266fd4e6338d13b845fcf289579d209c897823b9217da3e161936f031589\n";
        ProtocolUtil::writef(&packet, kMsgDTransferManifest, id, &text);
        receive(packet.output[0]);
    }

    void chunk(std::uint64_t offset, const std::string& data)
    {
        WireStream packet;
        ProtocolUtil::writef(&packet, kMsgDTransferChunk, id, UInt32(0), offset, &data);
        receive(packet.output[0]);
    }

    void expectAcceptance(std::uint64_t expectedOffset)
    {
        ASSERT_EQ(2u, stream.output.size());
        WireStream response;
        response.input = stream.output[0];
        std::uint64_t responseId = 0;
        UInt8 accepted = 0;
        ASSERT_TRUE(ProtocolUtil::readf(&response, kMsgDTransferAccept, &responseId, &accepted));
        EXPECT_EQ(id, responseId);
        EXPECT_EQ(1u, accepted);
        response.input = stream.output[1];
        UInt32 entry = 99;
        std::uint64_t offset = 99;
        ASSERT_TRUE(ProtocolUtil::readf(&response, kMsgDTransferResume, &responseId, &entry, &offset));
        EXPECT_EQ(id, responseId);
        EXPECT_EQ(0u, entry);
        EXPECT_EQ(expectedOffset, offset);
        stream.output.clear();
    }

    const std::uint64_t id = 9401;
    TestEventQueue events;
    ClientArgs args;
    testing::NiceMock<MockScreen> screen;
    Client client;
    WireStream stream;
    std::unique_ptr<ReceivingProxy> proxy;
};

TEST_F(ServerProxyTransferTests, repeatsManifestWithVerifiedWireOffset)
{
    manifest();
    expectAcceptance(0);
    chunk(0, "ab");
    stream.output.clear();
    manifest();
    expectAcceptance(2);
}

TEST_F(ServerProxyTransferTests, invalidChunkDiscardsThePartialSession)
{
    manifest();
    expectAcceptance(0);
    chunk(0, "ab");
    stream.output.clear();
    chunk(3, "c");
    ASSERT_EQ(1u, stream.output.size());
    WireStream response;
    response.input = stream.output[0];
    std::uint64_t responseId = 0;
    UInt8 success = 1;
    ASSERT_TRUE(ProtocolUtil::readf(&response, kMsgDTransferFinished, &responseId, &success));
    EXPECT_EQ(id, responseId);
    EXPECT_EQ(0u, success);
    stream.output.clear();
    manifest();
    expectAcceptance(0);
}

TEST_F(ServerProxyTransferTests, truncatedManifestDoesNotReplyWithUnknownTransferId)
{
    receive("DTRM");
    EXPECT_TRUE(stream.output.empty());
}

TEST_F(ServerProxyTransferTests, unsafeManifestDiscardsThePartialSession)
{
    manifest();
    expectAcceptance(0);
    chunk(0, "ab");
    stream.output.clear();
    WireStream packet;
    const std::string unsafe = "D 9:../secret 0 0:\n";
    ProtocolUtil::writef(&packet, kMsgDTransferManifest, id, &unsafe);
    receive(packet.output[0]);
    ASSERT_EQ(1u, stream.output.size());
    WireStream response;
    response.input = stream.output[0];
    std::uint64_t responseId = 0;
    UInt8 accepted = 1;
    ASSERT_TRUE(ProtocolUtil::readf(&response, kMsgDTransferAccept, &responseId, &accepted));
    EXPECT_EQ(0u, accepted);
    stream.output.clear();
    manifest();
    expectAcceptance(0);
}

TEST_F(ServerProxyTransferTests, replacementConnectionRetainsVerifiedReceiverOffset)
{
    manifest();
    expectAcceptance(0);
    chunk(0, "ab");
    stream.output.clear();
    proxy.reset();
    proxy.reset(new ReceivingProxy(&client, &stream, &events));
    manifest();
    expectAcceptance(2);
    chunk(2, "cd");
    stream.output.clear();
    manifest();
    expectAcceptance(4);
}

TEST_F(ServerProxyTransferTests, replacementConnectionRejectsChangedManifest)
{
    manifest();
    expectAcceptance(0);
    chunk(0, "ab");
    stream.output.clear();
    proxy.reset();
    proxy.reset(new ReceivingProxy(&client, &stream, &events));
    WireStream packet;
    const std::string changed =
        "F 8:note.txt 4 64:0000000000000000000000000000000000000000000000000000000000000000\n";
    ProtocolUtil::writef(&packet, kMsgDTransferManifest, id, &changed);
    receive(packet.output[0]);
    ASSERT_EQ(1u, stream.output.size());
    WireStream response;
    response.input = stream.output[0];
    std::uint64_t responseId = 0;
    UInt8 accepted = 1;
    ASSERT_TRUE(ProtocolUtil::readf(&response, kMsgDTransferAccept, &responseId, &accepted));
    EXPECT_EQ(id, responseId);
    EXPECT_EQ(0u, accepted);
    stream.output.clear();
    manifest();
    expectAcceptance(0);
}

TEST(ServerProxyLifetimeTests, destroyingReceiverRemovesItsOwnedStagingDirectories)
{
    const std::string prefix = "capybarrier-transfer-9751-";
    const auto temp = barrier::fs::temp_directory_path();
    std::set<std::string> before;
    for (const auto& entry : barrier::fs::directory_iterator(temp)) {
        if (entry.path().filename().string().find(prefix) == 0) before.insert(entry.path().string());
    }
    std::vector<barrier::fs::path> created;
    {
        TestEventQueue events;
        testing::NiceMock<MockScreen> screen;
        ClientArgs args;
        Client client(&events, "receiver", NetworkAddress("127.0.0.1", 24800),
                      new TCPSocketFactory(&events, nullptr), &screen, args);
        WireStream stream, packet;
        ReceivingProxy proxy(&client, &stream, &events);
        const std::string manifest = "D 6:folder 0 0:\n";
        ProtocolUtil::writef(&packet, kMsgDTransferManifest, std::uint64_t(9751), &manifest);
        stream.input = packet.output[0].substr(4);
        proxy.parseMessage(reinterpret_cast<const UInt8*>("DTRM"));
        for (const auto& entry : barrier::fs::directory_iterator(temp)) {
            if (entry.path().filename().string().find(prefix) == 0 &&
                before.count(entry.path().string()) == 0) created.push_back(entry.path());
        }
        ASSERT_EQ(1u, created.size());
        EXPECT_TRUE(barrier::fs::exists(created[0] / "folder"));
    }
    EXPECT_FALSE(barrier::fs::exists(created[0]));
}

} // namespace
