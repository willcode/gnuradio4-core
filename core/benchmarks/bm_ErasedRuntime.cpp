#include <boost/ut.hpp>

#include <chrono>
#include <format>
#include <functional>
#include <limits>
#include <print>
#include <string>
#include <vector>

#include <gnuradio-4.0/BlockRegistry.hpp>
#include <gnuradio-4.0/Graph.hpp>
#include <gnuradio-4.0/Port.hpp>
#include <gnuradio-4.0/Runtime.hpp>
#include <gnuradio-4.0/Scheduler.hpp>

/**
 * @brief What the erased entry costs at run time against the typed API.
 *
 * Both arms build the identical graph -- one source, N gain blocks, one counting sink, identical
 * parameters, identical edge sizes, identical scheduler and policy -- and both are executed by the
 * same JobLists of shared_ptr<BlockModel>, so the expectation is equality rather than a tolerance.
 * The gate exists to detect a design leak: something in the entry being consulted during the run.
 *
 * Arms are interleaved in one process and the warm-up repetition is discarded, so host variation
 * cancels between them.
 */

namespace bm_runtime {

inline constexpr float       kGain    = 1.0000001f;
inline constexpr std::size_t kSamples = 10'000'000UZ;
inline constexpr std::size_t kChain   = 8UZ;

struct Source : gr::Block<Source> {
    gr::PortOut<float> out;

    gr::Annotated<gr::Size_t, "n_samples"> n_samples = 1U;

    GR_MAKE_REFLECTABLE(Source, out, n_samples);

    std::size_t _emitted = 0UZ;

    explicit Source(gr::property_map init = {}) : gr::Block<Source>(std::move(init)) {}

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        const std::size_t total = static_cast<std::size_t>(n_samples);
        if (_emitted >= total) {
            outSpan.publish(0UZ);
            return gr::work::Status::DONE;
        }
        const std::size_t n = std::min(outSpan.size(), total - _emitted);
        for (std::size_t i = 0UZ; i < n; ++i) {
            outSpan[i] = 1.0f;
        }
        _emitted += n;
        outSpan.publish(n);
        return gr::work::Status::OK;
    }
};

struct GainOne : gr::Block<GainOne> {
    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    gr::Annotated<float, "gain"> gain = kGain;

    GR_MAKE_REFLECTABLE(GainOne, in, out, gain);

    explicit GainOne(gr::property_map init = {}) : gr::Block<GainOne>(std::move(init)) {}

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value * gain; }
};

struct Counter : gr::Block<Counter> {
    gr::PortIn<float> in;

    GR_MAKE_REFLECTABLE(Counter, in);

    std::size_t nReceived = 0UZ;

    explicit Counter(gr::property_map init = {}) : gr::Block<Counter>(std::move(init)) {}

    gr::work::Status processBulk(gr::InputSpanLike auto& inSpan) {
        const std::size_t n = inSpan.size();
        nReceived += n;
        inSpan.consumeTags(n);
        std::ignore = inSpan.consume(n);
        return gr::work::Status::OK;
    }
};

inline void registerBenchmarkBlocks() {
    static const bool registered = [] {
        gr::BlockRegistry& registry = gr::globalBlockRegistry();
        return registry.insert<Source>("=bm::Source") && registry.insert<GainOne>("=bm::GainOne") && registry.insert<Counter>("=bm::Counter");
    }();
    boost::ut::expect(registered) << "the benchmark blocks must reach the global registry";
}

/// builds the graph outside the timed region and returns how long the run itself took
std::chrono::nanoseconds runTyped(std::size_t nChain, std::size_t ringSize, bool withMessageSubscriber = false) {
    gr::Graph flow;
    auto&     source = flow.emplaceBlock<Source>({{"n_samples", static_cast<gr::Size_t>(kSamples)}});
    auto&     sink   = flow.emplaceBlock<Counter>();

    const gr::EdgeParameters edge{.minBufferSize = ringSize};

    std::vector<std::reference_wrapper<GainOne>> chain;
    chain.reserve(nChain);
    for (std::size_t i = 0UZ; i < nChain; ++i) {
        chain.emplace_back(flow.emplaceBlock<GainOne>({{"gain", kGain}}));
    }

    if (nChain == 0UZ) {
        std::ignore = flow.connect<"out", "in">(source, sink, edge);
    } else {
        std::ignore = flow.connect<"out", "in">(source, chain.front().get(), edge);
        for (std::size_t i = 1UZ; i < nChain; ++i) {
            std::ignore = flow.connect<"out", "in">(chain[i - 1UZ].get(), chain[i].get(), edge);
        }
        std::ignore = flow.connect<"out", "in">(chain.back().get(), sink, edge);
    }

    gr::scheduler::Simple<> scheduler;
    std::ignore = scheduler.exchange(std::move(flow));

    // the erased entry always holds one, so the diagnostic arm can tell its cost from the erasure's
    gr::MsgPortIn subscriber;
    if (withMessageSubscriber) {
        std::ignore = scheduler.msgOut.connect(subscriber);
    }

    const auto started = std::chrono::steady_clock::now();
    std::ignore        = scheduler.runAndWait();
    const auto elapsed = std::chrono::steady_clock::now() - started;

    boost::ut::expect(boost::ut::eq(sink.nReceived, kSamples));
    return std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed);
}

