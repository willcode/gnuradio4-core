#include <boost/ut.hpp>

#include <gnuradio-4.0/BlockModel.hpp>
#include <gnuradio-4.0/BlockRegistry.hpp>
#include <gnuradio-4.0/Graph.hpp>
#include <gnuradio-4.0/PluginLoader.hpp>
#include <gnuradio-4.0/Runtime.hpp>
#include <gnuradio-4.0/Scheduler.hpp>
#include <gnuradio-4.0/SchedulerModel.hpp>
#include <gnuradio-4.0/SchedulerRegistration.hpp>

#include <algorithm>
#include <chrono>
#include <format>
#include <functional>
#include <map>
#include <mutex>
#include <print>
#include <ranges>
#include <string>
#include <thread>
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

    Annotated<gr::Size_t, "n_samples", Doc<"samples to emit before finishing">>   n_samples = 4096U;
    Annotated<gr::Size_t, "tag_every", Doc<"tag period in samples, 0 = no tags">> tag_every = 512U;

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

    /// readable but never writable, so it belongs in activeParameters() and not in the writable set
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

inline void registerTestBlocks() {
    static const bool registered = [] {
        BlockRegistry& registry = globalBlockRegistry();
        return registry.insert<RampSource>("=qa::RampSource") && registry.insert<Scale>("=qa::Scale") //
               && registry.insert<RecordingSink>("=qa::RecordingSink") && registry.insert<SumInputs>("=qa::SumInputs");
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

/// "gr::Foo#17" -> "gr::Foo": the id is process-unique, so two arms never share one
inline std::string typeOf(std::string_view uniqueName) {
    const auto separator = uniqueName.rfind('#');
    return std::string(separator == std::string_view::npos ? uniqueName : uniqueName.substr(0UZ, separator));
}

inline bool sameTags(const std::vector<TagRecord>& lhs, const std::vector<TagRecord>& rhs) {
    return std::ranges::equal(lhs, rhs, [](const TagRecord& a, const TagRecord& b) { return a.index == b.index && a.map == b.map; });
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

    "port introspection lists the names and value types"_test = [] {
        registerTestBlocks();
        RuntimeGraph graph;
        const auto   scale = graph.emplace("qa::Scale", "scale");
        expect(scale.has_value());

        expect(graph.inputPortNames(*scale) == std::vector<std::string>{"in"});
        expect(graph.outputPortNames(*scale) == std::vector<std::string>{"out"});
        expect(graph.portTypeName(*scale, true, "in").find("float") != std::string::npos) << graph.portTypeName(*scale, true, "in");
    };

    // a block the caller built is indistinguishable from an emplaced one, which is what lets a
    // factory keep its own typed pointer and hand the graph the erased one
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
        expect(scale->activeParameters().contains(convert_string_domain(std::string("gain"))));
        expect(scale->set(property_map{{"taps", std::vector<float>{2.0f}}}).empty());
        expect(scale->setStaged(property_map{{"label", std::string("adopted")}}).empty());

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
        expect(graph.portTypeName(*sum, true, "in#0").find("float") != std::string::npos);

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
        const SettingsCtx probeCtx{.time = 1ULL, .context = std::string("qa-runtime-parity-probe")};

        std::vector<std::string> skipped;
        for (const std::string& key : globalBlockRegistry().keys()) {
            RuntimeGraph graph;
            auto         handle = graph.emplace(key, "probe");
            if (!handle.has_value()) {
                skipped.push_back(key);
                continue;
            }

            // a context nothing has stored under falls back to the block's whole writable-member set
            expect(handle->set({}, probeCtx).empty()) << key;
            const std::set<std::string> writable = handle->autoUpdateParameters(probeCtx);
            const property_map          active   = handle->activeParameters();
            expect(!writable.empty()) << key << " reports no writable members";

            for (const std::string& member : writable) {
                const auto value = active.find(convert_string_domain(member));
                expect(value != active.end()) << std::format("{}: writable member '{}' is not in activeParameters()", key, member);
                if (value == active.end()) {
                    continue;
                }
                const property_map rejected = handle->set(property_map{{convert_string_domain(member), value->second}});
                expect(rejected.empty()) << std::format("{}: writable member '{}' was rejected by the erased path", key, member);
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
        const auto timeFlat = [kSamples] {
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
};

int main() { /* tests are statically registered */ }
