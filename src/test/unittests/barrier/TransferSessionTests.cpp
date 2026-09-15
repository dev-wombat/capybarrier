/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "barrier/TransferManifest.h"
#include "barrier/TransferSession.h"
#include "base/String.h"
#include "barrier/ProtocolUtil.h"
#include "barrier/protocol_types.h"
#include "io/filesystem.h"

#include "test/global/gmock.h"
#include "test/global/gtest.h"
#include "test/mock/io/MockStream.h"

#include <cstring>
#include <vector>

using ::testing::_;
using ::testing::Invoke;

TEST(TransferProtocolTests, roundTripsUInt64ValuesInNetworkByteOrder)
{
	const std::uint64_t expected = static_cast<std::uint64_t>(0x10203040) << 32 | 0x50607080;
	std::vector<UInt8> wire;
	MockStream writer;
	EXPECT_CALL(writer, write(_, _)).WillOnce(Invoke(
		[&wire](const void* data, UInt32 size) {
			const UInt8* bytes = static_cast<const UInt8*>(data);
			wire.assign(bytes, bytes + size);
		}));
	ProtocolUtil::writef(&writer, "%8i", expected);

	MockStream reader;
	std::size_t offset = 0;
	EXPECT_CALL(reader, read(_, _)).WillRepeatedly(Invoke(
		[&wire, &offset](void* data, UInt32 size) {
			const UInt32 available = static_cast<UInt32>(wire.size() - offset);
			const UInt32 count = size < available ? size : available;
			std::memcpy(data, &wire[offset], count);
			offset += count;
			return count;
		}));
	std::uint64_t actual = 0;
	EXPECT_TRUE(ProtocolUtil::readf(&reader, "%8i", &actual));
	EXPECT_EQ(expected, actual);
}

TEST(TransferProtocolTests, roundTripsChunkIdentityOffsetAndPayload)
{
	const std::uint64_t transferId = static_cast<std::uint64_t>(0x10203040) << 32 | 0x50607080;
	const std::uint64_t expectedOffset = 4096;
	const UInt32 expectedEntry = 3;
	String payload = "chunk";
	std::vector<UInt8> wire;
	MockStream writer;
	EXPECT_CALL(writer, write(_, _)).WillOnce(Invoke(
		[&wire](const void* data, UInt32 size) {
			const UInt8* bytes = static_cast<const UInt8*>(data);
			wire.assign(bytes, bytes + size);
		}));
	ProtocolUtil::writef(&writer, kMsgDTransferChunk, transferId, expectedEntry, expectedOffset, &payload);

	MockStream reader;
	std::size_t position = 0;
	EXPECT_CALL(reader, read(_, _)).WillRepeatedly(Invoke(
		[&wire, &position](void* data, UInt32 size) {
			const UInt32 available = static_cast<UInt32>(wire.size() - position);
			const UInt32 count = size < available ? size : available;
			std::memcpy(data, &wire[position], count);
			position += count;
			return count;
		}));
	std::uint64_t actualTransferId = 0;
	std::uint64_t actualOffset = 0;
	UInt32 actualEntry = 0;
	String actualPayload;
	ASSERT_TRUE(ProtocolUtil::readf(&reader, kMsgDTransferChunk,
		&actualTransferId, &actualEntry, &actualOffset, &actualPayload));
	EXPECT_EQ(transferId, actualTransferId);
	EXPECT_EQ(expectedEntry, actualEntry);
	EXPECT_EQ(expectedOffset, actualOffset);
	EXPECT_EQ(payload, actualPayload);
}

TEST(TransferManifestTests, rejectsPathsOutsideTheTransferRoot)
{
	EXPECT_FALSE(TransferManifest::isSafeRelativePath("../secret"));
	EXPECT_FALSE(TransferManifest::isSafeRelativePath("/etc/passwd"));
	EXPECT_FALSE(TransferManifest::isSafeRelativePath("folder//file"));
	EXPECT_FALSE(TransferManifest::isSafeRelativePath(""));
	EXPECT_TRUE(TransferManifest::isSafeRelativePath("folder/file.txt"));
}

TEST(TransferManifestTests, parsesLengthPrefixedFileEntries)
{
	TransferManifest manifest;
	ASSERT_TRUE(TransferManifest::parse(
		"F 8:note.txt 4 64:88d4266fd4e6338d13b845fcf289579d209c897823b9217da3e161936f031589\n",
		manifest));
	ASSERT_EQ(1u, manifest.entries().size());
	EXPECT_FALSE(manifest.entries()[0].isDirectory);
	EXPECT_EQ("note.txt", manifest.entries()[0].path);
	EXPECT_EQ(4u, manifest.entries()[0].size);
	EXPECT_EQ("88d4266fd4e6338d13b845fcf289579d209c897823b9217da3e161936f031589",
		manifest.entries()[0].sha256);
}

TEST(TransferSessionTests, reportsTheLastContiguousChunkOffset)
{
	const barrier::fs::path root = barrier::fs::temp_directory_path() / "capybarrier-transfer-offset";
	barrier::fs::remove_all(root);
	TransferManifest manifest;
	ASSERT_TRUE(TransferManifest::parse(
		"F 8:note.txt 4 64:88d4266fd4e6338d13b845fcf289579d209c897823b9217da3e161936f031589\n",
		manifest));

	TransferSession session;
	ASSERT_TRUE(session.accept(manifest, root.string()));
	ASSERT_TRUE(session.writeChunk(0, 0, "abcd"));
	ASSERT_EQ(1u, session.verifiedOffsets().size());
	EXPECT_EQ(4u, session.verifiedOffsets()[0].offset);

	barrier::fs::remove_all(root);
}

TEST(TransferSessionTests, removesPartialFilesWhenTheDigestDoesNotMatch)
{
	const barrier::fs::path root = barrier::fs::temp_directory_path() / "capybarrier-transfer-mismatch";
	barrier::fs::remove_all(root);
	TransferManifest manifest;
	ASSERT_TRUE(TransferManifest::parse(
		"F 8:note.txt 4 64:0000000000000000000000000000000000000000000000000000000000000000\n",
		manifest));

	TransferSession session;
	ASSERT_TRUE(session.accept(manifest, root.string()));
	ASSERT_TRUE(session.writeChunk(0, 0, "abcd"));
	EXPECT_FALSE(session.finalize());
	EXPECT_FALSE(barrier::fs::exists(root));
}
