#include <boost/ut.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <format>
#include <functional>
#include <limits>
#include <print>
#include <string>
#include <tuple>
#include <vector>

#include <gnuradio-4.0/BlockMerging.hpp>
#include <gnuradio-4.0/Graph.hpp>
#include <gnuradio-4.0/Scheduler.hpp>

#ifdef __linux__
#include <sched.h>
#endif

/**
 * @brief A chain of cheap stages merged at compile time, against the same chain in a graph.
 *
 * gr::Merge nests two blocks into one at compile time: the right member's processOne is called on the
 * left member's return value inside one function, so the compiler inlines across the pair, vectorizes
 * the result, and no intermediate sample reaches memory. The three arms price that difference on
 * chains of stateless float stages of a few instructions each:
 *
 *   - graph : N blocks with a ring on every edge, which is the scheduler's ordinary path,
 *   - merged: the N stages nested into one block by gr::Merge, in a graph of source, block and sink,
 *   - loop  : the composed arithmetic in one loop over one buffer, the bound for the other two.
 *
 * Each kind of arm pays a cost no chain strategy can remove, and each is reported against the floor of
 * its own kind: the graph floor is a source connected to a sink, the loop floor is the same buffer
 * filled and summed with nothing between. At N = 1 the merged arm nests nothing and runs the same one
 * block the graph arm runs, so the distance between those two figures is the spread between two arms
 * that measure the same thing.
 *
 * A merged block holds its members as mutable members of itself and calls their processOne from its
 * own, so a member may carry state between samples. A member without a vector path costs more than
 * that state: the merged processOne takes vectors only where every member takes them, so one scalar
 * member puts the whole merged block on the scalar path. The static_asserts below pin both facts and
 * use OnePole, a one-pole filter that carries state and has no vector path, as the member outside the
 * chain.
 */

namespace bm_merge {

inline constexpr std::size_t kSamples = 10'000'000UZ;
inline constexpr std::size_t kRepeat  = 7UZ; // timed passes after one discarded warm-up

// every constant is a power of two or a sum of two of them, so each stage's output is exact in a float
// and the arms hold the same sum whatever order they add it in
inline constexpr float kScale   = 1.00390625f; // 1 + 2^-8
inline constexpr float kOffset  = 0.25f;
inline constexpr float kClip    = 1.25f;      // below scale and offset together, so the clip is reached
inline constexpr float kStep    = 0.0078125f; // 2^-7, the quantizer's step
inline constexpr float kInvStep = 128.0f;
inline constexpr float kPole    = 0.05f;

inline constexpr std::array kRingSizes = {4096UZ, 65536UZ};

inline void barrier() noexcept { asm volatile("" : : : "memory"); }

// one definition of each stage's arithmetic, so the blocks and the hand-written loop compose the same
// operations in the same order
template<gr::meta::t_or_simd<float> V>
[[nodiscard]] constexpr V scaled(V value) noexcept {
    return value * kScale;
}

template<gr::meta::t_or_simd<float> V>
[[nodiscard]] constexpr V offset(V value) noexcept {
    return value + kOffset;
}

template<gr::meta::t_or_simd<float> V>
[[nodiscard]] constexpr V clipped(V value) noexcept {
    if constexpr (gr::meta::any_simd<V>) {
        return gr::meta::stdx::min(gr::meta::stdx::max(value, V(-kClip)), V(kClip));
    } else {
        return std::min(std::max(value, -kClip), kClip);
    }
}

template<gr::meta::t_or_simd<float> V>
[[nodiscard]] V quantized(V value) noexcept {
    if constexpr (gr::meta::any_simd<V>) {
        return gr::meta::stdx::floor(value * kInvStep + 0.5f) * kStep;
    } else {
        return std::floor(value * kInvStep + 0.5f) * kStep;
    }
}

struct Source : gr::Block<Source> {
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(Source, out);

