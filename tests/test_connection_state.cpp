#include "test_framework.hpp"
#include "app/connection_state.hpp"
#include "common/os/unique_handle.hpp"

using namespace hub::app;

namespace {

/// A probe that must not be consulted; the test fails if the decision reads it.
struct MustNotProbe {
    bool operator()() const { throw std::runtime_error("probe consulted"); }
};

struct Probe {
    bool value;
    int* calls;
    bool operator()() const { ++*calls; return value; }
};

ConnectionObservation running(bool pipe_connected, bool unload_requested = false) {
    ConnectionObservation seen{};
    seen.process_alive = true;
    seen.handle_opened = true;
    seen.pipe_connected = pipe_connected;
    seen.unload_requested = unload_requested;
    return seen;
}

ConnectionDecision decide(ConnectionState current, const ConnectionObservation& seen,
                          bool window_ready, bool payload_loaded) {
    return decide_connection(current, seen, [=] { return window_ready; }, [=] { return payload_loaded; });
}

constexpr ConnectionDecision stay(ConnectionState state) { return { state, ConnectionAction::None }; }

} // namespace

TEST_CASE(Connection, NoProcessWaitsFromEveryState) {
    for (auto state : { ConnectionState::WaitingForGame, ConnectionState::Injecting,
                        ConnectionState::InjectedWaitingPipe, ConnectionState::Connected,
                        ConnectionState::Reconnecting, ConnectionState::Unloaded }) {
        const auto d = decide_connection(state, ConnectionObservation{}, MustNotProbe{}, MustNotProbe{});
        TEST_ASSERT(d == stay(ConnectionState::WaitingForGame));
    }
}

TEST_CASE(Connection, UnopenedHandleWaitsWithoutInjecting) {
    ConnectionObservation seen = running(false);
    seen.handle_opened = false;
    const auto d = decide_connection(ConnectionState::WaitingForGame, seen, MustNotProbe{}, MustNotProbe{});
    TEST_ASSERT(d == stay(ConnectionState::WaitingForGame));
}

TEST_CASE(Connection, WaitsForTheGameWindowBeforeInjecting) {
    int module_probes = 0;
    const auto d = decide_connection(ConnectionState::WaitingForGame, running(false),
                                     [] { return false; }, Probe{ false, &module_probes });
    TEST_ASSERT(d == stay(ConnectionState::WaitingForGame));
    TEST_ASSERT_EQ(module_probes, 0);
}

TEST_CASE(Connection, InjectsOnceTheWindowIsReady) {
    const auto d = decide(ConnectionState::WaitingForGame, running(false), true, false);
    TEST_ASSERT(d == (ConnectionDecision{ ConnectionState::Injecting, ConnectionAction::Inject }));
    TEST_ASSERT(after_injection(true) == ConnectionState::InjectedWaitingPipe);
    TEST_ASSERT(after_injection(false) == ConnectionState::WaitingForGame);
}

TEST_CASE(Connection, AdoptsAResidentPayloadInsteadOfInjecting) {
    const auto d = decide(ConnectionState::WaitingForGame, running(false), true, true);
    TEST_ASSERT(d == (ConnectionDecision{ ConnectionState::InjectedWaitingPipe, ConnectionAction::AdoptResident }));
}

TEST_CASE(Connection, WaitsForThePipeAfterInjecting) {
    const auto d = decide_connection(ConnectionState::InjectedWaitingPipe, running(false),
                                     MustNotProbe{}, MustNotProbe{});
    TEST_ASSERT(d == stay(ConnectionState::InjectedWaitingPipe));
}

TEST_CASE(Connection, PipeUpIsConnectedWithoutProbing) {
    for (auto state : { ConnectionState::WaitingForGame, ConnectionState::InjectedWaitingPipe,
                        ConnectionState::Connected, ConnectionState::Reconnecting }) {
        const auto d = decide_connection(state, running(true), MustNotProbe{}, MustNotProbe{});
        TEST_ASSERT(d == stay(ConnectionState::Connected));
    }
}

TEST_CASE(Connection, APipeDropWithoutUnloadReconnects) {
    // The pipe dropping while the game runs used to leave the state on Connected.
    auto d = decide_connection(ConnectionState::Connected, running(false), MustNotProbe{}, MustNotProbe{});
    TEST_ASSERT(d == stay(ConnectionState::Reconnecting));
    d = decide_connection(ConnectionState::Reconnecting, running(false), MustNotProbe{}, MustNotProbe{});
    TEST_ASSERT(d == stay(ConnectionState::Reconnecting));
    d = decide_connection(ConnectionState::Reconnecting, running(true), MustNotProbe{}, MustNotProbe{});
    TEST_ASSERT(d == stay(ConnectionState::Connected));
}

TEST_CASE(Connection, StaysConnectedUntilTheUnloadDropsThePipe) {
    const auto d = decide_connection(ConnectionState::Connected, running(true, true),
                                     MustNotProbe{}, MustNotProbe{});
    TEST_ASSERT(d == stay(ConnectionState::Connected));
}

