#include "recovery/cancellation.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>

namespace recovery {
namespace {

TEST(CancellationTest, DefaultTokenIsNeverCancelled) {
    const CancellationToken token;
    EXPECT_FALSE(token.canBeCancelled());
    EXPECT_FALSE(token.isCancellationRequested());
}

TEST(CancellationTest, SourceCancelsItsTokens) {
    CancellationSource source;
    const CancellationToken first = source.token();
    const CancellationToken second = source.token();
    EXPECT_TRUE(first.canBeCancelled());
    EXPECT_FALSE(first.isCancellationRequested());

    source.requestCancellation();

    EXPECT_TRUE(source.isCancellationRequested());
    EXPECT_TRUE(first.isCancellationRequested());
    EXPECT_TRUE(second.isCancellationRequested());
}

TEST(CancellationTest, CancellationIsIdempotent) {
    CancellationSource source;
    source.requestCancellation();
    source.requestCancellation();
    EXPECT_TRUE(source.token().isCancellationRequested());
}

TEST(CancellationTest, IndependentSourcesDoNotInterfere) {
    CancellationSource a;
    const CancellationSource b;
    a.requestCancellation();
    EXPECT_FALSE(b.token().isCancellationRequested());
}

TEST(CancellationTest, TokenOutlivesSource) {
    CancellationToken token;
    {
        CancellationSource source;
        token = source.token();
        source.requestCancellation();
    }
    EXPECT_TRUE(token.isCancellationRequested());
}

TEST(CancellationTest, CancellationIsVisibleAcrossThreads) {
    CancellationSource source;
    const CancellationToken token = source.token();
    std::atomic<bool> observed{false};

    std::thread worker([&] {
        while (!token.isCancellationRequested()) {
            std::this_thread::yield();
        }
        observed.store(true);
    });
    source.requestCancellation();
    worker.join();

    EXPECT_TRUE(observed.load());
}

}  // namespace
}  // namespace recovery