    std::size_t nTotal   = 0UZ;
    std::size_t _emitted = 0UZ;

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        if (_emitted >= nTotal) {
            outSpan.publish(0UZ);
            return gr::work::Status::DONE;
        }
        const std::size_t n = std::min(outSpan.size(), nTotal - _emitted);
        for (std::size_t i = 0UZ; i < n; ++i) {
            outSpan[i] = 1.0f;
        }
        _emitted += n;
        outSpan.publish(n);
        return gr::work::Status::OK;
    }
};

struct Sink : gr::Block<Sink> {
    gr::PortIn<float> in;

    GR_MAKE_REFLECTABLE(Sink, in);

    float       _acc       = 0.0f;
    std::size_t _nReceived = 0UZ;

    gr::work::Status processBulk(gr::InputSpanLike auto& inSpan) {
        const std::size_t n   = inSpan.size();
        float             acc = 0.0f;
        for (std::size_t i = 0UZ; i < n; ++i) {
            acc += inSpan[i];
        }
        _acc += acc;
        _nReceived += n;
        std::ignore = inSpan.consume(n);
        return gr::work::Status::OK;
    }
};

struct Scale : gr::Block<Scale> {
    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(Scale, in, out);

    template<gr::meta::t_or_simd<float> V>
    [[nodiscard]] constexpr V processOne(V value) const noexcept {
        return scaled(value);
    }
};

struct Offset : gr::Block<Offset> {
    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(Offset, in, out);

    template<gr::meta::t_or_simd<float> V>
    [[nodiscard]] constexpr V processOne(V value) const noexcept {
        return offset(value);
    }
};

struct Clip : gr::Block<Clip> {
    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(Clip, in, out);

    template<gr::meta::t_or_simd<float> V>
    [[nodiscard]] constexpr V processOne(V value) const noexcept {
        return clipped(value);
    }
};

struct Quantize : gr::Block<Quantize> {
    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(Quantize, in, out);

    template<gr::meta::t_or_simd<float> V>
    [[nodiscard]] V processOne(V value) const noexcept {
        return quantized(value);
    }
};

/// a one-pole low-pass: the filtered sample depends on the one before it
struct OnePole : gr::Block<OnePole> {
    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(OnePole, in, out);

    float _state = 0.0f;

    [[nodiscard]] float processOne(float value) noexcept {
        _state += kPole * (value - _state);
        return _state;
    }
};

static_assert(gr::traits::block::can_processOne_simd<Scale>);
static_assert(gr::traits::block::can_processOne_simd<Offset>);
static_assert(gr::traits::block::can_processOne_simd<Clip>);
static_assert(gr::traits::block::can_processOne_simd<Quantize>);
static_assert(gr::traits::block::can_processOne_const<Scale>);
static_assert(!gr::traits::block::can_processOne_const<OnePole>);
static_assert(!gr::traits::block::can_processOne_simd<OnePole>);

// left to right: Merge<Merge<Merge<s0, s1>, s2>, s3>
template<typename... TStages>
struct MergeChain;

template<typename TStage>
struct MergeChain<TStage> {
    using type = TStage;
};

template<typename TFirst, typename TSecond, typename... TRest>
struct MergeChain<TFirst, TSecond, TRest...> {
    using type = typename MergeChain<gr::Merge<TFirst, "out", TSecond, "in">, TRest...>::type;
};

template<typename... TStages>
using MergeChainT = typename MergeChain<TStages...>::type;

// a member that carries state merges, and takes the merged block off the vector path with it
static_assert(gr::traits::block::can_processOne_simd<MergeChainT<Scale, Offset, Clip, Quantize>>);
static_assert(!gr::traits::block::can_processOne_simd<gr::Merge<OnePole, "out", Scale, "in">>);

struct RunStats {
    float       acc       = 0.0f;
    std::size_t nReceived = 0UZ;
};

