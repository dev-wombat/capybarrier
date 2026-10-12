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
#include <fstream>
#include <iterator>
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
	EXPECT_FALSE(TransferManifest::isSafeRelativePath("..\\secret"));
	EXPECT_FALSE(TransferManifest::isSafeRelativePath("/etc/passwd"));
	EXPECT_FALSE(TransferManifest::isSafeRelativePath("folder//file"));
	EXPECT_FALSE(TransferManifest::isSafeRelativePath(""));
	EXPECT_TRUE(TransferManifest::isSafeRelativePath("folder/file.txt"));
}

TEST(TransferManifestTests, rejectsPathsThatAliasOrEscapeOnWindows)
{
	const std::string paths[] = {"C:/secret", "folder/file:stream", "NUL", "con.txt",
		"folder/COM1.log", "LPT9", "file.", "folder /file", "folder/../file",
		"bad?name", std::string("note\0.txt", 9), "line\rbreak", "COM\xc2\xb9.txt", "LPT\xc2\xb3"};
	for (const auto& path : paths) EXPECT_FALSE(TransferManifest::isSafeRelativePath(path)) << path;
	EXPECT_TRUE(TransferManifest::isSafeRelativePath("스크린샷/image.png"));
	EXPECT_TRUE(TransferManifest::isSafeRelativePath("COM10.txt"));
}