std::chrono::nanoseconds runErased(std::size_t nChain, std::size_t ringSize) {
    gr::RuntimeGraph graph;
    const auto       source = graph.emplace("bm::Source", "source", {{"n_samples", static_cast<gr::Size_t>(kSamples)}});
    const auto       sink   = graph.emplace("bm::Counter", "sink");
    boost::ut::expect(source.has_value() && sink.has_value());

    const gr::EdgeSpec           edge{.minBufferSize = ringSize};
    std::vector<gr::BlockHandle> chain;
    chain.reserve(nChain);
    for (std::size_t i = 0UZ; i < nChain; ++i) {
        auto block = graph.emplace("bm::GainOne", std::format("gain{}", i), {{"gain", kGain}});
        boost::ut::expect(block.has_value());
        chain.push_back(*block);
    }

    if (nChain == 0UZ) {
        std::ignore = graph.connect(*source, "out", *sink, "in", edge);
    } else {
        std::ignore = graph.connect(*source, "out", chain.front(), "in", edge);
        for (std::size_t i = 1UZ; i < nChain; ++i) {
            std::ignore = graph.connect(chain[i - 1UZ], "out", chain[i], "in", edge);
        }
        std::ignore = graph.connect(chain.back(), "out", *sink, "in", edge);
    }

    auto runtime = gr::Runtime::create(std::move(graph));
    boost::ut::expect(runtime.has_value());

    const auto started = std::chrono::steady_clock::now();
    std::ignore        = runtime->runAndWait();
    const auto elapsed = std::chrono::steady_clock::now() - started;

    return std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed);
}

} // namespace bm_runtime

const boost::ut::suite<"erased runtime throughput"> erasedRuntimeThroughput = [] {
    using namespace boost::ut;
    using namespace bm_runtime;

    "the erased and typed arms reach the same throughput"_test = [] {
        registerBenchmarkBlocks();
        constexpr std::size_t kGateRepeat = 9UZ;
        constexpr std::size_t kRings[]    = {1UZ << 16, 1UZ << 21};

        struct Arm {
            std::string                               label;
            std::size_t                               ring;
            bool                                      isErased;
            std::function<std::chrono::nanoseconds()> run;
            double                                    best  = 0.0;
            double                                    worst = std::numeric_limits<double>::max();
        };

        std::vector<Arm> arms;
        for (const std::size_t ring : kRings) {
            arms.emplace_back(std::format("typed  N={} ring={}Ki", kChain, ring / 1024UZ), ring, false, [ring] { return runTyped(kChain, ring); });
            arms.emplace_back(std::format("erased N={} ring={}Ki", kChain, ring / 1024UZ), ring, true, [ring] { return runErased(kChain, ring); });
            arms.emplace_back(std::format("typed+sub N={} ring={}Ki", kChain, ring / 1024UZ), ring, false, [ring] { return runTyped(kChain, ring, true); });
        }

        for (std::size_t repetition = 0UZ; repetition <= kGateRepeat; ++repetition) {
            for (Arm& arm : arms) {
                const double nsPer = static_cast<double>(arm.run().count()) / static_cast<double>(kSamples);
                if (repetition == 0UZ) {
                    continue; // warm-up
                }
                arm.best  = std::min(arm.best == 0.0 ? nsPer : arm.best, nsPer);
                arm.worst = std::max(arm.worst == std::numeric_limits<double>::max() ? nsPer : arm.worst, nsPer);
            }
        }

        std::println("");
        for (const Arm& arm : arms) {
            std::println("gate {:<28} : best {:6.3f} ns/input, spread {:4.1f}%", arm.label, arm.best, 100.0 * (arm.worst - arm.best) / arm.best);
        }

        for (const std::size_t ring : kRings) {
            const auto bestOf = [&arms, ring](bool erased) {
                double best = 0.0;
                for (const Arm& arm : arms) {
                    if (arm.ring == ring && arm.isErased == erased && !arm.label.starts_with("typed+sub")) {
                        best = arm.best;
                    }
                }
                return best;
            };
            const double typed  = bestOf(false);
            const double erased = bestOf(true);
            const double ratio  = typed / erased; // throughput ratio: ns/input is inverse
            std::println("ring={}Ki: erased/typed throughput {:.3f}x", ring / 1024UZ, ratio);
            expect(ge(ratio, 0.98)) << std::format("the erased arm lost throughput at ring={}Ki: {:.3f}x", ring / 1024UZ, ratio);
        }
    };
};

int main() { /* tests are statically registered */ }