TEST_CASE(Connection, AnUnloadThenAPipeDropIsUnloaded) {
    auto d = decide_connection(ConnectionState::Connected, running(false, true), MustNotProbe{}, MustNotProbe{});
    TEST_ASSERT(d == stay(ConnectionState::Unloaded));
    // The DLL is still mapped: it must never be re-injected or adopted.
    d = decide_connection(ConnectionState::Unloaded, running(false, true), MustNotProbe{}, MustNotProbe{});
    TEST_ASSERT(d == stay(ConnectionState::Unloaded));
    ConnectionObservation no_handle = running(false, true);
    no_handle.handle_opened = false;
    d = decide_connection(ConnectionState::Unloaded, no_handle, MustNotProbe{}, MustNotProbe{});
    TEST_ASSERT(d == stay(ConnectionState::Unloaded));
}

TEST_CASE(Connection, AGameRestartAfterAnUnloadAttachesAgain) {
    // The app keys the unload to the old PID, so the new process arrives without it.
    auto d = decide_connection(ConnectionState::Unloaded, ConnectionObservation{}, MustNotProbe{}, MustNotProbe{});
    TEST_ASSERT(d == stay(ConnectionState::WaitingForGame));
    d = decide(ConnectionState::WaitingForGame, running(false), true, false);
    TEST_ASSERT(d == (ConnectionDecision{ ConnectionState::Injecting, ConnectionAction::Inject }));
    // Even straight from Unloaded, should a tick miss the gap between processes.
    d = decide(ConnectionState::Unloaded, running(false), true, false);
    TEST_ASSERT(d == (ConnectionDecision{ ConnectionState::Injecting, ConnectionAction::Inject }));
}

TEST_CASE(Connection, NotificationsFollowTheState) {
    const auto attached = connection_notification(ConnectionState::InjectedWaitingPipe,
                                                  ConnectionState::Connected, 1234);
    TEST_ASSERT(attached.has_value());
    TEST_ASSERT(attached->find("PID 1234") != std::string::npos);

    const auto unloaded = connection_notification(ConnectionState::Connected, ConnectionState::Unloaded, 1);
    TEST_ASSERT(unloaded.has_value());
    TEST_ASSERT(unloaded->find("Restart the game") != std::string::npos);

    const auto closed = connection_notification(ConnectionState::Connected, ConnectionState::WaitingForGame, 0);
    TEST_ASSERT(closed.has_value());
    TEST_ASSERT(closed->find("closed") != std::string::npos);

    // An unload is not the game closing, and a reconnect is silent both ways.
    TEST_ASSERT(unloaded->find("closed") == std::string::npos);
    TEST_ASSERT(!connection_notification(ConnectionState::Connected, ConnectionState::Reconnecting, 1));
    TEST_ASSERT(!connection_notification(ConnectionState::Reconnecting, ConnectionState::Connected, 1));
    TEST_ASSERT(!connection_notification(ConnectionState::Connected, ConnectionState::Connected, 1));
    TEST_ASSERT(!connection_notification(ConnectionState::WaitingForGame, ConnectionState::InjectedWaitingPipe, 1));
}

namespace {

struct CountingTraits {
    using pointer = int;
    static inline int closed = 0;
    static inline int last_closed = 0;
    static pointer empty() noexcept { return 0; }
    static bool is_valid(pointer h) noexcept { return h > 0; }
    static void close(pointer h) noexcept { ++closed; last_closed = h; }
};

using CountingHandle = hub::os::BasicUniqueHandle<CountingTraits>;

} // namespace

TEST_CASE(OS, UniqueHandleClosesExactlyOnce) {
    CountingTraits::closed = 0;
    {
        CountingHandle a(7);
        TEST_ASSERT(static_cast<bool>(a));
        CountingHandle b(std::move(a));
        TEST_ASSERT(!a);
        TEST_ASSERT_EQ(b.get(), 7);
        CountingHandle c;
        c = std::move(b);
        TEST_ASSERT_EQ(CountingTraits::closed, 0);
    }
    TEST_ASSERT_EQ(CountingTraits::closed, 1);
    TEST_ASSERT_EQ(CountingTraits::last_closed, 7);

    {
        CountingHandle d(3);
        d.reset(4);
        TEST_ASSERT_EQ(CountingTraits::closed, 2);
        TEST_ASSERT_EQ(CountingTraits::last_closed, 3);
        const int raw = d.release();
        TEST_ASSERT_EQ(raw, 4);
    }
    TEST_ASSERT_EQ(CountingTraits::closed, 2);

    {
        CountingHandle invalid(-1);
        TEST_ASSERT(!invalid);
    }
    TEST_ASSERT_EQ(CountingTraits::closed, 2);

    // The Win32 traits treat both nullptr and INVALID_HANDLE_VALUE as empty.
    TEST_ASSERT(!hub::os::UniqueHandle());
    TEST_ASSERT(!hub::os::Win32HandleTraits::is_valid(
        reinterpret_cast<void*>(static_cast<std::intptr_t>(-1))));
}