template<typename... TStages>
[[nodiscard]] RunStats runChain(std::size_t ring, std::size_t nSamples = kSamples) {
    using namespace boost::ut;

    gr::Graph flow;
    auto&     source = flow.emplaceBlock<Source>(gr::property_map{{"name", std::string("src")}});
    auto&     sink   = flow.emplaceBlock<Sink>(gr::property_map{{"name", std::string("snk")}});
    source.nTotal    = nSamples;

    const gr::EdgeParameters edge{.minBufferSize = ring};

    const auto addStages = [&]<std::size_t I>(this const auto& self, auto& previous) -> void {
        if constexpr (I == sizeof...(TStages)) {
            expect(flow.connect<"out", "in">(previous, sink, edge).has_value());
        } else {
            using TStage = std::tuple_element_t<I, std::tuple<TStages...>>;
            auto& stage  = flow.emplaceBlock<TStage>(gr::property_map{{"name", std::format("s{}", I)}});
            expect(flow.connect<"out", "in">(previous, stage, edge).has_value());
            self.template operator()<I + 1UZ>(stage);
        }
    };
    addStages.template operator()<0UZ>(source);

    gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::singleThreaded> scheduler;
    expect(scheduler.exchange(std::move(flow)).has_value());
    expect(scheduler.runAndWait().has_value());
    return RunStats{sink._acc, sink._nReceived};
}

template<typename... TStages>
[[nodiscard]] RunStats runMerged(std::size_t ring, std::size_t nSamples = kSamples) {
    using namespace boost::ut;

    gr::Graph flow;
    auto&     source = flow.emplaceBlock<Source>(gr::property_map{{"name", std::string("src")}});
    auto&     merged = flow.emplaceBlock<MergeChainT<TStages...>>();
    auto&     sink   = flow.emplaceBlock<Sink>(gr::property_map{{"name", std::string("snk")}});
    source.nTotal    = nSamples;

    const gr::EdgeParameters edge{.minBufferSize = ring};
    expect(flow.connect<"out", "in">(source, merged, edge).has_value());
    expect(flow.connect<"out", "in">(merged, sink, edge).has_value());

    gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::singleThreaded> scheduler;
    expect(scheduler.exchange(std::move(flow)).has_value());
    expect(scheduler.runAndWait().has_value());
    return RunStats{sink._acc, sink._nReceived};
}

template<std::size_t NStages>
[[nodiscard]] float composed(float value) noexcept {
    float result = value;
    if constexpr (NStages >= 1UZ) {
        result = scaled(result);
    }
    if constexpr (NStages >= 2UZ) {
        result = offset(result);
    }
    if constexpr (NStages >= 3UZ) {
        result = clipped(result);
    }
    if constexpr (NStages >= 4UZ) {
        result = quantized(result);
    }
    return result;
}

template<std::size_t NStages>
[[nodiscard]] float runLoop(std::size_t chunk, std::size_t nSamples = kSamples) {
    std::vector<float> scratch(chunk);
    float              acc = 0.0f;

    for (std::size_t done = 0UZ; done < nSamples; done += chunk) {
        const std::size_t n = std::min(chunk, nSamples - done);
        for (std::size_t i = 0UZ; i < n; ++i) {
            scratch[i] = 1.0f;
        }
        // the barrier keeps the fill and the pass over the buffer two loops, so this arm pays one buffer
        // write and one buffer read as the graph arms do; the stages stay in one loop with nothing between
        barrier();
        for (std::size_t i = 0UZ; i < n; ++i) {
            acc += composed<NStages>(scratch[i]);
        }
        barrier();
    }
    return acc;
}

[[nodiscard]] std::string sizeLabel(std::size_t nElements) { return nElements >= (1UZ << 20) ? std::format("{}Mi", nElements >> 20) : std::format("{}Ki", nElements >> 10); }

void pinToFirstCore() {
#ifdef __linux__
    // the arms differ by less than two cores of this machine differ from each other
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(0, &set);
    if (sched_setaffinity(0, sizeof(set), &set) != 0) {
        std::println("note: the process is not pinned to one core and the spread will be wider than the differences");
    }
#endif
}

