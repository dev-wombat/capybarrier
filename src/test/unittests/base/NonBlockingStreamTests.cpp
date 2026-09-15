/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026
 */

#include "base/NonBlockingStream.h"
#include "test/global/gtest.h"

#if !defined(_WIN32)
#include <unistd.h>

TEST(NonBlockingStreamTests, tryReadChar_closedPipeReturnsFalse)
{
    int descriptors[2];
    ASSERT_EQ(0, pipe(descriptors));
    close(descriptors[1]);

    {
        NonBlockingStream stream(descriptors[0]);
        char ch;
        EXPECT_FALSE(stream.try_read_char(ch));
    }

    close(descriptors[0]);
}
#endif
