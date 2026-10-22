#include "arch/Arch.h"
#include "test/global/gtest.h"

#if SYSAPI_UNIX
#include <chrono>
#include <future>

TEST(ArchNetworkTests, wakeBeforeFirstPollIsDeliveredToTheTargetThread)
{
    auto socket = ARCH->newSocket(IArchNetwork::kINET, IArchNetwork::kSTREAM);
    auto address = ARCH->newAnyAddr(IArchNetwork::kINET);
    ARCH->bindSocket(socket, address);
    ARCH->closeAddr(address);
    ARCH->listenOnSocket(socket);
    IArchNetwork::PollEntry entry{socket, IArchNetwork::kPOLLIN, 0};
    std::promise<void> start;
    auto ready = start.get_future();
    std::promise<int> result;
    auto completed = result.get_future();
    auto worker = ARCH->newThread([&] {
        ready.wait();
        try { result.set_value(ARCH->pollSocket(&entry, 1, 5.0)); }
        catch (...) { result.set_exception(std::current_exception()); }
    });

    ARCH->unblockPollSocket(worker);
    start.set_value();
    const bool woke = completed.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    if (!woke) ARCH->unblockPollSocket(worker);
    const bool exited = ARCH->wait(worker, 6.0);
    ARCH->closeThread(worker);
    ARCH->closeSocket(socket);
    EXPECT_TRUE(exited);
    EXPECT_TRUE(woke);
    if (exited) EXPECT_EQ(0, completed.get());
}
#endif
