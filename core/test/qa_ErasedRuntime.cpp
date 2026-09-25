#include <boost/ut.hpp>

#include <gnuradio-4.0/BlockModel.hpp>
#include <gnuradio-4.0/BlockRegistry.hpp>
#include <gnuradio-4.0/Graph.hpp>
#include <gnuradio-4.0/PluginLoader.hpp>
#include <gnuradio-4.0/Runtime.hpp>
#include <gnuradio-4.0/Scheduler.hpp>
#include <gnuradio-4.0/SchedulerModel.hpp>
#include <gnuradio-4.0/SchedulerRegistration.hpp>
#include <gnuradio-4.0/YamlPmt.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <format>
#include <functional>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <print>
#include <ranges>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

/**
 * Parity between a graph built through gr::Runtime and the same graph built with the typed API:
 * every writable member is settable through the erased path, the two arms deliver identical samples
 * and tags, and a subgraph is transparent to both.
 *
 * gnuradio4-core carries no standard block library, so the blocks are defined and registered here.
 */

namespace qa_runtime {

using namespace gr;
using boost::ut::expect;

struct TagRecord {
    std::size_t  index{};
    property_map map{};
};

struct Collected {
    std::vector<float>     samples;
    std::vector<TagRecord> tags;
};

inline std::mutex& collectorMutex() {
    static std::mutex mutex;
    return mutex;
}

inline std::map<std::string, Collected>& collector() {
    static std::map<std::string, Collected> collected;
    return collected;
}

inline std::map<std::string, std::size_t>& counted() {
    static std::map<std::string, std::size_t> nCounted;
    return nCounted;
}

inline Collected takeCollected(const std::string& sinkName) {
    std::lock_guard guard(collectorMutex());
    auto            entry = collector().find(sinkName);
    if (entry == collector().end()) {
        return {};
    }
    Collected result = std::move(entry->second);
    collector().erase(entry);
    return result;
}

/// counts up from zero and tags every `tag_every` samples, so both arms see one deterministic stream
struct RampSource : Block<RampSource> {
    PortOut<float> out;

    Annotated<gr::Size_t, "n_samples", Doc<"samples to emit before finishing">>            n_samples = 4096U;
    Annotated<gr::Size_t, "tag_every", Doc<"tag period in samples, 0 to disable tagging">> tag_every = 512U;

    GR_MAKE_REFLECTABLE(RampSource, out, n_samples, tag_every);

    gr::Size_t _emitted = 0U;

    explicit RampSource(property_map init = {}) : Block<RampSource>(std::move(init)) {}

    work::Status processBulk(OutputSpanLike auto& outSpan) {
        if (_emitted >= n_samples) {
            outSpan.publish(0UZ);
            return work::Status::DONE;
        }
        const std::size_t n = std::min(outSpan.size(), static_cast<std::size_t>(n_samples - _emitted));
        if (n == 0UZ) {
            outSpan.publish(0UZ);
            return work::Status::INSUFFICIENT_OUTPUT_ITEMS;
        }
        for (std::size_t i = 0UZ; i < n; ++i) {
            const gr::Size_t index = _emitted + static_cast<gr::Size_t>(i);
            outSpan[i]             = static_cast<float>(index);
            if (tag_every > 0U && index % tag_every == 0U) {
                outSpan.publishTag(property_map{{"trigger_name", std::format("t{}", index)}}, i);
            }
        }
        _emitted += static_cast<gr::Size_t>(n);
        outSpan.publish(n);
        return work::Status::OK;
    }
};

struct Scale : Block<Scale> {
    PortIn<float>  in;
    PortOut<float> out;

    Annotated<float, "gain">              gain    = 1.0f;
    Annotated<std::string, "label">       label   = "scale";
    Annotated<bool, "enabled">            enabled = true;
    Annotated<std::vector<float>, "taps"> taps    = std::vector<float>{1.0f};

    /// readable but never writable, so get() lists it and writableMembers() does not
    gr::meta::immutable<gr::Size_t> revision = 7U;

    GR_MAKE_REFLECTABLE(Scale, in, out, gain, label, enabled, taps, revision);

    explicit Scale(property_map init = {}) : Block<Scale>(std::move(init)) {}

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return enabled ? value * gain : value; }
};

/// the shape of a multi-input arithmetic block: its inputs are one collection port, addressed "in#0"
struct SumInputs : Block<SumInputs> {
    std::vector<PortIn<float>> in;
    PortOut<float>             out;

    Annotated<gr::Size_t, "n_inputs", Limits<1U, 8U>> n_inputs = 2U;

    GR_MAKE_REFLECTABLE(SumInputs, in, out, n_inputs);

    explicit SumInputs(property_map init = {}) : Block<SumInputs>(std::move(init)) { in.resize(n_inputs); }

    void settingsChanged(const property_map& oldSettings, const property_map& newSettings) {
        if (newSettings.contains("n_inputs") && oldSettings.at("n_inputs") != newSettings.at("n_inputs")) {
            in.resize(n_inputs);
        }
    }

    template<InputSpanLike TInSpan>
    work::Status processBulk(const std::span<TInSpan>& inSpans, OutputSpanLike auto& outSpan) const {
        std::copy(inSpans[0].begin(), inSpans[0].end(), outSpan.begin());
        for (std::size_t port = 1UZ; port < inSpans.size(); ++port) {
            std::transform(outSpan.begin(), outSpan.end(), inSpans[port].begin(), outSpan.begin(), std::plus<float>{});
        }
        return work::Status::OK;
    }
};

struct RecordingSink : Block<RecordingSink> {
    PortIn<float> in;

    GR_MAKE_REFLECTABLE(RecordingSink, in);

    explicit RecordingSink(property_map init = {}) : Block<RecordingSink>(std::move(init)) {}

    work::Status processBulk(InputSpanLike auto& inSpan) {
        const std::size_t n = inSpan.size();
        {
            std::lock_guard guard(collectorMutex());
            Collected&      collected = collector()[std::string(this->name)];
            collected.samples.insert(collected.samples.end(), inSpan.begin(), inSpan.begin() + static_cast<std::ptrdiff_t>(n));
            for (const Tag& tag : inSpan.rawTags) {
                collected.tags.push_back(TagRecord{tag.index, tag.map});
            }
        }
        inSpan.consumeTags(n);
        std::ignore = inSpan.consume(n);
        return work::Status::OK;
    }
};

/// an ordinary output beside an optional one
struct DualSource : Block<DualSource> {
    PortOut<float>           out;
    PortOut<float, Optional> monitor;

    GR_MAKE_REFLECTABLE(DualSource, out, monitor);

    work::Status processBulk(OutputSpanLike auto& outSpan, OutputSpanLike auto& monitorSpan) {
        outSpan.publish(0UZ);
        monitorSpan.publish(0UZ);
        return work::Status::DONE;
    }
};

/// an asynchronous input beside a synchronous output
struct AsyncInput : Block<AsyncInput> {
    PortIn<float, Async> in;
    PortOut<float>       out;

    GR_MAKE_REFLECTABLE(AsyncInput, in, out);

    work::Status processBulk(InputSpanLike auto&, OutputSpanLike auto&) { return work::Status::OK; }
};

/// a message input beside a stream input that declares its sample counts
struct ControlledChunks : Block<ControlledChunks> {
    MsgPortIn                                  control;
    PortIn<float, RequiredSamples<64U, 1024U>> in;
    PortOut<float>                             out;

    GR_MAKE_REFLECTABLE(ControlledChunks, control, in, out);

    work::Status processBulk(InputSpanLike auto&, OutputSpanLike auto&) { return work::Status::OK; }
};

/// publishes zeros until the run is stopped
struct EndlessSource : Block<EndlessSource> {
    PortOut<float> out;

    GR_MAKE_REFLECTABLE(EndlessSource, out);

    explicit EndlessSource(property_map init = {}) : Block<EndlessSource>(std::move(init)) {}

    work::Status processBulk(OutputSpanLike auto& outSpan) {
        std::fill(outSpan.begin(), outSpan.end(), 0.0f);
        outSpan.publish(outSpan.size());
        return work::Status::OK;
    }
};

/// counts the samples it consumes and keeps none, so an endless source can feed it for a whole case
struct CountingSink : Block<CountingSink> {
    PortIn<float> in;

    GR_MAKE_REFLECTABLE(CountingSink, in);

    explicit CountingSink(property_map init = {}) : Block<CountingSink>(std::move(init)) {}

    work::Status processBulk(InputSpanLike auto& inSpan) {
        const std::size_t n = inSpan.size();
        {
            std::lock_guard guard(collectorMutex());
            counted()[std::string(this->name)] += n;
        }
        std::ignore = inSpan.consume(n);
        return work::Status::OK;
    }
};

/// a source whose start() throws, so every run of its graph fails
struct FailingStartSource : Block<FailingStartSource> {
    PortOut<float> out;

    GR_MAKE_REFLECTABLE(FailingStartSource, out);

    explicit FailingStartSource(property_map init = {}) : Block<FailingStartSource>(std::move(init)) {}

    void start() { throw gr::exception("the source refused to start"); }

    work::Status processBulk(OutputSpanLike auto& outSpan) {
        outSpan.publish(0UZ);
        return work::Status::DONE;
    }
};

/// holds each GatedStopSink's stop() until a case opens it, for at most ten seconds, and counts the stops it held
struct StopGate {
    std::mutex              mutex;
    std::condition_variable changed;
    bool                    isOpen = false;
    std::size_t             nStops = 0UZ;
};

inline StopGate& stopGate() {
    static StopGate gate;
    return gate;
}

/// consumes and keeps nothing, and its stop() returns only once stopGate() opens
struct GatedStopSink : Block<GatedStopSink> {
    PortIn<float> in;

    GR_MAKE_REFLECTABLE(GatedStopSink, in);

    explicit GatedStopSink(property_map init = {}) : Block<GatedStopSink>(std::move(init)) {}

    void stop() {
        StopGate&        gate = stopGate();
        std::unique_lock lock(gate.mutex);
        ++gate.nStops;
        gate.changed.notify_all();
        std::ignore = gate.changed.wait_for(lock, std::chrono::seconds(10), [&gate] { return gate.isOpen; });
    }

    work::Status processBulk(InputSpanLike auto& inSpan) {
        std::ignore = inSpan.consume(inSpan.size());
        return work::Status::OK;
    }
};

