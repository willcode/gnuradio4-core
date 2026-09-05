#include <boost/ut.hpp>

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include <gnuradio-4.0/Graph.hpp>
#include <gnuradio-4.0/Scheduler.hpp>

#include "RuntimeTest.hpp"

/**
 * The harness the migrated qa tier is built on: a graph wired through gr::test::RuntimeTest must
 * deliver what the same graph wired with gr::Graph and gr::scheduler::Simple delivers -- the same
 * samples, the same tags, the same settings on the blocks afterwards, and the same scheduler outcome
 * on a graph that fails.
 *
 * This is the one test that names both paths on purpose; every migrated test names only the erased one.
 */

namespace qa_runtime_test {

using namespace gr;

inline constexpr std::size_t kSamples  = 512UZ;
inline constexpr std::size_t kTagEvery = 64UZ;

struct TagSource : Block<TagSource> {
    PortOut<float> out;

    Annotated<gr::Size_t, "n_samples"> n_samples = static_cast<gr::Size_t>(kSamples);

    GR_MAKE_REFLECTABLE(TagSource, out, n_samples);

    std::size_t _emitted = 0UZ;

    work::Status processBulk(OutputSpanLike auto& outSpan) {
        if (_emitted >= static_cast<std::size_t>(n_samples)) {
            outSpan.publish(0UZ);
            return work::Status::DONE;
        }
        const std::size_t n = std::min(outSpan.size(), static_cast<std::size_t>(n_samples) - _emitted);
        if (n == 0UZ) {
            outSpan.publish(0UZ);
            return work::Status::INSUFFICIENT_OUTPUT_ITEMS;
        }
        for (std::size_t i = 0UZ; i < n; ++i) {
            const std::size_t index = _emitted + i;
            outSpan[i]              = static_cast<float>(index);
            if (index % kTagEvery == 0UZ) {
                outSpan.publishTag(property_map{{"signal_name", std::string("s") + std::to_string(index)}}, i);
            }
        }
        _emitted += n;
        outSpan.publish(n);
        return work::Status::OK;
    }
};

struct Scale : Block<Scale> {
    PortIn<float>  in;
    PortOut<float> out;

    Annotated<float, "gain">        gain  = 1.0f;
    Annotated<std::string, "label"> label = "unnamed";

    GR_MAKE_REFLECTABLE(Scale, in, out, gain, label);

    std::size_t nSettingsChanged = 0UZ;

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value * gain; }

    void settingsChanged(const property_map& /*oldSettings*/, const property_map& /*newSettings*/) { nSettingsChanged++; }
};

struct RecordingSink : Block<RecordingSink> {
    PortIn<float> in;

    GR_MAKE_REFLECTABLE(RecordingSink, in);

    std::vector<float>       samples;
    std::vector<std::string> tagNames;

    work::Status processBulk(InputSpanLike auto& inSpan) {
        for (const Tag& tag : inSpan.rawTags) {
            if (auto it = tag.map.find("signal_name"); it != tag.map.end()) {
                tagNames.push_back(std::string(it->second.value_or(std::string_view{})));
            }
        }
        samples.insert(samples.end(), inSpan.begin(), inSpan.end());
        const std::size_t n = inSpan.size();
        inSpan.consumeTags(n);
        std::ignore = inSpan.consume(n);
        return work::Status::OK;
    }
};

/// fails once it has forwarded a couple of items, so both arms can be compared on the error path
struct FailingRelay : Block<FailingRelay> {
    PortIn<float>  in;
    PortOut<float> out;

    GR_MAKE_REFLECTABLE(FailingRelay, in, out);

    std::size_t _nForwarded = 0UZ;

    work::Status processBulk(InputSpanLike auto& inSpan, OutputSpanLike auto& outSpan) {
        if (_nForwarded >= 2UZ) {
            std::ignore = inSpan.consume(0UZ);
            outSpan.publish(0UZ);
            return work::Status::ERROR;
        }
        const std::size_t n = std::min({inSpan.size(), outSpan.size(), 1UZ});
        for (std::size_t i = 0UZ; i < n; ++i) {
            outSpan[i] = inSpan[i];
        }
        std::ignore = inSpan.consume(n);
        outSpan.publish(n);
        _nForwarded += n;
        return work::Status::OK;
    }
};

struct Delivery {
    std::vector<float>       samples;
    std::vector<std::string> tagNames;
    float                    gain = 0.0f;
    std::string              label;
    std::size_t              nSettingsChanged = 0UZ;
};

inline constexpr float kGain = 3.0f;

[[nodiscard]] property_map scaleParameters() { return {{"gain", kGain}, {"label", std::string("scaled")}}; }