struct Arm {
    std::string           label;
    std::function<void()> run;
    double                best  = std::numeric_limits<double>::max(); // ns per input sample
    double                worst = 0.0;
};

/// where one (ring, chain length) shape's three arms sit in the arm list
struct Row {
    std::size_t ring    = 0UZ;
    std::size_t nStages = 0UZ;
    std::size_t graph   = 0UZ;
    std::size_t merged  = 0UZ;
    std::size_t loop    = 0UZ;
};

template<typename... TStages>
void appendArms(std::vector<Arm>& arms, std::vector<Row>& rows, std::size_t ring) {
    using namespace boost::ut;
    constexpr std::size_t n = sizeof...(TStages);

    Row row{ring, n, arms.size(), arms.size() + 1UZ, arms.size() + 2UZ};
    arms.emplace_back(std::format("graph  N={} ring={:>5}", n, sizeLabel(ring)), [ring] { expect(eq(runChain<TStages...>(ring).nReceived, kSamples)); });
    arms.emplace_back(std::format("merged N={} ring={:>5}", n, sizeLabel(ring)), [ring] { expect(eq(runMerged<TStages...>(ring).nReceived, kSamples)); });
    arms.emplace_back(std::format("loop   N={} chunk={:>5}", n, sizeLabel(ring)), [ring] { expect(runLoop<n>(ring) > 0.0f); });
    rows.push_back(row);
}

} // namespace bm_merge

