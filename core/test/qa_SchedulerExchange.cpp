#include <boost/ut.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <format>
#include <functional>
#include <optional>
#include <span>
#include <thread>

#include <gnuradio-4.0/Graph.hpp>
#include <gnuradio-4.0/Message.hpp>
#include <gnuradio-4.0/Scheduler.hpp>

namespace qa_exchange {

std::atomic<std::size_t> gFirstGraphSamples{0UZ};
std::atomic<std::size_t> gSecondGraphSamples{0UZ};

// reacts to a message on its own msgIn, i.e. on the scheduler worker that also runs
// processScheduledMessages(): exchange() must not self-deadlock when called from that thread
struct SwapRequester : gr::Block<SwapRequester> {
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(SwapRequester, out);

    std::function<void()> _onSwapRequest;
    std::function<void()> _onReset;
    std::atomic<bool>*    _swapReturned = nullptr;

    [[nodiscard]] constexpr float processOne() const noexcept { return 1.0f; }

    void reset() {
        if (_onReset) {
            _onReset();
        }
    }

    void processMessages(const gr::MsgPortInBuiltin&, std::span<const gr::Message> messages) {
        for (const gr::Message& message : messages) {
            if (message.endpoint != "swapGraph" || !_onSwapRequest) {
                continue;
            }
            _onSwapRequest();
            if (_swapReturned != nullptr) {
                _swapReturned->store(true);
                _swapReturned->notify_all();
            }
        }
    }
};

template<std::atomic<std::size_t>* counter>
struct CountingSink : gr::Block<CountingSink<counter>> {
    gr::PortIn<float> in;

    GR_MAKE_REFLECTABLE(CountingSink, in);

    void processOne(float) { counter->fetch_add(1UZ, std::memory_order_relaxed); }
};

using FirstSink     = CountingSink<&gFirstGraphSamples>;
using SecondSink    = CountingSink<&gSecondGraphSamples>;
using TestScheduler = gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::multiThreaded>;

// emits for 200 ms after its first work() call and then returns DONE
struct TimedSource : gr::Block<TimedSource> {
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(TimedSource, out);

    std::chrono::steady_clock::time_point _endAt{};

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        const auto now = std::chrono::steady_clock::now();
        if (_endAt == std::chrono::steady_clock::time_point{}) {
            _endAt = now + std::chrono::milliseconds(200);
        }
        if (now >= _endAt) {
            outSpan.publish(0UZ);
            return gr::work::Status::DONE;
        }
        outSpan.publish(std::min(outSpan.size(), 8UZ));
        return gr::work::Status::OK;
    }
};

// reads the flag that the destructor clears first
struct DestructionProbe : TestScheduler {
    using TestScheduler::TestScheduler;

    [[nodiscard]] bool valid() { return gr::atomic_ref(this->_valid).load_acquire(); }
};

[[nodiscard]] gr::Graph makeSecondGraph() {
    using namespace boost::ut;

    gr::Graph flow;
    auto&     source = flow.emplaceBlock<SwapRequester>();
    auto&     sink   = flow.emplaceBlock<SecondSink>();
    expect(flow.connect<"out", "in">(source, sink).has_value());
    return flow;
}

[[nodiscard]] gr::Graph makeTimedGraph() {
    using namespace boost::ut;

    gr::Graph flow;
    auto&     source = flow.emplaceBlock<TimedSource>();
    auto&     sink   = flow.emplaceBlock<SecondSink>();
    expect(flow.connect<"out", "in">(source, sink).has_value());
    return flow;
}

