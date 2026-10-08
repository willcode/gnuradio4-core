#include <boost/ut.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstddef>
#include <expected>
#include <format>
#include <functional>
#include <limits>
#include <map>
#include <memory_resource>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <gnuradio-4.0/BlockRegistry.hpp>
#include <gnuradio-4.0/CircularBuffer.hpp>
#include <gnuradio-4.0/ComputeDomain.hpp>
#include <gnuradio-4.0/Graph.hpp>
#include <gnuradio-4.0/Graph_yaml_importer.hpp>
#include <gnuradio-4.0/Message.hpp>
#include <gnuradio-4.0/Scheduler.hpp>

namespace qa_edit {

struct Tunable : gr::Block<Tunable> {
    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    gr::Annotated<float, "gain"> gain = 1.0f;

    GR_MAKE_REFLECTABLE(Tunable, in, out, gain);

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value * gain; }
};

struct Source : gr::Block<Source> {
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(Source, out);

    [[nodiscard]] constexpr float processOne() const noexcept { return 1.0f; }
};

struct CountingSource : gr::Block<CountingSource> {
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(CountingSource, out);

    static constexpr std::size_t kSamples = 4096UZ;

    std::size_t _nProduced = 0UZ;

    float processOne() {
        if (++_nProduced >= kSamples) {
            this->requestStop();
        }
        return 1.0f;
    }
};

struct Sink : gr::Block<Sink> {
    gr::PortIn<float> in;

    GR_MAKE_REFLECTABLE(Sink, in);

    std::size_t _nReceived = 0UZ;

    void processOne(float) { _nReceived++; }
};

// a source with a message output, for message edges
struct MessageSource : gr::Block<MessageSource> {
    gr::PortOut<float> out;
    gr::MsgPortOut     cmd;

    GR_MAKE_REFLECTABLE(MessageSource, out, cmd);

    [[nodiscard]] constexpr float processOne() const noexcept { return 1.0f; }
};

// a sink with a message input, for message edges
struct MessageSink : gr::Block<MessageSink> {
    gr::PortIn<float> in;
    gr::MsgPortIn     cmd;

    GR_MAKE_REFLECTABLE(MessageSink, in, cmd);

    void processOne(float) {}
};

// one connected and one deliberately unconnected optional output
struct DualSource : gr::Block<DualSource> {
    gr::PortOut<float>               out;
    gr::PortOut<float, gr::Optional> monitor;

    GR_MAKE_REFLECTABLE(DualSource, out, monitor);

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan, gr::OutputSpanLike auto& monitorSpan) {
        outSpan.publish(0UZ);
        monitorSpan.publish(0UZ);
        return gr::work::Status::DONE;
    }
};

// a source with a mandatory and an optional output, both fed with ones
struct MonitoredSource : gr::Block<MonitoredSource> {
    gr::PortOut<float>               out;
    gr::PortOut<float, gr::Optional> monitor;

    GR_MAKE_REFLECTABLE(MonitoredSource, out, monitor);

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan, gr::OutputSpanLike auto& monitorSpan) {
        std::ranges::fill(outSpan, 1.0f);
        outSpan.publish(outSpan.size());
        std::ranges::fill(monitorSpan, 1.0f);
        monitorSpan.publish(monitorSpan.size());
        return gr::work::Status::OK;
    }
};

// resolvable only through a test-local registry, never the global one, so a lookup that
// succeeds proves which loader served it
struct LoaderCanary : gr::Block<LoaderCanary> {
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(LoaderCanary, out);

    [[nodiscard]] constexpr float processOne() const noexcept { return 0.0f; }
};

using TestScheduler = gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::multiThreaded>;

// ends its stream at its first call
struct EndingSource : gr::Block<EndingSource> {
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(EndingSource, out);

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        outSpan.publish(0UZ);
        return gr::work::Status::DONE;
    }
};

constexpr std::string_view kLateFault = "the late source failed";

// reports kLateFault on its 50th call and ends its stream on its 300th
struct LateErrorSource : gr::Block<LateErrorSource> {
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(LateErrorSource, out);

    static constexpr std::size_t kCallsToError = 50UZ;
    static constexpr std::size_t kCallsToEnd   = 300UZ;

    std::size_t _nCalls = 0UZ;

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (++_nCalls == kCallsToError) {
            this->emitErrorMessage("processBulk", kLateFault);
        }
        outSpan.publish(0UZ);
        return _nCalls >= kCallsToEnd ? gr::work::Status::DONE : gr::work::Status::OK;
    }
};

inline std::atomic<std::size_t> gConstructions{0UZ};

// counts each instance it constructs in gConstructions. Its one output is optional and asynchronous
struct ConstructionCounter : gr::Block<ConstructionCounter> {
    gr::PortOut<float, gr::Async, gr::Optional> out;

    GR_MAKE_REFLECTABLE(ConstructionCounter, out);

    explicit ConstructionCounter(gr::property_map initParameters = {}) : gr::Block<ConstructionCounter>(std::move(initParameters)) { gConstructions.fetch_add(1UZ); }

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        outSpan.publish(0UZ);
        return gr::work::Status::OK;
    }
};

inline std::atomic<bool>        gHoldRequested{false};
inline std::atomic<bool>        gHoldEntered{false};
inline std::atomic<bool>        gHoldEnded{false};
inline std::atomic<std::size_t> gConstructionsDuringHold{0UZ};

// Its first work() call after gHoldRequested is set reports gHoldEntered. The call watches gConstructions for 100 ms or
// until the first change, stores the change in gConstructionsDuringHold and reports gHoldEnded
struct HoldingProbe : gr::Block<HoldingProbe> {
    gr::PortOut<float, gr::Async, gr::Optional> out;

    GR_MAKE_REFLECTABLE(HoldingProbe, out);

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        if (gHoldRequested.exchange(false)) {
            const std::size_t before = gConstructions.load();
            gHoldEntered.store(true);
            for (std::size_t i = 0UZ; i < 100UZ && gConstructions.load() == before; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            gConstructionsDuringHold.store(gConstructions.load() - before);
            gHoldEnded.store(true);
        }
        outSpan.publish(0UZ);
        return gr::work::Status::OK;
    }
};

inline std::atomic<std::size_t> gWorkingTickers{0UZ};

// counts itself once in gWorkingTickers at its first work() call. Its one output is optional and stays unconnected. The
// output is asynchronous. Each call reserves at most the free space in the port's buffer.
struct Ticker : gr::Block<Ticker> {
    gr::PortOut<float, gr::Async, gr::Optional> out;

    GR_MAKE_REFLECTABLE(Ticker, out);

    bool _counted = false;

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        if (!_counted) {
            _counted = true;
            gWorkingTickers.fetch_add(1UZ);
        }
        outSpan.publish(0UZ);
        return gr::work::Status::OK;
    }
};

// reads the job lists, the count of workers that have not left and the size of the zombie list
struct JobListProbe : TestScheduler {
    using TestScheduler::TestScheduler;

    [[nodiscard]] std::size_t nRunningJobs() const { return this->_nRunningJobs->value(); }

    [[nodiscard]] std::size_t nZombieBlocks() {
        std::lock_guard guard(this->_zombieBlocksMutex);
        return this->_zombieBlocks.size();
    }

    [[nodiscard]] std::size_t nJobLists() {
        std::lock_guard lock(this->_executionOrderMutex);
        return this->_executionOrder->size();
    }

    [[nodiscard]] bool firstJobListHolds(std::string_view uniqueName) {
        std::lock_guard lock(this->_executionOrderMutex);
        return !this->_executionOrder->empty() && std::ranges::any_of(this->_executionOrder->front(), [uniqueName](const std::shared_ptr<gr::BlockModel>& block) { return block->uniqueName() == uniqueName; });
    }

    // the index of the job list that holds the named block
    [[nodiscard]] std::optional<std::size_t> jobListOf(std::string_view uniqueName) {
        std::lock_guard lock(this->_executionOrderMutex);
        for (std::size_t i = 0UZ; i < this->_executionOrder->size(); ++i) {
            if (std::ranges::any_of((*this->_executionOrder)[i], [uniqueName](const std::shared_ptr<gr::BlockModel>& block) { return block->uniqueName() == uniqueName; })) {
                return i;
            }
        }
        return std::nullopt;
    }
};

inline std::atomic<std::size_t> gCountedSamples{0UZ};

// a Tunable that counts the samples it passes in gCountedSamples
struct CountedTunable : gr::Block<CountedTunable> {
    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(CountedTunable, in, out);

    [[nodiscard]] float processOne(float value) noexcept {
        gCountedSamples.fetch_add(1UZ, std::memory_order_relaxed);
        return value;
    }
};

inline std::atomic<std::size_t> gStagePassed{0UZ};

// passes kPassed samples, counted in gStagePassed, and then requests its own stop
struct EndingStage : gr::Block<EndingStage> {
    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(EndingStage, in, out);

    static constexpr std::size_t kPassed = 64UZ;

    [[nodiscard]] float processOne(float value) {
        if (gStagePassed.fetch_add(1UZ) + 1UZ >= kPassed) {
            this->requestStop();
        }
        return value;
    }
};

inline std::atomic<std::size_t> gHeldPublished{0UZ};
inline std::atomic<std::size_t> gHeldPassed{0UZ};
inline std::atomic<bool>        gSinkReleased{false};
inline std::atomic<std::size_t> gSinkReceived{0UZ};

// publishes kSamples samples, counted in gHeldPublished, and then nothing, without ending its stream
struct HeldSource : gr::Block<HeldSource> {
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(HeldSource, out);

    static constexpr std::size_t kSamples = 64UZ;

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        const std::size_t nPublish = std::min(outSpan.size(), kSamples - gHeldPublished.load());
        for (std::size_t i = 0UZ; i < nPublish; ++i) {
            outSpan[i] = 1.0f;
        }
        outSpan.publish(nPublish);
        gHeldPublished.fetch_add(nPublish);
        return gr::work::Status::OK;
    }
};

inline std::atomic<std::size_t> gSourcedSamples{0UZ};

// a Source that counts the samples it produces in gSourcedSamples
struct CountedSource : gr::Block<CountedSource> {
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(CountedSource, out);

    [[nodiscard]] float processOne() noexcept {
        gSourcedSamples.fetch_add(1UZ, std::memory_order_relaxed);
        return 1.0f;
    }
};

constexpr std::string_view kStopEndpoint = "Stop";

// a CountedSource that requests its own stop on a message to kStopEndpoint. Its worker handles the message between two
// work() calls. The source stops without an end-of-stream tag
struct StoppableSource : gr::Block<StoppableSource> {
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(StoppableSource, out);

    explicit StoppableSource(gr::property_map initParameters = {}) : gr::Block<StoppableSource>(std::move(initParameters)) { propertyCallbacks[std::string(kStopEndpoint)] = static_cast<gr::BlockBase::PropertyCallback>(&StoppableSource::propertyCallbackStop); }

    std::optional<gr::Message> propertyCallbackStop(std::string_view, gr::Message) {
        this->requestStop();
        return std::nullopt;
    }

    [[nodiscard]] float processOne() noexcept {
        gSourcedSamples.fetch_add(1UZ, std::memory_order_relaxed);
        return 1.0f;
    }
};

inline std::atomic<float> gLevel{0.0f};

// a source that produces its level and stores each level it takes in gLevel
struct LevelSource : gr::Block<LevelSource> {
    gr::PortOut<float> out;

    gr::Annotated<float, "level"> level = 1.0f;

    GR_MAKE_REFLECTABLE(LevelSource, out, level);

    [[nodiscard]] float processOne() const noexcept { return level; }

    void settingsChanged(const gr::property_map&, const gr::property_map&) { gLevel.store(level); }
};

// passes its first kPassed samples, counted in gHeldPassed, and keeps every later one at its input
struct HoldingStage : gr::Block<HoldingStage> {
    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(HoldingStage, in, out);

    static constexpr std::size_t kPassed = 16UZ;

    gr::work::Status processBulk(gr::InputSpanLike auto& inSpan, gr::OutputSpanLike auto& outSpan) {
        const std::size_t nPass = std::min({inSpan.size(), outSpan.size(), kPassed - gHeldPassed.load()});
        for (std::size_t i = 0UZ; i < nPass; ++i) {
            outSpan[i] = inSpan[i];
        }
        std::ignore = inSpan.consume(nPass);
        outSpan.publish(nPass);
        gHeldPassed.fetch_add(nPass);
        return gr::work::Status::OK;
    }
};

// takes no sample until gSinkReleased is set, then counts every sample it takes in gSinkReceived
struct HeldSink : gr::Block<HeldSink> {
    gr::PortIn<float> in;

    GR_MAKE_REFLECTABLE(HeldSink, in);

    gr::work::Status processBulk(gr::InputSpanLike auto& inSpan) {
        const std::size_t nTake = gSinkReleased.load() ? inSpan.size() : 0UZ;
        gSinkReceived.fetch_add(nTake);
        std::ignore = inSpan.consume(nTake);
        return gr::work::Status::OK;
    }
};

inline std::atomic<std::size_t> gFreeRunningSamples{0UZ};
inline std::atomic<std::size_t> gSamplesDuringStop{0UZ};
inline std::atomic<bool>        gStopProbed{false};

// counts every sample it takes in gFreeRunningSamples
struct FreeSink : gr::Block<FreeSink> {
    gr::PortIn<float> in;

    GR_MAKE_REFLECTABLE(FreeSink, in);

    void processOne(float) { gFreeRunningSamples.fetch_add(1UZ, std::memory_order_relaxed); }
};

constexpr std::string_view kSecondStart = "the source starts once";

inline std::atomic<std::size_t> gOneRunSourceCalls{0UZ};

// ends its stream at its first call. Its start() throws kSecondStart from the second start on, and every later call
// publishes nothing. It counts its calls in gOneRunSourceCalls
struct OneRunSource : gr::Block<OneRunSource> {
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(OneRunSource, out);

    std::size_t _nStarts = 0UZ;
    bool        _ended   = false;

    void start() {
        if (++_nStarts > 1UZ) {
            throw gr::exception(std::string(kSecondStart));
        }
    }

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        gOneRunSourceCalls.fetch_add(1UZ, std::memory_order_relaxed);
        outSpan.publish(0UZ);
        return std::exchange(_ended, true) ? gr::work::Status::OK : gr::work::Status::DONE;
    }
};

// its stop() hook watches gFreeRunningSamples for 100 ms or until the first change, and stores the change in
// gSamplesDuringStop
struct StopProbe : gr::Block<StopProbe> {
    gr::PortOut<float, gr::Async, gr::Optional> out;

    GR_MAKE_REFLECTABLE(StopProbe, out);

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        outSpan.publish(0UZ);
        return gr::work::Status::OK;
    }

    void stop() {
        const std::size_t before = gFreeRunningSamples.load();
        for (std::size_t i = 0UZ; i < 100UZ && gFreeRunningSamples.load() == before; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        gSamplesDuringStop.store(gFreeRunningSamples.load() - before);
        gStopProbed.store(true);
    }
};

// reads the state of the scheduler under test. A case sets it while its scheduler lives
inline std::function<gr::lifecycle::State()> gSchedulerState;
inline std::atomic<bool>                     gRacingEntered{false};

// a Tunable whose settingsChanged() reports that it has begun in gRacingEntered and returns once gSchedulerState reads a
// stopping scheduler. The scheduler applies the settings while it builds the block as a replacement
struct RacingStage : gr::Block<RacingStage> {
    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    gr::Annotated<float, "gain"> gain = 1.0f;

