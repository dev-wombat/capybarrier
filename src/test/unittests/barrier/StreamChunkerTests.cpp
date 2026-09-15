/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026
 */

#include "barrier/StreamChunker.h"
#include "test/global/gtest.h"

TEST(StreamChunkerTests, sendsChunksOnlyAfterTransferIsAccepted)
{
    StreamChunker::beginTransfer(42);
    StreamChunker::acceptTransfer(42, true);
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