inline const boost::ut::suite<"merge chain"> _merge_chain = [] {
    using namespace boost::ut;
    using namespace bm_merge;

    pinToFirstCore();

    "every arm computes the same stream"_test = [] {
        // 32768 samples of a stage output that is exact in a float keep every partial sum exact too, so the
        // arms are compared at the precision of the arithmetic and not at the precision of the summation
        constexpr std::size_t kShort = 1UZ << 15;
        constexpr float       kEps   = 1e-6f;

        for (const std::size_t ring : kRingSizes) {
            const RunStats one  = runChain<Scale>(ring, kShort);
            const RunStats two  = runChain<Scale, Offset>(ring, kShort);
            const RunStats four = runChain<Scale, Offset, Clip, Quantize>(ring, kShort);

            expect(eq(one.nReceived, kShort));
            expect(eq(four.nReceived, kShort));

            const RunStats mergedTwo  = runMerged<Scale, Offset>(ring, kShort);
            const RunStats mergedFour = runMerged<Scale, Offset, Clip, Quantize>(ring, kShort);
            expect(eq(mergedFour.nReceived, kShort));

            const float loopTwo  = runLoop<2UZ>(ring, kShort);
            const float loopFour = runLoop<4UZ>(ring, kShort);
            expect(std::abs(two.acc - mergedTwo.acc) <= kEps * std::abs(two.acc)) << "graph and merged disagree at N=2";
            expect(std::abs(two.acc - loopTwo) <= kEps * std::abs(two.acc)) << "graph and loop disagree at N=2";
            expect(std::abs(four.acc - mergedFour.acc) <= kEps * std::abs(four.acc)) << "graph and merged disagree at N=4";
            expect(std::abs(four.acc - loopFour) <= kEps * std::abs(four.acc)) << "graph and loop disagree at N=4";
            expect(neq(one.acc, four.acc)) << "the stages past the first change the stream";
        }
    };

    "compile-time merging against the graph and the hand-written loop"_test = [] {
        std::vector<Arm>         arms;
        std::vector<Row>         rows;
        std::vector<std::size_t> graphFloor;
        std::vector<std::size_t> loopFloor;

        for (const std::size_t ring : kRingSizes) {
            graphFloor.push_back(arms.size());
            arms.emplace_back(std::format("floor  graph  ring={:>5}", sizeLabel(ring)), [ring] { expect(eq(runChain<>(ring).nReceived, kSamples)); });
            loopFloor.push_back(arms.size());
            arms.emplace_back(std::format("floor  loop   chunk={:>5}", sizeLabel(ring)), [ring] { expect(runLoop<0UZ>(ring) > 0.0f); });

            appendArms<Scale>(arms, rows, ring);
            appendArms<Scale, Offset>(arms, rows, ring);
            appendArms<Scale, Offset, Clip, Quantize>(arms, rows, ring);
        }

        for (std::size_t repetition = 0UZ; repetition <= kRepeat; ++repetition) {
            for (Arm& arm : arms) {
                const auto t0 = std::chrono::steady_clock::now();
                arm.run();
                const auto   ns       = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
                const double perInput = static_cast<double>(ns) / static_cast<double>(kSamples);
                if (repetition == 0UZ) {
                    continue; // warm-up
                }
                arm.best  = std::min(arm.best, perInput);
                arm.worst = std::max(arm.worst, perInput);
            }
        }

        std::println("");
        std::println("merge chain: {} samples an arm, best of {} after a discarded warm-up, one P-core", kSamples, kRepeat);
        std::println("");
        std::println("  {:<28} {:>10} {:>12} {:>8}", "arm", "ns/input", "less floor", "spread");
        const auto floorOf = [&](std::size_t index) { return arms[index].best; };
        for (std::size_t r = 0UZ; r < kRingSizes.size(); ++r) {
            std::println("  {:<28} {:10.3f} {:>12} {:7.1f}%", arms[graphFloor[r]].label, arms[graphFloor[r]].best, "-", 100.0 * (arms[graphFloor[r]].worst - arms[graphFloor[r]].best) / arms[graphFloor[r]].best);
            std::println("  {:<28} {:10.3f} {:>12} {:7.1f}%", arms[loopFloor[r]].label, arms[loopFloor[r]].best, "-", 100.0 * (arms[loopFloor[r]].worst - arms[loopFloor[r]].best) / arms[loopFloor[r]].best);
            for (const Row& row : rows) {
                if (row.ring != kRingSizes[r]) {
                    continue;
                }
                for (const std::size_t index : {row.graph, row.merged, row.loop}) {
                    const double floor = index == row.loop ? floorOf(loopFloor[r]) : floorOf(graphFloor[r]);
                    std::println("  {:<28} {:10.3f} {:12.3f} {:7.1f}%", arms[index].label, arms[index].best, arms[index].best - floor, 100.0 * (arms[index].worst - arms[index].best) / arms[index].best);
                }
            }
        }

        std::println("");
        std::println("  ratios of whole arms, and of stage cost with each arm's own floor removed");
        for (std::size_t r = 0UZ; r < kRingSizes.size(); ++r) {
            for (const Row& row : rows) {
                if (row.ring != kRingSizes[r]) {
                    continue;
                }
                const double graph        = arms[row.graph].best;
                const double merged       = arms[row.merged].best;
                const double loop         = arms[row.loop].best;
                const double graphStages  = graph - floorOf(graphFloor[r]);
                const double mergedStages = merged - floorOf(graphFloor[r]);
                const double loopStages   = loop - floorOf(loopFloor[r]);
                // a stage cost under a tenth of a nanosecond an input is smaller than the spread of the arm it
                // came from, so a ratio taken against it carries no information
                const auto ratio = [](double numerator, double denominator) { return denominator > 0.1 ? std::format("{:5.2f}", numerator / denominator) : std::string("    -"); };
                std::println("  N={} ring={:>5} : whole merged/graph {:5.2f} merged/loop {:5.2f} | stages merged/graph {} merged/loop {}", //
                    row.nStages, sizeLabel(row.ring), merged / graph, merged / loop, ratio(mergedStages, graphStages), ratio(mergedStages, loopStages));
            }
        }
        std::println("");
    };
};

int main() { /* not needed by the UT framework */ }
