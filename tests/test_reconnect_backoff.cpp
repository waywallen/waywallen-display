#include <algorithm>
#include <cassert>
#include <cstdio>

static constexpr int kInitialMs = 2000;
static constexpr int kMaxMs     = 30000;

static int advance(int& delay) {
    const int d = delay;
    delay       = std::min(d * 2, kMaxMs);
    return d;
}

static void test_delay_doubles_to_cap() {
    int d = kInitialMs;
    assert(advance(d) == 2000);
    assert(advance(d) == 4000);
    assert(advance(d) == 8000);
    assert(advance(d) == 16000);
    assert(advance(d) == 30000);
    assert(advance(d) == 30000);
}

static void test_reset_restores_initial() {
    int d = kInitialMs;
    advance(d);
    advance(d);
    d = kInitialMs;
    assert(advance(d) == 2000);
}

static void test_cap_is_never_exceeded() {
    int d = kInitialMs;
    for (int i = 0; i < 20; ++i) assert(advance(d) <= kMaxMs);
}

int main() {
    test_delay_doubles_to_cap();
    test_reset_restores_initial();
    test_cap_is_never_exceeded();
    std::puts("test_reconnect_backoff: OK");
    return 0;
}
