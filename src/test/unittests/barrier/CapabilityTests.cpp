/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "barrier/ProtocolUtil.h"
#include "barrier/protocol_types.h"

#include "test/global/gmock.h"
#include "test/global/gtest.h"
#include "test/mock/io/MockStream.h"

#include <cstring>
#include <vector>

using ::testing::_;
using ::testing::Invoke;

TEST(CapabilityTests, rejectsOldProtocolMajorVersion)
{
    EXPECT_FALSE(Capabilities::isCompatible(1, kProtocolMinorVersion));
}

TEST(CapabilityTests, roundTripsRequiredCapabilities)
{
    const Capabilities sent = { true, true, true };
    std::vector<UInt8> wire;
    MockStream writer;

    EXPECT_CALL(writer, write(_, _)).WillOnce(Invoke(
        [&wire](const void* data, UInt32 size) {
            const UInt8* bytes = static_cast<const UInt8*>(data);
            wire.assign(bytes, bytes + size);
        }));
    EXPECT_TRUE(ProtocolUtil::writeCapabilities(&writer, sent));

    MockStream reader;
    size_t offset = 0;
    EXPECT_CALL(reader, read(_, _)).WillRepeatedly(Invoke(
        [&wire, &offset](void* data, UInt32 size) {
            const UInt32 available = static_cast<UInt32>(wire.size() - offset);
            const UInt32 count = (size < available) ? size : available;
            std::memcpy(data, &wire[offset], count);
            offset += count;
            return count;
        }));
    Capabilities received = { false, false, false };
    EXPECT_TRUE(ProtocolUtil::readCapabilities(&reader, received));
    EXPECT_EQ(sent, received);
}