inline void registerTestBlocks() {
    static const bool registered = [] {
        BlockRegistry& registry = globalBlockRegistry();
        return registry.insert<RampSource>("=qa::RampSource") && registry.insert<Scale>("=qa::Scale")                        //
               && registry.insert<RecordingSink>("=qa::RecordingSink") && registry.insert<SumInputs>("=qa::SumInputs")       //
               && registry.insert<EndlessSource>("=qa::EndlessSource") && registry.insert<CountingSink>("=qa::CountingSink") //
               && registry.insert<FailingStartSource>("=qa::FailingStartSource");
    }();
    expect(registered) << "the test blocks must reach the global registry";
}

/// the same three-block chain, built with the typed API
inline gr::Graph typedChain(std::string_view sinkName, gr::Size_t nSamples, float gain) {
    gr::Graph flow;
    auto&     source = flow.emplaceBlock<RampSource>({{"name", std::string("source")}, {"n_samples", nSamples}});
    auto&     scale  = flow.emplaceBlock<Scale>({{"name", std::string("scale")}, {"gain", gain}});
    auto&     sink   = flow.emplaceBlock<RecordingSink>({{"name", std::string(sinkName)}});
    expect(flow.connect<"out", "in">(source, scale).has_value());
    expect(flow.connect<"out", "in">(scale, sink).has_value());
    return flow;
}

inline void runTyped(gr::Graph&& flow) {
    gr::scheduler::Simple<> scheduler;
    expect(scheduler.exchange(std::move(flow)).has_value());
    expect(scheduler.runAndWait().has_value());
}

/// the same three-block chain, built through the entry
inline std::expected<gr::Runtime, gr::RuntimeError> erasedChain(std::string_view sinkName, gr::Size_t nSamples, float gain, std::string_view schedulerType = gr::Runtime::kDefaultScheduler) {
    RuntimeGraph graph;
    auto         source = graph.emplace("qa::RampSource", "source", {{"n_samples", nSamples}});
    auto         scale  = graph.emplace("qa::Scale", "scale", {{"gain", gain}});
    auto         sink   = graph.emplace("qa::RecordingSink", sinkName);
    expect(source.has_value() && scale.has_value() && sink.has_value());
    expect(graph.connect(*source, "out", *scale, "in").has_value());
    expect(graph.connect(*scale, "out", *sink, "in").has_value());
    return Runtime::create(std::move(graph), schedulerType);
}

inline std::size_t collectedSize(const std::string& sinkName) {
    std::lock_guard guard(collectorMutex());
    const auto      entry = collector().find(sinkName);
    return entry == collector().end() ? 0UZ : entry->second.samples.size();
}

/**
 * Runs until the sink holds `nExpected` samples, then stops, and reports how long that took.
 *
 * A graph containing a transparent subgroup never reports DONE: graph::flatten puts the group block
 * itself in the execution order, and Block::work returns OK for every non-NormalBlock category, so
 * the scheduler always sees an unfinished block. runAndWait() would not return.
 */