[[nodiscard]] bool awaitFlag(const std::atomic<bool>& flag) {
    for (std::size_t i = 0UZ; i < 3000UZ && !flag.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return flag.load();
}

[[nodiscard]] bool awaitState(const TestScheduler& scheduler, gr::lifecycle::State expected) {
    for (std::size_t i = 0UZ; i < 3000UZ && scheduler.state() != expected; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return scheduler.state() == expected;
}

[[nodiscard]] bool awaitCount(const std::atomic<std::size_t>& counter, std::size_t atLeast) {
    for (std::size_t i = 0UZ; i < 3000UZ && counter.load(std::memory_order_relaxed) < atLeast; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return counter.load(std::memory_order_relaxed) >= atLeast;
}

void requestSwap(SwapRequester& block) {
    gr::Message request;
    request.cmd      = gr::message::Command::Set;
    request.endpoint = "swapGraph";
    request.data     = gr::property_map{};

    auto writer = block.msgIn.buffer().streamBuffer.new_writer();
    auto span   = writer.tryReserve<gr::SpanReleasePolicy::ProcessAll>(1UZ);
    boost::ut::expect(!span.empty()) << "could not queue the swap request";
    span[0] = std::move(request);
    span.publish(1UZ);
}

} // namespace qa_exchange

const boost::ut::suite<"scheduler graph exchange"> schedulerExchangeTests = [] {
    using namespace boost::ut;
    using enum gr::lifecycle::State;

    "a graph swap requested from a scheduler worker completes"_test = [] {
        qa_exchange::gFirstGraphSamples  = 0UZ;
        qa_exchange::gSecondGraphSamples = 0UZ;

        std::atomic<bool>           swapReturned{false};
        qa_exchange::TestScheduler  scheduler;
        qa_exchange::SwapRequester* requester = nullptr;

        {
            gr::Graph flow;
            auto&     source = flow.emplaceBlock<qa_exchange::SwapRequester>();
            auto&     sink   = flow.emplaceBlock<qa_exchange::FirstSink>();
            expect(flow.connect<"out", "in">(source, sink).has_value());

            source._swapReturned  = &swapReturned;
            source._onSwapRequest = [&scheduler] { std::ignore = scheduler.exchange(qa_exchange::makeSecondGraph()); };
            requester             = &source; // the wrapper holding it lives on the heap, so this survives the move

            expect(scheduler.exchange(std::move(flow)).has_value());
        }

        expect(scheduler.changeStateTo(INITIALISED).has_value());
        expect(scheduler.changeStateTo(RUNNING).has_value());
        expect(qa_exchange::awaitCount(qa_exchange::gFirstGraphSamples, 1UZ)) << "first graph never ran";

        qa_exchange::requestSwap(*requester);

        expect(qa_exchange::awaitFlag(swapReturned)) << "exchange() never returned from the worker thread";
        expect(qa_exchange::awaitState(scheduler, RUNNING)) << "the deferred swap never restarted the scheduler";
        expect(qa_exchange::awaitCount(qa_exchange::gSecondGraphSamples, 1UZ)) << "the exchanged-in graph never processed samples";

        expect(scheduler.changeStateTo(REQUESTED_STOP).has_value());
        expect(qa_exchange::awaitState(scheduler, STOPPED)) << "scheduler did not settle after the swap";
    };

    "a restarted scheduler runs the graph it was given last"_test = [] {
        qa_exchange::gFirstGraphSamples  = 0UZ;
        qa_exchange::gSecondGraphSamples = 0UZ;

        qa_exchange::TestScheduler scheduler;

        gr::Graph first;
        auto&     firstSource = first.emplaceBlock<qa_exchange::SwapRequester>();
        auto&     firstSink   = first.emplaceBlock<qa_exchange::FirstSink>();
        expect(first.connect<"out", "in">(firstSource, firstSink).has_value());

        expect(scheduler.exchange(std::move(first)).has_value());
        expect(scheduler.changeStateTo(INITIALISED).has_value());
        expect(scheduler.changeStateTo(RUNNING).has_value());
        expect(qa_exchange::awaitCount(qa_exchange::gFirstGraphSamples, 1UZ)) << "first graph never ran";

        expect(scheduler.changeStateTo(REQUESTED_STOP).has_value());
        expect(qa_exchange::awaitState(scheduler, STOPPED)) << "first graph did not stop";

        expect(scheduler.exchange(qa_exchange::makeSecondGraph()).has_value());
        expect(scheduler.changeStateTo(INITIALISED).has_value());
        expect(scheduler.changeStateTo(RUNNING).has_value());
        expect(qa_exchange::awaitCount(qa_exchange::gSecondGraphSamples, 1UZ)) << "the second graph's blocks never ran";

        expect(scheduler.changeStateTo(REQUESTED_STOP).has_value());
        expect(qa_exchange::awaitState(scheduler, STOPPED)) << "second graph did not stop";
    };

    "a destruction retires the watchdog of a run that a deferred swap restarts"_test = [] {
        // timed on the wall clock. The swap passes its check of _valid, and the old graph's reset() hook then holds it
        // until the destruction has begun and has passed its check of the state. The restart spawns a watchdog with a
        // 2 s period while the destructor waits for the swap. The bound holds only when the destructor retires that
        // watchdog and stops the restarted run.
        constexpr auto kBound = std::chrono::milliseconds(1000);

        qa_exchange::gFirstGraphSamples  = 0UZ;
        qa_exchange::gSecondGraphSamples = 0UZ;

        std::atomic<bool>                            resetEntered{false};
        std::optional<qa_exchange::DestructionProbe> scheduler;
        scheduler.emplace(gr::property_map{{"watchdog_timeout", gr::Size_t(2'000)}});
        qa_exchange::DestructionProbe* probe     = &*scheduler;
        qa_exchange::SwapRequester*    requester = nullptr;

        {
            gr::Graph flow;
            auto&     source = flow.emplaceBlock<qa_exchange::SwapRequester>();
            auto&     sink   = flow.emplaceBlock<qa_exchange::FirstSink>();
            expect(flow.connect<"out", "in">(source, sink).has_value());

            source._onSwapRequest = [probe] { std::ignore = probe->exchange(qa_exchange::makeTimedGraph()); };
            source._onReset       = [probe, &resetEntered] {
                if (resetEntered.exchange(true)) {
                    return;
                }
                for (std::size_t i = 0UZ; i < 3000UZ && probe->valid(); ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20)); // the destructor reads the state meanwhile
            };
            requester = &source;

            expect(scheduler->exchange(std::move(flow)).has_value());
        }

        expect(scheduler->changeStateTo(INITIALISED).has_value());
        expect(scheduler->changeStateTo(RUNNING).has_value());
        expect(qa_exchange::awaitCount(qa_exchange::gFirstGraphSamples, 1UZ)) << "first graph never ran";

        qa_exchange::requestSwap(*requester);
        expect(qa_exchange::awaitFlag(resetEntered)) << "the deferred swap never reset the old graph";

        const auto destructionStart = std::chrono::steady_clock::now();
        scheduler.reset();
        const auto destructionTime = std::chrono::steady_clock::now() - destructionStart;

        expect(destructionTime < kBound) << std::format("the destruction took {}", std::chrono::duration_cast<std::chrono::milliseconds>(destructionTime));
    };
};

int main() { /* tests are statically registered */ }