TEST(TransferManifestTests, rejectsDuplicateAndConflictingEntries)
{
	const std::string file = "F 8:note.txt 4 64:88d4266fd4e6338d13b845fcf289579d209c897823b9217da3e161936f031589\n";
	TransferManifest manifest;
	EXPECT_FALSE(TransferManifest::parse(file + file, manifest));
	EXPECT_FALSE(TransferManifest::parse(file + "D 8:NOTE.TXT 0 0:\n", manifest));
	EXPECT_FALSE(TransferManifest::parse(file + "D 12:note.txt/sub 0 0:\n", manifest));
	EXPECT_FALSE(TransferManifest::parse("D 12:note.txt/sub 0 0:\n" + file, manifest));
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

TEST(TransferManifestTests, createsASingleFileManifestWithDigest)
{
	const barrier::fs::path root = barrier::fs::temp_directory_path() / "capybarrier-manifest-file";
	barrier::fs::remove_all(root);
	barrier::fs::create_directories(root);
	const barrier::fs::path file = root / "note.txt";
	std::ofstream output;
	barrier::open_utf8_path(output, file, std::ios::out | std::ios::binary);
	output << "abcd";
	output.close();

	TransferManifest manifest;
	ASSERT_TRUE(TransferManifest::createForFile(file.string(), manifest));
	ASSERT_EQ(1u, manifest.entries().size());
	EXPECT_EQ("note.txt", manifest.entries()[0].path);
	EXPECT_EQ(4u, manifest.entries()[0].size);
	EXPECT_EQ("88d4266fd4e6338d13b845fcf289579d209c897823b9217da3e161936f031589",
		manifest.entries()[0].sha256);
	std::string text;
	ASSERT_TRUE(manifest.serialize(text));
	TransferManifest parsed;
	EXPECT_TRUE(TransferManifest::parse(text, parsed));
	EXPECT_EQ(manifest.entries()[0].path, parsed.entries()[0].path);

	barrier::fs::remove_all(root);
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

TEST(TransferSessionTests, keepsVerifiedOffsetWhenTheManifestMatches)
{
	const barrier::fs::path root = barrier::fs::temp_directory_path() / "capybarrier-transfer-resume";
	barrier::fs::remove_all(root);
	TransferManifest manifest;
	ASSERT_TRUE(TransferManifest::parse(
		"F 8:note.txt 4 64:88d4266fd4e6338d13b845fcf289579d209c897823b9217da3e161936f031589\n",
		manifest));

	TransferSession session;
	ASSERT_TRUE(session.accept(manifest, root.string()));
	ASSERT_TRUE(session.writeChunk(0, 0, "ab"));
	EXPECT_TRUE(session.resumes(manifest));
	EXPECT_EQ(2u, session.verifiedOffsets()[0].offset);

	barrier::fs::remove_all(root);
}

TEST(TransferSessionTests, rejectsResumeWhenTheManifestDiffers)
{
	const barrier::fs::path root = barrier::fs::temp_directory_path() / "capybarrier-transfer-resume-mismatch";
	barrier::fs::remove_all(root);
	TransferManifest expected;
	TransferManifest changed;
	ASSERT_TRUE(TransferManifest::parse(
		"F 8:note.txt 4 64:88d4266fd4e6338d13b845fcf289579d209c897823b9217da3e161936f031589\n",
		expected));
	ASSERT_TRUE(TransferManifest::parse(
		"F 8:note.txt 4 64:0000000000000000000000000000000000000000000000000000000000000000\n",
		changed));

	TransferSession session;
	ASSERT_TRUE(session.accept(expected, root.string()));
	EXPECT_FALSE(session.resumes(changed));

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

TEST(TransferSessionTests, removesPartialFilesWhenCancelled)
{
	const barrier::fs::path root = barrier::fs::temp_directory_path() / "capybarrier-transfer-cancel";
	barrier::fs::remove_all(root);
	TransferManifest manifest;
	ASSERT_TRUE(TransferManifest::parse(
		"F 8:note.txt 4 64:88d4266fd4e6338d13b845fcf289579d209c897823b9217da3e161936f031589\n",
		manifest));

	TransferSession session;
	ASSERT_TRUE(session.accept(manifest, root.string()));
	ASSERT_TRUE(session.writeChunk(0, 0, "ab"));
	session.cancel();
	EXPECT_FALSE(barrier::fs::exists(root));
}

TEST(TransferSessionTests, copiesFinalizedFileToDropDirectory)
{
	const barrier::fs::path root = barrier::fs::temp_directory_path() / "capybarrier-transfer-drop-source";
	const barrier::fs::path drop = barrier::fs::temp_directory_path() / "capybarrier-transfer-drop-target";
	barrier::fs::remove_all(root);
	barrier::fs::remove_all(drop);
	TransferManifest manifest;
	ASSERT_TRUE(TransferManifest::parse(
		"F 8:note.txt 4 64:88d4266fd4e6338d13b845fcf289579d209c897823b9217da3e161936f031589\n",
		manifest));

	TransferSession session;
	ASSERT_TRUE(session.accept(manifest, root.string()));
	ASSERT_TRUE(session.writeChunk(0, 0, "abcd"));
	ASSERT_TRUE(session.finalize());
	EXPECT_TRUE(session.copyTo(drop.string()));
	EXPECT_TRUE(barrier::fs::exists(drop / "note.txt"));

	barrier::fs::remove_all(root);
	barrier::fs::remove_all(drop);
}

class TransferDeliveryTests : public testing::Test {
protected:
	void SetUp() override
	{
		root = barrier::fs::temp_directory_path() / "capybarrier-delivery-tests";
		barrier::fs::remove_all(root);
		barrier::fs::create_directories(root);
		source = root / "source";
		drop = root / "drop";
		ASSERT_TRUE(TransferManifest::parse(
			"F 8:note.txt 4 64:88d4266fd4e6338d13b845fcf289579d209c897823b9217da3e161936f031589\n", manifest));
	}

	void TearDown() override { barrier::fs::remove_all(root); }

	std::string read(const barrier::fs::path& path)
	{
		std::ifstream file;
		barrier::open_utf8_path(file, path, std::ios::binary | std::ios::in);
		return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
	}

	barrier::fs::path root, source, drop;
	TransferManifest manifest;
	TransferSession session;
};

TEST_F(TransferDeliveryTests, doesNotExposeFileBeforeHashVerification)
{
	ASSERT_TRUE(session.accept(manifest, source.string()));
	ASSERT_TRUE(session.writeChunk(0, 0, "abcd"));
	EXPECT_FALSE(session.copyTo(drop.string()));
	EXPECT_FALSE(barrier::fs::exists(drop));
}

TEST_F(TransferDeliveryTests, preservesExistingDestinationFile)
{
	ASSERT_TRUE(session.accept(manifest, source.string()));
	ASSERT_TRUE(session.writeChunk(0, 0, "abcd"));
	ASSERT_TRUE(session.finalize());
	barrier::fs::create_directories(drop);
	std::ofstream existing;
	barrier::open_utf8_path(existing, drop / "note.txt", std::ios::binary | std::ios::out);
	existing << "keep me";
	existing.close();
	EXPECT_FALSE(session.copyTo(drop.string()));
	EXPECT_EQ("keep me", read(drop / "note.txt"));
}

TEST_F(TransferDeliveryTests, reportsDestinationIoErrorWithoutThrowing)
{
	ASSERT_TRUE(session.accept(manifest, source.string()));
	ASSERT_TRUE(session.writeChunk(0, 0, "abcd"));
	ASSERT_TRUE(session.finalize());
	std::ofstream blocked;
	barrier::open_utf8_path(blocked, drop, std::ios::binary | std::ios::out);
	blocked << "keep me";
	blocked.close();
	bool result = true;
	EXPECT_NO_THROW(result = session.copyTo(drop.string()));
	EXPECT_FALSE(result);
	EXPECT_EQ("keep me", read(drop));
}

TEST_F(TransferDeliveryTests, preservesEmptyFilesAndDirectories)
{
	ASSERT_TRUE(TransferManifest::parse(
		"D 6:folder 0 0:\nD 12:folder/empty 0 0:\n"
		"F 15:folder/zero.txt 0 64:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855\n", manifest));
	ASSERT_TRUE(session.accept(manifest, source.string()));
	ASSERT_TRUE(session.finalize());
	EXPECT_TRUE(session.copyTo(drop.string()));
	EXPECT_TRUE(barrier::fs::is_directory(drop / "folder/empty"));
	EXPECT_TRUE(barrier::fs::is_regular_file(drop / "folder/zero.txt"));
}

TEST_F(TransferDeliveryTests, cancelledSessionCannotRecreatePartialFiles)
{
	ASSERT_TRUE(session.accept(manifest, source.string()));
	ASSERT_TRUE(session.writeChunk(0, 0, "ab"));
	session.cancel();
	EXPECT_FALSE(session.writeChunk(0, 2, "cd"));
	EXPECT_FALSE(session.finalize());
	EXPECT_FALSE(barrier::fs::exists(source));
}

TEST_F(TransferDeliveryTests, refusesChunksLargerThanOneMiB)
{
	ASSERT_TRUE(TransferManifest::parse(
		"F 8:note.txt 1048577 64:0000000000000000000000000000000000000000000000000000000000000000\n", manifest));
	ASSERT_TRUE(session.accept(manifest, source.string()));
	EXPECT_FALSE(session.writeChunk(0, 0, std::string(1048577, 'a')));
	EXPECT_EQ(0u, session.verifiedOffsets()[0].offset);
}

TEST_F(TransferDeliveryTests, refusesExistingReceiveRootWithoutModifyingIt)
{
	barrier::fs::create_directories(source);
	std::ofstream existing;
	barrier::open_utf8_path(existing, source / "note.txt", std::ios::binary | std::ios::out);
	existing << "keep me";
	existing.close();
	EXPECT_FALSE(session.accept(manifest, source.string()));
	session.cancel();
	EXPECT_EQ("keep me", read(source / "note.txt"));
}

TEST_F(TransferDeliveryTests, finalizedFileCannotBeModifiedByFurtherChunks)
{
	ASSERT_TRUE(session.accept(manifest, source.string()));
	ASSERT_TRUE(session.writeChunk(0, 0, "abcd"));
	ASSERT_TRUE(session.finalize());
	EXPECT_FALSE(session.writeChunk(0, 4, ""));
}

#if SYSAPI_UNIX
TEST_F(TransferDeliveryTests, refusesSymlinkDestinationWithoutWritingOutsideIt)
{
	ASSERT_TRUE(session.accept(manifest, source.string()));
	ASSERT_TRUE(session.writeChunk(0, 0, "abcd"));
	ASSERT_TRUE(session.finalize());
	const auto outside = root / "outside";
	barrier::fs::create_directory(outside);
	barrier::fs::create_directory_symlink(outside, drop);
	EXPECT_FALSE(session.copyTo(drop.string()));
	EXPECT_FALSE(barrier::fs::exists(outside / "note.txt"));
}

TEST_F(TransferDeliveryTests, refusesDanglingDestinationSymlink)
{
	ASSERT_TRUE(session.accept(manifest, source.string()));
	ASSERT_TRUE(session.writeChunk(0, 0, "abcd"));
	ASSERT_TRUE(session.finalize());
	barrier::fs::create_directory(drop);
	const auto outside = root / "outside.txt";
	barrier::fs::create_symlink(outside, drop / "note.txt");
	EXPECT_FALSE(session.copyTo(drop.string()));
	EXPECT_FALSE(barrier::fs::exists(outside));
}
#endif