inline std::chrono::microseconds runUntilCollected(gr::Runtime& runtime, const std::string& sinkName, std::size_t nExpected) {
    constexpr auto kCap    = std::chrono::seconds(3);
    const auto     started = std::chrono::steady_clock::now();

    runtime.start();
    while (collectedSize(sinkName) < nExpected && std::chrono::steady_clock::now() - started < kCap) {
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;
    runtime.stop();
    return std::chrono::duration_cast<std::chrono::microseconds>(elapsed);
}

/// an endless source feeding a sink that counts and keeps nothing
inline std::expected<gr::Runtime, gr::RuntimeError> endlessChain(std::string_view sinkName) {
    RuntimeGraph graph;
    auto         source = graph.emplace("qa::EndlessSource", "source");
    auto         sink   = graph.emplace("qa::CountingSink", sinkName);
    expect(source.has_value() && sink.has_value());
    expect(graph.connect(*source, "out", *sink, "in").has_value());
    return Runtime::create(std::move(graph));
}

inline std::size_t countedSize(const std::string& sinkName) {
    std::lock_guard guard(collectorMutex());
    const auto      entry = counted().find(sinkName);
    return entry == counted().end() ? 0UZ : entry->second;
}

/// waits until the sink has counted more than `nBefore` samples, for at most three seconds, and returns its count
inline std::size_t awaitCountAbove(const std::string& sinkName, std::size_t nBefore) {
    const auto started = std::chrono::steady_clock::now();
    while (countedSize(sinkName) <= nBefore && std::chrono::steady_clock::now() - started < std::chrono::seconds(3)) {
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
    return countedSize(sinkName);
}

/// closes the gate and clears its count
inline void closeStopGate() {
    StopGate&       gate = stopGate();
    std::lock_guard lock(gate.mutex);
    gate.isOpen = false;
    gate.nStops = 0UZ;
}

inline void openStopGate() {
    StopGate&       gate = stopGate();
    std::lock_guard lock(gate.mutex);
    gate.isOpen = true;
    gate.changed.notify_all();
}

/// waits until the gate has held `nStops` stops, for at most three seconds, and returns how many it held
inline std::size_t awaitHeldStops(std::size_t nStops) {
    StopGate&        gate = stopGate();
    std::unique_lock lock(gate.mutex);
    std::ignore = gate.changed.wait_for(lock, std::chrono::seconds(3), [&gate, nStops] { return gate.nStops >= nStops; });
    return gate.nStops;
}

/// "gr::Foo#17" -> "gr::Foo": the id is process-unique, so two arms never share one
inline std::string typeOf(std::string_view uniqueName) {
    const auto separator = uniqueName.rfind('#');
    return std::string(separator == std::string_view::npos ? uniqueName : uniqueName.substr(0UZ, separator));
}

inline bool sameTags(const std::vector<TagRecord>& lhs, const std::vector<TagRecord>& rhs) {
    return std::ranges::equal(lhs, rhs, [](const TagRecord& a, const TagRecord& b) { return a.index == b.index && a.map == b.map; });
}

inline std::optional<float> floatAt(const property_map& map, const std::string& key) {
    const auto entry = map.find(convert_string_domain(key));
    if (entry == map.end() || entry->second.get_if<float>() == nullptr) {
        return std::nullopt;
    }
    return *entry->second.get_if<float>();
}

/// each block's settings by its name, less the process-unique name no loaded copy can share
inline std::map<std::string, property_map> settingsByName(const RuntimeGraph& graph) {
    std::map<std::string, property_map> settings;
    for (const BlockHandle& block : graph.blocks()) {
        property_map values = block.get();
        values.erase(convert_string_domain(std::string("unique_name")));
        settings.emplace(std::string(block.name()), std::move(values));
    }
    return settings;
}

} // namespace qa_runtime

const boost::ut::suite<"erased runtime"> erasedRuntimeTests = [] {
    using namespace boost::ut;
    using namespace gr;
    using namespace qa_runtime;

    "a graph built by name runs and produces the expected stream"_test = [] {
        registerTestBlocks();
        auto runtime = erasedChain("basic", 1024U, 2.0f);
        expect(runtime.has_value()) << (runtime ? std::string{} : runtime.error().message);
        expect(runtime->runAndWait().has_value());

        const Collected collected = takeCollected("basic");
        expect(eq(collected.samples.size(), 1024UZ));
        expect(eq(collected.samples.front(), 0.0f));
        expect(eq(collected.samples.back(), 2.0f * 1023.0f));
    };

    // a graph that did not come from a RuntimeGraph -- loaded by gr::loadGrc, or built with the typed
    // API -- runs on the same runtime path as one that did
    "a graph the caller already owns runs through the entry"_test = [] {
        registerTestBlocks();

        auto runtime = Runtime::create(typedChain("adopted", 1024U, 3.0f));
        expect(runtime.has_value()) << (runtime ? std::string{} : runtime.error().message);
        expect(runtime->runAndWait().has_value());

        const Collected collected = takeCollected("adopted");
        expect(eq(collected.samples.size(), 1024UZ));
        expect(eq(collected.samples.back(), 3.0f * 1023.0f));
    };

    "an unknown type and a mistyped port name are reported, not thrown"_test = [] {
        registerTestBlocks();
        RuntimeGraph graph;

        const auto missing = graph.emplace("qa::NoSuchBlock", "nope");
        expect(!missing.has_value());
        expect(missing.error().message.find("qa::NoSuchBlock") != std::string::npos) << missing.error().message;

        const auto source = graph.emplace("qa::RampSource", "source");
        const auto sink   = graph.emplace("qa::RecordingSink", "sink");
        expect(source.has_value() && sink.has_value());

        const auto typo = graph.connect(*source, "outt", *sink, "in");
        expect(!typo.has_value());
        expect(typo.error().message.find("out") != std::string::npos) << typo.error().message;
    };

    "a setting the block does not declare is emplace()'s error, and nothing throws"_test = [] {
        registerTestBlocks();
        using Emplaced = std::expected<BlockHandle, RuntimeError>;
        RuntimeGraph graph;

        std::optional<Emplaced> misspelled;
        expect(nothrow([&] { misspelled.emplace(graph.emplace("qa::Scale", "scale", {{"gian", 2.0f}})); })) << "the refusal left emplace() as an exception";
        expect(fatal(misspelled.has_value() && !misspelled->has_value())) << "emplace() accepted a key the block does not declare";
        expect(misspelled->error().message.contains("gian")) << misspelled->error().message;
        expect(eq(misspelled->error().where, std::string("RuntimeGraph::emplace")));
    };

    "port introspection lists the names and value types"_test = [] {
        registerTestBlocks();
        RuntimeGraph graph;
        const auto   scale = graph.emplace("qa::Scale", "scale");
        expect(scale.has_value());

        expect(graph.inputPortNames(*scale) == std::vector<std::string>{"in"});
        expect(graph.outputPortNames(*scale) == std::vector<std::string>{"out"});
        expect(graph.inputPorts(*scale).front().typeName.find("float") != std::string::npos) << graph.inputPorts(*scale).front().typeName;
    };

    // a block the caller built is indistinguishable from an emplaced one. A factory therefore keeps
    // its own typed pointer and hands the graph the erased one
    "an added block behaves exactly as an emplaced one"_test = [] {
        registerTestBlocks();
        runTyped(typedChain("add-reference", 4096U, 3.0f));
        const Collected reference = takeCollected("add-reference");

        // built here, where the type is named, and kept: this is the pair a factory produces
        auto         scaleBlock = std::make_shared<BlockWrapper<Scale>>(property_map{{"gain", 3.0f}});
        Scale* const typedScale = static_cast<Scale*>(scaleBlock->raw());
        auto         sinkBlock  = std::make_shared<BlockWrapper<RecordingSink>>();

        RuntimeGraph graph;
        auto         source = graph.emplace("qa::RampSource", "source", {{"n_samples", gr::Size_t{4096U}}});
        auto         scale  = graph.add(std::shared_ptr<BlockModel>(scaleBlock), "scale");
        auto         sink   = graph.add(std::shared_ptr<BlockModel>(sinkBlock), "add-erased");
        expect(source.has_value() && scale.has_value() && sink.has_value()) << (scale ? std::string{} : scale.error().message);

        expect(eq(scale->name(), std::string_view("scale"))) << "add() must name the block before init() runs";
        expect(!scale->uniqueName().empty());
        expect(scale->typeName().find("Scale") != std::string_view::npos) << scale->typeName();

        // settings: the same forwards reach the same SettingsBase, and a staged value lands on the
        // caller's own object -- which is the live-control path the handle exists for
        expect(scale->get().contains(convert_string_domain(std::string("gain"))));
        expect(scale->setStaged(property_map{{"taps", std::vector<float>{2.0f}}}) == property_map{});
        expect(scale->setStaged(property_map{{"label", std::string("adopted")}}) == property_map{});

        // connect: an added block is addressable by name like any other
        expect(graph.inputPortNames(*scale) == std::vector<std::string>{"in"});
        expect(graph.connect(*source, "out", *scale, "in").has_value());
        expect(graph.connect(*scale, "out", *sink, "in").has_value());

        // find() and blocks() see it
        expect(graph.find(scale->uniqueName(), RuntimeGraph::Recursive::No).has_value());
        expect(eq(graph.blocks().size(), 3UZ));

        auto runtime = Runtime::create(std::move(graph));
        expect(runtime.has_value()) << (runtime ? std::string{} : runtime.error().message);
        expect(runtime->runAndWait().has_value());
        expect(runtime->state() == Runtime::State::Stopped) << "lifecycle: the run must settle stopped";

        const Collected added = takeCollected("add-erased");
        expect(reference.samples == added.samples) << "an added block changed the stream";
        expect(sameTags(reference.tags, added.tags)) << "an added block changed the tag sequence";

        // the caller's own pointer stayed valid throughout, which is the whole point
        expect(eq(typedScale->label.value, std::string("adopted")));
        expect(eq(typedScale->gain.value, 3.0f));
    };

    "add rejects an empty graph handle and a null block"_test = [] {
        registerTestBlocks();
        RuntimeGraph graph;
        expect(!graph.add(nullptr).has_value());
        expect(graph.add(nullptr).error().message.find("null") != std::string::npos);
    };

    // a collection input port is addressed element by element, exactly as edge resolution does
    "a collection input port is addressed by its sub-index"_test = [] {
        registerTestBlocks();
        RuntimeGraph graph;
        auto         source = graph.emplace("qa::RampSource", "source", {{"n_samples", gr::Size_t{1024U}}});
        auto         sum    = graph.emplace("qa::SumInputs", "sum", {{"n_inputs", gr::Size_t{2U}}});
        auto         sink   = graph.emplace("qa::RecordingSink", "collection");
        expect(source.has_value() && sum.has_value() && sink.has_value());

        expect(graph.inputPortNames(*sum) == std::vector<std::string>{"in#0", "in#1"}) << "the listed names must be the ones connect accepts";
        expect(graph.inputPorts(*sum).front().typeName.find("float") != std::string::npos);

        // the base name of a collection resolves to nothing, here and at edge resolution alike
        const auto bare = graph.connect(*source, "out", *sum, "in");
        expect(!bare.has_value()) << "the base name of a collection must be refused at connect(), not at start()";
        expect(bare.error().message.find("in#0") != std::string::npos) << bare.error().message;

        expect(graph.connect(*source, "out", *sum, "in#0").has_value());
        expect(graph.connect(*source, "out", *sum, "in#1").has_value());
        expect(graph.connect(*sum, "out", *sink, "in").has_value());

        auto runtime = Runtime::create(std::move(graph));
        expect(runtime.has_value()) << (runtime ? std::string{} : runtime.error().message);
        expect(runtime->runAndWait().has_value());

        const Collected collected = takeCollected("collection");
        expect(eq(collected.samples.size(), 1024UZ));
        expect(eq(collected.samples.front(), 0.0f));
        expect(eq(collected.samples.back(), 2.0f * 1023.0f)) << "both elements of the collection must have carried the stream";
    };

    // every writable member of every registered block is visible and settable through the entry
    "settings parity over the whole block registry"_test = [] {
        registerTestBlocks();

        std::vector<std::string> skipped;
        for (const std::string& key : globalBlockRegistry().keys()) {
            RuntimeGraph graph;
            auto         handle = graph.emplace(key, "probe");
            if (!handle.has_value()) {
                skipped.push_back(key);
                continue;
            }

            const std::set<std::string> writable = handle->writableMembers();
            const property_map          active   = handle->get();
            expect(!writable.empty()) << key << " reports no writable members";

            for (const std::string& member : writable) {
                const auto value = active.find(convert_string_domain(member));
                expect(value != active.end()) << std::format("{}: writable member '{}' is not in get()", key, member);
                if (value == active.end()) {
                    continue;
                }
                const auto rejected = handle->setStaged(property_map{{convert_string_domain(member), value->second}});
                expect(rejected.has_value() && rejected->empty()) << std::format("{}: writable member '{}' was rejected by the erased path", key, member);
            }

            handle->setMetaInformation(property_map{{"qa_probe", std::string("round-trip")}});
            const property_map meta = handle->metaInformation();
            expect(meta.contains(convert_string_domain(std::string("qa_probe")))) << key;
        }
        expect(skipped.empty()) << std::format("registered but not constructible through the entry: {}", skipped);
    };

    // the two arms deliver the identical stream and the identical tags
    "typed and erased graphs deliver identical samples and tags"_test = [] {
        registerTestBlocks();
        runTyped(typedChain("e2-typed", 4096U, 3.0f));
        const Collected fromTyped = takeCollected("e2-typed");

        auto runtime = erasedChain("e2-erased", 4096U, 3.0f);
        expect(runtime.has_value());
        expect(runtime->runAndWait().has_value());
        const Collected fromErased = takeCollected("e2-erased");

        expect(eq(fromTyped.samples.size(), 4096UZ));
        expect(fromTyped.samples == fromErased.samples) << "sample streams differ";
        expect(!fromTyped.tags.empty()) << "the source must have tagged the stream";
        expect(sameTags(fromTyped.tags, fromErased.tags)) << "tag sequences differ";
    };

    // mixed index-based and string-based addressing on one output port
    "a fan-out addressed uniformly feeds both consumers"_test = [] {
        registerTestBlocks();

        const auto runUniformString = [] {
            RuntimeGraph graph;
            auto         source = graph.emplace("qa::RampSource", "source", {{"n_samples", gr::Size_t{512U}}});
            auto         first  = graph.emplace("qa::RecordingSink", "e3-string-a");
            auto         second = graph.emplace("qa::RecordingSink", "e3-string-b");
            expect(graph.connect(*source, "out", *first, "in").has_value());
            expect(graph.connect(*source, "out", *second, "in").has_value());
            auto runtime = Runtime::create(std::move(graph));
            expect(runtime.has_value());
            expect(runtime->runAndWait().has_value());
        };

        const auto runUniformTyped = [] {
            gr::Graph flow;
            auto&     source = flow.emplaceBlock<RampSource>({{"name", std::string("source")}, {"n_samples", gr::Size_t{512U}}});
            auto&     first  = flow.emplaceBlock<RecordingSink>({{"name", std::string("e3-typed-a")}});
            auto&     second = flow.emplaceBlock<RecordingSink>({{"name", std::string("e3-typed-b")}});
            expect(flow.connect<"out", "in">(source, first).has_value());
            expect(flow.connect<"out", "in">(source, second).has_value());
            runTyped(std::move(flow));
        };

        runUniformString();
        const std::size_t nStringA = takeCollected("e3-string-a").samples.size();
        const std::size_t nStringB = takeCollected("e3-string-b").samples.size();
        expect(eq(nStringA, 512UZ));
        expect(eq(nStringA, nStringB)) << "an all-string fan-out must feed both consumers equally";

        runUniformTyped();
        const std::size_t nTypedA = takeCollected("e3-typed-a").samples.size();
        const std::size_t nTypedB = takeCollected("e3-typed-b").samples.size();
        expect(eq(nTypedA, 512UZ));
        expect(eq(nTypedA, nTypedB)) << "an all-typed fan-out must feed both consumers equally";

        // one output port addressed both ways, in both orders: the by-name edge records a
        // string-based PortDefinition and the typed one an index-based definition
        const auto runMixed = [](bool nameFirst, std::string_view suffix) {
            gr::Graph mixed;
            auto&     source  = mixed.emplaceBlock<RampSource>({{"name", std::string("source")}, {"n_samples", gr::Size_t{512U}}});
            auto&     byName  = mixed.emplaceBlock<RecordingSink>({{"name", std::format("e3-mixed-name-{}", suffix)}});
            auto&     byIndex = mixed.emplaceBlock<RecordingSink>({{"name", std::format("e3-mixed-index-{}", suffix)}});

            const auto connectByName = [&] {
                auto sourceModel = graph::findBlock(mixed, source);
                auto byNameModel = graph::findBlock(mixed, byName);
                expect(sourceModel.has_value() && byNameModel.has_value());
                expect(mixed.connect(*sourceModel, PortDefinition("out"), *byNameModel, PortDefinition("in")).has_value());
            };
            const auto connectByIndex = [&] { expect(mixed.connect<"out", "in">(source, byIndex).has_value()); };

            if (nameFirst) {
                connectByName();
                connectByIndex();
            } else {
                connectByIndex();
                connectByName();
            }
            runTyped(std::move(mixed));

            const std::size_t nByName  = takeCollected(std::format("e3-mixed-name-{}", suffix)).samples.size();
            const std::size_t nByIndex = takeCollected(std::format("e3-mixed-index-{}", suffix)).samples.size();
            std::println("e3 mixed addressing ({} first): by-name {} samples, by-index {} samples", nameFirst ? "name" : "index", nByName, nByIndex);
            expect(eq(nByName, 512UZ)) << "the string-addressed consumer starved";
            expect(eq(nByIndex, 512UZ)) << "the index-addressed consumer starved -- mixed addressing is unsafe again";
        };

        runMixed(true, "nf");
        runMixed(false, "if");
    };

    // a subgraph is transparent to the scheduler
    "a nested subgraph delivers the same stream as the flat chain"_test = [] {
        registerTestBlocks();
        runTyped(typedChain("e4-flat", 4096U, 3.0f));
        const Collected flat = takeCollected("e4-flat");

        RuntimeGraph graph;
        auto         source   = graph.emplace("qa::RampSource", "source", {{"n_samples", gr::Size_t{4096U}}});
        auto         subgraph = graph.emplaceSubgraph("inner");
        auto         sink     = graph.emplace("qa::RecordingSink", "e4-nested");
        expect(source.has_value() && subgraph.has_value() && sink.has_value());

        auto inner = graph.interior(*subgraph);
        expect(inner.has_value()) << (inner ? std::string{} : inner.error().message);
        auto scale = inner->emplace("qa::Scale", "scale", {{"gain", 3.0f}});
        expect(scale.has_value());
        expect(inner->exportPort(*scale, true, "in", "in").has_value());
        expect(inner->exportPort(*scale, false, "out", "out").has_value());

        expect(graph.connect(*source, "out", *subgraph, "in").has_value());
        expect(graph.connect(*subgraph, "out", *sink, "in").has_value());

        auto runtime = Runtime::create(std::move(graph));
        expect(runtime.has_value()) << (runtime ? std::string{} : runtime.error().message);
        std::ignore = runUntilCollected(*runtime, "e4-nested", flat.samples.size());

        const Collected nested = takeCollected("e4-nested");
        expect(eq(nested.samples.size(), flat.samples.size()));
        expect(flat.samples == nested.samples) << "a subgraph changed the stream";
        expect(sameTags(flat.tags, nested.tags)) << "a subgraph changed the tag sequence";
    };

    // the entry's recursive view is the view the scheduler executes
    "recursive traversal agrees with the framework's own flatten"_test = [] {
        registerTestBlocks();
        std::ignore = registerBuiltinSchedulers();

        RuntimeGraph graph;
        auto         source   = graph.emplace("qa::RampSource", "source", {{"n_samples", gr::Size_t{64U}}});
        auto         subgraph = graph.emplaceSubgraph("inner");
        auto         nested   = graph.emplace("gr::scheduler::Simple<singleThreaded>", "nestedScheduler");
        expect(source.has_value() && subgraph.has_value() && nested.has_value()) << (nested ? std::string{} : nested.error().message);

        auto inner = graph.interior(*subgraph);
        expect(inner.has_value());
        expect(inner->emplace("qa::Scale", "innerScale").has_value());
        auto deeper = inner->emplaceSubgraph("deeper");
        expect(deeper.has_value());
        auto deepest = inner->interior(*deeper);
        expect(deepest.has_value());
        expect(deepest->emplace("qa::Scale", "deepestScale").has_value());

        auto nestedInterior = graph.interior(*nested);
        expect(nestedInterior.has_value()) << "a nested scheduler must open like any other graph-like block";
        expect(nestedInterior->emplace("qa::Scale", "scheduledScale").has_value());

        std::vector<std::string> fromEntry;
        for (const BlockHandle& handle : graph.blocks(RuntimeGraph::Recursive::Yes)) {
            fromEntry.push_back(typeOf(handle.uniqueName()));
        }

        // the same nesting, built typed, is the reference: the entry does not hand out its gr::Graph,
        // so the two are compared by what each block is rather than by its process-unique id
        gr::Graph reference;
        reference.emplaceBlock<RampSource>({{"name", std::string("source")}});
        auto  referenceSubgraph = std::make_shared<GraphWrapper<gr::Graph>>();
        auto& referenceInner    = *referenceSubgraph->graph();
        referenceInner.emplaceBlock<Scale>({{"name", std::string("innerScale")}});
        auto  referenceDeeper  = std::make_shared<GraphWrapper<gr::Graph>>();
        auto& referenceDeepest = *referenceDeeper->graph();
        referenceDeepest.emplaceBlock<Scale>({{"name", std::string("deepestScale")}});
        referenceInner.addBlock(referenceDeeper);
        reference.addBlock(referenceSubgraph);

        auto referenceScheduler = globalPluginLoader().instantiateScheduler("gr::scheduler::Simple<singleThreaded>");
        expect(referenceScheduler != nullptr);
        auto referenceSchedulerBlock = SchedulerModel::asBlockModelPtr(referenceScheduler);
        referenceSchedulerBlock->graph()->emplaceBlock<Scale>({{"name", std::string("scheduledScale")}});
        reference.addBlock(referenceSchedulerBlock);

        std::vector<std::string> fromFlatten;
        for (const std::shared_ptr<BlockModel>& block : graph::flatten<block::Category::All>(reference).blocks()) {
            fromFlatten.push_back(typeOf(block->uniqueName()));
        }

        std::ranges::sort(fromEntry);
        std::ranges::sort(fromFlatten);
        expect(eq(fromEntry.size(), fromFlatten.size())) << std::format("entry saw [{}], flatten saw [{}]", fromEntry, fromFlatten);
        expect(fromEntry == fromFlatten) << std::format("entry saw [{}], flatten saw [{}]", fromEntry, fromFlatten);
    };

    // an exported port appears on the subgraph's own handle and disappears again
    "exporting a port round-trips through the subgraph handle"_test = [] {
        registerTestBlocks();
        RuntimeGraph graph;
        auto         subgraph = graph.emplaceSubgraph("inner");
        expect(subgraph.has_value());

        auto inner = graph.interior(*subgraph);
        expect(inner.has_value());
        auto scale = inner->emplace("qa::Scale", "scale");
        expect(scale.has_value());

        expect(graph.outputPortNames(*subgraph).empty()) << "an unexported subgraph has no ports";

        expect(inner->exportPort(*scale, false, "out", "exported").has_value());
        expect(graph.outputPortNames(*subgraph) == std::vector<std::string>{"exported"});
        expect(!inner->exportedOutputPorts().empty());

        auto sink = graph.emplace("qa::RecordingSink", "sink");
        expect(sink.has_value());
        expect(graph.connect(*subgraph, "exported", *sink, "in").has_value());

        expect(inner->unexportPort(*scale, false, "out").has_value());
        expect(graph.outputPortNames(*subgraph).empty());
        expect(inner->exportedOutputPorts().empty());
    };

    // a subgraph's children hold the subgraph's progress sequence while the scheduler waits on the
    // top-level one, so nesting could cost wake-up latency. Printed, not asserted.
    "wall time of a flat and a nested chain"_test = [] {
        registerTestBlocks();
        constexpr gr::Size_t kSamples = 100000U;

        // both arms are timed the same way -- start, wait for the whole stream, stop -- because the
        // nested one cannot use runAndWait()
        const auto timeFlat = [] {
            auto runtime = erasedChain("e4-timing-flat", kSamples, 2.0f, gr::Runtime::kDefaultScheduler);
            expect(runtime.has_value());
            const auto elapsed = runUntilCollected(*runtime, "e4-timing-flat", kSamples);
            std::ignore        = takeCollected("e4-timing-flat");
            return elapsed;
        };

        const auto timeNested = [kSamples] {
            RuntimeGraph graph;
            auto         source   = graph.emplace("qa::RampSource", "source", {{"n_samples", kSamples}});
            auto         subgraph = graph.emplaceSubgraph("inner");
            auto         sink     = graph.emplace("qa::RecordingSink", "e4-timing-nested");
            expect(source.has_value() && subgraph.has_value() && sink.has_value());
            auto inner = graph.interior(*subgraph);
            expect(inner.has_value());
            auto scale = inner->emplace("qa::Scale", "scale", {{"gain", 2.0f}});
            expect(scale.has_value());
            expect(inner->exportPort(*scale, true, "in", "in").has_value());
            expect(inner->exportPort(*scale, false, "out", "out").has_value());
            expect(graph.connect(*source, "out", *subgraph, "in").has_value());
            expect(graph.connect(*subgraph, "out", *sink, "in").has_value());

            auto runtime = Runtime::create(std::move(graph), gr::Runtime::kDefaultScheduler);
            expect(runtime.has_value());
            const auto elapsed = runUntilCollected(*runtime, "e4-timing-nested", kSamples);
            std::ignore        = takeCollected("e4-timing-nested");
            return elapsed;
        };

        std::ignore       = timeFlat(); // discard the first run of each shape
        std::ignore       = timeNested();
        const auto flat   = timeFlat();
        const auto nested = timeNested();
        std::println("e4 progress sequence, {} samples: flat {} us, nested {} us, ratio {:.3f}", //
            kSamples, flat.count(), nested.count(), static_cast<double>(nested.count()) / static_cast<double>(flat.count()));
    };

    // the erased path reaches the framework's own string-keyed endpoints
    "the message plane serializes the running graph back"_test = [] {
        registerTestBlocks();
        auto runtime = erasedChain("e5-messages", 256U, 1.0f);
        expect(runtime.has_value());

        expect(runtime->send(Runtime::Command::Get, "", gr::scheduler::property::kGraphGRC).has_value());
        expect(runtime->runAndWait().has_value());
        std::ignore = takeCollected("e5-messages");

        const std::vector<RuntimeEvent> events = runtime->pollEvents();
        const auto                      reply  = std::ranges::find_if(events, [](const RuntimeEvent& event) { return event.endpoint == gr::scheduler::property::kGraphGRC; });
        expect(reply != events.end()) << "the scheduler did not answer on the GraphGRC endpoint";
        if (reply != events.end()) {
            expect(!reply->isError) << reply->text;
            const auto yaml = reply->data.find(convert_string_domain(std::string("value")));
            expect(yaml != reply->data.end()) << "the reply carried no serialized graph";
            if (yaml != reply->data.end()) {
                const std::string_view text = yaml->second.value_or(std::string_view{});
                expect(text.find("qa::Scale") != std::string_view::npos) << text;
            }
        }
    };

    // a reply is told from a notification by its command and matched to its request by its id
    "an event carries its message's command and request id"_test = [] {
        registerTestBlocks();
        constexpr float kInitialGain = 2.0f;
        constexpr float kStagedGain  = 5.0f;

        RuntimeGraph graph;
        auto         source = graph.emplace("qa::RampSource", "source", {{"n_samples", gr::Size_t{1U << 20U}}, {"tag_every", gr::Size_t{0U}}});
        auto         scale  = graph.emplace("qa::Scale", "scale", {{"gain", kInitialGain}});
        auto         sink   = graph.emplace("qa::RecordingSink", "e6-events");
        expect(fatal(source.has_value() && scale.has_value() && sink.has_value()));
        expect(graph.connect(*source, "out", *scale, "in").has_value());
        expect(graph.connect(*scale, "out", *sink, "in").has_value());
        const std::string scaleName(scale->uniqueName());

        auto runtime = Runtime::create(std::move(graph));
        expect(fatal(runtime.has_value()));

        // queued before the run, handled by its first sweeps: the staged gain is applied while the graph runs
        const std::string settings(gr::block::property::kSetting);
        expect(runtime->send(Runtime::Command::Get, scaleName, settings, {}, "get-1").has_value());
        expect(runtime->send(Runtime::Command::Subscribe, scaleName, settings, {}, "subscribe-2").has_value());
        expect(runtime->send(Runtime::Command::Set, scaleName, settings, {{"gain", kStagedGain}}).has_value());
        expect(runtime->send(Runtime::Command::Get, scaleName, settings).has_value());
        const property_map absentEdge{{std::pmr::string(serialization_fields::EDGE_SOURCE_BLOCK), std::string("qa_no_such_block")}, {std::pmr::string(serialization_fields::EDGE_SOURCE_PORT), std::string("out")}};
        expect(runtime->send(Runtime::Command::Set, "", gr::scheduler::property::kRemoveEdge, absentEdge, "remove-3").has_value());
        expect(runtime->runAndWait().has_value());
        std::ignore = takeCollected("e6-events");

        const std::vector<RuntimeEvent> events    = runtime->pollEvents(1024UZ);
        const auto                      fromScale = [&events, &scaleName, &settings](RuntimeCommand command, std::string_view id) {
            std::vector<RuntimeEvent> matching;
            std::ranges::copy_if(events, std::back_inserter(matching), [&](const RuntimeEvent& event) { return event.source == scaleName && event.endpoint == settings && event.command == command && event.clientRequestID == id; });
            return matching;
        };

        const std::vector<RuntimeEvent> replies = fromScale(RuntimeCommand::Final, "get-1");
        expect(fatal(eq(replies.size(), 1UZ))) << "a Get with an id is answered exactly once, by a Final carrying that id";
        expect(!replies.front().isError) << replies.front().text;
        expect(floatAt(replies.front().data, "gain") == kInitialGain) << "the reply holds the settings the block had when it answered";
        for (const auto& setting : scale->get()) {
            expect(replies.front().data.contains(setting.first)) << std::format("the reply lacks setting '{}'", std::string_view(setting.first));
        }

        const std::vector<RuntimeEvent> notifications = fromScale(RuntimeCommand::Notify, "subscribe-2");
        expect(std::ranges::any_of(notifications, [](const RuntimeEvent& event) { return floatAt(event.data, "gain").value_or(0.0f) == kStagedGain; })) << "a Notify carries the subscription's id and the staged value";
        expect(eq(fromScale(RuntimeCommand::Final, "").size(), 1UZ)) << "the reply to a Get sent without an id carries an empty id";

        const auto refused = std::ranges::find_if(events, [](const RuntimeEvent& event) { return event.isError && event.clientRequestID == "remove-3"; });
        expect(fatal(refused != events.end())) << "an error reply carries the id of its request";
        expect(refused->command == RuntimeCommand::Final) << "an error reply is a Final";
    };

    // a saved document loads into a graph that runs to the same stream with the same settings
    "a graph document saved and loaded again runs the same"_test = [] {
        registerTestBlocks();
        RuntimeGraph original;
        auto         source = original.emplace("qa::RampSource", "source", {{"n_samples", gr::Size_t{4096U}}});
        auto         scale  = original.emplace("qa::Scale", "scale", {{"gain", 3.0f}, {"label", std::string("saved")}, {"taps", std::vector<float>{0.5f, 0.25f}}});
        auto         sink   = original.emplace("qa::RecordingSink", "e7-document");
        expect(fatal(source.has_value() && scale.has_value() && sink.has_value()));
        expect(original.connect(*source, "out", *scale, "in").has_value());
        expect(original.connect(*scale, "out", *sink, "in").has_value());

        const auto saved = original.toYaml();
        expect(fatal(saved.has_value())) << (saved ? std::string{} : saved.error().message);
        auto loaded = RuntimeGraph::fromYaml(*saved);
        expect(fatal(loaded.has_value())) << (loaded ? std::string{} : loaded.error().message);
        const bool sameSettings = settingsByName(*loaded) == settingsByName(original);
        expect(sameSettings) << "a loaded block holds other settings than the one saved";

        auto originalRuntime = Runtime::create(std::move(original));
        expect(fatal(originalRuntime.has_value()));
        expect(originalRuntime->runAndWait().has_value());
        const Collected fromOriginal = takeCollected("e7-document");

        const auto resaved = originalRuntime->graph().toYaml();
        expect(fatal(resaved.has_value())) << (resaved ? std::string{} : resaved.error().message);
        expect(*resaved == *saved) << std::format("saved before the run:\n{}\nsaved after it:\n{}", *saved, *resaved);

        auto loadedRuntime = Runtime::create(std::move(*loaded));
        expect(fatal(loadedRuntime.has_value())) << (loadedRuntime ? std::string{} : loadedRuntime.error().message);
        expect(loadedRuntime->runAndWait().has_value());
        const Collected fromLoaded = takeCollected("e7-document");

        expect(eq(fromOriginal.samples.size(), 4096UZ));
        expect(fromOriginal.samples == fromLoaded.samples) << "the loaded graph changed the stream";
        expect(sameTags(fromOriginal.tags, fromLoaded.tags)) << "the loaded graph changed the tag sequence";
    };

    "settings given to fromYaml replace the document's, and a name no block carries is an error"_test = [] {
        registerTestBlocks();
        constexpr std::string_view document = "blocks:\n  - id: qa::Scale\n    parameters:\n      name: scale\n      gain: 3.0\n      label: from-the-document\n";

        auto loaded = RuntimeGraph::fromYaml(document, {{"scale", {{"gain", 5.0f}}}});
        expect(fatal(loaded.has_value())) << (loaded ? std::string{} : loaded.error().message);
        const std::vector<BlockHandle> blocks = loaded->blocks();
        expect(fatal(eq(blocks.size(), 1UZ)));
        expect(blocks.front().get(std::string("gain")) == std::optional<pmt::Value>(5.0f));
        expect(blocks.front().get(std::string("label")) == std::optional<pmt::Value>(std::string("from-the-document")));

        std::optional<std::expected<RuntimeGraph, RuntimeError>> unknownName;
        expect(nothrow([&] { unknownName.emplace(RuntimeGraph::fromYaml(document, {{"scal", {{"gain", 5.0f}}}})); }));
        expect(fatal(unknownName.has_value() && !unknownName->has_value()));
        expect(unknownName->error().message.contains("block 'scal'")) << unknownName->error().message;
        expect(eq(unknownName->error().where, std::string("RuntimeGraph::fromYaml")));
    };

    "a document the reader refuses is an error, never an exception"_test = [] {
        registerTestBlocks();
        using Loaded = std::expected<RuntimeGraph, RuntimeError>;

        // the quote on the fourth line never closes
        constexpr std::string_view malformed = "blocks:\n  - id: qa::Scale\n    parameters:\n      name: \"scale\n";
        std::optional<Loaded>      fromMalformed;
        expect(nothrow([&] { fromMalformed.emplace(RuntimeGraph::fromYaml(malformed)); }));
        expect(fatal(fromMalformed.has_value() && !fromMalformed->has_value()));
        const auto parsed = pmt::yaml::deserialize(malformed);
        expect(fatal(!parsed.has_value()));
        const RuntimeError& parseError = fromMalformed->error();
        expect(parseError.message.starts_with(std::format("line {}, column {}: ", parsed.error().line, parsed.error().column))) << parseError.message;
        expect(parseError.message.contains(parsed.error().message)) << parseError.message;
        expect(!parseError.message.contains(malformed)) << "the message carries the document text: " << parseError.message;
        expect(eq(parseError.where, std::string("RuntimeGraph::fromYaml")));

        constexpr std::string_view unknownType = "blocks:\n  - id: qa::NoSuchBlock\n    parameters:\n      name: nope\n";
        std::optional<Loaded>      fromUnknownType;
        expect(nothrow([&] { fromUnknownType.emplace(RuntimeGraph::fromYaml(unknownType)); }));
        expect(fatal(fromUnknownType.has_value() && !fromUnknownType->has_value()));
        expect(fromUnknownType->error().message.contains("qa::NoSuchBlock")) << fromUnknownType->error().message;
        expect(eq(fromUnknownType->error().where, std::string("RuntimeGraph::fromYaml")));
    };

    "the scheduler's own settings are read and staged through a block handle"_test = [] {
        registerTestBlocks();
        const auto sourceToSink = [](std::string_view sinkName) {
            RuntimeGraph graph;
            auto         source = graph.emplace("qa::RampSource", "source", {{"n_samples", gr::Size_t{64U}}});
            auto         sink   = graph.emplace("qa::RecordingSink", sinkName);
            expect(source.has_value() && sink.has_value());
            expect(graph.connect(*source, "out", *sink, "in").has_value());
            return graph;
        };
        const auto timeoutOf = [](const BlockHandle& scheduler) {
            const std::optional<pmt::Value> value = scheduler.get(std::string("timeout_ms"));
            return value.has_value() && value->get_if<gr::Size_t>() != nullptr ? std::optional<gr::Size_t>(*value->get_if<gr::Size_t>()) : std::nullopt;
        };

        auto withDefaults = Runtime::create(sourceToSink("e8-default"));
        expect(fatal(withDefaults.has_value()));
        const std::optional<gr::Size_t> defaultTimeout = timeoutOf(withDefaults->scheduler());
        expect(fatal(defaultTimeout.has_value())) << "the scheduler handle reports no timeout_ms";

        const gr::Size_t chosenTimeout = *defaultTimeout + 23U;
        auto             runtime       = Runtime::create(sourceToSink("e8-chosen"), Runtime::kDefaultScheduler, {{"timeout_ms", chosenTimeout}});
        expect(fatal(runtime.has_value()));
        BlockHandle scheduler = runtime->scheduler();
        expect(fatal(scheduler.valid()));
        expect(timeoutOf(scheduler) == chosenTimeout) << "the handle reports the timeout the scheduler was created with";

        const auto rejected = scheduler.setStaged({{"qa_undeclared", 1.0f}});
        expect(rejected.has_value() && rejected->contains(convert_string_domain(std::string("qa_undeclared")))) << "a key the scheduler does not declare comes back";
        expect(scheduler.setStaged({{"timeout_ms", chosenTimeout + 1U}}) == property_map{}) << "a key the scheduler declares is staged";

        BlockHandle               outliving;
        std::optional<gr::Size_t> timeoutWhileOwned;
        {
            Runtime moved = std::move(*runtime);
            expect(!runtime->scheduler().valid()) << "an empty Runtime has no scheduler";
            outliving = moved.scheduler();
            expect(fatal(outliving.valid()));
            timeoutWhileOwned = timeoutOf(outliving);
            expect(fatal(timeoutWhileOwned.has_value()));
        }
        expect(outliving.valid()) << "a scheduler handle outlives its Runtime";
        expect(timeoutOf(outliving) == timeoutWhileOwned) << "a scheduler handle reads the same settings after its Runtime is destroyed";
    };

    "a graph lists its edges by the port names connect() accepts"_test = [] {
        registerTestBlocks();
        const auto edgeInto = [](const std::vector<RuntimeEdge>& edges, std::string_view block, std::string_view port) {
            const auto found = std::ranges::find_if(edges, [&](const RuntimeEdge& edge) { return edge.destinationBlock.uniqueName() == block && edge.destinationPort == port; });
            return found == edges.end() ? std::optional<RuntimeEdge>{} : std::optional<RuntimeEdge>(*found);
        };

        const auto           disconnectListed = [](RuntimeGraph& graph, const RuntimeEdge& edge) { return graph.disconnect(edge.sourceBlock, edge.sourcePort, edge.destinationBlock, edge.destinationPort); };
        constexpr gr::Size_t kSamples         = 4096U;

        RuntimeGraph graph;
        auto         source = graph.emplace("qa::RampSource", "source", {{"n_samples", kSamples}, {"tag_every", gr::Size_t{0U}}});
        auto         sum    = graph.emplace("qa::SumInputs", "sum", {{"n_inputs", gr::Size_t{2U}}});
        auto         sink   = graph.emplace("qa::RecordingSink", "e9-edges");
        auto         tap    = graph.emplace("qa::RecordingSink", "e9-tap");
        expect(fatal(source.has_value() && sum.has_value() && sink.has_value() && tap.has_value()));

        const EdgeSpec explicitSpec{.minBufferSize = 8192UZ, .weight = 7, .name = "explicit"};
        expect(graph.connect(*source, "out", *sum, "in#1", explicitSpec).has_value());
        expect(graph.connect(*source, "out", *sum, "in#0").has_value());
        expect(graph.connect(*sum, "out", *sink, "in").has_value());
        expect(graph.connect(*source, "out", *tap, "in").has_value());

        const std::vector<RuntimeEdge> edges = graph.edges();
        expect(eq(edges.size(), 4UZ));
        const std::optional<RuntimeEdge> withSpec = edgeInto(edges, sum->uniqueName(), "in#1");
        expect(fatal(withSpec.has_value())) << "a collection element's edge is listed by the element's name";
        expect(eq(withSpec->sourceBlock.uniqueName(), source->uniqueName()));
        expect(eq(withSpec->sourcePort, std::string("out")));
        expect(eq(withSpec->edge.minBufferSize, explicitSpec.minBufferSize));
        expect(eq(withSpec->edge.weight, explicitSpec.weight));
        expect(eq(withSpec->edge.name, explicitSpec.name));

        const std::optional<RuntimeEdge> withoutSpec = edgeInto(edges, sum->uniqueName(), "in#0");
        expect(fatal(withoutSpec.has_value()));
        expect(gt(withoutSpec->edge.minBufferSize, 0UZ)) << "an edge given no size reports the size the framework chose";

        // Before create(), by the listed names. A block whose input no edge feeds never finishes, and the
        // tap leaves the graph before the run.
        const std::optional<RuntimeEdge> toTap = edgeInto(edges, tap->uniqueName(), "in");
        expect(fatal(toTap.has_value()));
        const auto beforeRun = disconnectListed(graph, *toTap);
        expect(fatal(beforeRun.has_value())) << (beforeRun ? std::string{} : beforeRun.error().message);
        expect(!edgeInto(graph.edges(), tap->uniqueName(), "in").has_value()) << "a disconnected edge is not listed";
        expect(eq(graph.edges().size(), 3UZ));
        expect(fatal(graph.remove(*tap).has_value()));

        const std::string sumName(sum->uniqueName());
        auto              runtime = Runtime::create(std::move(graph));
        expect(fatal(runtime.has_value()));
        expect(runtime->runAndWait().has_value());
        expect(eq(takeCollected("e9-edges").samples.size(), std::size_t{kSamples}));

        // after the run to completion, through the view of the scheduler's graph
        RuntimeGraph                     running = runtime->graph();
        const std::optional<RuntimeEdge> toFirst = edgeInto(running.edges(), sumName, "in#0");
        expect(fatal(toFirst.has_value()));
        const auto afterRun = disconnectListed(running, *toFirst);
        expect(afterRun.has_value()) << (afterRun ? std::string{} : afterRun.error().message);
        const std::vector<RuntimeEdge> remaining = running.edges();
        expect(eq(remaining.size(), 2UZ));
        expect(!edgeInto(remaining, sumName, "in#0").has_value()) << "a disconnected edge is not listed";
        expect(edgeInto(remaining, sumName, "in#1").has_value());
    };

    "a loaded document's edges by index are listed by port name"_test = [] {
        registerTestBlocks();
        constexpr std::string_view document = R"yaml(blocks:
  - id: qa::RampSource
    parameters:
      name: source
  - id: qa::SumInputs
    parameters:
      name: sum
  - id: qa::RecordingSink
    parameters:
      name: sink
connections:
  - [source, 0, sum, [0, 0]]
  - [source, 0, sum, [0, 1]]
  - [sum, 0, sink, 0]
)yaml";
        auto                       loaded   = RuntimeGraph::fromYaml(document);
        expect(fatal(loaded.has_value())) << (loaded ? std::string{} : loaded.error().message);

        std::vector<std::string> spelled;
        for (const RuntimeEdge& edge : loaded->edges()) {
            spelled.push_back(std::format("{}.{} -> {}.{}", edge.sourceBlock.name(), edge.sourcePort, edge.destinationBlock.name(), edge.destinationPort));
            const std::vector<std::string> outputs = loaded->outputPortNames(edge.sourceBlock);
            const std::vector<std::string> inputs  = loaded->inputPortNames(edge.destinationBlock);
            expect(std::ranges::find(outputs, edge.sourcePort) != outputs.end()) << edge.sourcePort;
            expect(std::ranges::find(inputs, edge.destinationPort) != inputs.end()) << edge.destinationPort;
            expect(gt(edge.edge.minBufferSize, 0UZ));
        }
        std::ranges::sort(spelled);
        expect(spelled == std::vector<std::string>{"source.out -> sum.in#0", "source.out -> sum.in#1", "sum.out -> sink.in"}) << std::format("{}", spelled);

        // the listed names select an edge made by index, here after a run to completion
        auto runtime = Runtime::create(std::move(*loaded));
        expect(fatal(runtime.has_value())) << (runtime ? std::string{} : runtime.error().message);
        expect(runtime->runAndWait().has_value());
        std::ignore = takeCollected("sink");

        RuntimeGraph                   running = runtime->graph();
        const std::vector<RuntimeEdge> listed  = running.edges();
        const auto                     byIndex = std::ranges::find_if(listed, [](const RuntimeEdge& edge) { return edge.destinationPort == "in#1"; });
        expect(fatal(byIndex != listed.end()));
        const auto disconnected = running.disconnect(byIndex->sourceBlock, byIndex->sourcePort, byIndex->destinationBlock, byIndex->destinationPort);
        expect(disconnected.has_value()) << (disconnected ? std::string{} : disconnected.error().message);
        const std::vector<RuntimeEdge> remaining = running.edges();
        expect(eq(remaining.size(), 2UZ));
        expect(std::ranges::none_of(remaining, [](const RuntimeEdge& edge) { return edge.destinationPort == "in#1"; })) << "a disconnected edge is not listed";
    };

    "a block's ports are described one by one, as the name lists give them"_test = [] {
        registerTestBlocks();
        RuntimeGraph graph;
        auto         scale        = graph.emplace("qa::Scale", "scale");
        auto         sum          = graph.emplace("qa::SumInputs", "sum");
        auto         dual         = graph.add(std::make_shared<BlockWrapper<DualSource>>(), "dual");
        auto         async        = graph.add(std::make_shared<BlockWrapper<AsyncInput>>(), "async");
        auto         chunkedModel = std::make_shared<BlockWrapper<ControlledChunks>>();
        auto         chunked      = graph.add(chunkedModel, "chunked");
        expect(fatal(scale.has_value() && sum.has_value() && dual.has_value() && async.has_value() && chunked.has_value()));

        const auto describedAsListed = [&graph](const BlockHandle& block, bool isInput) {
            const std::vector<std::string> names = isInput ? graph.inputPortNames(block) : graph.outputPortNames(block);
            const std::vector<RuntimePort> ports = isInput ? graph.inputPorts(block) : graph.outputPorts(block);
            expect(fatal(eq(ports.size(), names.size()))) << block.name();
            for (std::size_t i = 0UZ; i < ports.size(); ++i) {
                expect(eq(ports[i].name, names[i])) << block.name();
                expect(ports[i].isInput == isInput) << names[i];
            }
            return ports;
        };
        for (const BlockHandle& block : graph.blocks()) {
            std::ignore = describedAsListed(block, true);
            std::ignore = describedAsListed(block, false);
        }

        const std::vector<RuntimePort> elements = describedAsListed(*sum, true);
        expect(fatal(!elements.empty()));
        for (std::size_t i = 0UZ; i < elements.size(); ++i) {
            expect(eq(elements[i].collection, std::string("in"))) << elements[i].name;
            expect(eq(elements[i].index, i)) << elements[i].name;
        }
        const std::vector<RuntimePort> scaleInputs = describedAsListed(*scale, true);
        expect(fatal(eq(scaleInputs.size(), 1UZ)));
        expect(scaleInputs.front().collection.empty()) << "a port outside a collection names none";
        expect(scaleInputs.front().isSynchronous && !scaleInputs.front().isOptional);

        const auto flagOf = [](const std::vector<RuntimePort>& ports, std::string_view name, bool RuntimePort::*flag) -> std::optional<bool> {
            const auto found = std::ranges::find_if(ports, [name](const RuntimePort& port) { return port.name == name; });
            return found == ports.end() ? std::nullopt : std::optional<bool>((*found).*flag);
        };
        const std::vector<RuntimePort> dualOutputs = describedAsListed(*dual, false);
        expect(flagOf(dualOutputs, "monitor", &RuntimePort::isOptional) == std::optional<bool>(true)) << "a port declared Optional reports so";
        expect(flagOf(dualOutputs, "out", &RuntimePort::isOptional) == std::optional<bool>(false));
        expect(flagOf(describedAsListed(*async, true), "in", &RuntimePort::isSynchronous) == std::optional<bool>(false)) << "a port declared Async reports so";
        expect(flagOf(describedAsListed(*async, false), "out", &RuntimePort::isSynchronous) == std::optional<bool>(true));

        const std::vector<RuntimePort> chunkedInputs = describedAsListed(*chunked, true);
        expect(flagOf(chunkedInputs, "control", &RuntimePort::isMessage) == std::optional<bool>(true)) << "a message port reports so";
        expect(flagOf(chunkedInputs, "in", &RuntimePort::isMessage) == std::optional<bool>(false));
        const auto chunkedIn = std::ranges::find_if(chunkedInputs, [](const RuntimePort& port) { return port.name == "in"; });
        expect(fatal(chunkedIn != chunkedInputs.end()));
        const ControlledChunks& chunkedBlock = chunkedModel->blockRef();
        expect(eq(chunkedIn->minSamples, chunkedBlock.in.min_samples)) << "the minimum the port declares";
        expect(eq(chunkedIn->maxSamples, chunkedBlock.in.max_samples)) << "the maximum the port declares";
        expect(eq(chunkedIn->domain, std::string(chunkedBlock.in.domain()))) << "the port's compute domain";
    };

    "a port reads connected only while a running scheduler holds it"_test = [] {
        registerTestBlocks();
        const auto connectedOf  = [](const std::vector<RuntimePort>& ports) { return !ports.empty() && ports.front().isConnected; };
        const auto sourceToSink = [](gr::Size_t nSamples, std::string_view sinkName) {
            RuntimeGraph graph;
            auto         source = graph.emplace("qa::RampSource", "source", {{"n_samples", nSamples}, {"tag_every", gr::Size_t{0U}}});
            auto         sink   = graph.emplace("qa::RecordingSink", sinkName);
            expect(fatal(source.has_value() && sink.has_value()));
            expect(graph.connect(*source, "out", *sink, "in").has_value());
            return std::tuple{std::move(graph), *source, *sink};
        };

        auto [running, source, sink] = sourceToSink(gr::Size_t{1U << 24U}, "e11-running");
        expect(!connectedOf(running.outputPorts(source))) << "an output reads connected before create()";
        expect(!connectedOf(running.inputPorts(sink))) << "an input reads connected before create()";
        auto runtime = Runtime::create(std::move(running));
        expect(fatal(runtime.has_value()));
        runtime->start();
        const auto started = std::chrono::steady_clock::now();
        while (collectedSize("e11-running") == 0UZ && std::chrono::steady_clock::now() - started < std::chrono::seconds(3)) {
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
        expect(fatal(gt(collectedSize("e11-running"), 0UZ))) << "no sample reached the sink";
        const RuntimeGraph view = runtime->graph();
        expect(connectedOf(view.outputPorts(source))) << "a running scheduler holds the output connected";
        expect(connectedOf(view.inputPorts(sink))) << "a running scheduler holds the input connected";
        runtime->stop();
        std::ignore = takeCollected("e11-running");

        auto [finite, finiteSource, finiteSink] = sourceToSink(gr::Size_t{4096U}, "e11-finished");
        auto finished                           = Runtime::create(std::move(finite));
        expect(fatal(finished.has_value()));
        expect(finished->runAndWait().has_value());
        std::ignore = takeCollected("e11-finished");
        expect(!connectedOf(finished->graph().outputPorts(finiteSource))) << "an output reads connected after its blocks stopped";
        expect(!connectedOf(finished->graph().inputPorts(finiteSink))) << "an input reads connected after its block stopped";
    };
};

// start() runs the graph on the Runtime's own thread; stop() and stopFor() end that run, and wait(), waitFor() and
// result() report it
const boost::ut::suite<"background run"> backgroundRunTests = [] {
    using namespace boost::ut;
    using namespace gr;
    using namespace qa_runtime;
    using namespace std::chrono_literals;

    "a finite graph started in the background ends by itself, and its result is success"_test = [] {
        registerTestBlocks();
        auto runtime = erasedChain("r1-finite", 1024U, 2.0f);
        expect(fatal(runtime.has_value()));
        const std::optional<RuntimeError> refused = runtime->start();
        expect(fatal(!refused.has_value())) << (refused ? refused->message : std::string{});
        runtime->wait();
        expect(runtime->waitFor(0ns)) << "wait() returned while the run was in progress";
        const std::expected<void, RuntimeError> result = runtime->result();
        expect(result.has_value()) << (result ? std::string{} : result.error().message);
        expect(runtime->state() == Runtime::State::Stopped) << "a run that ended by itself settles stopped";
        expect(eq(takeCollected("r1-finite").samples.size(), 1024UZ));
    };

    "a failed run's result carries the block's error"_test = [] {
        registerTestBlocks();
        RuntimeGraph graph;
        auto         source = graph.emplace("qa::FailingStartSource", "failing");
        auto         sink   = graph.emplace("qa::RecordingSink", "r2-failed");
        expect(fatal(source.has_value() && sink.has_value()));
        expect(graph.connect(*source, "out", *sink, "in").has_value());
        const std::string sourceName(source->uniqueName());
        auto              runtime = Runtime::create(std::move(graph));
        expect(fatal(runtime.has_value()));

        expect(fatal(!runtime->start().has_value()));
        runtime->wait();
        const std::expected<void, RuntimeError> result = runtime->result();
        expect(fatal(!result.has_value())) << "a run whose block could not start reported success";
        expect(result.error().message.contains("the source refused to start")) << result.error().message;
        expect(result.error().message.contains(sourceName)) << result.error().message;
        expect(runtime->state() == Runtime::State::Error);
        runtime->stop();
        expect(runtime->state() == Runtime::State::Error) << "a stop after the failed run changed the state";
        expect(eq(takeCollected("r2-failed").samples.size(), 0UZ));
    };

    "a stop before any run leaves the next run whole"_test = [] {
        registerTestBlocks();
        auto runtime = erasedChain("r3-stopped-first", 1024U, 1.0f);
        expect(fatal(runtime.has_value()));
        runtime->stop();
        expect(runtime->state() == Runtime::State::Idle) << "a stop without a run changed the state";
        expect(runtime->runAndWait().has_value());
        expect(eq(takeCollected("r3-stopped-first").samples.size(), 1024UZ)) << "the run after a stop without a run delivered less than the stream";
    };

    "a second stop and a stop after the run ended leave the scheduler stopped"_test = [] {
        registerTestBlocks();
        auto endless = endlessChain("r4-endless");
        expect(fatal(endless.has_value()));
        expect(fatal(!endless->start().has_value()));
        expect(gt(awaitCountAbove("r4-endless", 0UZ), 0UZ)) << "the endless run delivered nothing";
        endless->stop();
        expect(endless->waitFor(0ns)) << "stop() returned before the run ended";
        expect(endless->state() == Runtime::State::Stopped);
        endless->stop();
        expect(endless->state() == Runtime::State::Stopped) << "a second stop changed the state";
        expect(endless->result().has_value()) << "a stopped run is a successful run";

        auto background = erasedChain("r4-background", 256U, 1.0f);
        expect(fatal(background.has_value()));
        expect(fatal(!background->start().has_value()));
        background->wait();
        background->stop();
        expect(background->state() == Runtime::State::Stopped) << "a stop after the background run ended changed the state";
        std::ignore = takeCollected("r4-background");

        auto foreground = erasedChain("r4-foreground", 256U, 1.0f);
        expect(fatal(foreground.has_value()));
        expect(foreground->runAndWait().has_value());
        foreground->stop();
        expect(foreground->state() == Runtime::State::Stopped) << "a stop after runAndWait() returned changed the state";
        std::ignore = takeCollected("r4-foreground");
    };

    "a run after a stop runs the graph again"_test = [] {
        registerTestBlocks();
        auto runtime = endlessChain("r5-again");
        expect(fatal(runtime.has_value()));
        expect(fatal(!runtime->start().has_value()));
        expect(gt(awaitCountAbove("r5-again", 0UZ), 0UZ)) << "the first run delivered nothing";
        runtime->stop();
        const std::size_t nFirst = countedSize("r5-again");
        expect(fatal(!runtime->start().has_value())) << "a start after a stop was refused";
        expect(gt(awaitCountAbove("r5-again", nFirst), nFirst)) << "the second run delivered nothing";
        runtime->stop();
        expect(runtime->state() == Runtime::State::Stopped);
        expect(runtime->result().has_value());
    };

    "a stop right after a start ends the run, on a fresh scheduler and on a stopped one"_test = [] {
        registerTestBlocks();
        auto runtime = endlessChain("r9-immediate");
        expect(fatal(runtime.has_value()));
        for (std::size_t run = 0UZ; run < 3UZ; ++run) {
            expect(fatal(!runtime->start().has_value()));
            runtime->stop();
            expect(runtime->waitFor(0ns)) << std::format("run {}: stop() returned before the run ended", run);
            expect(runtime->state() == Runtime::State::Stopped) << std::format("run {}", run);
            expect(runtime->result().has_value()) << std::format("run {}", run);
        }
    };

    "a start while a run is in progress is refused"_test = [] {
        registerTestBlocks();
        auto runtime = endlessChain("r6-refused");
        expect(fatal(runtime.has_value()));
        expect(fatal(!runtime->start().has_value()));
        expect(gt(awaitCountAbove("r6-refused", 0UZ), 0UZ)) << "the run delivered nothing";

        const std::optional<RuntimeError> second = runtime->start();
        expect(fatal(second.has_value())) << "a second start() during the run was accepted";
        expect(second->message.contains("a run is in progress")) << second->message;
        const std::expected<void, RuntimeError> foreground = runtime->runAndWait();
        expect(fatal(!foreground.has_value())) << "runAndWait() during the run was accepted";
        expect(foreground.error().message.contains("a run is in progress")) << foreground.error().message;
        expect(!runtime->waitFor(0ns)) << "a refused start ended the run in progress";
        expect(!runtime->result().has_value()) << "result() reported a run in progress as ended";

        runtime->stop();
        expect(runtime->result().has_value()) << "the refusals changed the result of the run they did not start";
    };

    "a timed wait returns false while the run goes on and true once it is stopped"_test = [] {
        registerTestBlocks();
        auto runtime = endlessChain("r7-timed");
        expect(fatal(runtime.has_value()));
        expect(fatal(!runtime->start().has_value()));

        constexpr std::chrono::milliseconds kShortWait{20};
        const auto                          beforeShortWait = std::chrono::steady_clock::now();
        expect(!runtime->waitFor(kShortWait)) << "an endless run read as ended";
        expect(std::chrono::steady_clock::now() - beforeShortWait >= kShortWait) << "the timed wait returned before its timeout";

        constexpr std::chrono::seconds kLongWait{10};
        std::thread                    stopper([&runtime] { runtime->stop(); });
        const auto                     beforeLongWait = std::chrono::steady_clock::now();
        const bool                     ended          = runtime->waitFor(kLongWait);
        const auto                     waited         = std::chrono::steady_clock::now() - beforeLongWait;
        stopper.join();
        expect(ended) << "the wait timed out although another thread stopped the run";
        expect(waited < kLongWait) << "the wait did not return when the run ended";
        expect(runtime->waitFor(0ms)) << "a wait after the run ended returned false";
    };

    "a runtime destroyed during a run stops the run and joins its thread"_test = [] {
        registerTestBlocks();
        std::optional<Runtime> runtime;
        {
            auto created = endlessChain("r8-destroyed");
            expect(fatal(created.has_value()));
            runtime.emplace(std::move(*created));
        }
        expect(fatal(!runtime->start().has_value()));
        expect(gt(awaitCountAbove("r8-destroyed", 0UZ), 0UZ)) << "the run delivered nothing";
        runtime.reset(); // a joinable thread destroyed unjoined terminates the process, and an unstopped run never ends
    };

    "a timed stop of an endless graph ends it and returns true"_test = [] {
        registerTestBlocks();
        auto runtime = endlessChain("r10-timed-stop");
        expect(fatal(runtime.has_value()));
        expect(fatal(!runtime->start().has_value()));
        expect(gt(awaitCountAbove("r10-timed-stop", 0UZ), 0UZ)) << "the endless run delivered nothing";
        expect(runtime->stopFor(10s)) << "a timed stop did not end an endless run";
        expect(runtime->waitFor(0ns)) << "stopFor() returned true before the run ended";
        expect(runtime->state() == Runtime::State::Stopped);
        expect(runtime->result().has_value()) << "a stopped run is a successful run";
    };

    "a zero timeout during a run returns false, and a later timed stop ends the run"_test = [] {
        registerTestBlocks();
        auto runtime = endlessChain("r11-zero-timeout");
        expect(fatal(runtime.has_value()));
        expect(fatal(!runtime->start().has_value()));
        expect(gt(awaitCountAbove("r11-zero-timeout", 0UZ), 0UZ)) << "the endless run delivered nothing";
        expect(!runtime->stopFor(0ns)) << "a zero timeout read an endless run as ended";
        expect(runtime->stopFor(10s)) << "the later timed stop did not end the run";
        expect(runtime->waitFor(0ns)) << "stopFor() returned true before the run ended";
        expect(runtime->state() == Runtime::State::Stopped);
        expect(runtime->result().has_value()) << "a stopped run is a successful run";
    };

    "a timed stop without a run returns true and changes nothing"_test = [] {
        registerTestBlocks();
        auto runtime = erasedChain("r12-no-run", 1024U, 1.0f);
        expect(fatal(runtime.has_value()));
        expect(runtime->stopFor(0ns)) << "a timed stop before any run returned false";
        expect(runtime->stopFor(20ms)) << "a timed stop before any run returned false";
        expect(runtime->state() == Runtime::State::Idle) << "a timed stop without a run changed the state";
        expect(runtime->runAndWait().has_value());
        expect(eq(takeCollected("r12-no-run").samples.size(), 1024UZ)) << "the run after a timed stop without a run delivered less than the stream";
        expect(runtime->stopFor(0ns)) << "a timed stop after the run ended returned false";
        expect(runtime->state() == Runtime::State::Stopped) << "a timed stop after the run ended changed the state";
        expect(runtime->result().has_value()) << "a timed stop after the run ended changed its result";
    };

    "a stop held in a block's stop() past the timeout returns false, and a later call waits for the same stop"_test = [] {
        registerTestBlocks();
        closeStopGate();
        RuntimeGraph graph;
        auto         source = graph.emplace("qa::EndlessSource", "source");
        auto         sink   = graph.add(std::make_shared<BlockWrapper<GatedStopSink>>(), "gated");
        expect(fatal(source.has_value() && sink.has_value()));
        expect(graph.connect(*source, "out", *sink, "in").has_value());
        auto runtime = Runtime::create(std::move(graph));
        expect(fatal(runtime.has_value()));
        expect(fatal(!runtime->start().has_value()));

        constexpr std::chrono::milliseconds kTimeout{20};
        expect(!runtime->stopFor(kTimeout)) << "a stop held in a block's stop() read as ended";
        expect(eq(awaitHeldStops(1UZ), 1UZ)) << "the stop did not reach the block";
        const auto beforeLaterCall = std::chrono::steady_clock::now();
        expect(!runtime->stopFor(kTimeout)) << "a later call read the held stop as ended";
        expect(std::chrono::steady_clock::now() - beforeLaterCall < 5s) << "a later call waited for the held stop() past its own timeout";
        expect(!runtime->waitFor(0ns)) << "the run ended while its block was inside stop()";

        openStopGate();
        expect(runtime->stopFor(10s)) << "the run did not end once the block's stop() returned";
        expect(runtime->waitFor(0ns)) << "stopFor() returned true before the run ended";
        expect(eq(awaitHeldStops(1UZ), 1UZ)) << "the block's stop() ran more than once";
        expect(runtime->state() == Runtime::State::Stopped);
        expect(runtime->result().has_value()) << "a stopped run is a successful run";
    };
};

const boost::ut::suite<"refusals return"> refusalTests = [] {
    using namespace boost::ut;
    using namespace gr;
    using namespace qa_runtime;

    "a staged value the block cannot convert is returned, not thrown"_test = [] {
        registerTestBlocks();
        RuntimeGraph graph;
        auto         scale = graph.emplace("qa::Scale", "scale");
        expect(fatal(scale.has_value()));
        const property_map mixed{{"label", std::string("staged")}, {"gain", std::string("loud")}, {"qa_undeclared", 1.0f}};
        expect(nothrow([&] { std::ignore = scale->setStaged(mixed); }));
        const auto refused = scale->setStaged(mixed);
        expect(!refused.has_value()) << "the value was taken";
        expect(refused.has_value() || refused.error().message.find("gain") != std::string::npos) << (refused ? std::string{} : refused.error().message);
        expect(scale->stagedParameters().empty()) << "a refused call staged part of its values";
    };

    "a block built with a value it refuses is refused by add()"_test = [] {
        RuntimeGraph graph;
        expect(nothrow([&] { std::ignore = graph.add(std::make_shared<BlockWrapper<Scale>>(property_map{{"gain", std::string("loud")}}), "scale"); }));
        const auto refused = graph.add(std::make_shared<BlockWrapper<Scale>>(property_map{{"gain", std::string("loud")}}), "refused");
        expect(!refused.has_value()) << "a block that refused its parameters was added";
        expect(graph.blocks().empty()) << "a refused block stayed in the graph";
    };

    "a nested graph with a setting it does not declare is refused by emplaceSubgraph()"_test = [] {
        RuntimeGraph graph;
        expect(nothrow([&] { std::ignore = graph.emplaceSubgraph("sub", property_map{{"qa_undeclared", 1.0f}}); }));
        const auto refused = graph.emplaceSubgraph("refused", property_map{{"qa_undeclared", 1.0f}});
        expect(!refused.has_value()) << "a nested graph that refused its parameters was added";
        expect(graph.blocks().empty()) << "a refused nested graph stayed in the graph";
        expect(graph.emplaceSubgraph("sub").has_value()) << "a nested graph with no parameters is added";
    };

    "a scheduler setting that is misspelled or refused is refused by create()"_test = [] {
        registerTestBlocks();
        const auto emptyGraph = [] { return RuntimeGraph{}; };

        const auto misspelled = Runtime::create(emptyGraph(), Runtime::kDefaultScheduler, property_map{{"timeout_msx", gr::Size_t{10U}}});
        expect(!misspelled.has_value()) << "a key the scheduler does not declare was dropped";

        expect(!misspelled.has_value() && misspelled.error().message.find("timeout_msx") != std::string::npos) << "the refusal names the key";

        RuntimeGraph kept;
        expect(fatal(kept.emplace("qa::Scale", "scale").has_value()));
        expect(!Runtime::create(std::move(kept), Runtime::kDefaultScheduler, property_map{{"timeout_msx", gr::Size_t{10U}}}).has_value());
        expect(eq(kept.blocks().size(), 1UZ)) << "a refused create() took the caller's graph";
        expect(Runtime::create(std::move(kept)).has_value()) << "the graph a refused create() left runs under a good one";

        expect(nothrow([&] { std::ignore = Runtime::create(emptyGraph(), Runtime::kDefaultScheduler, property_map{{"timeout_ms", std::string("soon")}}); }));
        const auto refused = Runtime::create(emptyGraph(), Runtime::kDefaultScheduler, property_map{{"timeout_ms", std::string("soon")}});
        expect(!refused.has_value()) << "a value the scheduler refuses was taken";

        expect(Runtime::create(emptyGraph(), Runtime::kDefaultScheduler, property_map{{"timeout_ms", gr::Size_t{10U}}}).has_value()) << "a declared key with a good value is taken";
    };
};

int main() { /* tests are statically registered */ }