[[nodiscard]] Delivery runTyped() {
    using namespace boost::ut;

    gr::Graph flow;
    auto&     source = flow.emplaceBlock<TagSource>();
    auto&     scale  = flow.emplaceBlock<Scale>(scaleParameters());
    auto&     sink   = flow.emplaceBlock<RecordingSink>();
    expect(flow.connect<"out", "in">(source, scale).has_value());
    expect(flow.connect<"out", "in">(scale, sink).has_value());

    gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::singleThreaded> scheduler{};
    expect(scheduler.exchange(std::move(flow)).has_value());
    expect(scheduler.runAndWait().has_value());

    return {sink.samples, sink.tagNames, scale.gain.value, scale.label.value, scale.nSettingsChanged};
}

[[nodiscard]] Delivery runErased() {
    using namespace boost::ut;

    gr::test::RuntimeTest test;
    auto&                 source = test.emplace<TagSource>();
    auto&                 scale  = test.emplace<Scale>(scaleParameters());
    auto&                 sink   = test.emplace<RecordingSink>();
    expect(test.connect(source, "out", scale, "in").has_value());
    expect(test.connect(scale, "out", sink, "in").has_value());

    expect(test.run().has_value());

    return {sink.samples, sink.tagNames, scale.gain.value, scale.label.value, scale.nSettingsChanged};
}

} // namespace qa_runtime_test

const boost::ut::suite<"erased test harness"> _runtimeTest = [] {
    using namespace boost::ut;
    using namespace qa_runtime_test;

    "the erased and the typed arm deliver the same stream"_test = [] {
        const Delivery typed  = runTyped();
        const Delivery erased = runErased();

        expect(eq(typed.samples.size(), kSamples)) << "the typed arm must deliver the whole stream";
        expect(eq(erased.samples.size(), typed.samples.size())) << "the arms deliver a different number of samples";
        expect(std::ranges::equal(erased.samples, typed.samples)) << "the arms deliver different samples";
    };

    "the erased and the typed arm deliver the same tags"_test = [] {
        const Delivery typed  = runTyped();
        const Delivery erased = runErased();

        expect(eq(typed.tagNames.size(), kSamples / kTagEvery)) << "the typed arm must carry every tag";
        expect(eq(erased.tagNames.size(), typed.tagNames.size())) << "the arms carry a different number of tags";
        expect(std::ranges::equal(erased.tagNames, typed.tagNames)) << "the arms carry different tags";
    };

    "settings given at construction reach the block on both arms"_test = [] {
        const Delivery typed  = runTyped();
        const Delivery erased = runErased();

        expect(eq(typed.gain, kGain));
        expect(eq(erased.gain, typed.gain)) << "the arms applied a different gain";
        expect(eq(erased.label, typed.label)) << "the arms applied a different label";
        expect(eq(erased.nSettingsChanged, typed.nSettingsChanged)) << "the arms applied settings a different number of times";
    };

    "a block emplaced by name is the block the registry holds"_test = [] {
        expect(gr::globalBlockRegistry().insert<Scale>("=qa::HarnessScale")) << "the registry would not take the block";

        gr::RuntimeGraph graph;
        const auto       scale = graph.emplace("qa::HarnessScale", "by-name", scaleParameters());
        expect(fatal(scale.has_value())) << (scale.has_value() ? "" : scale.error().message);
        expect(eq(scale->get("gain").value_or(gr::pmt::Value(0.0f)).value_or(0.0f), kGain)) << "the by-name path lost the parameters";
    };

    "a graph that fails drives both arms to the same state"_test = [] {
        gr::Graph flow;
        auto&     typedSource = flow.emplaceBlock<TagSource>();
        auto&     typedRelay  = flow.emplaceBlock<FailingRelay>();
        auto&     typedSink   = flow.emplaceBlock<RecordingSink>();
        expect(flow.connect<"out", "in">(typedSource, typedRelay).has_value());
        expect(flow.connect<"out", "in">(typedRelay, typedSink).has_value());

        gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::singleThreaded> scheduler{};
        expect(scheduler.exchange(std::move(flow)).has_value());
        expect(!scheduler.runAndWait().has_value()) << "a run ending in ERROR is reported as failed";
        const bool typedFailed = scheduler.state() == gr::lifecycle::State::ERROR;

        gr::test::RuntimeTest test;
        auto&                 source = test.emplace<TagSource>();
        auto&                 relay  = test.emplace<FailingRelay>();
        auto&                 sink   = test.emplace<RecordingSink>();
        expect(test.connect(source, "out", relay, "in").has_value());
        expect(test.connect(relay, "out", sink, "in").has_value());
        expect(!test.run().has_value()) << "the erased arm reports the failed run the same way";
        const bool erasedFailed = test.state() == gr::Runtime::State::Error;

        expect(typedFailed) << "the typed arm must reach the error state";
        expect(eq(erasedFailed, typedFailed)) << "the arms disagree on whether the graph failed";
        expect(eq(relay._nForwarded, typedRelay._nForwarded)) << "the arms forwarded a different number of items before failing";
    };
};

int main() { /* tests are statically registered */ }