    GR_MAKE_REFLECTABLE(RacingStage, in, out, gain);

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value * gain; }

    void settingsChanged(const gr::property_map&, const gr::property_map&) {
        if (!gSchedulerState) {
            return;
        }
        gRacingEntered.store(true);
        for (std::size_t i = 0UZ; i < 3000UZ && !gr::lifecycle::isShuttingDown(gSchedulerState()); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
};

void registerTestBlocks() {
    static const bool registered = [] {
        std::ignore = gr::globalBlockRegistry().insert<Tunable>();
        std::ignore = gr::globalBlockRegistry().insert<Source>();
        std::ignore = gr::globalBlockRegistry().insert<Sink>();
        std::ignore = gr::globalBlockRegistry().insert<Ticker>();
        std::ignore = gr::globalBlockRegistry().insert<ConstructionCounter>();
        std::ignore = gr::globalBlockRegistry().insert<CountedTunable>();
        std::ignore = gr::globalBlockRegistry().insert<RacingStage>();
        return true;
    }();
    std::ignore = registered;
}

// formats a port as the edge formatter prints it; the '/' keeps one block name from matching inside a longer one
[[nodiscard]] std::string portOf(std::string_view blockName, std::string_view port) { return std::format("{}/{}", blockName, port); }

[[nodiscard]] bool awaitReply(gr::MsgPortIn& port, std::string_view endpoint) {
    for (std::size_t i = 0UZ; i < 3000UZ; ++i) {
        auto messages = port.streamReader().get();
        for (const gr::Message& message : messages) {
            if (message.endpoint == endpoint) {
                return true;
            }
        }
        std::ignore = messages.consume(messages.size());
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

[[nodiscard]] std::optional<gr::property_map> awaitReplyData(gr::MsgPortIn& port, std::string_view endpoint) {
    for (std::size_t i = 0UZ; i < 3000UZ; ++i) {
        auto messages = port.streamReader().get();
        for (const gr::Message& message : messages) {
            if (message.endpoint == endpoint && message.data.has_value()) {
                return message.data.value();
            }
        }
        std::ignore = messages.consume(messages.size());
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return std::nullopt;
}

[[nodiscard]] std::string awaitError(gr::MsgPortIn& port, std::string_view endpoint) {
    for (std::size_t i = 0UZ; i < 3000UZ; ++i) {
        auto messages = port.streamReader().get();
        for (const gr::Message& message : messages) {
            if (message.endpoint == endpoint && !message.data.has_value()) {
                return message.data.error().message;
            }
        }
        std::ignore = messages.consume(messages.size());
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return {};
}

// consumes every message it reads, so a later call sees only a later reply
[[nodiscard]] bool takeReply(gr::MsgPortIn& port, std::string_view endpoint) {
    for (std::size_t i = 0UZ; i < 3000UZ; ++i) {
        auto       messages = port.streamReader().get();
        const bool found    = std::ranges::any_of(messages, [endpoint](const gr::Message& message) { return message.endpoint == endpoint; });
        std::ignore         = messages.consume(messages.size());
        if (found) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

template<typename TPredicate>
[[nodiscard]] bool awaitCondition(TPredicate satisfied) {
    for (std::size_t i = 0UZ; i < 3000UZ; ++i) {
        if (satisfied()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return satisfied();
}

// publishes nSamples samples of value 1 on an output port
void publishOnes(gr::PortOut<float>& port, std::size_t nSamples) {
    auto span = port.streamWriter().reserve(nSamples);
    std::ranges::fill(span, 1.0f);
    span.publish(nSamples);
}

// takes a block from the state its graph left it in to RUNNING. A case can then call its work()
void startForWork(gr::BlockModel& block) {
    if (block.state() == gr::lifecycle::State::IDLE) {
        std::ignore = block.changeStateTo(gr::lifecycle::State::INITIALISED);
    }
    std::ignore = block.changeStateTo(gr::lifecycle::State::RUNNING);
}

void sendMessage(gr::MsgPortOut& port, std::string_view endpoint, gr::property_map data) { gr::sendMessage<gr::message::Command::Set>(port, "", endpoint, std::move(data)); }

// the unique names of the graph's blocks in insertion order, comma separated
[[nodiscard]] std::string blockNames(const gr::Graph& graph) {
    std::string names;
    for (const std::shared_ptr<gr::BlockModel>& block : graph.blocks()) {
        names += std::format("{}{}", names.empty() ? "" : ", ", block->uniqueName());
    }
    return names;
}

// the message of the gr::exception that emplace throws, empty when it returns
[[nodiscard]] std::string refusal(auto emplace) {
    try {
        emplace();
    } catch (const gr::exception& e) {
        return e.message;
    }
    return {};
}

struct CountingResource : std::pmr::memory_resource {
    std::size_t nAllocations = 0UZ;

    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        ++nAllocations;
        return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }
    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override { std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment); }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override { return this == &other; }
};

// a compute-domain provider whose resource outlives every buffer bound to it
std::pmr::memory_resource* domainResource(const gr::ComputeDomain&, void*) {
    static CountingResource resource;
    return &resource;
}

// the unique names of a chain: a source feeding a Tunable stage feeding a sink, and a second sink without an edge
struct ChainNames {
    std::string source;
    std::string stage;
    std::string sink;
    std::string spareSink;
};

// gives the scheduler the chain and returns the names of its blocks
template<typename TScheduler>
[[nodiscard]] ChainNames exchangeChain(TScheduler& scheduler) {
    gr::Graph flow;
    auto&     source    = flow.emplaceBlock<Source>();
    auto&     stage     = flow.emplaceBlock<Tunable>();
    auto&     sink      = flow.emplaceBlock<Sink>();
    auto&     spareSink = flow.emplaceBlock<Sink>();
    boost::ut::expect(flow.connect<"out", "in">(source, stage).has_value());
    boost::ut::expect(flow.connect<"out", "in">(stage, sink).has_value());
    ChainNames names{std::string(source.unique_name), std::string(stage.unique_name), std::string(sink.unique_name), std::string(spareSink.unique_name)};
    boost::ut::expect(boost::ut::fatal(scheduler.exchange(std::move(flow)).has_value()));
    return names;
}

// the data of an EmplaceEdge request
[[nodiscard]] gr::property_map edgeRequest(std::string_view sourceBlock, std::string_view destinationBlock, gr::pmt::Value weight) {
    using namespace gr::serialization_fields;
    return {{std::pmr::string(EDGE_SOURCE_BLOCK), std::string(sourceBlock)}, {std::pmr::string(EDGE_SOURCE_PORT), std::string("out")}, //
        {std::pmr::string(EDGE_DESTINATION_BLOCK), std::string(destinationBlock)}, {std::pmr::string(EDGE_DESTINATION_PORT), std::string("in")}, {std::pmr::string(EDGE_MIN_BUFFER_SIZE), gr::undefined_Size}, {std::pmr::string(EDGE_WEIGHT), std::move(weight)}, {std::pmr::string(EDGE_NAME), std::string("requested")}};
}

// a request to the scheduler with the clientRequestID it carries, and whether the scheduler carries out the edit
struct EditRequest {
    std::string          id;
    std::string_view     endpoint;
    gr::property_map     data;
    bool                 succeeds = false;
    gr::message::Command cmd      = gr::message::Command::Set;
};

// sends every request to a scheduler that is not running and handles them on this thread. Each request must have one
// answer: Final, under replyEndpoint, with the request's clientRequestID, and with data when the edit succeeds or an
// error when the scheduler refuses it
template<typename TScheduler>
void expectRepliesUnder(TScheduler& scheduler, std::string_view replyEndpoint, std::span<const EditRequest> requests) {
    using namespace boost::ut;
    gr::MsgPortOut toScheduler;
    gr::MsgPortIn  fromScheduler;
    expect(fatal(toScheduler.connect(scheduler.msgIn).has_value()));
    expect(fatal(scheduler.msgOut.connect(fromScheduler).has_value()));

    for (const EditRequest& request : requests) {
        if (request.cmd == gr::message::Command::Get) {
            gr::sendMessage<gr::message::Command::Get>(toScheduler, scheduler.unique_name, request.endpoint, request.data, request.id);
        } else {
            gr::sendMessage<gr::message::Command::Set>(toScheduler, scheduler.unique_name, request.endpoint, request.data, request.id);
        }
    }
    scheduler.processScheduledMessages();

    auto replies = fromScheduler.streamReader().get();
    for (const EditRequest& request : requests) {
        auto answersRequest = [&request](const gr::Message& reply) { return reply.clientRequestID == request.id; };
        expect(eq(static_cast<std::size_t>(std::ranges::count_if(replies, answersRequest)), 1UZ)) << std::format("'{}': the number of replies with the request's id", request.id);
        const auto reply = std::ranges::find_if(replies, answersRequest);
        if (reply == replies.end()) {
            continue;
        }
        expect(reply->cmd == gr::message::Command::Final) << std::format("'{}': the reply is not Final", request.id);
        expect(eq(reply->endpoint, std::string(replyEndpoint))) << std::format("'{}': the reply's endpoint", request.id);
        expect(eq(reply->data.has_value(), request.succeeds)) << std::format("'{}': {}", request.id, reply->data.has_value() ? std::string("the refusal was answered with data") : reply->data.error().message);
    }
    std::ignore = replies.consume(replies.size());
}

// the reply that carries the clientRequestID, if one arrives. Consumes every message it reads
[[nodiscard]] std::optional<gr::Message> takeReplyTo(gr::MsgPortIn& port, std::string_view clientRequestID) {
    for (std::size_t i = 0UZ; i < 3000UZ; ++i) {
        auto                       messages = port.streamReader().get();
        std::optional<gr::Message> reply;
        if (const auto it = std::ranges::find_if(messages, [clientRequestID](const gr::Message& message) { return message.clientRequestID == clientRequestID; }); it != messages.end()) {
            reply = *it;
        }
        std::ignore = messages.consume(messages.size());
        if (reply.has_value()) {
            return reply;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return std::nullopt;
}

// the job list index and the unique name that a BlockReplaced reply names, each empty when the reply lacks it
struct ReplacementRecord {
    std::optional<std::size_t> jobList;
    std::string                uniqueName;
};

[[nodiscard]] ReplacementRecord replacementRecord(const gr::property_map& reply) {
    ReplacementRecord record;
    if (const auto it = reply.find("jobList"); it != reply.end()) {
        if (const auto* index = it->second.get_if<gr::Size_t>(); index != nullptr) {
            record.jobList = static_cast<std::size_t>(*index);
        }
    }
    if (const auto it = reply.find(std::pmr::string(gr::serialization_fields::BLOCK_UNIQUE_NAME)); it != reply.end()) {
        record.uniqueName = it->second.value_or(std::string());
    }
    return record;
}

// the unique names that a reply lists under the key, empty when the reply lacks the list
[[nodiscard]] std::vector<std::string> listedNames(const gr::property_map& reply, std::string_view key) {
    std::vector<std::string> names;
    if (const auto it = reply.find(std::pmr::string(key)); it != reply.end()) {
        if (const auto* list = it->second.get_if<gr::Tensor<gr::pmt::Value>>(); list != nullptr) {
            for (std::size_t i = 0UZ; i < list->size(); ++i) {
                names.push_back((*list)[i].value_or(std::string()));
            }
        }
    }
    return names;
}

// the data of a RemoveEdge request for the edge from the source's "out" to the destination's "in"
[[nodiscard]] gr::property_map removeEdgeRequest(std::string_view sourceBlock, std::string_view destinationBlock) {
    using namespace gr::serialization_fields;
    return {{std::pmr::string(EDGE_SOURCE_BLOCK), std::string(sourceBlock)}, {std::pmr::string(EDGE_SOURCE_PORT), std::string("out")}, //
        {std::pmr::string(EDGE_DESTINATION_BLOCK), std::string(destinationBlock)}, {std::pmr::string(EDGE_DESTINATION_PORT), std::string("in")}};
}

// Runs first -> sink and a second source without an edge, which stops at its first call. The case then adds the edge
// from the second source to the sink. The added edge displaces the first source's edge, and the second source runs
// again and feeds the sink
template<typename TScheduler>
void expectEmplacedEdgeRestartsStoppedSource() {
    using namespace boost::ut;
    using enum gr::lifecycle::State;
    gSourcedSamples.store(0UZ);
    gFreeRunningSamples.store(0UZ);

    gr::Graph flow;
    auto&     sink   = flow.emplaceBlock<FreeSink>();
    auto&     first  = flow.emplaceBlock<Source>();
    auto&     second = flow.emplaceBlock<CountedSource>();
    expect(flow.connect<"out", "in">(first, sink).has_value());
    const std::string sinkName{sink.unique_name};
    const std::string firstName{first.unique_name};
    const std::string secondName{second.unique_name};
    const auto*       secondBlock = &second;

    TScheduler scheduler;
    expect(fatal(scheduler.exchange(std::move(flow)).has_value()));
    gr::MsgPortOut toScheduler;
    gr::MsgPortIn  fromScheduler;
    expect(fatal(toScheduler.connect(scheduler.msgIn).has_value()));
    expect(fatal(scheduler.msgOut.connect(fromScheduler).has_value()));

    std::thread runner([&scheduler] { std::ignore = scheduler.runAndWait(); }); // a single-threaded run executes on the thread that starts it
    expect(awaitCondition([secondBlock] { return secondBlock->state() == STOPPED; })) << "the source without an edge did not stop";
    expect(awaitCondition([] { return gFreeRunningSamples.load() > 0UZ; })) << "the first source never fed the sink";

    sendMessage(toScheduler, gr::scheduler::property::kEmplaceEdge, edgeRequest(secondName, sinkName, std::int32_t{0}));
    const std::optional<gr::property_map> reply = awaitReplyData(fromScheduler, gr::scheduler::property::kEdgeEmplaced);
    expect(reply.has_value()) << "the edge was refused";
    expect(reply.has_value() && listedNames(*reply, "restartedBlocks") == std::vector{secondName}) << "the reply does not name the restarted source";
    expect(reply.has_value() && listedNames(*reply, "sourcesWithoutReader") == std::vector{firstName}) << "the reply does not name the displaced source";
    expect(awaitCondition([] { return gSourcedSamples.load() > 0UZ; })) << "the source at the end of the added edge never ran again";
    expect(secondBlock->state() == RUNNING);

    expect(scheduler.changeStateTo(REQUESTED_STOP).has_value());
    runner.join();
}

// Runs Source -> EndingStage -> FreeSink beside a free-running Source -> Sink, each block of the chain with the given
// disconnect_on_done. The stage ends, and the sink stops at the stage's end-of-stream tag. The case then replaces the
// stage, and the reply names the restarted sink. A sink that released its input on done is connected to the
// replacement at the ring's write position, and samples reach it. A sink that kept its input reads the same ring,
// reads the end-of-stream tag again and stops without a sample
template<bool disconnectOnDone>
void expectSinkAfterEndedStageReplaced() {
    using namespace boost::ut;
    using enum gr::lifecycle::State;
    registerTestBlocks();
    gStagePassed.store(0UZ);
    gCountedSamples.store(0UZ);
    gFreeRunningSamples.store(0UZ);

    gr::Graph              flow;
    const gr::property_map chainSettings{{"disconnect_on_done", disconnectOnDone}};
    auto&                  source     = flow.emplaceBlock<Source>(chainSettings);
    auto&                  stage      = flow.emplaceBlock<EndingStage>(chainSettings);
    auto&                  sink       = flow.emplaceBlock<FreeSink>(chainSettings);
    auto&                  keptSource = flow.emplaceBlock<Source>();
    auto&                  keptSink   = flow.emplaceBlock<Sink>();
    expect(flow.connect<"out", "in">(source, stage).has_value());
    expect(flow.connect<"out", "in">(stage, sink).has_value());
    expect(flow.connect<"out", "in">(keptSource, keptSink).has_value());
    const std::string sinkName{sink.unique_name};
    const std::string stageName{stage.unique_name};
    const auto*       stageBlock = &stage;
    const auto*       sinkBlock  = &sink;

    TestScheduler scheduler;
    expect(fatal(scheduler.exchange(std::move(flow)).has_value()));
    gr::MsgPortOut toScheduler;
    gr::MsgPortIn  fromScheduler;
    expect(fatal(toScheduler.connect(scheduler.msgIn).has_value()));
    expect(fatal(scheduler.msgOut.connect(fromScheduler).has_value()));
    expect(scheduler.changeStateTo(INITIALISED).has_value());
    expect(scheduler.changeStateTo(RUNNING).has_value());
    expect(fatal(awaitCondition([stageBlock, sinkBlock] { return stageBlock->state() == STOPPED && sinkBlock->state() == STOPPED; }))) << "the stage did not end, or the sink did not stop at its end";
    const std::size_t nBeforeReplacement = gFreeRunningSamples.load();

    sendMessage(toScheduler, gr::scheduler::property::kReplaceBlock, {{"uniqueName", stageName}, {"type", gr::meta::type_name<CountedTunable>()}});
    const std::optional<gr::property_map> reply = awaitReplyData(fromScheduler, gr::scheduler::property::kBlockReplaced);
    expect(fatal(reply.has_value())) << "the replacement was refused";
    expect(std::ranges::contains(listedNames(*reply, "restartedBlocks"), sinkName)) << "the reply does not name the restarted sink";
    if constexpr (disconnectOnDone) {
        expect(awaitCondition([nBeforeReplacement] { return gCountedSamples.load() > 0UZ && gFreeRunningSamples.load() > nBeforeReplacement; })) << std::format("the replacement passed {} samples and the sink took {} after the replacement", gCountedSamples.load(), gFreeRunningSamples.load() - nBeforeReplacement);
        expect(sinkBlock->state() == RUNNING);
    } else {
        expect(awaitCondition([sinkBlock] { return gCountedSamples.load() > 0UZ && sinkBlock->state() == STOPPED; })) << "the replacement passed no sample, or the sink did not stop again";
        expect(eq(gFreeRunningSamples.load(), nBeforeReplacement)) << "the sink took a sample past the end-of-stream tag";
    }

    expect(scheduler.changeStateTo(REQUESTED_STOP).has_value());
    expect(awaitCondition([&scheduler] { return scheduler.state() == STOPPED; })) << "the scheduler did not stop";
}

// Runs HeldSource -> HoldingStage -> HeldSink until the source has published its samples and the stage has passed its
// share, which the sink holds. The case then replaces the stage with a Tunable and releases the sink. Every sample
// reaches the sink once: the replacement passes the samples queued at its input, and the sink reads those queued at
// its output
template<typename TScheduler>
void expectQueuedSamplesPassReplacement() {
    using namespace boost::ut;
    using enum gr::lifecycle::State;
    registerTestBlocks();
    gHeldPublished.store(0UZ);
    gHeldPassed.store(0UZ);
    gSinkReleased.store(false);
    gSinkReceived.store(0UZ);

    gr::Graph flow;
    auto&     source = flow.emplaceBlock<HeldSource>();
    auto&     stage  = flow.emplaceBlock<HoldingStage>();
    auto&     sink   = flow.emplaceBlock<HeldSink>();
    expect(flow.connect<"out", "in">(source, stage).has_value());
    expect(flow.connect<"out", "in">(stage, sink).has_value());
    const std::string stageName{stage.unique_name};

    TScheduler scheduler;
    expect(fatal(scheduler.exchange(std::move(flow)).has_value()));
    gr::MsgPortOut toScheduler;
    gr::MsgPortIn  fromScheduler;
    expect(fatal(toScheduler.connect(scheduler.msgIn).has_value()));
    expect(fatal(scheduler.msgOut.connect(fromScheduler).has_value()));

    std::thread runner([&scheduler] { std::ignore = scheduler.runAndWait(); }); // a single-threaded run executes on the thread that starts it
    expect(awaitCondition([] { return gHeldPublished.load() == HeldSource::kSamples && gHeldPassed.load() == HoldingStage::kPassed; })) << "the stage did not hold samples at both its ports";

    sendMessage(toScheduler, gr::scheduler::property::kReplaceBlock, {{"uniqueName", stageName}, {"type", gr::meta::type_name<Tunable>()}});
    expect(awaitReplyData(fromScheduler, gr::scheduler::property::kBlockReplaced).has_value()) << "the replacement was refused";
    gSinkReleased.store(true);
    expect(awaitCondition([] { return gSinkReceived.load() == HeldSource::kSamples; })) << std::format("the sink received {} of {} samples", gSinkReceived.load(), HeldSource::kSamples);

    expect(scheduler.changeStateTo(REQUESTED_STOP).has_value());
    runner.join();
    expect(eq(gSinkReceived.load(), HeldSource::kSamples)) << "the sink received a sample twice";
}

} // namespace qa_edit

const boost::ut::suite<"graph editing"> graphEditTests = [] {
    using namespace boost::ut;
    using enum gr::lifecycle::State;

#ifndef GR_TEST_WITHOUT_BLOCK_REGISTRY // emplacement by name resolves the type through the registry
    "a block emplaced from yaml applies its serialized settings"_test = [] {
        qa_edit::registerTestBlocks();

        qa_edit::TestScheduler scheduler;
        {
            gr::Graph flow;
            auto&     source = flow.emplaceBlock<qa_edit::Source>();
            auto&     sink   = flow.emplaceBlock<qa_edit::Sink>();
            expect(flow.connect<"out", "in">(source, sink).has_value());
            expect(scheduler.exchange(std::move(flow)).has_value());
        }

        gr::MsgPortOut toScheduler;
        gr::MsgPortIn  fromScheduler;
        expect(toScheduler.connect(scheduler.msgIn).has_value());
        expect(scheduler.msgOut.connect(fromScheduler).has_value());

        const std::string blockYaml = std::format("id: {}\nparameters:\n  gain: !!float32 4.5\n", gr::meta::type_name<qa_edit::Tunable>());
        qa_edit::sendMessage(toScheduler, gr::scheduler::property::kEmplaceBlock, {{"yaml", blockYaml}});

        expect(scheduler.changeStateTo(INITIALISED).has_value());
        expect(scheduler.changeStateTo(RUNNING).has_value());
        expect(qa_edit::awaitReply(fromScheduler, gr::scheduler::property::kBlockEmplaced)) << "the block was never emplaced";

        const auto* emplaced = [&]() -> const gr::BlockModel* {
            for (const auto& block : scheduler.graph().blocks()) {
                if (block->typeName().find("Tunable") != std::string_view::npos) {
                    return block.get();
                }
            }
            return nullptr;
        }();
        expect(fatal(emplaced != nullptr)) << "the emplaced block is not in the graph";

        const auto gain = emplaced->settings().get("gain");
        expect(fatal(gain.has_value())) << "the emplaced block reports no gain setting";
        expect(eq(gain->value_or(0.0f), 4.5f)) << "the emplaced block kept its constructor default instead of the serialized value";

        expect(scheduler.changeStateTo(REQUESTED_STOP).has_value());
    };

    "a subgraph emplaced by name inherits its parent's plugin loader"_test = [] {
        gr::BlockRegistry     localRegistry;
        gr::SchedulerRegistry localSchedulers;
        std::ignore = localRegistry.insert<qa_edit::LoaderCanary>();
        gr::PluginLoader localLoader(localRegistry, localSchedulers, {});

        const std::string canaryType{gr::meta::type_name<qa_edit::LoaderCanary>()};
        expect(gr::globalPluginLoader().instantiate(canaryType) == nullptr) << "the canary resolves globally, so this test cannot discriminate the loaders";

        gr::Graph                              flow(localLoader);
        const std::shared_ptr<gr::BlockModel>& wrapped = flow.emplaceBlock("gr::Graph", {{"name", std::string("inner")}});
        expect(fatal(wrapped != nullptr));
        expect(eq(std::string{wrapped->name()}, std::string{"inner"})) << "the emplaced subgraph dropped its settings";

        gr::Graph* inner = wrapped->graph();
        expect(fatal(inner != nullptr));
        expect(inner->_pluginLoader == &localLoader) << "the nested graph bound a loader other than its parent's";
        expect(nothrow([&] { std::ignore = inner->emplaceBlock(canaryType, {}); })) << "a type the parent's loader resolves must resolve inside the subgraph";

        auto& toReplace = flow.emplaceBlock<qa_edit::Source>();
        expect(nothrow([&] { std::ignore = flow.replaceBlock(toReplace.unique_name, canaryType, {}); })) << "replaceBlock must consult the graph's own loader";
    };

    // A pool of two threads gives the run two job lists. The probe holds one work() call in the first list, and the
    // worker of the second list handles the request meanwhile
    "a subgraph emplaced from yaml in a running graph loads while no block of the run is inside work()"_test = [] {
        constexpr std::string_view kPoolName = "qa_edit_two_threads";
        gr::thread_pool::Manager::instance().replacePool(std::string(kPoolName), std::make_shared<gr::thread_pool::ThreadPoolWrapper>(std::make_unique<gr::thread_pool::BasicThreadPool>(kPoolName, gr::thread_pool::TaskType::CPU_BOUND, 2U, 2U), "CPU"));
        qa_edit::registerTestBlocks();
        qa_edit::gHoldRequested.store(false);
        qa_edit::gHoldEntered.store(false);
        qa_edit::gHoldEnded.store(false);
        qa_edit::gConstructionsDuringHold.store(0UZ);

        gr::Graph flow;
        auto&     probe  = flow.emplaceBlock<qa_edit::HoldingProbe>();
        auto&     source = flow.emplaceBlock<qa_edit::Source>();
        auto&     sink   = flow.emplaceBlock<qa_edit::Sink>();
        expect(flow.connect<"out", "in">(source, sink).has_value());
        const std::string probeName{probe.unique_name};

        qa_edit::JobListProbe scheduler({{"poolName", std::string(kPoolName)}});
        expect(fatal(scheduler.exchange(std::move(flow)).has_value()));
        gr::MsgPortOut toScheduler;
        gr::MsgPortIn  fromScheduler;
        expect(fatal(toScheduler.connect(scheduler.msgIn).has_value()));
        expect(fatal(scheduler.msgOut.connect(fromScheduler).has_value()));
        expect(scheduler.changeStateTo(INITIALISED).has_value());
        expect(scheduler.changeStateTo(RUNNING).has_value());
        expect(fatal(eq(scheduler.nJobLists(), 2UZ))) << "the graph was not split into two job lists";
        expect(fatal(scheduler.firstJobListHolds(probeName))) << "the probe is not in the first job list";

        qa_edit::gHoldRequested.store(true);
        expect(fatal(qa_edit::awaitCondition([] { return qa_edit::gHoldEntered.load(); }))) << "the probe never held a work() call";
        const std::string subgraphYaml = std::format("id: SUBGRAPH\nparameters:\n  name: group\ngraph:\n  blocks:\n    - id: {}\n      parameters:\n        name: inner\n", gr::meta::type_name<qa_edit::ConstructionCounter>());
        qa_edit::sendMessage(toScheduler, gr::scheduler::property::kEmplaceBlock, {{"yaml", subgraphYaml}});
        expect(qa_edit::awaitReplyData(fromScheduler, gr::scheduler::property::kBlockEmplaced).has_value()) << "the subgraph was refused";
        expect(fatal(qa_edit::awaitCondition([] { return qa_edit::gHoldEnded.load(); }))) << "the probe's held call never returned";
        expect(eq(qa_edit::gConstructionsDuringHold.load(), 0UZ)) << "the subgraph loaded while the probe was inside work()";

        expect(scheduler.changeStateTo(REQUESTED_STOP).has_value());
        expect(qa_edit::awaitCondition([&scheduler] { return scheduler.state() == STOPPED; })) << "the scheduler did not stop";
    };

    "a block emplaced by name with a setting it does not declare stays out of the graph"_test = [] {
        qa_edit::registerTestBlocks();
        const std::string tunableType{gr::meta::type_name<qa_edit::Tunable>()};

        gr::Graph flow;
        std::ignore                     = flow.emplaceBlock<qa_edit::Source>();
        const std::size_t nBlocksBefore = flow.blocks().size();
        const std::string namesBefore   = qa_edit::blockNames(flow);

        const std::string refused = qa_edit::refusal([&] { std::ignore = flow.emplaceBlock(tunableType, {{"gian", 2.0f}}); });
        expect(refused.starts_with("settings could not be applied") && refused.contains("gian")) << "the refusal must reach the caller as thrown, got: " << refused;
        expect(eq(flow.blocks().size(), nBlocksBefore)) << "the block stayed in the graph after throwing: " << refused;
        expect(eq(qa_edit::blockNames(flow), namesBefore)) << "the graph holds other blocks after throwing: " << refused;

        const std::shared_ptr<gr::BlockModel>& accepted = flow.emplaceBlock(tunableType, {{"gain", 2.0f}});
        expect(eq(flow.blocks().size(), nBlocksBefore + 1UZ)) << "a block with settings it declares must join the graph";
        const std::optional<gr::pmt::Value> gain = accepted->settings().get("gain");
        expect(fatal(gain.has_value())) << "the accepted block reports no gain setting";
        expect(eq(gain->value_or(0.0f), 2.0f)) << "the accepted block must apply its settings";
    };

    "a block emplaced by name with a value its setting refuses stays out of the graph"_test = [] {
        qa_edit::registerTestBlocks();
        const std::string tunableType{gr::meta::type_name<qa_edit::Tunable>()};

        gr::Graph flow;
        std::ignore                     = flow.emplaceBlock<qa_edit::Source>();
        const std::size_t nBlocksBefore = flow.blocks().size();
        const std::string namesBefore   = qa_edit::blockNames(flow);

        const std::string refused = qa_edit::refusal([&] { std::ignore = flow.emplaceBlock(tunableType, {{"gain", std::string("loud")}}); });
        expect(refused.contains("'gain'")) << "the refusal must reach the caller as thrown, got: " << refused;
        expect(eq(flow.blocks().size(), nBlocksBefore)) << "the block stayed in the graph after throwing: " << refused;
        expect(eq(qa_edit::blockNames(flow), namesBefore)) << "the graph holds other blocks after throwing: " << refused;
    };

    "a replacement takes over the samples queued at the ports of the block it replaces"_test = [] {
        constexpr std::size_t kQueuedAtInput  = 5UZ;
        constexpr std::size_t kQueuedAtOutput = 3UZ;
        qa_edit::registerTestBlocks();

        gr::Graph flow;
        auto&     source = flow.emplaceBlock<qa_edit::Source>();
        auto&     stage  = flow.emplaceBlock<qa_edit::Tunable>();
        auto&     sink   = flow.emplaceBlock<qa_edit::Sink>();
        expect(flow.connect<"out", "in">(source, stage).has_value());
        expect(flow.connect<"out", "in">(stage, sink).has_value());
        expect(fatal(flow.connectPendingEdges())) << "the graph did not connect";
        qa_edit::publishOnes(source.out, kQueuedAtInput);
        qa_edit::publishOnes(stage.out, kQueuedAtOutput);
        const std::shared_ptr<gr::BlockModel> sinkModel = flow.blocks()[2UZ];

        auto [oldBlock, newBlock] = flow.replaceBlock(stage.unique_name, gr::meta::type_name<qa_edit::Tunable>(), {{"gain", 2.0f}});
        expect(fatal(newBlock != nullptr));
        expect(flow.blocks()[1UZ] == newBlock) << "the replacement does not hold the place of the replaced block";
        expect(!stage.in.isConnected()) << "the replaced block still reads the source's ring";
        expect(!stage.out.isConnected()) << "the sink still reads the replaced block's ring";
        for (const gr::Edge& edge : flow.edges()) {
            expect(edge.sourceBlock() != oldBlock && edge.destinationBlock() != oldBlock) << std::format("{}", edge) << ": the edge names the replaced block";
            expect(edge._sourcePort == edge.sourceBlock()->dynamicOutputPort("out").value()) << std::format("{}", edge) << ": the edge names another source port";
            expect(edge._destinationPort == edge.destinationBlock()->dynamicInputPort("in").value()) << std::format("{}", edge) << ": the edge names another destination port";
        }

        oldBlock.reset();
        qa_edit::startForWork(*newBlock);
        qa_edit::startForWork(*sinkModel);
        std::ignore = newBlock->work(std::numeric_limits<std::size_t>::max());
        std::ignore = sinkModel->work(std::numeric_limits<std::size_t>::max());
        expect(eq(sink._nReceived, kQueuedAtInput + kQueuedAtOutput)) << "the sink did not receive the samples queued at the replaced block's ports";
    };

    "a replacement that lacks a port of the replaced block is refused and the graph is unchanged"_test = [] {
        constexpr std::size_t kQueuedAtInput = 5UZ;
        qa_edit::registerTestBlocks();

        gr::Graph flow;
        auto&     source = flow.emplaceBlock<qa_edit::Source>();
        auto&     stage  = flow.emplaceBlock<qa_edit::Tunable>();
        auto&     sink   = flow.emplaceBlock<qa_edit::Sink>();
        expect(flow.connect<"out", "in">(source, stage).has_value());
        expect(flow.connect<"out", "in">(stage, sink).has_value());
        expect(fatal(flow.connectPendingEdges())) << "the graph did not connect";
        qa_edit::publishOnes(source.out, kQueuedAtInput);
        const std::string namesBefore = qa_edit::blockNames(flow);

        const std::string refused = qa_edit::refusal([&] { std::ignore = flow.replaceBlock(stage.unique_name, gr::meta::type_name<qa_edit::Source>(), {}); });
        expect(!refused.empty()) << "a replacement without an input port was accepted";
        expect(eq(qa_edit::blockNames(flow), namesBefore)) << "a refused replacement changed the blocks: " << refused;
        expect(eq(flow.edges()[0UZ].destinationBlock()->uniqueName(), std::string_view(stage.unique_name))) << "a refused replacement repointed an edge";
        expect(eq(stage.in.streamReader().available(), kQueuedAtInput)) << "a refused replacement moved the queued samples";
        expect(eq(sink.in.nWriters(), 1UZ)) << "a refused replacement moved the sink's ring";
    };
#endif

    "a typed emplacement with a setting the block does not declare leaves the graph as it was"_test = [] {
        gr::Graph flow;
        std::ignore                     = flow.emplaceBlock<qa_edit::Source>();
        const std::size_t nBlocksBefore = flow.blocks().size();
        const std::string namesBefore   = qa_edit::blockNames(flow);

        const std::string refused = qa_edit::refusal([&] { std::ignore = flow.emplaceBlock<qa_edit::Tunable>({{"gian", 2.0f}}); });
        expect(refused.starts_with("settings could not be applied") && refused.contains("gian")) << "the refusal must reach the caller as thrown, got: " << refused;
        expect(eq(flow.blocks().size(), nBlocksBefore)) << "the block stayed in the graph after throwing: " << refused;
        expect(eq(qa_edit::blockNames(flow), namesBefore)) << "the graph holds other blocks after throwing: " << refused;

        auto& accepted = flow.emplaceBlock<qa_edit::Tunable>({{"gain", 2.0f}});
        expect(eq(flow.blocks().size(), nBlocksBefore + 1UZ)) << "a block with settings it declares must join the graph";
        expect(eq(accepted.gain.value, 2.0f)) << "the accepted block must apply its settings";
        expect(accepted.state() == INITIALISED) << "the accepted block must be initialized";
    };

    "an exported output with interior consumers feeds both sides of the boundary"_test = [] {
        gr::Graph flow;
        auto      wrapper   = std::make_shared<gr::GraphWrapper<gr::Graph>>();
        auto&     inner     = *wrapper->graph();
        auto&     producer  = inner.emplaceBlock<qa_edit::CountingSource>();
        auto&     innerSink = inner.emplaceBlock<qa_edit::Sink>();
        expect(inner.connect<"out", "in">(producer, innerSink).has_value());

        const std::shared_ptr<gr::BlockModel>& subgraph = flow.addBlock(wrapper);
        expect(wrapper->exportPort(true, producer.unique_name, gr::PortDirection::OUTPUT, "out", "out").has_value());
        std::ignore = flow.emplaceBlock<qa_edit::Sink>();
        expect(flow.connect(subgraph, gr::PortDefinition{"out"}, flow.blocks()[1], gr::PortDefinition{"in"}).has_value());

        expect(flow.edges()[0].hasSameSourcePort(inner.edges()[0])) << "the exported alias and the interior edge reference the same port and must compare equal";

        // the wiring order a scheduler uses: the top-level graph's edges, then the subgraph's
        expect(flow.connectPendingEdges());
        expect(inner.connectPendingEdges());

        expect(eq(producer.out.nReaders(), 2UZ)) << "the boundary split the fan-out across two buffers, so one consumer starves";
    };

    "an unconnected optional output is sized with its connected sibling"_test = [] {
        gr::Graph flow;
        auto&     source = flow.emplaceBlock<qa_edit::DualSource>();
        auto&     sink   = flow.emplaceBlock<qa_edit::Sink>();
        expect(flow.connect<"out", "in">(source, sink).has_value());
        expect(flow.connectPendingEdges());
        expect(eq(source.monitor.bufferSize(), source.out.bufferSize())) << "a span request past the default capacity returns empty with nothing signaled";
    };

    "an edge naming no memory resource gets the buffer's own default"_test = [] {
        gr::Graph flow;
        auto&     source = flow.emplaceBlock<qa_edit::Source>();
        auto&     sink   = flow.emplaceBlock<qa_edit::Sink>();
        expect(flow.connect<"out", "in">(source, sink).has_value());
        expect(fatal(eq(flow.edges().size(), 1UZ)));
        expect(flow.connectPendingEdges());

        const gr::Edge& edge = flow.edges()[0];
        expect(edge._dataResource == nullptr) << "an edge nobody gave a resource must leave the choice to the buffer";
        expect(edge._tagResource == nullptr) << "an edge nobody gave a resource must leave the choice to the buffer";
        expect(!source.out.buffer().tagBuffer.isMmapAllocated()) << "a tag is not trivially copyable, so its ring stays on the heap";
        if constexpr (gr::has_posix_mmap_interface) {
            expect(source.out.buffer().streamBuffer.isMmapAllocated()) << "a trivially copyable sample on a platform with mmap must get the double-mapped ring";
        }
    };

    "an edge naming the default resource gets a heap buffer"_test = [] {
        gr::Graph flow;
        auto&     source = flow.emplaceBlock<qa_edit::Source>();
        auto&     sink   = flow.emplaceBlock<qa_edit::Sink>();
        expect(flow.connect<"out", "in">(source, sink, gr::EdgeParameters{.dataResource = std::pmr::get_default_resource(), .tagResource = std::pmr::get_default_resource()}).has_value());
        expect(flow.connectPendingEdges());

        expect(flow.edges()[0]._dataResource == std::pmr::get_default_resource()) << "a named resource must stay as named";
        expect(source.out.dataResource() == std::pmr::get_default_resource()) << "the port's buffer must come from the named resource";
        expect(!source.out.buffer().streamBuffer.isMmapAllocated()) << "a named heap resource must not be replaced by the double-mapped ring";
    };

#ifndef GR_TEST_WITHOUT_BLOCK_REGISTRY // a graph file resolves its block ids through the registry
    "an edge whose graph file names a memory resource keeps it"_test = [] {
        qa_edit::registerTestBlocks();
        gr::ComputeRegistry::instance().register_provider("qa-edit-graph-file", &qa_edit::domainResource);
        auto&             resource        = static_cast<qa_edit::CountingResource&>(*qa_edit::domainResource({}, nullptr));
        const std::size_t nAllocationsOld = resource.nAllocations;

        const std::string graphFile = std::format("blocks:\n"
                                                  "  - id: {}\n"
                                                  "    parameters:\n"
                                                  "      name: source\n"
                                                  "      compute_domain: \"gpu:qa-edit-graph-file\"\n"
                                                  "  - id: {}\n"
                                                  "    parameters:\n"
                                                  "      name: sink\n"
                                                  "connections:\n"
                                                  "  - [source, out, sink, in]\n",
            gr::meta::type_name<qa_edit::Source>(), gr::meta::type_name<qa_edit::Sink>());
        auto              flow      = gr::loadGrc(gr::globalPluginLoader(), graphFile);
        expect(fatal(eq(flow->edges().size(), 1UZ)));
        expect(flow->connectPendingEdges());

        const gr::Edge& edge = flow->edges()[0];
        expect(edge._dataResource == &resource) << "the resource the graph file's compute domain names must reach the edge";
        expect(edge._tagResource == &resource) << "the resource the graph file's compute domain names must reach the edge";
        auto& source = *static_cast<qa_edit::Source*>(edge._sourceBlock->raw());
        expect(source.out.dataResource() == &resource) << "the port's buffer must come from the named resource";
        expect(gt(resource.nAllocations, nAllocationsOld)) << "the named resource must have served the buffer";
    };
#endif

    "an edge naming only its tag resource keeps it while the compute domain fills the data resource"_test = [] {
        gr::ComputeRegistry::instance().register_provider("qa-edit-mixed", &qa_edit::domainResource);
        auto&                     domainPool = static_cast<qa_edit::CountingResource&>(*qa_edit::domainResource({}, nullptr));
        qa_edit::CountingResource tagPool;
        gr::Graph                 flow;
        auto&                     source = flow.emplaceBlock<qa_edit::Source>({{"compute_domain", std::string("gpu:qa-edit-mixed")}});
        auto&                     sink   = flow.emplaceBlock<qa_edit::Sink>();
        expect(flow.connect<"out", "in">(source, sink, gr::EdgeParameters{.tagResource = &tagPool}).has_value());
        expect(flow.connectPendingEdges());

        const gr::Edge& edge = flow.edges()[0];
        expect(edge._tagResource == &tagPool) << "a tag resource the caller named must stay as named";
        expect(source.out.tagResource() == &tagPool) << "the port's tag ring must come from the named resource";
        expect(edge._dataResource == &domainPool) << "the compute domain must fill the data resource nobody named";
        expect(source.out.dataResource() == &domainPool) << "the port's stream ring must come from the compute domain's resource";
    };

    "an edge naming only its data resource keeps it while the compute domain fills the tag resource"_test = [] {
        gr::ComputeRegistry::instance().register_provider("qa-edit-mixed", &qa_edit::domainResource);
        auto&                     domainPool = static_cast<qa_edit::CountingResource&>(*qa_edit::domainResource({}, nullptr));
        qa_edit::CountingResource dataPool;
        gr::Graph                 flow;
        auto&                     source = flow.emplaceBlock<qa_edit::Source>({{"compute_domain", std::string("gpu:qa-edit-mixed")}});
        auto&                     sink   = flow.emplaceBlock<qa_edit::Sink>();
        expect(flow.connect<"out", "in">(source, sink, gr::EdgeParameters{.dataResource = &dataPool}).has_value());
        expect(flow.connectPendingEdges());

        const gr::Edge& edge = flow.edges()[0];
        expect(edge._dataResource == &dataPool) << "a data resource the caller named must stay as named";
        expect(source.out.dataResource() == &dataPool) << "the port's stream ring must come from the named resource";
        expect(edge._tagResource == &domainPool) << "the compute domain must fill the tag resource nobody named";
        expect(source.out.tagResource() == &domainPool) << "the port's tag ring must come from the compute domain's resource";
    };

    "an edge naming both resources keeps both under a compute domain"_test = [] {
        gr::ComputeRegistry::instance().register_provider("qa-edit-mixed", &qa_edit::domainResource);
        qa_edit::CountingResource dataPool;
        qa_edit::CountingResource tagPool;
        gr::Graph                 flow;
        auto&                     source = flow.emplaceBlock<qa_edit::Source>({{"compute_domain", std::string("gpu:qa-edit-mixed")}});
        auto&                     sink   = flow.emplaceBlock<qa_edit::Sink>();
        expect(flow.connect<"out", "in">(source, sink, gr::EdgeParameters{.dataResource = &dataPool, .tagResource = &tagPool}).has_value());
        expect(flow.connectPendingEdges());

        const gr::Edge& edge = flow.edges()[0];
        expect(edge._dataResource == &dataPool) << "a data resource the caller named must stay as named";
        expect(edge._tagResource == &tagPool) << "a tag resource the caller named must stay as named";
        expect(source.out.dataResource() == &dataPool) << "the port's stream ring must come from the named resource";
        expect(source.out.tagResource() == &tagPool) << "the port's tag ring must come from the named resource";
    };

    "a fan-out mixing typed and dynamic connects feeds every consumer"_test = [] {
        auto runMixedFanOut = [](bool typedFirst) {
            const std::string order = typedFirst ? "typed edge first" : "dynamic edge first";

            gr::Graph flow;
            auto&     source      = flow.emplaceBlock<qa_edit::CountingSource>();
            auto&     typedSink   = flow.emplaceBlock<qa_edit::Sink>();
            auto&     dynamicSink = flow.emplaceBlock<qa_edit::Sink>();

            const auto connectTyped   = [&] { return flow.connect<"out", "in">(source, typedSink).has_value(); };
            const auto connectDynamic = [&] { return flow.connect(source, gr::PortDefinition("out"), dynamicSink, gr::PortDefinition("in")).has_value(); };

            if (typedFirst) {
                expect(connectTyped()) << order << ": the typed edge was not accepted";
                expect(connectDynamic()) << order << ": the dynamic edge was not accepted";
            } else {
                expect(connectDynamic()) << order << ": the dynamic edge was not accepted";
                expect(connectTyped()) << order << ": the typed edge was not accepted";
            }

            gr::scheduler::Simple scheduler;
            expect(fatal(scheduler.exchange(std::move(flow)).has_value()));

            std::thread runner([&scheduler] { std::ignore = scheduler.runAndWait(); });
            for (std::size_t i = 0UZ; i < 2000UZ && (typedSink._nReceived == 0UZ || dynamicSink._nReceived == 0UZ); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            std::ignore = scheduler.changeStateTo(REQUESTED_STOP);
            runner.join();

            expect(gt(typedSink._nReceived, 0UZ)) << order << ": the index-addressed consumer of the fan-out received nothing";
            expect(gt(dynamicSink._nReceived, 0UZ)) << order << ": the name-addressed consumer of the fan-out received nothing";
        };

        runMixedFanOut(true);
        runMixedFanOut(false);
    };

    "a mixed-style fan-out is one adjacency-list entry"_test = [] {
        gr::Graph flow;
        auto&     source      = flow.emplaceBlock<qa_edit::Source>();
        auto&     typedSink   = flow.emplaceBlock<qa_edit::Sink>();
        auto&     dynamicSink = flow.emplaceBlock<qa_edit::Sink>();
        expect(flow.connect<"out", "in">(source, typedSink).has_value());
        expect(flow.connect(source, gr::PortDefinition("out"), dynamicSink, gr::PortDefinition("in")).has_value());

        const gr::graph::AdjacencyList         adjacencyList = gr::graph::computeAdjacencyList(flow);
        const std::shared_ptr<gr::BlockModel>& sourceModel   = flow.blocks().front();

        expect(fatal(adjacencyList.contains(sourceModel)));
        expect(eq(adjacencyList.at(sourceModel).size(), 1UZ)) << "one output port must not occupy two entries";
        expect(eq(gr::graph::outgoingEdges(adjacencyList, sourceModel, gr::PortDefinition("out")).size(), 2UZ)) << "the name-addressed query missed an edge";
        expect(eq(gr::graph::outgoingEdges(adjacencyList, sourceModel, gr::PortDefinition(0UZ)).size(), 2UZ)) << "the index-addressed query missed an edge";
    };

    "removing one edge of a fan-out leaves the sibling flowing and stays removed"_test = [] {
        gr::Graph flow;
        auto&     source = flow.emplaceBlock<qa_edit::Source>();
        auto&     sinkA  = flow.emplaceBlock<qa_edit::Sink>();
        auto&     sinkB  = flow.emplaceBlock<qa_edit::Sink>();
        expect(flow.connect<"out", "in">(source, sinkA).has_value());
        expect(flow.connect<"out", "in">(source, sinkB).has_value());
        expect(eq(flow.edges().size(), 2UZ));

        expect(flow.connectPendingEdges()) << "the fan-out did not connect";

        const auto removed = flow.removeEdgeBySourcePort(source.unique_name, "out", sinkA.unique_name, "in");
        expect(fatal(removed.has_value())) << "the edge could not be removed: " << (removed.has_value() ? std::string{} : removed.error().message);
        expect(eq(*removed, 1UZ)) << "removing one edge of the fan-out removed a different number of edges";
        expect(eq(flow.edges().size(), 1UZ)) << "the removed edge was left in the edge list and will be resurrected on restart";
        expect(eq(flow.edges()[0].destinationBlock()->uniqueName(), std::string_view(sinkB.unique_name))) << "the wrong edge was removed";
        expect(flow.edges()[0].state() == gr::Edge::EdgeState::Connected) << "the sibling edge was left dead after the port teardown";

        // a restart must not bring the removed edge back
        flow.disconnectAllEdges();
        expect(flow.connectPendingEdges()) << "the graph did not reconnect after a restart";
        expect(eq(flow.edges().size(), 1UZ)) << "the removed edge came back on restart";

        expect(!flow.removeEdgeBySourcePort(source.unique_name, "out", sinkA.unique_name, "in").has_value()) << "removing an absent edge reported success";
    };

#ifndef GR_TEST_WITHOUT_BLOCK_REGISTRY // emplacement by name resolves the type through the registry
    "emplacing and removing blocks while the graph runs"_test = [] {
        constexpr std::size_t kCycles = 8UZ;

        qa_edit::registerTestBlocks();

        qa_edit::TestScheduler scheduler;
        {
            gr::Graph flow;
            auto&     source = flow.emplaceBlock<qa_edit::Source>();
            auto&     sink   = flow.emplaceBlock<qa_edit::Sink>();
            expect(flow.connect<"out", "in">(source, sink).has_value());
            expect(scheduler.exchange(std::move(flow)).has_value());
        }

        gr::MsgPortOut toScheduler;
        gr::MsgPortIn  fromScheduler;
        expect(toScheduler.connect(scheduler.msgIn).has_value());
        expect(scheduler.msgOut.connect(fromScheduler).has_value());

        expect(scheduler.changeStateTo(INITIALISED).has_value());
        expect(scheduler.changeStateTo(RUNNING).has_value());

        const std::string blockYaml = std::format("id: {}\nparameters:\n  gain: !!float32 2.0\n", gr::meta::type_name<qa_edit::Tunable>());
        for (std::size_t cycle = 0UZ; cycle < kCycles; ++cycle) {
            qa_edit::sendMessage(toScheduler, gr::scheduler::property::kEmplaceBlock, {{"yaml", blockYaml}});
            expect(qa_edit::awaitReply(fromScheduler, gr::scheduler::property::kBlockEmplaced)) << "cycle " << cycle << ": block was never emplaced";

            std::string emplacedName;
            for (const auto& block : scheduler.graph().blocks()) {
                if (block->typeName().find("Tunable") != std::string_view::npos) {
                    emplacedName = block->uniqueName();
                }
            }
            expect(!emplacedName.empty()) << "cycle " << cycle << ": the emplaced block is not in the graph";

            qa_edit::sendMessage(toScheduler, gr::scheduler::property::kRemoveBlock, {{"uniqueName", emplacedName}});
            expect(qa_edit::awaitReply(fromScheduler, gr::scheduler::property::kBlockRemoved)) << "cycle " << cycle << ": block was never removed";
        }

        expect(scheduler.changeStateTo(REQUESTED_STOP).has_value());
        for (std::size_t i = 0UZ; i < 3000UZ && scheduler.state() != STOPPED; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        expect(scheduler.state() == STOPPED) << "the scheduler did not stop after the edit cycles";
    };

#endif

    // the fields of an edge message come from whoever sent it, so a value of the wrong type is
    // unusable input: the message is answered as incomplete rather than ending the process, which
    // is what a terminating pointer read of the buffer size or the weight would do
    "an edge message whose weight is of the wrong type is refused"_test = [] {
        using namespace gr::serialization_fields;

        qa_edit::TestScheduler scheduler;
        {
            gr::Graph flow;
            auto&     source = flow.emplaceBlock<qa_edit::Source>();
            auto&     sink   = flow.emplaceBlock<qa_edit::Sink>();
            expect(flow.connect<"out", "in">(source, sink).has_value());
            expect(scheduler.exchange(std::move(flow)).has_value());
        }

        gr::MsgPortOut toScheduler;
        gr::MsgPortIn  fromScheduler;
        expect(toScheduler.connect(scheduler.msgIn).has_value());
        expect(scheduler.msgOut.connect(fromScheduler).has_value());

        qa_edit::sendMessage(toScheduler, gr::scheduler::property::kEmplaceEdge,
            {{std::pmr::string(EDGE_SOURCE_BLOCK), std::string("source")}, {std::pmr::string(EDGE_SOURCE_PORT), std::string("out")},           //
                {std::pmr::string(EDGE_DESTINATION_BLOCK), std::string("sink")}, {std::pmr::string(EDGE_DESTINATION_PORT), std::string("in")}, //
                {std::pmr::string(EDGE_MIN_BUFFER_SIZE), gr::undefined_Size}, {std::pmr::string(EDGE_WEIGHT), std::string("heavy")},           //
                {std::pmr::string(EDGE_NAME), std::string("wrong weight")}});

        expect(scheduler.changeStateTo(INITIALISED).has_value());
        expect(scheduler.changeStateTo(RUNNING).has_value());

        const std::string reported = qa_edit::awaitError(fromScheduler, gr::scheduler::property::kEdgeEmplaced);
        expect(!reported.empty()) << "a weight of the wrong type must be reported, not end the process";

        expect(scheduler.changeStateTo(REQUESTED_STOP).has_value());
    };

    // the same shape on the subgraph export path, and reached without a scheduler because the
    // handler answers the message the wrapper processes
    "a subgraph export message whose exportFlag is of the wrong type is refused"_test = [] {
        gr::GraphWrapper<gr::Graph> subgraph;

        gr::MsgPortOut toSubgraph;
        gr::MsgPortIn  fromSubgraph;
        expect(toSubgraph.connect(*subgraph.msgIn).has_value());
        expect(subgraph.msgOut->connect(fromSubgraph).has_value());

        qa_edit::sendMessage(toSubgraph, gr::graph::property::kSubgraphExportPort,
            {{"uniqueBlockName", std::string("inner")}, {"portDirection", std::string("output")}, //
                {"portName", std::string("out")}, {"exportFlag", std::string("yes")}});

        subgraph.processScheduledMessages();

        bool refused = false;
        auto replies = fromSubgraph.streamReader().get();
        for (const gr::Message& reply : replies) {
            refused = refused || !reply.data.has_value();
        }
        std::ignore = replies.consume(replies.size());
        expect(refused) << "an exportFlag that is not a bool must be reported, not end the process";
    };

    "an edge sized by duration follows the sample rate"_test = [] {
        using gr::graph::edgeBufferSizeFor;
        using gr::graph::kDefaultEdgeBufferSeconds;
        using gr::graph::kMaxEdgeBufferSize;
        using gr::graph::kMinEdgeBufferSize;

        constexpr double rates[] = {48.0e3, 2.4e6, 25.0e6, 61.44e6};
        for (const double rate : rates) {
            const std::size_t nSamples = edgeBufferSizeFor(rate);
            expect(eq(nSamples, std::bit_ceil(nSamples))) << rate << ": the ring is not a power of two";
            expect(ge(nSamples, kMinEdgeBufferSize)) << rate << ": the ring is below the floor";
            expect(le(nSamples, kMaxEdgeBufferSize)) << rate << ": the ring is above the ceiling";
            if (nSamples > kMinEdgeBufferSize && nSamples < kMaxEdgeBufferSize) {
                expect(ge(static_cast<double>(nSamples) / rate, kDefaultEdgeBufferSeconds)) << rate << ": the ring holds less than the stated duration";
                expect(lt(static_cast<double>(nSamples) / rate, 2.0 * kDefaultEdgeBufferSeconds)) << rate << ": rounding is the only excess allowed";
            }
        }

        expect(gt(edgeBufferSizeFor(25.0e6), edgeBufferSizeFor(2.4e6))) << "ten times the rate must not give the same ring, which is what a fixed count does";
        expect(eq(edgeBufferSizeFor(48.0e3), kMinEdgeBufferSize)) << "a rate too low to fill the smallest ring must get the smallest ring";
        expect(eq(edgeBufferSizeFor(61.44e6), kMaxEdgeBufferSize)) << "a rate asking for more than the ceiling must be held at it";
        expect(eq(edgeBufferSizeFor(0.0), kMinEdgeBufferSize)) << "an unknown rate must get the smallest ring, not an empty one";
        expect(eq(edgeBufferSizeFor(-1.0), kMinEdgeBufferSize)) << "a negative rate must get the smallest ring, not an empty one";
        expect(eq(edgeBufferSizeFor(1.0e12), kMaxEdgeBufferSize)) << "a rate asking for hundreds of megabytes must be held at the ceiling";
        expect(eq(edgeBufferSizeFor(2.4e6, 0.001), kMinEdgeBufferSize)) << "a shorter duration must reach the floor at a rate the default does not";
    };

    "a second edge into a taken stream input is refused and the refusal names both edges"_test = [] {
        gr::Graph flow;
        auto&     first  = flow.emplaceBlock<qa_edit::Source>({{"name", std::string("first")}});
        auto&     second = flow.emplaceBlock<qa_edit::Source>({{"name", std::string("second")}});
        auto&     sink   = flow.emplaceBlock<qa_edit::Sink>();
        expect(flow.connect<"out", "in">(first, sink).has_value());

        // the typed connect names the input by index and this one names it by name
        const std::expected<void, gr::Error> refused = flow.connect(second, gr::PortDefinition("out"), sink, gr::PortDefinition("in"));
        expect(fatal(!refused.has_value())) << "the second edge into the input was accepted";
        const std::string& reason = refused.error().message;
        expect(reason.contains(qa_edit::portOf("first", "0")) && reason.contains(qa_edit::portOf("second", "out"))) << "the refusal must name both edges: " << reason;

        expect(fatal(eq(flow.edges().size(), 1UZ))) << "the refused edge was listed";
        expect(eq(flow.edges()[0].sourceBlock()->uniqueName(), std::string_view(first.unique_name))) << "the refusal removed the listed edge";
        expect(flow.connectPendingEdges());
        expect(eq(first.out.nReaders(), 1UZ)) << "the listed edge carries no data";
        expect(eq(second.out.nReaders(), 0UZ)) << "the refused source feeds the input";
    };

    "a second edge into a taken message input is refused and the refusal names both edges"_test = [] {
        gr::Graph flow;
        auto&     first  = flow.emplaceBlock<qa_edit::MessageSource>({{"name", std::string("first")}});
        auto&     second = flow.emplaceBlock<qa_edit::MessageSource>({{"name", std::string("second")}});
        auto&     sink   = flow.emplaceBlock<qa_edit::MessageSink>();
        expect(flow.connect(first, gr::PortDefinition("cmd"), sink, gr::PortDefinition("cmd")).has_value());

        const std::expected<void, gr::Error> refused = flow.connect(second, gr::PortDefinition("cmd"), sink, gr::PortDefinition("cmd"));
        expect(fatal(!refused.has_value())) << "the second edge into the message input was accepted";
        const std::string& reason = refused.error().message;
        expect(reason.contains(qa_edit::portOf("first", "cmd")) && reason.contains(qa_edit::portOf("second", "cmd"))) << "the refusal must name both edges: " << reason;

        expect(fatal(eq(flow.edges().size(), 1UZ))) << "the refused edge was listed";
        expect(eq(flow.edges()[0].sourceBlock()->uniqueName(), std::string_view(first.unique_name))) << "the refusal removed the listed edge";
        expect(flow.connectPendingEdges());
        expect(eq(first.cmd.nReaders(), 1UZ)) << "the listed edge carries no message";
        expect(eq(second.cmd.nReaders(), 0UZ)) << "the refused source feeds the input";
    };

    "an edge emplaced into a taken stream input replaces the listed edge"_test = [] {
        using namespace gr::serialization_fields;

        qa_edit::TestScheduler scheduler;
        gr::Graph              flow;
        auto&                  first  = flow.emplaceBlock<qa_edit::Source>({{"name", std::string("first")}});
        auto&                  second = flow.emplaceBlock<qa_edit::Source>({{"name", std::string("second")}});
        auto&                  sink   = flow.emplaceBlock<qa_edit::Sink>();
        expect(flow.connect<"out", "in">(first, sink).has_value());
        expect(fatal(scheduler.exchange(std::move(flow)).has_value()));

        gr::MsgPortOut toScheduler;
        gr::MsgPortIn  fromScheduler;
        expect(toScheduler.connect(scheduler.msgIn).has_value());
        expect(scheduler.msgOut.connect(fromScheduler).has_value());

        expect(scheduler.changeStateTo(INITIALISED).has_value());
        expect(scheduler.changeStateTo(RUNNING).has_value());

        qa_edit::sendMessage(toScheduler, gr::scheduler::property::kEmplaceEdge,
            {{std::pmr::string(EDGE_SOURCE_BLOCK), std::string(second.unique_name)}, {std::pmr::string(EDGE_SOURCE_PORT), std::string("out")},           //
                {std::pmr::string(EDGE_DESTINATION_BLOCK), std::string(sink.unique_name)}, {std::pmr::string(EDGE_DESTINATION_PORT), std::string("in")}, //
                {std::pmr::string(EDGE_MIN_BUFFER_SIZE), gr::undefined_Size}, {std::pmr::string(EDGE_WEIGHT), std::int32_t{0}},                          //
                {std::pmr::string(EDGE_NAME), std::string("replacement")}});
        const std::optional<gr::property_map> reply = qa_edit::awaitReplyData(fromScheduler, gr::scheduler::property::kEdgeEmplaced);
        expect(fatal(reply.has_value())) << "the edge was never emplaced";

        const std::span<const gr::Edge> edges = scheduler.graph().edges();
        expect(fatal(eq(edges.size(), 1UZ))) << "the displaced edge stayed listed";
        expect(eq(edges[0].sourceBlock()->uniqueName(), std::string_view(second.unique_name))) << "the earlier edge was kept instead of the emplaced one";

        const auto* displaced = reply->contains(std::pmr::string("displacedEdges")) ? reply->at(std::pmr::string("displacedEdges")).get_if<gr::property_map>() : nullptr;
        expect(fatal(displaced != nullptr)) << "the reply does not list the displaced edges";
        expect(fatal(eq(displaced->size(), 1UZ))) << "the reply lists a different number of displaced edges";
        const auto* displacedEdge = displaced->begin()->second.get_if<gr::property_map>();
        expect(fatal(displacedEdge != nullptr));
        expect(eq(displacedEdge->at(std::pmr::string(EDGE_SOURCE_BLOCK)).value_or(std::string_view{}), std::string_view(first.unique_name))) << "the reply names another edge as displaced";
        expect(eq(displacedEdge->at(std::pmr::string(EDGE_DESTINATION_BLOCK)).value_or(std::string_view{}), std::string_view(sink.unique_name)));
        expect(qa_edit::listedNames(*reply, "sourcesWithoutReader") == std::vector{std::string(first.unique_name)}) << "the reply does not name the displaced source";
        expect(eq(first.out.nReaders(), 0UZ)) << "the displaced source still feeds the input";
        expect(eq(second.out.nReaders(), 1UZ)) << "the emplaced source does not feed the input";

        expect(scheduler.changeStateTo(REQUESTED_STOP).has_value());
        for (std::size_t i = 0UZ; i < 3000UZ && scheduler.state() != STOPPED; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        expect(scheduler.state() == STOPPED);
    };

    "an edge emplaced into a taken message input is refused and the graph keeps its edges"_test = [] {
        using namespace gr::serialization_fields;

        qa_edit::TestScheduler scheduler;
        gr::Graph              flow;
        auto&                  first  = flow.emplaceBlock<qa_edit::MessageSource>({{"name", std::string("first")}});
        auto&                  second = flow.emplaceBlock<qa_edit::MessageSource>({{"name", std::string("second")}});
        auto&                  sink   = flow.emplaceBlock<qa_edit::MessageSink>();
        expect(flow.connect<"out", "in">(first, sink).has_value());
        expect(flow.connect(first, gr::PortDefinition("cmd"), sink, gr::PortDefinition("cmd")).has_value());
        expect(fatal(scheduler.exchange(std::move(flow)).has_value()));

        gr::MsgPortOut toScheduler;
        gr::MsgPortIn  fromScheduler;
        expect(toScheduler.connect(scheduler.msgIn).has_value());
        expect(scheduler.msgOut.connect(fromScheduler).has_value());

        expect(scheduler.changeStateTo(INITIALISED).has_value());
        expect(scheduler.changeStateTo(RUNNING).has_value());

        qa_edit::sendMessage(toScheduler, gr::scheduler::property::kEmplaceEdge,
            {{std::pmr::string(EDGE_SOURCE_BLOCK), std::string(second.unique_name)}, {std::pmr::string(EDGE_SOURCE_PORT), std::string("cmd")},            //
                {std::pmr::string(EDGE_DESTINATION_BLOCK), std::string(sink.unique_name)}, {std::pmr::string(EDGE_DESTINATION_PORT), std::string("cmd")}, //
                {std::pmr::string(EDGE_MIN_BUFFER_SIZE), gr::undefined_Size}, {std::pmr::string(EDGE_WEIGHT), std::int32_t{0}},                           //
                {std::pmr::string(EDGE_NAME), std::string("second command")}});
        const std::string reason = qa_edit::awaitError(fromScheduler, gr::scheduler::property::kEdgeEmplaced);
        expect(reason.contains(qa_edit::portOf("first", "cmd")) && reason.contains(qa_edit::portOf("second", "cmd"))) << "the refusal must name both edges: " << reason;

        expect(eq(scheduler.graph().edges().size(), 2UZ)) << "the refused edge was listed";
        expect(eq(first.cmd.nReaders(), 1UZ)) << "the listed message edge lost its reader";
        expect(eq(second.cmd.nReaders(), 0UZ)) << "the refused source feeds the input";

        expect(scheduler.changeStateTo(REQUESTED_STOP).has_value());
        for (std::size_t i = 0UZ; i < 3000UZ && scheduler.state() != STOPPED; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        expect(scheduler.state() == STOPPED);
    };

    // the functions the RemoveEdge and EmplaceEdge handlers call, in the order a rewire takes
    "an input freed by removing its edge takes a new edge without a replacement"_test = [] {
        gr::Graph flow;
        auto&     first  = flow.emplaceBlock<qa_edit::Source>({{"name", std::string("first")}});
        auto&     second = flow.emplaceBlock<qa_edit::Source>({{"name", std::string("second")}});
        auto&     sink   = flow.emplaceBlock<qa_edit::Sink>();
        expect(flow.connect<"out", "in">(first, sink).has_value());
        expect(flow.connectPendingEdges());

        const auto removed = flow.removeEdgeBySourcePort(first.unique_name, "out", sink.unique_name, "in");
        expect(fatal(removed.has_value())) << "the edge could not be removed: " << (removed.has_value() ? std::string{} : removed.error().message);

        const std::expected<std::vector<gr::Edge>, gr::Error> emplaced = flow.emplaceEdge(second.unique_name, "out", sink.unique_name, "in", gr::undefined_size, 0, "rewired");
        expect(fatal(emplaced.has_value())) << "the edge could not be emplaced: " << (emplaced.has_value() ? std::string{} : emplaced.error().message);
        expect(emplaced->empty()) << "a free input returned a displaced edge";

        expect(fatal(eq(flow.edges().size(), 1UZ))) << "the rewire left a different number of edges";
        expect(eq(flow.edges()[0].sourceBlock()->uniqueName(), std::string_view(second.unique_name))) << "the rewired edge is not the one listed";
        expect(eq(first.out.nReaders(), 0UZ)) << "the removed source still feeds the input";
        expect(eq(second.out.nReaders(), 1UZ)) << "the rewired source does not feed the input";
    };
};

const boost::ut::suite<"messages after a job list ends"> laterMessageTests = [] {
    using namespace boost::ut;
    using enum gr::lifecycle::State;

    // under multiThreaded each job list has a worker of its own, and a worker leaves once every block of its job list
    // is done. Here the first job list ends at once and the second runs until the stop.
    "the scheduler replies after its first job list has ended"_test = [] {
        constexpr std::string_view kPoolName = "qa_edit_two_threads";
        gr::thread_pool::Manager::instance().replacePool(std::string(kPoolName), std::make_shared<gr::thread_pool::ThreadPoolWrapper>(std::make_unique<gr::thread_pool::BasicThreadPool>(kPoolName, gr::thread_pool::TaskType::CPU_BOUND, 2U, 2U), "CPU"));

        gr::Graph flow;
        auto&     ending     = flow.emplaceBlock<qa_edit::EndingSource>();
        auto&     endingSink = flow.emplaceBlock<qa_edit::Sink>();
        auto&     source     = flow.emplaceBlock<qa_edit::Source>();
        auto&     sink       = flow.emplaceBlock<qa_edit::Sink>();
        expect(flow.connect<"out", "in">(ending, endingSink).has_value());
        expect(flow.connect<"out", "in">(source, sink).has_value());
        const std::string endingName{ending.unique_name};

        qa_edit::JobListProbe scheduler({{"poolName", std::string(kPoolName)}});
        expect(scheduler.exchange(std::move(flow)).has_value());

        gr::MsgPortOut toScheduler;
        gr::MsgPortIn  fromScheduler;
        expect(toScheduler.connect(scheduler.msgIn).has_value());
        expect(scheduler.msgOut.connect(fromScheduler).has_value());

        qa_edit::sendMessage(toScheduler, gr::scheduler::property::kSchedulerInspect, {});
        expect(scheduler.changeStateTo(INITIALISED).has_value());
        expect(scheduler.changeStateTo(RUNNING).has_value());
        expect(qa_edit::takeReply(fromScheduler, gr::scheduler::property::kSchedulerInspected)) << "the request sent before the run got no reply";

        expect(fatal(eq(scheduler.nJobLists(), 2UZ))) << "the graph was not split into two job lists";
        expect(fatal(scheduler.firstJobListHolds(endingName))) << "the ending source is not in the first job list";
        expect(fatal(qa_edit::awaitCondition([&scheduler] { return scheduler.nRunningJobs() == 1UZ; }))) << "the first job list did not end";

        qa_edit::sendMessage(toScheduler, gr::scheduler::property::kSchedulerInspect, {});
        expect(qa_edit::takeReply(fromScheduler, gr::scheduler::property::kSchedulerInspected)) << "the request sent after the first job list ended got no reply";

        expect(scheduler.changeStateTo(REQUESTED_STOP).has_value());
        expect(qa_edit::awaitCondition([&scheduler] { return scheduler.state() == STOPPED; })) << "the scheduler did not stop";
    };

    // no reader is connected to msgOut, so a child's error ends the run. The first job list ends at once, and the
    // source in the second one reports its error later
    "a child's error after the first job list has ended fails runAndWait()"_test = [] {
        constexpr std::string_view kPoolName = "qa_edit_two_threads";
        gr::thread_pool::Manager::instance().replacePool(std::string(kPoolName), std::make_shared<gr::thread_pool::ThreadPoolWrapper>(std::make_unique<gr::thread_pool::BasicThreadPool>(kPoolName, gr::thread_pool::TaskType::CPU_BOUND, 2U, 2U), "CPU"));

        gr::Graph flow;
        auto&     ending      = flow.emplaceBlock<qa_edit::EndingSource>();
        auto&     endingSink  = flow.emplaceBlock<qa_edit::Sink>();
        auto&     failing     = flow.emplaceBlock<qa_edit::LateErrorSource>();
        auto&     failingSink = flow.emplaceBlock<qa_edit::Sink>();
        expect(flow.connect<"out", "in">(ending, endingSink).has_value());
        expect(flow.connect<"out", "in">(failing, failingSink).has_value());
        const std::string endingName{ending.unique_name};
        const std::string failingName{failing.unique_name};
        const auto*       failingBlock = &failing;

        qa_edit::JobListProbe scheduler({{"poolName", std::string(kPoolName)}});
        expect(scheduler.exchange(std::move(flow)).has_value());

        const std::expected<void, gr::Error> result = scheduler.runAndWait();

        expect(fatal(eq(scheduler.nJobLists(), 2UZ))) << "the graph was not split into two job lists";
        expect(fatal(scheduler.firstJobListHolds(endingName))) << "the ending source is not in the first job list";
        expect(fatal(!result.has_value())) << "runAndWait() reported success";
        expect(result.error().message.find(failingName) != std::string::npos) << "the error must name the block: " << result.error().message;
        expect(result.error().message.find(qa_edit::kLateFault) != std::string::npos) << "the error must carry the block's reason: " << result.error().message;
        expect(scheduler.state() == ERROR) << "the run did not end in ERROR";
        expect(lt(failingBlock->_nCalls, qa_edit::LateErrorSource::kCallsToEnd)) << "the run went on to the source's own end";
    };

#ifndef GR_TEST_WITHOUT_BLOCK_REGISTRY // emplacement by name resolves the type through the registry
    "a block added after the first job list has ended runs"_test = [] {
        constexpr std::size_t      kBlocks   = 4UZ;
        constexpr std::string_view kPoolName = "qa_edit_two_threads";
        gr::thread_pool::Manager::instance().replacePool(std::string(kPoolName), std::make_shared<gr::thread_pool::ThreadPoolWrapper>(std::make_unique<gr::thread_pool::BasicThreadPool>(kPoolName, gr::thread_pool::TaskType::CPU_BOUND, 2U, 2U), "CPU"));
        qa_edit::registerTestBlocks();
        qa_edit::gWorkingTickers.store(0UZ);

        gr::Graph flow;
        auto&     ending     = flow.emplaceBlock<qa_edit::EndingSource>();
        auto&     endingSink = flow.emplaceBlock<qa_edit::Sink>();
        auto&     source     = flow.emplaceBlock<qa_edit::Source>();
        auto&     sink       = flow.emplaceBlock<qa_edit::Sink>();
        expect(flow.connect<"out", "in">(ending, endingSink).has_value());
        expect(flow.connect<"out", "in">(source, sink).has_value());
        const std::string endingName{ending.unique_name};

        qa_edit::JobListProbe scheduler({{"poolName", std::string(kPoolName)}});
        expect(scheduler.exchange(std::move(flow)).has_value());

        gr::MsgPortOut toScheduler;
        gr::MsgPortIn  fromScheduler;
        expect(toScheduler.connect(scheduler.msgIn).has_value());
        expect(scheduler.msgOut.connect(fromScheduler).has_value());

        expect(scheduler.changeStateTo(INITIALISED).has_value());
        expect(scheduler.changeStateTo(RUNNING).has_value());
        expect(fatal(eq(scheduler.nJobLists(), 2UZ))) << "the graph was not split into two job lists";
        expect(fatal(scheduler.firstJobListHolds(endingName))) << "the ending source is not in the first job list";
        expect(fatal(qa_edit::awaitCondition([&scheduler] { return scheduler.nRunningJobs() == 1UZ; }))) << "the first job list did not end";

        for (std::size_t i = 0UZ; i < kBlocks; ++i) {
            qa_edit::sendMessage(toScheduler, gr::scheduler::property::kEmplaceBlock, {{"type", gr::meta::type_name<qa_edit::Ticker>()}});
            expect(qa_edit::takeReply(fromScheduler, gr::scheduler::property::kBlockEmplaced)) << std::format("block {} was never emplaced", i);
        }
        expect(qa_edit::awaitCondition([] { return qa_edit::gWorkingTickers.load() == kBlocks; })) << std::format("{} of {} added blocks ran", qa_edit::gWorkingTickers.load(), kBlocks);

        expect(scheduler.changeStateTo(REQUESTED_STOP).has_value());
        expect(qa_edit::awaitCondition([&scheduler] { return scheduler.state() == STOPPED; })) << "the scheduler did not stop";
    };
#endif
};

#ifndef GR_TEST_WITHOUT_BLOCK_REGISTRY // emplacement and replacement by name resolve the type through the registry
const boost::ut::suite<"edit replies"> editReplyTests = [] {
    using namespace boost::ut;
    using enum gr::lifecycle::State;
    using namespace gr::scheduler::property;
    using qa_edit::EditRequest;

    "an EmplaceBlock request is answered under BlockEmplaced with its id"_test = [] {
        qa_edit::registerTestBlocks();
        qa_edit::TestScheduler scheduler;
        std::ignore = qa_edit::exchangeChain(scheduler);
        const std::string tunable{gr::meta::type_name<qa_edit::Tunable>()};

        const std::array requests{EditRequest{"emplaced", kEmplaceBlock, {{"type", tunable}}, true}, //
            EditRequest{"empty type", kEmplaceBlock, {{"type", std::string()}}},                     //
            EditRequest{"no type", kEmplaceBlock, {}},                                               //
            EditRequest{"unknown type", kEmplaceBlock, {{"type", std::string("qa_edit::Absent")}}},  //
            EditRequest{"empty yaml", kEmplaceBlock, {{"yaml", std::string()}}},                     //
            EditRequest{"no target graph", kEmplaceBlock, {{"type", tunable}, {"_targetGraph", std::string("absent")}}}};
        qa_edit::expectRepliesUnder(scheduler, kBlockEmplaced, requests);
    };

    "a RemoveBlock request is answered under BlockRemoved with its id"_test = [] {
        qa_edit::registerTestBlocks();
        qa_edit::TestScheduler    scheduler;
        const qa_edit::ChainNames chain = qa_edit::exchangeChain(scheduler);

        const std::array requests{EditRequest{"empty name", kRemoveBlock, {{"uniqueName", std::string()}}}, //
            EditRequest{"no name", kRemoveBlock, {}},                                                       //
            EditRequest{"absent block", kRemoveBlock, {{"uniqueName", std::string("absent")}}},             //
            EditRequest{"removed", kRemoveBlock, {{"uniqueName", chain.spareSink}}, true}};
        qa_edit::expectRepliesUnder(scheduler, kBlockRemoved, requests);
    };

    "a RemoveEdge request is answered under EdgeRemoved with its id"_test = [] {
        using namespace gr::serialization_fields;
        qa_edit::registerTestBlocks();
        qa_edit::TestScheduler    scheduler;
        const qa_edit::ChainNames chain = qa_edit::exchangeChain(scheduler);
        expect(fatal(scheduler.graph().connectPendingEdges())) << "the chain did not connect"; // an edge is removed from its connected ports

        const std::array requests{EditRequest{"empty source", kRemoveEdge, {{std::pmr::string(EDGE_SOURCE_BLOCK), std::string()}, {std::pmr::string(EDGE_SOURCE_PORT), std::string()}}}, //
            EditRequest{"no source", kRemoveEdge, {}},                                                                                                                                   //
            EditRequest{"absent edge", kRemoveEdge, {{std::pmr::string(EDGE_SOURCE_BLOCK), chain.source}, {std::pmr::string(EDGE_SOURCE_PORT), std::string("out")}, {std::pmr::string(EDGE_DESTINATION_BLOCK), std::string("absent")}, {std::pmr::string(EDGE_DESTINATION_PORT), std::string("in")}}}, EditRequest{"removed", kRemoveEdge, {{std::pmr::string(EDGE_SOURCE_BLOCK), chain.source}, {std::pmr::string(EDGE_SOURCE_PORT), std::string("out")}}, true}};
        qa_edit::expectRepliesUnder(scheduler, kEdgeRemoved, requests);
    };

    "an EmplaceEdge request is answered under EdgeEmplaced with its id"_test = [] {
        qa_edit::registerTestBlocks();
        qa_edit::TestScheduler    scheduler;
        const qa_edit::ChainNames chain = qa_edit::exchangeChain(scheduler);

        const std::array requests{EditRequest{"incomplete", kEmplaceEdge, {}},                                                    //
            EditRequest{"wrong weight", kEmplaceEdge, qa_edit::edgeRequest(chain.source, chain.spareSink, std::string("heavy"))}, //
            EditRequest{"absent source", kEmplaceEdge, qa_edit::edgeRequest("absent", chain.spareSink, std::int32_t{0})},         //
            EditRequest{"emplaced", kEmplaceEdge, qa_edit::edgeRequest(chain.source, chain.spareSink, std::int32_t{0}), true}};
        qa_edit::expectRepliesUnder(scheduler, kEdgeEmplaced, requests);
    };

    "a ReplaceBlock request is answered under BlockReplaced with its id"_test = [] {
        qa_edit::registerTestBlocks();
        qa_edit::TestScheduler    scheduler;
        const qa_edit::ChainNames chain = qa_edit::exchangeChain(scheduler);
        const std::string         tunable{gr::meta::type_name<qa_edit::Tunable>()};

        const std::array requests{EditRequest{"empty name", kReplaceBlock, {{"uniqueName", std::string()}, {"type", tunable}}}, //
            EditRequest{"no type", kReplaceBlock, {{"uniqueName", chain.stage}}},                                               //
            EditRequest{"absent block", kReplaceBlock, {{"uniqueName", std::string("absent")}, {"type", tunable}}},             //
            EditRequest{"replaced", kReplaceBlock, {{"uniqueName", chain.stage}, {"type", tunable}}, true}};
        qa_edit::expectRepliesUnder(scheduler, kBlockReplaced, requests);
    };

    "a GraphGRC request is answered under GraphGRC with its id"_test = [] {
        qa_edit::registerTestBlocks();
        qa_edit::TestScheduler scheduler;
        std::ignore = qa_edit::exchangeChain(scheduler);

        const std::array requests{EditRequest{"read", kGraphGRC, {}, true, gr::message::Command::Get}, //
            EditRequest{"empty yaml", kGraphGRC, {{"value", std::string()}}},                          //
            EditRequest{"no yaml", kGraphGRC, {}},                                                     //
            EditRequest{"exchanged", kGraphGRC, {{"value", gr::saveGrc(gr::globalPluginLoader(), gr::Graph{})}}, true}};
        qa_edit::expectRepliesUnder(scheduler, kGraphGRC, requests);
    };

    // the run's two job lists end at once, and the scheduler stays RUNNING with no worker left to take the block
    "a block emplaced after every worker of the run has left is answered with the reason"_test = [] {
        constexpr std::string_view kPoolName = "qa_edit_two_threads";
        gr::thread_pool::Manager::instance().replacePool(std::string(kPoolName), std::make_shared<gr::thread_pool::ThreadPoolWrapper>(std::make_unique<gr::thread_pool::BasicThreadPool>(kPoolName, gr::thread_pool::TaskType::CPU_BOUND, 2U, 2U), "CPU"));
        qa_edit::registerTestBlocks();
        qa_edit::gWorkingTickers.store(0UZ);

        gr::Graph flow;
        auto&     ending     = flow.emplaceBlock<qa_edit::EndingSource>();
        auto&     endingSink = flow.emplaceBlock<qa_edit::Sink>();
        expect(flow.connect<"out", "in">(ending, endingSink).has_value());

        qa_edit::JobListProbe scheduler({{"poolName", std::string(kPoolName)}});
        expect(scheduler.exchange(std::move(flow)).has_value());

        gr::MsgPortOut toScheduler;
        gr::MsgPortIn  fromScheduler;
        expect(toScheduler.connect(scheduler.msgIn).has_value());
        expect(scheduler.msgOut.connect(fromScheduler).has_value());

        expect(scheduler.changeStateTo(INITIALISED).has_value());
        expect(scheduler.changeStateTo(RUNNING).has_value());
        expect(fatal(qa_edit::awaitCondition([&scheduler] { return scheduler.nRunningJobs() == 0UZ; }))) << "the run's workers did not leave";
        expect(fatal(scheduler.state() == RUNNING));

        gr::sendMessage<gr::message::Command::Set>(toScheduler, scheduler.unique_name, kEmplaceBlock, {{"type", gr::meta::type_name<qa_edit::Ticker>()}}, "late");
        scheduler.processScheduledMessages();
        const std::optional<gr::Message> reply = qa_edit::takeReplyTo(fromScheduler, "late");
        expect(fatal(reply.has_value())) << "the request got no reply with its id";
        expect(eq(reply->endpoint, std::string(kBlockEmplaced)));
        expect(fatal(!reply->data.has_value())) << "the reply says the block joined a run that has no worker";
        expect(reply->data.error().message.contains("next start")) << "the reply does not say when the block runs: " << reply->data.error().message;
        expect(eq(qa_edit::gWorkingTickers.load(), 0UZ));

        expect(scheduler.changeStateTo(REQUESTED_STOP).has_value());
        expect(qa_edit::awaitCondition([&scheduler] { return scheduler.state() == STOPPED; })) << "the scheduler did not stop";
    };
};

const boost::ut::suite<"a replacement in a running graph"> runningReplacementTests = [] {
    using namespace boost::ut;
    using enum gr::lifecycle::State;
    using namespace gr::scheduler::property;

    "a replacement in a multi-threaded run passes on the samples queued at its ports"_test = [] { qa_edit::expectQueuedSamplesPassReplacement<gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::multiThreaded>>(); };

    "a replacement in a single-threaded run passes on the samples queued at its ports"_test = [] { qa_edit::expectQueuedSamplesPassReplacement<gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::singleThreaded>>(); };

    "a replacement in a single-threaded blocking run passes on the samples queued at its ports"_test = [] { qa_edit::expectQueuedSamplesPassReplacement<gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::singleThreadedBlocking>>(); };

    // a pool of three threads splits the three blocks into three job lists
    "a replacement joins the job list of the block it replaces, and the reply names that job list"_test = [] {
        constexpr std::string_view kPoolName = "qa_edit_three_threads";
        gr::thread_pool::Manager::instance().replacePool(std::string(kPoolName), std::make_shared<gr::thread_pool::ThreadPoolWrapper>(std::make_unique<gr::thread_pool::BasicThreadPool>(kPoolName, gr::thread_pool::TaskType::CPU_BOUND, 3U, 3U), "CPU"));
        qa_edit::registerTestBlocks();
        qa_edit::gCountedSamples.store(0UZ);

        gr::Graph flow;
        auto&     source = flow.emplaceBlock<qa_edit::Source>();
        auto&     stage  = flow.emplaceBlock<qa_edit::Tunable>();
        auto&     sink   = flow.emplaceBlock<qa_edit::Sink>();
        expect(flow.connect<"out", "in">(source, stage).has_value());
        expect(flow.connect<"out", "in">(stage, sink).has_value());
        const std::string stageName{stage.unique_name};

        qa_edit::JobListProbe scheduler({{"poolName", std::string(kPoolName)}});
        expect(scheduler.exchange(std::move(flow)).has_value());
        gr::MsgPortOut toScheduler;
        gr::MsgPortIn  fromScheduler;
        expect(toScheduler.connect(scheduler.msgIn).has_value());
        expect(scheduler.msgOut.connect(fromScheduler).has_value());

        expect(scheduler.changeStateTo(INITIALISED).has_value());
        expect(scheduler.changeStateTo(RUNNING).has_value());
        expect(eq(scheduler.nJobLists(), 3UZ)) << "the graph was not split into three job lists";
        const std::optional<std::size_t> heldBy = scheduler.jobListOf(stageName);
        expect(heldBy.has_value()) << "no job list holds the stage";

        qa_edit::sendMessage(toScheduler, kReplaceBlock, {{"uniqueName", stageName}, {"type", gr::meta::type_name<qa_edit::CountedTunable>()}});
        const std::optional<gr::property_map> reply = qa_edit::awaitReplyData(fromScheduler, kBlockReplaced);
        expect(reply.has_value()) << "the replacement was refused";
        const qa_edit::ReplacementRecord record = reply.has_value() ? qa_edit::replacementRecord(*reply) : qa_edit::ReplacementRecord{};
        expect(record.jobList.has_value() && record.jobList == heldBy) << "the reply does not name the job list of the replaced block";
        expect(qa_edit::awaitCondition([&scheduler, &record, &heldBy] { return scheduler.jobListOf(record.uniqueName) == heldBy; })) << "the replacement runs in another job list";
        expect(qa_edit::awaitCondition([] { return qa_edit::gCountedSamples.load() > 0UZ; })) << "the replacement never ran";

        expect(scheduler.changeStateTo(REQUESTED_STOP).has_value());
        expect(qa_edit::awaitCondition([&scheduler] { return scheduler.state() == STOPPED; })) << "the scheduler did not stop";
    };

    "a replacement inside a subgraph of a running graph runs"_test = [] {
        qa_edit::registerTestBlocks();
        qa_edit::gCountedSamples.store(0UZ);

        gr::Graph flow;
        auto      wrapper = std::make_shared<gr::GraphWrapper<gr::Graph>>();
        auto&     inner   = *wrapper->graph();
        auto&     source  = inner.emplaceBlock<qa_edit::Source>();
        auto&     stage   = inner.emplaceBlock<qa_edit::Tunable>();
        auto&     sink    = inner.emplaceBlock<qa_edit::Sink>();
        expect(inner.connect<"out", "in">(source, stage).has_value());
        expect(inner.connect<"out", "in">(stage, sink).has_value());
        const std::string stageName{stage.unique_name};
        const std::string subgraphName{flow.addBlock(wrapper)->uniqueName()};

        qa_edit::TestScheduler scheduler;
        expect(scheduler.exchange(std::move(flow)).has_value());
        gr::MsgPortOut toScheduler;
        gr::MsgPortIn  fromScheduler;
        expect(toScheduler.connect(scheduler.msgIn).has_value());
        expect(scheduler.msgOut.connect(fromScheduler).has_value());

        expect(scheduler.changeStateTo(INITIALISED).has_value());
        expect(scheduler.changeStateTo(RUNNING).has_value());

        qa_edit::sendMessage(toScheduler, kReplaceBlock, {{"uniqueName", stageName}, {"type", gr::meta::type_name<qa_edit::CountedTunable>()}, {"_targetGraph", subgraphName}});
        expect(qa_edit::awaitReplyData(fromScheduler, kBlockReplaced).has_value()) << "the replacement was refused";
        expect(qa_edit::awaitCondition([] { return qa_edit::gCountedSamples.load() > 0UZ; })) << "the replacement in the subgraph never ran";

        expect(scheduler.changeStateTo(REQUESTED_STOP).has_value());
        expect(qa_edit::awaitCondition([&scheduler] { return scheduler.state() == STOPPED; })) << "the scheduler did not stop";
    };

    // A pool of two threads splits the five blocks into two job lists, and each holds a free-running source and sink.
    // Whichever worker handles the replacement, the other one works a free-running pair unless it is held
    "a replaced block stops while no block of the run is inside work()"_test = [] {
        constexpr std::string_view kPoolName = "qa_edit_two_threads";
        gr::thread_pool::Manager::instance().replacePool(std::string(kPoolName), std::make_shared<gr::thread_pool::ThreadPoolWrapper>(std::make_unique<gr::thread_pool::BasicThreadPool>(kPoolName, gr::thread_pool::TaskType::CPU_BOUND, 2U, 2U), "CPU"));
        qa_edit::registerTestBlocks();
        qa_edit::gFreeRunningSamples.store(0UZ);
        qa_edit::gSamplesDuringStop.store(0UZ);
        qa_edit::gStopProbed.store(false);

        gr::Graph flow;
        auto&     firstSource  = flow.emplaceBlock<qa_edit::Source>();
        auto&     firstSink    = flow.emplaceBlock<qa_edit::FreeSink>();
        auto&     secondSource = flow.emplaceBlock<qa_edit::Source>();
        auto&     secondSink   = flow.emplaceBlock<qa_edit::FreeSink>();
        auto&     probe        = flow.emplaceBlock<qa_edit::StopProbe>();
        expect(flow.connect<"out", "in">(firstSource, firstSink).has_value());
        expect(flow.connect<"out", "in">(secondSource, secondSink).has_value());
        const std::string probeName{probe.unique_name};

        qa_edit::JobListProbe scheduler({{"poolName", std::string(kPoolName)}});
        expect(scheduler.exchange(std::move(flow)).has_value());
        gr::MsgPortOut toScheduler;
        gr::MsgPortIn  fromScheduler;
        expect(toScheduler.connect(scheduler.msgIn).has_value());
        expect(scheduler.msgOut.connect(fromScheduler).has_value());

        expect(scheduler.changeStateTo(INITIALISED).has_value());
        expect(scheduler.changeStateTo(RUNNING).has_value());
        expect(eq(scheduler.nJobLists(), 2UZ)) << "the graph was not split into two job lists";
        expect(qa_edit::awaitCondition([] { return qa_edit::gFreeRunningSamples.load() > 0UZ; })) << "the free-running sinks never ran";

        qa_edit::sendMessage(toScheduler, kReplaceBlock, {{"uniqueName", probeName}, {"type", gr::meta::type_name<qa_edit::Ticker>()}});
        expect(qa_edit::awaitReplyData(fromScheduler, kBlockReplaced).has_value()) << "the replacement was refused";
        expect(qa_edit::awaitCondition([] { return qa_edit::gStopProbed.load(); })) << "the replaced block's stop() hook never ran";
        expect(eq(qa_edit::gSamplesDuringStop.load(), 0UZ)) << "another block worked while the replaced block stopped";

        expect(scheduler.changeStateTo(REQUESTED_STOP).has_value());
        expect(qa_edit::awaitCondition([&scheduler] { return scheduler.state() == STOPPED; })) << "the scheduler did not stop";
    };

    // the replacement's settingsChanged() holds the edit until the stop has begun
    "a stop that begins during a replacement leaves the replacement out of the run"_test = [] {
        qa_edit::registerTestBlocks();
        qa_edit::gRacingEntered.store(false);
        {
            gr::Graph flow;
            auto&     source = flow.emplaceBlock<qa_edit::Source>();
            auto&     stage  = flow.emplaceBlock<qa_edit::Tunable>();
            auto&     sink   = flow.emplaceBlock<qa_edit::Sink>();
            expect(flow.connect<"out", "in">(source, stage).has_value());
            expect(flow.connect<"out", "in">(stage, sink).has_value());
            const std::string stageName{stage.unique_name};

            qa_edit::TestScheduler scheduler;
            expect(scheduler.exchange(std::move(flow)).has_value());
            gr::MsgPortOut toScheduler;
            gr::MsgPortIn  fromScheduler;
            expect(toScheduler.connect(scheduler.msgIn).has_value());
            expect(scheduler.msgOut.connect(fromScheduler).has_value());
            qa_edit::gSchedulerState = [&scheduler] { return scheduler.state(); };

            expect(scheduler.changeStateTo(INITIALISED).has_value());
            expect(scheduler.changeStateTo(RUNNING).has_value());

            qa_edit::sendMessage(toScheduler, kReplaceBlock, {{"uniqueName", stageName}, {"type", gr::meta::type_name<qa_edit::RacingStage>()}, {"properties", gr::property_map{{"gain", 2.0f}}}});
            expect(qa_edit::awaitCondition([] { return qa_edit::gRacingEntered.load(); })) << "the replacement's settings were never applied";
            expect(scheduler.changeStateTo(REQUESTED_STOP).has_value());

            const std::optional<gr::property_map> reply = qa_edit::awaitReplyData(fromScheduler, kBlockReplaced);
            expect(reply.has_value()) << "the replacement was refused";
            const qa_edit::ReplacementRecord record = reply.has_value() ? qa_edit::replacementRecord(*reply) : qa_edit::ReplacementRecord{};
            expect(!record.jobList.has_value()) << "the replacement joined a run whose stop had begun";
            expect(qa_edit::awaitCondition([&scheduler] { return scheduler.state() == STOPPED; })) << "the scheduler did not stop";
            for (const std::shared_ptr<gr::BlockModel>& block : scheduler.graph().blocks()) {
                if (block->uniqueName() == record.uniqueName) {
                    expect(block->state() != RUNNING) << "the stop left the replacement running";
                }
            }
        }
        qa_edit::gSchedulerState = nullptr;
    };
};
#endif

const boost::ut::suite<"edits that give a block a reader or take its last one"> readerEditTests = [] {
    using namespace boost::ut;
    using enum gr::lifecycle::State;
    using namespace gr::scheduler::property;

    "a RemoveEdge reply names a source that the removal leaves without a reader"_test = [] {
        gr::Graph flow;
        auto&     source  = flow.emplaceBlock<qa_edit::Source>();
        auto&     kept    = flow.emplaceBlock<qa_edit::Sink>();
        auto&     removed = flow.emplaceBlock<qa_edit::Sink>();
        expect(flow.connect<"out", "in">(source, kept).has_value());
        expect(flow.connect<"out", "in">(source, removed).has_value());
        const std::string sourceName{source.unique_name};
        const std::string keptName{kept.unique_name};
        const std::string removedName{removed.unique_name};

        qa_edit::TestScheduler scheduler;
        expect(fatal(scheduler.exchange(std::move(flow)).has_value()));
        gr::MsgPortOut toScheduler;
        gr::MsgPortIn  fromScheduler;
        expect(fatal(toScheduler.connect(scheduler.msgIn).has_value()));
        expect(fatal(scheduler.msgOut.connect(fromScheduler).has_value()));
        expect(scheduler.changeStateTo(INITIALISED).has_value());
        expect(scheduler.changeStateTo(RUNNING).has_value());

        gr::sendMessage<gr::message::Command::Set>(toScheduler, "", kRemoveEdge, qa_edit::removeEdgeRequest(sourceName, removedName), "fan-out");
        const std::optional<gr::Message> fanOutReply = qa_edit::takeReplyTo(fromScheduler, "fan-out");
        expect(fatal(fanOutReply.has_value() && fanOutReply->data.has_value())) << "the removal of one edge of the fan-out was refused";
        expect(fanOutReply->data->find("sourcesWithoutReader") == fanOutReply->data->end()) << "the reply names a source that keeps a reader";

        gr::sendMessage<gr::message::Command::Set>(toScheduler, "", kRemoveEdge, qa_edit::removeEdgeRequest(sourceName, keptName), "last");
        const std::optional<gr::Message> lastReply = qa_edit::takeReplyTo(fromScheduler, "last");
        expect(fatal(lastReply.has_value() && lastReply->data.has_value())) << "the removal of the last edge was refused";
        expect(qa_edit::listedNames(*lastReply->data, "sourcesWithoutReader") == std::vector{sourceName}) << "the reply does not name the source left without a reader";

        expect(scheduler.changeStateTo(REQUESTED_STOP).has_value());
        expect(qa_edit::awaitCondition([&scheduler] { return scheduler.state() == STOPPED; })) << "the scheduler did not stop";
    };

    // a block stops for want of a reader when no mandatory stream output has one
    "a RemoveEdge reply names a source whose remaining edges leave an optional or a message output"_test = [] {
        gr::Graph flow;
        auto&     monitored   = flow.emplaceBlock<qa_edit::MonitoredSource>();
        auto&     mainSink    = flow.emplaceBlock<qa_edit::Sink>();
        auto&     monitorSink = flow.emplaceBlock<qa_edit::Sink>();
        auto&     messenger   = flow.emplaceBlock<qa_edit::MessageSource>();
        auto&     messageSink = flow.emplaceBlock<qa_edit::MessageSink>();
        expect(flow.connect<"out", "in">(monitored, mainSink).has_value());
        expect(flow.connect<"monitor", "in">(monitored, monitorSink).has_value());
        expect(flow.connect<"out", "in">(messenger, messageSink).has_value());
        expect(flow.connect<"cmd", "cmd">(messenger, messageSink).has_value());
        const std::string monitoredName{monitored.unique_name};
        const std::string messengerName{messenger.unique_name};

        qa_edit::TestScheduler scheduler;
        expect(fatal(scheduler.exchange(std::move(flow)).has_value()));
        gr::MsgPortOut toScheduler;
        gr::MsgPortIn  fromScheduler;
        expect(fatal(toScheduler.connect(scheduler.msgIn).has_value()));
        expect(fatal(scheduler.msgOut.connect(fromScheduler).has_value()));
        expect(scheduler.changeStateTo(INITIALISED).has_value());
        expect(scheduler.changeStateTo(RUNNING).has_value());

        gr::sendMessage<gr::message::Command::Set>(toScheduler, "", kRemoveEdge, qa_edit::removeEdgeRequest(monitoredName, std::string(mainSink.unique_name)), "optional");
        const std::optional<gr::Message> optionalReply = qa_edit::takeReplyTo(fromScheduler, "optional");
        expect(fatal(optionalReply.has_value() && optionalReply->data.has_value())) << "the removal of the mandatory output's edge was refused";
        expect(qa_edit::listedNames(*optionalReply->data, "sourcesWithoutReader") == std::vector{monitoredName}) << "the reply does not name a source whose optional output keeps an edge";

        gr::sendMessage<gr::message::Command::Set>(toScheduler, "", kRemoveEdge, qa_edit::removeEdgeRequest(messengerName, std::string(messageSink.unique_name)), "message");
        const std::optional<gr::Message> messageReply = qa_edit::takeReplyTo(fromScheduler, "message");
        expect(fatal(messageReply.has_value() && messageReply->data.has_value())) << "the removal of the stream edge was refused";
        expect(qa_edit::listedNames(*messageReply->data, "sourcesWithoutReader") == std::vector{messengerName}) << "the reply does not name a source whose message output keeps an edge";

        expect(scheduler.changeStateTo(REQUESTED_STOP).has_value());
        expect(qa_edit::awaitCondition([&scheduler] { return scheduler.state() == STOPPED; })) << "the scheduler did not stop";
    };

    "a RemoveBlock reply names a source that the removal leaves without a reader"_test = [] {
        gr::Graph flow;
        auto&     shared    = flow.emplaceBlock<qa_edit::Source>();
        auto&     keptStage = flow.emplaceBlock<qa_edit::Tunable>();
        auto&     keptSink  = flow.emplaceBlock<qa_edit::Sink>();
        auto&     lone      = flow.emplaceBlock<qa_edit::Source>();
        auto&     loneStage = flow.emplaceBlock<qa_edit::Tunable>();
        expect(flow.connect<"out", "in">(shared, keptStage).has_value());
        expect(flow.connect<"out", "in">(shared, keptSink).has_value());
        expect(flow.connect<"out", "in">(lone, loneStage).has_value());
        const std::string keptStageName{keptStage.unique_name};
        const std::string loneName{lone.unique_name};
        const std::string loneStageName{loneStage.unique_name};

        qa_edit::TestScheduler scheduler;
        expect(fatal(scheduler.exchange(std::move(flow)).has_value()));
        gr::MsgPortOut toScheduler;
        gr::MsgPortIn  fromScheduler;
        expect(fatal(toScheduler.connect(scheduler.msgIn).has_value()));
        expect(fatal(scheduler.msgOut.connect(fromScheduler).has_value()));
        expect(scheduler.changeStateTo(INITIALISED).has_value());
        expect(scheduler.changeStateTo(RUNNING).has_value());

        gr::sendMessage<gr::message::Command::Set>(toScheduler, "", kRemoveBlock, {{"uniqueName", keptStageName}}, "kept");
        const std::optional<gr::Message> keptReply = qa_edit::takeReplyTo(fromScheduler, "kept");
        expect(fatal(keptReply.has_value() && keptReply->data.has_value())) << "the removal of the stage of the fan-out was refused";
        expect(keptReply->data->find("sourcesWithoutReader") == keptReply->data->end()) << "the reply names a source that keeps a reader";

        gr::sendMessage<gr::message::Command::Set>(toScheduler, "", kRemoveBlock, {{"uniqueName", loneStageName}}, "lone");
        const std::optional<gr::Message> loneReply = qa_edit::takeReplyTo(fromScheduler, "lone");
        expect(fatal(loneReply.has_value() && loneReply->data.has_value())) << "the removal of the lone stage was refused";
        expect(qa_edit::listedNames(*loneReply->data, "sourcesWithoutReader") == std::vector{loneName}) << "the reply does not name the source left without a reader";

        expect(scheduler.changeStateTo(REQUESTED_STOP).has_value());
        expect(qa_edit::awaitCondition([&scheduler] { return scheduler.state() == STOPPED; })) << "the scheduler did not stop";
    };

    // An edge from the subgraph's exported output counts for the interior source. Each edit below takes the last such
    // edge away, and each reply names the interior source
    "the replies name the block behind a subgraph's exported output as the source left without a reader"_test = [] {
        gr::Graph                             flow;
        auto                                  wrapper  = std::make_shared<gr::GraphWrapper<gr::Graph>>();
        auto&                                 interior = wrapper->graph()->emplaceBlock<qa_edit::Source>();
        const std::string                     interiorName{interior.unique_name};
        const auto*                           interiorBlock = &interior;
        const std::shared_ptr<gr::BlockModel> subgraph      = flow.addBlock(wrapper);
        const std::string                     subgraphName{subgraph->uniqueName()};
        expect(wrapper->exportPort(true, interior.unique_name, gr::PortDirection::OUTPUT, "out", "out").has_value());
        const std::string                     firstSinkName{flow.emplaceBlock<qa_edit::Sink>().unique_name};
        const std::shared_ptr<gr::BlockModel> firstSink  = flow.blocks().back();
        auto&                                 secondSink = flow.emplaceBlock<qa_edit::Sink>();
        auto&                                 spare      = flow.emplaceBlock<qa_edit::Source>();
        expect(flow.connect(subgraph, gr::PortDefinition{"out"}, firstSink, gr::PortDefinition{"in"}).has_value());
        expect(flow.connect<"out", "in">(spare, secondSink).has_value());
        const std::string secondSinkName{secondSink.unique_name};
        const std::string spareName{spare.unique_name};

        qa_edit::TestScheduler scheduler;
        expect(fatal(scheduler.exchange(std::move(flow)).has_value()));
        gr::MsgPortOut toScheduler;
        gr::MsgPortIn  fromScheduler;
        expect(fatal(toScheduler.connect(scheduler.msgIn).has_value()));
        expect(fatal(scheduler.msgOut.connect(fromScheduler).has_value()));
        expect(scheduler.changeStateTo(INITIALISED).has_value());
        expect(scheduler.changeStateTo(RUNNING).has_value());

        gr::sendMessage<gr::message::Command::Set>(toScheduler, "", kEmplaceEdge, qa_edit::edgeRequest(spareName, firstSinkName, std::int32_t{0}), "displacing");
        const std::optional<gr::Message> displacingReply = qa_edit::takeReplyTo(fromScheduler, "displacing");
        expect(fatal(displacingReply.has_value() && displacingReply->data.has_value())) << "the displacing edge was refused";
        expect(qa_edit::listedNames(*displacingReply->data, "sourcesWithoutReader") == std::vector{interiorName}) << "the EmplaceEdge reply does not name the interior source";
        expect(fatal(qa_edit::awaitCondition([interiorBlock] { return interiorBlock->state() == STOPPED; }))) << "the interior source without a reader did not stop";

        gr::sendMessage<gr::message::Command::Set>(toScheduler, "", kEmplaceEdge, qa_edit::edgeRequest(subgraphName, secondSinkName, std::int32_t{0}), "rejoined");
        const std::optional<gr::Message> rejoinedReply = qa_edit::takeReplyTo(fromScheduler, "rejoined");
        expect(fatal(rejoinedReply.has_value() && rejoinedReply->data.has_value())) << "the edge from the exported output was refused";
        expect(rejoinedReply->data->find("sourcesWithoutReader") == rejoinedReply->data->end()) << "the reply names the spare source, which keeps a reader";
        expect(qa_edit::listedNames(*rejoinedReply->data, "restartedBlocks") == std::vector{interiorName}) << "the EmplaceEdge reply does not name the restarted interior source";

        gr::sendMessage<gr::message::Command::Set>(toScheduler, "", kRemoveEdge, qa_edit::removeEdgeRequest(subgraphName, secondSinkName), "removed");
        const std::optional<gr::Message> removedReply = qa_edit::takeReplyTo(fromScheduler, "removed");
        expect(fatal(removedReply.has_value() && removedReply->data.has_value())) << "the removal of the edge from the exported output was refused";
        expect(qa_edit::listedNames(*removedReply->data, "sourcesWithoutReader") == std::vector{interiorName}) << "the RemoveEdge reply does not name the interior source";

        gr::sendMessage<gr::message::Command::Set>(toScheduler, "", kEmplaceEdge, qa_edit::edgeRequest(subgraphName, secondSinkName, std::int32_t{0}), "restored");
        const std::optional<gr::Message> restoredReply = qa_edit::takeReplyTo(fromScheduler, "restored");
        expect(fatal(restoredReply.has_value() && restoredReply->data.has_value())) << "the restored edge was refused";

        gr::sendMessage<gr::message::Command::Set>(toScheduler, "", kRemoveBlock, {{"uniqueName", secondSinkName}}, "sink removed");
        const std::optional<gr::Message> sinkRemovedReply = qa_edit::takeReplyTo(fromScheduler, "sink removed");
        expect(fatal(sinkRemovedReply.has_value() && sinkRemovedReply->data.has_value())) << "the removal of the sink was refused";
        expect(qa_edit::listedNames(*sinkRemovedReply->data, "sourcesWithoutReader") == std::vector{interiorName}) << "the RemoveBlock reply does not name the interior source";

        expect(scheduler.changeStateTo(REQUESTED_STOP).has_value());
        expect(qa_edit::awaitCondition([&scheduler] { return scheduler.state() == STOPPED; })) << "the scheduler did not stop";
    };

    "an edge emplaced in a multi-threaded run restarts its stopped source, and the reply names it"_test = [] { qa_edit::expectEmplacedEdgeRestartsStoppedSource<gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::multiThreaded>>(); };

    "an edge emplaced in a single-threaded run restarts its stopped source, and the reply names it"_test = [] { qa_edit::expectEmplacedEdgeRestartsStoppedSource<gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::singleThreaded>>(); };

#ifndef GR_TEST_WITHOUT_BLOCK_REGISTRY // replacement by name resolves the type through the registry
    // the source stops without an end-of-stream tag, and the stage keeps waiting on its input
    "a replacement restarts a stopped source at the other end of its edge, and the reply names it"_test = [] {
        qa_edit::registerTestBlocks();
        qa_edit::gSourcedSamples.store(0UZ);

        gr::Graph flow;
        auto&     source = flow.emplaceBlock<qa_edit::StoppableSource>();
        auto&     stage  = flow.emplaceBlock<qa_edit::Tunable>();
        auto&     sink   = flow.emplaceBlock<qa_edit::Sink>();
        expect(flow.connect<"out", "in">(source, stage).has_value());
        expect(flow.connect<"out", "in">(stage, sink).has_value());
        const std::string sourceName{source.unique_name};
        const std::string stageName{stage.unique_name};
        const auto*       sourceBlock = &source;

        qa_edit::TestScheduler scheduler;
        expect(fatal(scheduler.exchange(std::move(flow)).has_value()));
        gr::MsgPortOut toScheduler;
        gr::MsgPortIn  fromScheduler;
        expect(fatal(toScheduler.connect(scheduler.msgIn).has_value()));
        expect(fatal(scheduler.msgOut.connect(fromScheduler).has_value()));
        expect(scheduler.changeStateTo(INITIALISED).has_value());
        expect(scheduler.changeStateTo(RUNNING).has_value());
        expect(qa_edit::awaitCondition([] { return qa_edit::gSourcedSamples.load() > 0UZ; })) << "the source never ran";

        gr::sendMessage<gr::message::Command::Set>(toScheduler, sourceName, qa_edit::kStopEndpoint, gr::property_map{});
        expect(fatal(qa_edit::awaitCondition([sourceBlock] { return sourceBlock->state() == STOPPED; }))) << "the source did not stop";
        const std::size_t nBeforeReplacement = qa_edit::gSourcedSamples.load();

        qa_edit::sendMessage(toScheduler, kReplaceBlock, {{"uniqueName", stageName}, {"type", gr::meta::type_name<qa_edit::CountedTunable>()}});
        const std::optional<gr::property_map> reply = qa_edit::awaitReplyData(fromScheduler, kBlockReplaced);
        expect(fatal(reply.has_value())) << "the replacement was refused";
        expect(qa_edit::listedNames(*reply, "restartedBlocks") == std::vector{sourceName}) << "the reply does not name the restarted source";
        expect(qa_edit::awaitCondition([nBeforeReplacement] { return qa_edit::gSourcedSamples.load() > nBeforeReplacement; })) << "the stopped source never ran again";

        expect(scheduler.changeStateTo(REQUESTED_STOP).has_value());
        expect(qa_edit::awaitCondition([&scheduler] { return scheduler.state() == STOPPED; })) << "the scheduler did not stop";
    };

    "a replacement of a stage that ended reconnects the sink that released its input, and samples reach it"_test = [] { qa_edit::expectSinkAfterEndedStageReplaced<true>(); };

    "a sink that kept its input stops again at the stage's end-of-stream tag after the restart"_test = [] { qa_edit::expectSinkAfterEndedStageReplaced<false>(); };
#endif

    // the run's two job lists end at once, and the scheduler stays RUNNING with no worker left to restart a block
    "a restart refused because every worker has left is listed with the reason"_test = [] {
        constexpr std::string_view kPoolName = "qa_edit_two_threads";
        gr::thread_pool::Manager::instance().replacePool(std::string(kPoolName), std::make_shared<gr::thread_pool::ThreadPoolWrapper>(std::make_unique<gr::thread_pool::BasicThreadPool>(kPoolName, gr::thread_pool::TaskType::CPU_BOUND, 2U, 2U), "CPU"));

        gr::Graph flow;
        auto&     ending      = flow.emplaceBlock<qa_edit::EndingSource>();
        auto&     endingSink  = flow.emplaceBlock<qa_edit::Sink>();
        auto&     spareEnding = flow.emplaceBlock<qa_edit::EndingSource>();
        expect(flow.connect<"out", "in">(ending, endingSink).has_value());
        const std::string endingSinkName{endingSink.unique_name};
        const std::string spareEndingName{spareEnding.unique_name};

        qa_edit::JobListProbe scheduler({{"poolName", std::string(kPoolName)}});
        expect(fatal(scheduler.exchange(std::move(flow)).has_value()));
        gr::MsgPortOut toScheduler;
        gr::MsgPortIn  fromScheduler;
        expect(fatal(toScheduler.connect(scheduler.msgIn).has_value()));
        expect(fatal(scheduler.msgOut.connect(fromScheduler).has_value()));
        expect(scheduler.changeStateTo(INITIALISED).has_value());
        expect(scheduler.changeStateTo(RUNNING).has_value());
        expect(fatal(qa_edit::awaitCondition([&scheduler] { return scheduler.nRunningJobs() == 0UZ; }))) << "the run's workers did not leave";
        expect(fatal(scheduler.state() == RUNNING));

        gr::sendMessage<gr::message::Command::Set>(toScheduler, scheduler.unique_name, kEmplaceEdge, qa_edit::edgeRequest(spareEndingName, endingSinkName, std::int32_t{0}), "late");
        scheduler.processScheduledMessages();
        const std::optional<gr::Message> reply = qa_edit::takeReplyTo(fromScheduler, "late");
        expect(fatal(reply.has_value() && reply->data.has_value())) << "the edge was refused";
        expect(qa_edit::listedNames(*reply->data, "restartedBlocks").empty()) << "the reply names a block restarted in a run that has no worker";
        const auto  refusedEntry = reply->data->find("refusedRestarts");
        const auto* refused      = refusedEntry != reply->data->end() ? refusedEntry->second.get_if<gr::property_map>() : nullptr;
        expect(fatal(refused != nullptr)) << "the reply does not list the refused restarts";
        for (const std::string& name : {spareEndingName, endingSinkName}) {
            const auto entry = refused->find(std::pmr::string(name));
            expect(entry != refused->end() && entry->second.value_or(std::string()).contains("next start")) << std::format("the reply does not list '{}' with the reason", name);
        }

        expect(scheduler.changeStateTo(REQUESTED_STOP).has_value());
        expect(qa_edit::awaitCondition([&scheduler] { return scheduler.state() == STOPPED; })) << "the scheduler did not stop";
    };

    // A pool of two threads gives the level source a job list of its own. The source stops at its first call, and its
    // worker leaves. A setting forwarded to it then waits at its message input until the restart
    "a restarted block reads a message forwarded to it while it was stopped"_test = [] {
        constexpr std::string_view kPoolName = "qa_edit_two_threads";
        gr::thread_pool::Manager::instance().replacePool(std::string(kPoolName), std::make_shared<gr::thread_pool::ThreadPoolWrapper>(std::make_unique<gr::thread_pool::BasicThreadPool>(kPoolName, gr::thread_pool::TaskType::CPU_BOUND, 2U, 2U), "CPU"));
        qa_edit::gLevel.store(0.0f);

        gr::Graph flow;
        auto&     level      = flow.emplaceBlock<qa_edit::LevelSource>();
        auto&     keptSource = flow.emplaceBlock<qa_edit::Source>();
        auto&     keptSink   = flow.emplaceBlock<qa_edit::FreeSink>();
        expect(flow.connect<"out", "in">(keptSource, keptSink).has_value());
        const std::string levelName{level.unique_name};
        const std::string keptSinkName{keptSink.unique_name};
        auto*             levelBlock = &level;

        qa_edit::JobListProbe scheduler({{"poolName", std::string(kPoolName)}});
        expect(fatal(scheduler.exchange(std::move(flow)).has_value()));
        gr::MsgPortOut toScheduler;
        gr::MsgPortIn  fromScheduler;
        expect(fatal(toScheduler.connect(scheduler.msgIn).has_value()));
        expect(fatal(scheduler.msgOut.connect(fromScheduler).has_value()));
        expect(scheduler.changeStateTo(INITIALISED).has_value());
        expect(scheduler.changeStateTo(RUNNING).has_value());
        expect(fatal(eq(scheduler.nJobLists(), 2UZ))) << "the graph was not split into two job lists";
        expect(fatal(qa_edit::awaitCondition([&scheduler, levelBlock] { return levelBlock->state() == STOPPED && scheduler.nRunningJobs() == 1UZ; }))) << "the level source did not stop, or its worker did not leave";

        gr::sendMessage<gr::message::Command::Set>(toScheduler, levelName, gr::block::property::kSetting, {{"level", 2.0f}});
        expect(fatal(qa_edit::awaitCondition([levelBlock] { return levelBlock->msgIn.available() > 0UZ; }))) << "the setting was not forwarded to the stopped source";

        qa_edit::sendMessage(toScheduler, kEmplaceEdge, qa_edit::edgeRequest(levelName, keptSinkName, std::int32_t{0}));
        const std::optional<gr::property_map> reply = qa_edit::awaitReplyData(fromScheduler, kEdgeEmplaced);
        expect(fatal(reply.has_value())) << "the edge was refused";
        expect(qa_edit::listedNames(*reply, "restartedBlocks") == std::vector{levelName}) << "the reply does not name the restarted source";
        expect(qa_edit::awaitCondition([] { return qa_edit::gLevel.load() == 2.0f; })) << "the restarted source never read the setting";

        expect(scheduler.changeStateTo(REQUESTED_STOP).has_value());
        expect(qa_edit::awaitCondition([&scheduler] { return scheduler.state() == STOPPED; })) << "the scheduler did not stop";
    };

    // the interior source has no reader until the edit and stops at its first call
    "an edge from a subgraph's exported output restarts the stopped block behind the port"_test = [] {
        qa_edit::gSourcedSamples.store(0UZ);

        gr::Graph         flow;
        auto              wrapper  = std::make_shared<gr::GraphWrapper<gr::Graph>>();
        auto&             interior = wrapper->graph()->emplaceBlock<qa_edit::CountedSource>();
        const std::string interiorName{interior.unique_name};
        const auto*       interiorBlock = &interior;
        const std::string subgraphName{flow.addBlock(wrapper)->uniqueName()};
        expect(wrapper->exportPort(true, interior.unique_name, gr::PortDirection::OUTPUT, "out", "out").has_value());
        const std::string sinkName{flow.emplaceBlock<qa_edit::FreeSink>().unique_name};

        qa_edit::TestScheduler scheduler;
        expect(fatal(scheduler.exchange(std::move(flow)).has_value()));
        gr::MsgPortOut toScheduler;
        gr::MsgPortIn  fromScheduler;
        expect(fatal(toScheduler.connect(scheduler.msgIn).has_value()));
        expect(fatal(scheduler.msgOut.connect(fromScheduler).has_value()));
        expect(scheduler.changeStateTo(INITIALISED).has_value());
        expect(scheduler.changeStateTo(RUNNING).has_value());
        expect(fatal(qa_edit::awaitCondition([interiorBlock] { return interiorBlock->state() == STOPPED; }))) << "the interior source without a reader did not stop";
        const std::size_t nBeforeEdge = qa_edit::gSourcedSamples.load();

        qa_edit::sendMessage(toScheduler, kEmplaceEdge, qa_edit::edgeRequest(subgraphName, sinkName, std::int32_t{0}));
        const std::optional<gr::property_map> reply = qa_edit::awaitReplyData(fromScheduler, kEdgeEmplaced);
        expect(fatal(reply.has_value())) << "the edge was refused";
        expect(qa_edit::listedNames(*reply, "restartedBlocks") == std::vector{interiorName}) << "the reply does not name the interior source";
        expect(qa_edit::awaitCondition([nBeforeEdge] { return qa_edit::gSourcedSamples.load() > nBeforeEdge; })) << "the interior source never ran again";

        expect(scheduler.changeStateTo(REQUESTED_STOP).has_value());
        expect(qa_edit::awaitCondition([&scheduler] { return scheduler.state() == STOPPED; })) << "the scheduler did not stop";
    };

#ifndef GR_TEST_WITHOUT_BLOCK_REGISTRY // replacement by name resolves the type through the registry
    // The source ends, and the stage and the sink stop at its end-of-stream tag. A free-running pair keeps a worker of
    // the run. The replacement of the stage connects the source's output to the replacement's input, and the source's
    // start() then throws
    "a refused restart releases the input that the edit connected to the refused block's output"_test = [] {
        qa_edit::registerTestBlocks();

        gr::Graph              flow;
        const gr::property_map sourceSettings{{"disconnect_on_done", false}};
        auto&                  source     = flow.emplaceBlock<qa_edit::OneRunSource>(sourceSettings);
        auto&                  stage      = flow.emplaceBlock<qa_edit::Tunable>();
        auto&                  sink       = flow.emplaceBlock<qa_edit::Sink>();
        auto&                  keptSource = flow.emplaceBlock<qa_edit::Source>();
        auto&                  keptSink   = flow.emplaceBlock<qa_edit::Sink>();
        expect(flow.connect<"out", "in">(source, stage).has_value());
        expect(flow.connect<"out", "in">(stage, sink).has_value());
        expect(flow.connect<"out", "in">(keptSource, keptSink).has_value());
        const std::string sourceName{source.unique_name};
        const std::string stageName{stage.unique_name};
        const std::string sinkName{sink.unique_name};
        const auto*       sourceBlock = &source;
        const auto*       stageBlock  = &stage;
        const auto*       sinkBlock   = &sink;

        qa_edit::TestScheduler scheduler;
        expect(fatal(scheduler.exchange(std::move(flow)).has_value()));
        gr::MsgPortOut toScheduler;
        gr::MsgPortIn  fromScheduler;
        expect(fatal(toScheduler.connect(scheduler.msgIn).has_value()));
        expect(fatal(scheduler.msgOut.connect(fromScheduler).has_value()));
        expect(scheduler.changeStateTo(INITIALISED).has_value());
        expect(scheduler.changeStateTo(RUNNING).has_value());
        expect(fatal(qa_edit::awaitCondition([sourceBlock, stageBlock, sinkBlock] { return sourceBlock->state() == STOPPED && stageBlock->state() == STOPPED && sinkBlock->state() == STOPPED && sourceBlock->out.nReaders() == 0UZ; }))) << "the chain did not stop at the source's end, or the stage kept its input";

        qa_edit::sendMessage(toScheduler, kReplaceBlock, {{"uniqueName", stageName}, {"type", gr::meta::type_name<qa_edit::Tunable>()}});
        const std::optional<gr::property_map> reply = qa_edit::awaitReplyData(fromScheduler, kBlockReplaced);
        expect(fatal(reply.has_value())) << "the replacement was refused";
        expect(qa_edit::listedNames(*reply, "restartedBlocks") == std::vector{sinkName}) << "the reply does not name the restarted sink";
        const auto  refusedEntry = reply->find("refusedRestarts");
        const auto* refused      = refusedEntry != reply->end() ? refusedEntry->second.get_if<gr::property_map>() : nullptr;
        expect(fatal(refused != nullptr)) << "the reply does not list the refused restarts";
        const auto sourceEntry = refused->find(std::pmr::string(sourceName));
        expect(sourceEntry != refused->end() && sourceEntry->second.value_or(std::string()).contains(qa_edit::kSecondStart)) << "the reply does not list the source with its start() failure";
        expect(eq(sourceBlock->out.nReaders(), 0UZ)) << "the replacement still reads the output of the source whose restart was refused";

        expect(scheduler.changeStateTo(REQUESTED_STOP).has_value());
        expect(qa_edit::awaitCondition([&scheduler] { return scheduler.state() == STOPPED; })) << "the scheduler did not stop";
    };
#endif

    // A pool of one thread gives the run one job list, and a free-running pair keeps its worker. The source has no
    // reader and stops at its first call. The edge from the source to the spare sink restarts it, and its start() then
    // throws. The free-running sink's count proves that the worker traversed its list after the edit
    "a block whose start() throws on its restart is in no job list, and its work() is never called"_test = [] {
        constexpr std::string_view kPoolName = "qa_edit_one_thread";
        gr::thread_pool::Manager::instance().replacePool(std::string(kPoolName), std::make_shared<gr::thread_pool::ThreadPoolWrapper>(std::make_unique<gr::thread_pool::BasicThreadPool>(kPoolName, gr::thread_pool::TaskType::CPU_BOUND, 1U, 1U), "CPU"));
        qa_edit::gOneRunSourceCalls.store(0UZ);
        qa_edit::gFreeRunningSamples.store(0UZ);

        gr::Graph flow;
        auto&     source     = flow.emplaceBlock<qa_edit::OneRunSource>();
        auto&     spareSink  = flow.emplaceBlock<qa_edit::Sink>();
        auto&     keptSource = flow.emplaceBlock<qa_edit::Source>();
        auto&     keptSink   = flow.emplaceBlock<qa_edit::FreeSink>();
        expect(flow.connect<"out", "in">(keptSource, keptSink).has_value());
        const std::string sourceName{source.unique_name};
        const std::string spareSinkName{spareSink.unique_name};
        const auto*       sourceBlock = &source;

        qa_edit::JobListProbe scheduler({{"poolName", std::string(kPoolName)}});
        expect(fatal(scheduler.exchange(std::move(flow)).has_value()));
        gr::MsgPortOut toScheduler;
        gr::MsgPortIn  fromScheduler;
        expect(fatal(toScheduler.connect(scheduler.msgIn).has_value()));
        expect(fatal(scheduler.msgOut.connect(fromScheduler).has_value()));
        expect(scheduler.changeStateTo(INITIALISED).has_value());
        expect(scheduler.changeStateTo(RUNNING).has_value());
        expect(fatal(eq(scheduler.nJobLists(), 1UZ))) << "the run has more than one job list";
        expect(fatal(qa_edit::awaitCondition([sourceBlock] { return sourceBlock->state() == STOPPED; }))) << "the source without a reader did not stop";
        expect(fatal(scheduler.jobListOf(sourceName) == std::optional<std::size_t>{0UZ})) << "the stopped source left its job list";
        const std::size_t nCallsBefore = qa_edit::gOneRunSourceCalls.load();

        qa_edit::sendMessage(toScheduler, kEmplaceEdge, qa_edit::edgeRequest(sourceName, spareSinkName, std::int32_t{0}));
        const std::optional<gr::property_map> reply = qa_edit::awaitReplyData(fromScheduler, kEdgeEmplaced);
        expect(fatal(reply.has_value())) << "the edge was refused";
        expect(qa_edit::listedNames(*reply, "restartedBlocks").empty()) << "the reply names the source as restarted";
        const auto  refusedEntry = reply->find("refusedRestarts");
        const auto* refused      = refusedEntry != reply->end() ? refusedEntry->second.get_if<gr::property_map>() : nullptr;
        expect(fatal(refused != nullptr)) << "the reply does not list the refused restarts";
        const auto sourceEntry = refused->find(std::pmr::string(sourceName));
        expect(sourceEntry != refused->end() && sourceEntry->second.value_or(std::string()).contains(qa_edit::kSecondStart)) << "the reply does not list the source with its start() failure";

        const std::size_t nFreeAfterReply = qa_edit::gFreeRunningSamples.load();
        expect(fatal(qa_edit::awaitCondition([nFreeAfterReply] { return qa_edit::gFreeRunningSamples.load() > nFreeAfterReply; }))) << "the worker did not traverse its job list after the edit";
        expect(!scheduler.jobListOf(sourceName).has_value()) << "the source whose restart was refused is in a job list";
        expect(eq(qa_edit::gOneRunSourceCalls.load(), nCallsBefore)) << "the source whose restart was refused was called";

        expect(scheduler.changeStateTo(REQUESTED_STOP).has_value());
        expect(qa_edit::awaitCondition([&scheduler] { return scheduler.state() == STOPPED; })) << "the scheduler did not stop";
    };

    // The source's restart is refused as in the case above, and the source has left its job list before its removal
    "a removed block that no job list holds leaves the zombie list"_test = [] {
        constexpr std::string_view kPoolName = "qa_edit_one_thread";
        gr::thread_pool::Manager::instance().replacePool(std::string(kPoolName), std::make_shared<gr::thread_pool::ThreadPoolWrapper>(std::make_unique<gr::thread_pool::BasicThreadPool>(kPoolName, gr::thread_pool::TaskType::CPU_BOUND, 1U, 1U), "CPU"));
        qa_edit::gFreeRunningSamples.store(0UZ);

        gr::Graph flow;
        auto&     source     = flow.emplaceBlock<qa_edit::OneRunSource>();
        auto&     spareSink  = flow.emplaceBlock<qa_edit::Sink>();
        auto&     keptSource = flow.emplaceBlock<qa_edit::Source>();
        auto&     keptSink   = flow.emplaceBlock<qa_edit::FreeSink>();
        expect(flow.connect<"out", "in">(keptSource, keptSink).has_value());
        const std::string sourceName{source.unique_name};
        const std::string spareSinkName{spareSink.unique_name};
        const auto*       sourceBlock = &source;

        qa_edit::JobListProbe scheduler({{"poolName", std::string(kPoolName)}});
        expect(fatal(scheduler.exchange(std::move(flow)).has_value()));
        gr::MsgPortOut toScheduler;
        gr::MsgPortIn  fromScheduler;
        expect(fatal(toScheduler.connect(scheduler.msgIn).has_value()));
        expect(fatal(scheduler.msgOut.connect(fromScheduler).has_value()));
        expect(scheduler.changeStateTo(INITIALISED).has_value());
        expect(scheduler.changeStateTo(RUNNING).has_value());
        expect(fatal(qa_edit::awaitCondition([sourceBlock] { return sourceBlock->state() == STOPPED; }))) << "the source without a reader did not stop";

        qa_edit::sendMessage(toScheduler, kEmplaceEdge, qa_edit::edgeRequest(sourceName, spareSinkName, std::int32_t{0}));
        expect(fatal(qa_edit::awaitReplyData(fromScheduler, kEdgeEmplaced).has_value())) << "the edge was refused";
        const std::size_t nFreeAfterEdge = qa_edit::gFreeRunningSamples.load();
        expect(fatal(qa_edit::awaitCondition([nFreeAfterEdge] { return qa_edit::gFreeRunningSamples.load() > nFreeAfterEdge; }))) << "the worker did not traverse its job list after the edit";
        expect(fatal(!scheduler.jobListOf(sourceName).has_value())) << "the source whose restart was refused is in a job list";

        qa_edit::sendMessage(toScheduler, kRemoveBlock, {{"uniqueName", sourceName}});
        expect(fatal(qa_edit::awaitReplyData(fromScheduler, kBlockRemoved).has_value())) << "the removal was refused";
        const std::size_t nFreeAfterRemoval = qa_edit::gFreeRunningSamples.load();
        expect(fatal(qa_edit::awaitCondition([nFreeAfterRemoval] { return qa_edit::gFreeRunningSamples.load() > nFreeAfterRemoval; }))) << "the worker did not traverse its job list after the removal";
        expect(eq(scheduler.nZombieBlocks(), 0UZ)) << "the removed block stays in the zombie list";

        expect(scheduler.changeStateTo(REQUESTED_STOP).has_value());
        expect(qa_edit::awaitCondition([&scheduler] { return scheduler.state() == STOPPED; })) << "the scheduler did not stop";
    };

    // A pool of one thread gives the run one job list. The worker answers the inspection request and then reads the
    // paused state. No work() call follows until the resume. The source therefore stays in REQUESTED_STOP until the
    // edit
    "an edge emplaced at a source in REQUESTED_STOP restarts the source, and the reply names it"_test = [] {
        constexpr std::string_view kPoolName = "qa_edit_one_thread";
        gr::thread_pool::Manager::instance().replacePool(std::string(kPoolName), std::make_shared<gr::thread_pool::ThreadPoolWrapper>(std::make_unique<gr::thread_pool::BasicThreadPool>(kPoolName, gr::thread_pool::TaskType::CPU_BOUND, 1U, 1U), "CPU"));
        qa_edit::gSourcedSamples.store(0UZ);

        gr::Graph flow;
        auto&     source     = flow.emplaceBlock<qa_edit::CountedSource>();
        auto&     firstSink  = flow.emplaceBlock<qa_edit::Sink>();
        auto&     kept       = flow.emplaceBlock<qa_edit::Source>();
        auto&     secondSink = flow.emplaceBlock<qa_edit::Sink>();
        expect(flow.connect<"out", "in">(source, firstSink).has_value());
        expect(flow.connect<"out", "in">(kept, secondSink).has_value());
        const std::string sourceName{source.unique_name};
        const std::string secondSinkName{secondSink.unique_name};
        auto*             sourceBlock = &source;

        qa_edit::JobListProbe scheduler({{"poolName", std::string(kPoolName)}});
        expect(fatal(scheduler.exchange(std::move(flow)).has_value()));
        gr::MsgPortOut toScheduler;
        gr::MsgPortIn  fromScheduler;
        expect(fatal(toScheduler.connect(scheduler.msgIn).has_value()));
        expect(fatal(scheduler.msgOut.connect(fromScheduler).has_value()));
        expect(scheduler.changeStateTo(INITIALISED).has_value());
        expect(scheduler.changeStateTo(RUNNING).has_value());
        expect(fatal(eq(scheduler.nJobLists(), 1UZ))) << "the run has more than one job list";
        expect(fatal(qa_edit::awaitCondition([] { return qa_edit::gSourcedSamples.load() > 0UZ; }))) << "the source never ran";

        expect(fatal(scheduler.changeStateTo(REQUESTED_PAUSE).has_value()));
        expect(fatal(qa_edit::awaitCondition([&scheduler] { return scheduler.state() == PAUSED; }))) << "the scheduler did not pause";
        qa_edit::sendMessage(toScheduler, kSchedulerInspect, {});
        expect(fatal(qa_edit::takeReply(fromScheduler, kSchedulerInspected))) << "the paused worker did not answer";
        sourceBlock->requestStop();
        expect(fatal(sourceBlock->state() == REQUESTED_STOP)) << "the source did not enter REQUESTED_STOP";

        qa_edit::sendMessage(toScheduler, kEmplaceEdge, qa_edit::edgeRequest(sourceName, secondSinkName, std::int32_t{0}));
        const std::optional<gr::property_map> reply = qa_edit::awaitReplyData(fromScheduler, kEdgeEmplaced);
        expect(fatal(reply.has_value())) << "the edge was refused";
        expect(qa_edit::listedNames(*reply, "restartedBlocks") == std::vector{sourceName}) << "the reply does not name the source in REQUESTED_STOP";
        const std::size_t nBeforeResume = qa_edit::gSourcedSamples.load();

        expect(fatal(scheduler.changeStateTo(RUNNING).has_value()));
        expect(qa_edit::awaitCondition([nBeforeResume] { return qa_edit::gSourcedSamples.load() > nBeforeResume; })) << "the source never ran after the edit";
        expect(sourceBlock->state() == RUNNING);

        expect(scheduler.changeStateTo(REQUESTED_STOP).has_value());
        expect(qa_edit::awaitCondition([&scheduler] { return scheduler.state() == STOPPED; })) << "the scheduler did not stop";
    };
};

int main() { /* tests are statically registered */ }
