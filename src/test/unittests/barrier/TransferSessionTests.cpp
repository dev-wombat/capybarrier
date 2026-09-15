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
#include "io/filesystem.h"

#include "test/global/gtest.h"

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
