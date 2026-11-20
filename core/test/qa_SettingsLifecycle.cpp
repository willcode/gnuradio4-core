#include <boost/ut.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <expected>
#include <format>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <gnuradio-4.0/Block.hpp>
#include <gnuradio-4.0/Graph.hpp>
#include <gnuradio-4.0/Scheduler.hpp>
#include <gnuradio-4.0/SchedulerModel.hpp>
#include <gnuradio-4.0/Settings.hpp>

#include "settings_utils.hpp"

namespace qa_settings_lifecycle {

using namespace boost::ut;
using namespace std::chrono_literals;
using gr::property_map;
using gr::settings::Rejected;
using gr::settings::Rejection;
using gr::settings::detail::FrameworkAccess;
using gr::testing::CerrCapture;
using gr::testing::textOf;
using gr::testing::waitUntil;
using Entry = gr::settings::Value;

// a source that emits `n_samples` ones, or nothing at all until `finish` is set when `n_samples` is 0
struct Ones : gr::Block<Ones> {
    gr::PortOut<float> out;

    struct Settings {
        gr::Size_t n_samples = 64U;

        GR_MAKE_REFLECTABLE(Settings, n_samples);
    } settings;

    GR_MAKE_REFLECTABLE(Ones, out, settings);

    std::size_t       sent = 0UZ;
    std::atomic<bool> finish{false};

    void start() { sent = 0UZ; }

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        if (settings.n_samples == 0U) {
            outSpan.publish(0UZ);
            return finish.load() ? gr::work::Status::DONE : gr::work::Status::OK;
        }
        const std::size_t n = std::min(outSpan.size(), static_cast<std::size_t>(settings.n_samples) - sent);
        std::fill_n(outSpan.begin(), n, 1.f);
        outSpan.publish(n);
        sent += n;
        return sent >= settings.n_samples ? gr::work::Status::DONE : gr::work::Status::OK;
    }
};

struct Gain : gr::Block<Gain> {
    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    struct Settings {
        float       gain = 1.f;
        std::string code{};

        GR_MAKE_REFLECTABLE(Settings, gain, code);
    } settings;

    GR_MAKE_REFLECTABLE(Gain, in, out, settings);

    std::atomic<int> nStarts{0};
    std::atomic<int> nStops{0};
    std::string      startThrow{};
    std::string      stopThrow{};
    bool             failCodeWithoutValue = false;
    bool             refuseEmptyCode      = false;
    bool             refuseNames          = false;
    float            failAbove            = 1e9f;
    bool             failGainAtStop       = false;

    void start() {
        ++nStarts;
        if (!startThrow.empty()) {
            throw std::runtime_error(startThrow);
        }
        if (failCodeWithoutValue) {
            this->fail("code", "the device has no code");
        }
    }

    void stop() {
        ++nStops;
        if (!stopThrow.empty()) {
            throw std::runtime_error(stopThrow);
        }
        if (failGainAtStop) {
            this->fail("gain", "the device lost its lock");
        }
    }

    std::optional<gr::Refused> checkSettings(const Settings& s) const {
        if (refuseEmptyCode && s.code.empty()) {
            return gr::Refused(s.code, "a code is required");
        }
        if (refuseNames) {
            return gr::Refused("name", "the device keeps its name");
        }
        return {};
    }

    // a gain the device refuses takes back its batch
    void applySettings(const Settings& s, const Settings& old) {
        if (s.gain != old.gain && s.gain > failAbove) {
            this->fail(s.gain, "the device refuses this gain");
        }
    }

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value * settings.gain; }
};

struct Record : gr::Block<Record> {
    gr::PortIn<float> in;

    GR_MAKE_REFLECTABLE(Record, in);

    std::vector<float> samples;

    gr::work::Status processBulk(gr::InputSpanLike auto& inSpan) {
        samples.insert(samples.end(), inSpan.begin(), inSpan.end());
        std::ignore = inSpan.consume(inSpan.size());
        return gr::work::Status::OK;
    }
};

// sums its inputs; the count of its inputs is a port key. A case may fail the count after the hook resized the inputs,
// and may run a step of its own inside the check hook
struct Sum : gr::Block<Sum> {
    std::vector<gr::PortIn<float>> inputs{2UZ};
    gr::PortOut<float>             out;

    struct Settings {
        gr::Annotated<gr::Size_t, gr::PortCountFor<"inputs">> n_inputs = 2U;

        GR_MAKE_REFLECTABLE(Settings, n_inputs);
    } settings;

    GR_MAKE_REFLECTABLE(Sum, inputs, out, settings);

    bool                  failAfterResize = false;
    std::function<void()> inCheck{};

    std::optional<gr::Refused> checkSettings([[maybe_unused]] const Settings& s) const {
        if (inCheck) {
            inCheck();
        }
        return {};
    }

    void applySettings(const Settings& s) {
        inputs.resize(s.n_inputs.value);
        if (failAfterResize) {
            this->fail(s.n_inputs, "the device cannot open the inputs");
        }
    }

    template<gr::InputSpanLike TInSpan>
    gr::work::Status processBulk(const std::span<TInSpan>& inSpans, gr::OutputSpanLike auto& outSpan) const {
        std::ranges::copy(inSpans[0], outSpan.begin());
        for (std::size_t port = 1UZ; port < inSpans.size(); ++port) {
            std::ranges::transform(outSpan, inSpans[port], outSpan.begin(), std::plus<float>{});
        }
        return gr::work::Status::OK;
    }
};

// sets a key of another block from its work call, once
struct Caller : gr::Block<Caller> {
    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(Caller, in, out);

    gr::SettingsBase*                                                          target = nullptr;
    std::optional<std::expected<gr::settings::Values, gr::settings::Rejected>> result{};
    std::chrono::steady_clock::duration                                        took{};
    std::atomic<bool>                                                          called{false}; // the test reads the result once this is set

    [[nodiscard]] float processOne(float value) {
        if (target != nullptr && !result) {
            const auto calledAt = std::chrono::steady_clock::now();
            result              = target->set("gain", 5.f);
            took                = std::chrono::steady_clock::now() - calledAt;
            called.store(true, std::memory_order_release);
        }
        return value;
    }
};

// a block without ports that reports its count from its start hook, its settings hook and its work function, or
// writes its plain level directly from its work function
struct Publisher : gr::Block<Publisher> {
    struct Settings {
        float      gain    = 1.f;
        gr::Size_t trigger = 0U;
        float      level   = 0.f;

        GR_MAKE_REFLECTABLE(Settings, gain, trigger, level);
    } settings;

    struct Status {
        gr::Size_t count = 0U;

        GR_MAKE_REFLECTABLE(Status, count);
    } status;

    GR_MAKE_REFLECTABLE(Publisher, settings, status);

    bool writeDirectly = false;
    bool countInWork   = true;

    void start() { status.count = 3U; }

    void applySettings(const Settings& s, const Settings& old) {
        if (s.trigger != old.trigger && s.trigger != 0U) {
            status.count = 9U;
        }
    }

    [[nodiscard]] gr::work::Status processBulk() {
        if (writeDirectly) {
            settings.level = 5.f;
        } else if (countInWork) {
            status.count = 7U;
        }
        return gr::work::Status::OK;
    }
};

// a block whose constructor assigns a setting in place of taking it from its map
struct Preset : gr::Block<Preset> {
    struct Settings {
        float gain   = 1.f;
        float offset = 0.f;

        GR_MAKE_REFLECTABLE(Settings, gain, offset);
    } settings;

    GR_MAKE_REFLECTABLE(Preset, settings);

    explicit Preset(property_map init = {}) : gr::Block<Preset>(std::move(init)) { settings.gain = 4.f; }

    [[nodiscard]] gr::work::Status processBulk() { return gr::work::Status::OK; }
};

// a latch: a caller sets `capture` to ask for one capture, and the settings hook takes it and clears the setting
struct Latch : gr::Block<Latch> {
    struct Settings {
        bool capture = false;

        GR_MAKE_REFLECTABLE(Settings, capture);
    } settings;

    GR_MAKE_REFLECTABLE(Latch, settings);

    int captureChanges = 0;
    int captures       = 0;

    // clears `capture` once it fires, so each `true` a caller sends captures once
    void applySettings(Settings& s, const Settings& old) {
        if (s.capture != old.capture) {
            ++captureChanges;
        }
        if (s.capture) {
            ++captures;
            s.capture = false;
        }
    }

    [[nodiscard]] gr::work::Status processBulk() { return gr::work::Status::OK; }
};

// a block without ports whose setting is a member with a destructor; the short label stays in the member's own storage
struct Labeled : gr::Block<Labeled> {
    struct Settings {
        std::string label = "start";

        GR_MAKE_REFLECTABLE(Settings, label);
    } settings;

    GR_MAKE_REFLECTABLE(Labeled, settings);

    [[nodiscard]] gr::work::Status processBulk() { return gr::work::Status::OK; }
};

// a block without ports that calls set, set with noWait and resetDefaults on its own settings, and reads its gain,
// from its start hook, its work function and its stop hook
struct OwnCaller : gr::Block<OwnCaller> {
    struct Settings {
        float gain = 1.f;

        GR_MAKE_REFLECTABLE(Settings, gain);
    } settings;

    GR_MAKE_REFLECTABLE(OwnCaller, settings);

    struct Calls {
        std::vector<std::expected<gr::settings::Values, Rejected>> refused{};
        std::optional<std::expected<float, Rejected>>              read{};
    };

    Calls inStart{};
    Calls inWork{};
    Calls inStop{};

    void callOwnSettings(Calls& calls) {
        calls.refused.push_back(this->set("gain", 2.f));
        calls.refused.push_back(this->set(property_map{{"gain", 3.f}}, gr::settings::noWait));
        calls.refused.push_back(this->resetDefaults(gr::settings::noWait));
        calls.read = this->get<float>("gain");
    }

    void start() { callOwnSettings(inStart); }

    void stop() { callOwnSettings(inStop); }

    [[nodiscard]] gr::work::Status processBulk() {
        if (inWork.refused.empty()) {
            callOwnSettings(inWork);
        }
        return gr::work::Status::OK;
    }
};

// a block without ports with one key declared Stopped and one key that declares no policy
struct Policies : gr::Block<Policies> {
    struct Settings {
        gr::Annotated<float, gr::StoppedOnly> at_start = 1.f;
        float                                 plain    = 1.f;

        GR_MAKE_REFLECTABLE(Settings, at_start, plain);
    } settings;

    GR_MAKE_REFLECTABLE(Policies, settings);

    [[nodiscard]] gr::work::Status processBulk() { return gr::work::Status::OK; }
};

// a block without ports whose settings hook marks an apply that overlaps another apply, or, when it reads its gain, the
// block's processing, and counts the applies run on the caller's thread
struct Applier : gr::Block<Applier> {
    struct Settings {
        float gain = 1.f;

        GR_MAKE_REFLECTABLE(Settings, gain);
    } settings;

    GR_MAKE_REFLECTABLE(Applier, settings);

    std::atomic<int>         nInApply{0};
    std::atomic<bool>        inWork{false};
    std::atomic<bool>        overlapped{false};
    std::atomic<int>         nApplies{0};
    std::atomic<int>         nCallerApplies{0};
    std::atomic<std::size_t> nWorkCalls{0UZ};
    std::thread::id          callerThread{};
    bool                     readsGain = false; // a block used alone leaves the program to keep its work call and a set apart
    float                    sum       = 0.f;

    void applySettings([[maybe_unused]] const Settings& s) {
        if (nInApply.fetch_add(1) != 0 || (readsGain && inWork.load())) {
            overlapped = true;
        }
        ++nApplies;
        std::this_thread::sleep_for(100us); // holds the apply open for an apply the lock keeps out, which no barrier can await
        if (readsGain && inWork.load()) {
            overlapped = true;
        }
        nInApply.fetch_sub(1);
        if (std::this_thread::get_id() == callerThread) {
            ++nCallerApplies;
        }
    }

    [[nodiscard]] gr::work::Status processBulk() {
        inWork = true;
        if (readsGain) {
            sum += settings.gain; // the field an apply writes
        }
        inWork = false;
        nWorkCalls.fetch_add(1UZ);
        return gr::work::Status::OK;
    }
};

// a block without ports whose settings hook takes back each new gain by assigning the gain before the batch, and records
// the flag of the gain after that assignment and the flag a copy of its settings holds
struct TakesBack : gr::Block<TakesBack> {
    struct Settings {
        gr::Annotated<float> gain = 1.f;

        GR_MAKE_REFLECTABLE(Settings, gain);
    } settings;

    GR_MAKE_REFLECTABLE(TakesBack, settings);

    std::optional<bool> changedAfterTakeBack{};
    std::optional<bool> copyChanged{};

    void applySettings(Settings& s, const Settings& old) {
        if (s.gain != old.gain) {
            const Settings copy  = s;
            copyChanged          = copy.gain.changed();
            s.gain               = old.gain;
            changedAfterTakeBack = s.gain.changed();
        }
    }

    [[nodiscard]] gr::work::Status processBulk() { return gr::work::Status::OK; }
};

// a block without ports whose settings hook runs a step of the case's own
struct Stepping : gr::Block<Stepping> {
    struct Settings {
        float gain  = 1.f;
        float level = 0.f;

        GR_MAKE_REFLECTABLE(Settings, gain, level);
    } settings;

    GR_MAKE_REFLECTABLE(Stepping, settings);

    std::function<void()> inApply{};

    void applySettings([[maybe_unused]] const Settings& s) {
        if (inApply) {
            inApply();
        }
    }

    [[nodiscard]] gr::work::Status processBulk() { return gr::work::Status::OK; }
};

// reads the pool a scheduler dispatches its workers on, and how often it woke its workers
template<typename TScheduler>
struct Probe : TScheduler {
    using TScheduler::TScheduler;

    [[nodiscard]] std::string poolInUse() const { return std::string(this->_pool->name()); }
    [[nodiscard]] std::size_t nWakes() const { return this->_wake->value(); }
};

// passes its input on and publishes the count of its work calls by key at each of them
struct Counter : gr::Block<Counter> {
    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    struct Settings {
        gr::Size_t count = 0U;

        GR_MAKE_REFLECTABLE(Settings, count);
    } settings;

    GR_MAKE_REFLECTABLE(Counter, in, out, settings);

    std::atomic<std::size_t> nWorkCalls{0UZ};

    gr::work::Status processBulk(gr::InputSpanLike auto& inSpan, gr::OutputSpanLike auto& outSpan) {
        const std::size_t n = std::min(inSpan.size(), outSpan.size());
        std::copy_n(inSpan.begin(), n, outSpan.begin());
        std::ignore = inSpan.consume(n);
        outSpan.publish(n);
        const std::size_t calls = nWorkCalls.fetch_add(1UZ) + 1UZ;
        this->publish("count", static_cast<gr::Size_t>(calls));
        return gr::work::Status::OK;
    }
};

// consumes and keeps nothing
struct Drop : gr::Block<Drop> {
    gr::PortIn<float> in;

    GR_MAKE_REFLECTABLE(Drop, in);

    gr::work::Status processBulk(gr::InputSpanLike auto& inSpan) {
        std::ignore = inSpan.consume(inSpan.size());
        return gr::work::Status::OK;
    }
};

// a source, a gain and a sink, the source silent until it is told to finish
struct Chain {
    gr::Graph flow;
    Ones&     source = flow.emplaceBlock<Ones>({{"n_samples", 0U}});
    Gain&     gain;
    Record&   sink = flow.emplaceBlock<Record>();

    explicit Chain(const property_map& gainSettings = {}) : gain(flow.emplaceBlock<Gain>(gainSettings)) {
        expect(fatal(flow.connect<"out", "in">(source, gain).has_value()));
        expect(fatal(flow.connect<"out", "in">(gain, sink).has_value()));
    }
};

// the error messages a scheduler sent on msgOut whose text holds `text`
[[nodiscard]] std::size_t errorMessages(gr::MsgPortIn& port, std::string_view text) {
    std::size_t count = 0UZ;
    for (const gr::Message& message : port.streamReader().get()) {
        count += !message.data.has_value() && message.data.error().message.contains(text) ? 1UZ : 0UZ;
    }
    return count;
}

// runs `block` alone: init, start, then the work calls the test makes
template<typename TBlock>
void startAlone(TBlock& block) {
    block.init(std::make_shared<gr::Sequence>());
    expect(fatal(block.changeStateTo(gr::lifecycle::State::RUNNING).has_value()));
}

template<typename TBlock>
void stopAlone(TBlock& block) {
    expect(fatal(block.changeStateTo(gr::lifecycle::State::REQUESTED_STOP).has_value()));
    expect(fatal(block.changeStateTo(gr::lifecycle::State::STOPPED).has_value()));
}

const suite<"settings of a graph's block"> graphLinkTests = [] {
    "a stopped graph holds a change for its start, which applies it before the first sample"_test = [] {
        gr::Graph flow;
        auto&     source = flow.emplaceBlock<Ones>();
        auto&     gain   = flow.emplaceBlock<Gain>();
        auto&     sink   = flow.emplaceBlock<Record>();
        expect(fatal(flow.connect<"out", "in">(source, gain).has_value()));
        expect(fatal(flow.connect<"out", "in">(gain, sink).has_value()));

        const std::expected<gr::settings::Values, Rejected> held = gain.set("gain", 3.f);
        expect(fatal(held.has_value())) << textOf(held);
        expect(held->at("gain").state == Entry::State::pending);
        expect(eq(held->at("gain").reason, std::string("not running")));
        expect(eq(gain.settings.gain, 1.f)) << "a stopped graph's block applied a change at the call";
        expect(eq(gain.get<float>("gain").value_or(0.f), 3.f)) << "a read shows the change that waits for the start";

        gr::scheduler::Simple<> scheduler;
        expect(fatal(scheduler.exchange(std::move(flow)).has_value()));
        expect(fatal(scheduler.runAndWait().has_value()));
        expect(fatal(eq(sink.samples.size(), 64UZ)));
        expect(std::ranges::all_of(sink.samples, [](float sample) { return sample == 3.f; })) << "a sample left before the held change applied";
    };

    "a port key that would leave an edge on a removed port is refused while the graph is stopped"_test = [] {
        gr::Graph flow;
        auto&     left  = flow.emplaceBlock<Ones>();
        auto&     right = flow.emplaceBlock<Ones>();
        auto&     sum   = flow.emplaceBlock<Sum>();
        expect(fatal(flow.connect(flow.blocks()[0], gr::PortDefinition{"out"}, flow.blocks()[2], gr::PortDefinition{"inputs#0"}).has_value()));
        expect(fatal(flow.connect(flow.blocks()[1], gr::PortDefinition{"out"}, flow.blocks()[2], gr::PortDefinition{"inputs#1"}).has_value()));
        std::ignore = left;
        std::ignore = right;

        const std::expected<gr::settings::Values, Rejected> lowered = sum.set("n_inputs", 1U);
        expect(fatal(!lowered.has_value())) << "a count that removes a connected port was accepted";
        expect(fatal(!lowered.error().empty()));
        expect(eq(lowered.error().front().key, std::string("n_inputs")));
        expect(lowered.error().front().reason.contains("inputs#1")) << lowered.error().front().reason;

        const std::expected<gr::settings::Values, Rejected> raised = sum.set("n_inputs", 3U);
        expect(fatal(raised.has_value())) << textOf(raised);
        expect(eq(sum.inputs.size(), 3UZ)) << "a port key applies at the call on a stopped graph";
        expect(eq(flow.blocks()[2]->dynamicInputPortsSize(0UZ), 3UZ)) << "the graph did not list the block's ports again";
    };

    "a hook that resizes its inputs and then fails leaves them at the restored count, and the graph's list agrees"_test = [] {
        gr::Graph flow;
        auto&     sum       = flow.emplaceBlock<Sum>();
        sum.failAfterResize = true;
        for (const gr::Size_t count : {3U, 1U}) {
            const std::expected<gr::settings::Values, Rejected> failed = sum.set("n_inputs", count);
            expect(fatal(failed.has_value())) << textOf(failed);
            expect(fatal(failed->contains("n_inputs") && failed->at("n_inputs").isFailed())) << textOf(failed);
            expect(eq(sum.settings.n_inputs.value, 2U)) << "the count went back";
            expect(eq(sum.inputs.size(), 2UZ)) << "the inputs keep the size the failed hook gave them, from a count of " << count;
            expect(eq(flow.blocks()[0]->dynamicInputPortsSize(0UZ), 2UZ)) << "the graph lists the inputs the failed hook made, from a count of " << count;
        }
    };

    "a connection made inside the check of a lowered port count refuses the count, which removes no port"_test = [] {
        gr::Graph flow;
        std::ignore = flow.emplaceBlock<Ones>();
        std::ignore = flow.emplaceBlock<Ones>();
        auto& sum   = flow.emplaceBlock<Sum>();
        expect(fatal(flow.connect(flow.blocks()[0], gr::PortDefinition{"out"}, flow.blocks()[2], gr::PortDefinition{"inputs#0"}).has_value()));
        int  connections = 0;
        bool connected   = false;
        sum.inCheck      = [&] {
            if (connections++ == 0) { // between the call's check of the edges and its apply
                connected = flow.connect(flow.blocks()[1], gr::PortDefinition{"out"}, flow.blocks()[2], gr::PortDefinition{"inputs#1"}).has_value();
            }
        };
        const std::expected<gr::settings::Values, Rejected> lowered = sum.set("n_inputs", 1U);
        sum.inCheck                                                 = {};
        expect(ge(connections, 1)) << "the check hook did not run";
        expect(connected) << "the connection in the check hook failed";
        expect(eq(sum.settings.n_inputs.value, 2U)) << "the count took";
        expect(eq(sum.inputs.size(), 2UZ)) << "the hook removed the input the edge holds";
        expect(fatal(!lowered.has_value())) << "a count that removes a port an edge now holds was accepted";
        expect(eq(lowered.error().front().reason, std::string("must keep the port inputs#1, which an edge holds")));
    };

    "a block removed from a scheduler's graph leaves the scheduler, and outlives it"_test = [] {
        std::shared_ptr<gr::BlockModel> kept;
        {
            gr::Graph flow;
            auto&     source = flow.emplaceBlock<Ones>();
            auto&     sink   = flow.emplaceBlock<Record>();
            auto&     lone   = flow.emplaceBlock<Applier>();
            expect(fatal(flow.connect<"out", "in">(source, sink).has_value()));
            gr::scheduler::Simple<> scheduler;
            expect(fatal(scheduler.exchange(std::move(flow)).has_value()));
            expect(fatal(FrameworkAccess::schedulerLink(gr::settings::detail::settingsOf(lone)) != nullptr));
            const auto removed = scheduler.graph().removeBlockByName(lone.unique_name);
            expect(fatal(removed.has_value()));
            kept = *removed;
            expect(FrameworkAccess::schedulerLink(gr::settings::detail::settingsOf(*kept)) == nullptr) << "a removed block kept the scheduler's link";
        }
        const std::expected<gr::settings::Values, Rejected> set = kept->set("gain", 2.f);
        expect(set.has_value()) << textOf(set);
    };

    "a start refuses a graph that holds a block added without init(), and names the block"_test = [] {
        gr::Graph                             flow;
        const std::shared_ptr<gr::BlockModel> added = flow.addBlock(std::make_shared<gr::BlockWrapper<Applier>>(property_map{{"gain", 2.f}}), false);
        gr::scheduler::Simple<>               scheduler;
        expect(fatal(scheduler.exchange(std::move(flow)).has_value()));
        const std::expected<void, gr::Error> ran = scheduler.runAndWait();
        expect(fatal(!ran.has_value())) << "a start ran a block added without init()";
        expect(ran.error().message.contains(added->uniqueName())) << ran.error().message;
        expect(ran.error().message.contains("without init()")) << ran.error().message;
        expect(eq(static_cast<Applier*>(added->raw())->nWorkCalls.load(), 0UZ));
    };

    "a block renamed and placed through its model after it joined a graph keeps both through a run, reads and saves them"_test = [] {
        gr::Graph flow;
        std::ignore = flow.emplaceBlock<Ones>();
        std::ignore = flow.emplaceBlock<Gain>();
        std::ignore = flow.emplaceBlock<Record>();
        expect(fatal(flow.connect(flow.blocks()[0], gr::PortDefinition{"out"}, flow.blocks()[1], gr::PortDefinition{"in"}).has_value()));
        expect(fatal(flow.connect(flow.blocks()[1], gr::PortDefinition{"out"}, flow.blocks()[2], gr::PortDefinition{"in"}).has_value()));
        const std::shared_ptr<gr::BlockModel> gain = flow.blocks()[1];

        CerrCapture captured;
        expect(fatal(gain->setName("renamed").has_value()));
        expect(fatal(gain->set("ui_constraints", property_map{{"x", std::string("left")}}).has_value()));
        expect(fatal(gain->set("gain", 2.f).has_value()));
        expect(eq(gain->get<std::string>("name").value_or(""), std::string("renamed"))) << "a read misses the name that waits for the start";
        const property_map  savedBlock = gr::serializeBlock(gr::globalPluginLoader(), gain, gr::BlockSerializationFlags::Settings);
        const property_map* parameters = savedBlock.at(std::pmr::string(gr::serialization_fields::BLOCK_PARAMETERS)).get_if<property_map>();
        expect(fatal(parameters != nullptr));
        expect(parameters->at(std::pmr::string("name")) == gr::pmt::Value(std::string("renamed"))) << "a save misses the name that waits for the start";
        gr::scheduler::Simple<> scheduler;
        expect(fatal(scheduler.exchange(std::move(flow)).has_value()));
        expect(fatal(scheduler.runAndWait().has_value()));

        expect(eq(gain->name(), std::string_view("renamed"))) << "the run took back the name";
        expect(eq(gain->get<std::string>("name").value_or(""), std::string("renamed")));
        const std::expected<gr::settings::Value, Rejected> placed = gain->get("ui_constraints");
        expect(fatal(placed.has_value()));
        const property_map* placedMap = placed->value.get_if<property_map>();
        expect(placedMap != nullptr && placedMap->contains("x")) << "the run took back the UI map";
        const property_map saved = FrameworkAccess::savedValues(gr::settings::detail::settingsOf(*gain));
        expect(saved.contains("name") && saved.at("name") == gr::pmt::Value(std::string("renamed"))) << "the saved settings lack the name";
        const auto savedPlace = saved.find("ui_constraints");
        expect(savedPlace != saved.end() && savedPlace->second.get_if<property_map>() != nullptr && savedPlace->second.get_if<property_map>()->contains("x")) << "the saved settings lack the UI map";
        expect(eq(captured.lines("outside applySettings()"), 0UZ)) << captured.str();
    };

    "a subgraph renamed through its model after it joined a stopped graph saves the new name"_test = [] {
        gr::Graph flow;
        auto      wrapper                              = std::make_shared<gr::GraphWrapper<gr::Graph>>();
        std::ignore                                    = wrapper->graph()->emplaceBlock<Gain>();
        const std::shared_ptr<gr::BlockModel> subgraph = flow.addBlock(wrapper);
        expect(fatal(FrameworkAccess::isSetUp(gr::settings::detail::settingsOf(*subgraph)))) << "the rename would not go through the subgraph's settings";

        expect(fatal(subgraph->setName("inner").has_value()));
        expect(eq(subgraph->get<std::string>("name").value_or(""), std::string("inner"))) << "a read misses the new name";
        expect(eq(subgraph->name(), std::string_view("inner"))) << "no work call reaches a subgraph, so its new name applies at the call";
        const property_map  savedBlock = gr::serializeBlock(gr::globalPluginLoader(), subgraph, gr::BlockSerializationFlags::Settings);
        const property_map* parameters = savedBlock.at(std::pmr::string(gr::serialization_fields::BLOCK_PARAMETERS)).get_if<property_map>();
        expect(fatal(parameters != nullptr));
        expect(parameters->at(std::pmr::string("name")) == gr::pmt::Value(std::string("inner"))) << "a save writes the subgraph's old name";
    };

    "a scheduler-managed subgraph renamed through its model while the flowgraph runs saves the new name"_test = [] {
        gr::Graph flow;
        auto&     source = flow.emplaceBlock<Ones>({{"n_samples", 0U}});
        auto&     sink   = flow.emplaceBlock<Record>();
        expect(fatal(flow.connect<"out", "in">(source, sink).has_value()));

        using Nested = gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::multiThreaded>;
        auto inner   = std::make_shared<gr::SchedulerWrapper<Nested>>();
        inner->setGraph(std::move(flow));
        gr::Graph                             outer;
        const std::shared_ptr<gr::BlockModel> subgraph = outer.addBlock(gr::SchedulerModel::asBlockModelPtr(inner));
        const std::string                     oldName(subgraph->name());
        Nested                                scheduler;
        expect(fatal(scheduler.exchange(std::move(outer)).has_value()));
        expect(fatal(scheduler.changeStateTo(gr::lifecycle::State::INITIALISED).has_value()));
        expect(fatal(scheduler.changeStateTo(gr::lifecycle::State::RUNNING).has_value()));
        expect(fatal(waitUntil([&] { return FrameworkAccess::isRunning(gr::settings::detail::settingsOf(source)); })));

        // no work call applies a nested scheduler's settings, so the new name waits for that scheduler's next start
        expect(fatal(subgraph->setName("inner").has_value()));
        const property_map  savedBlock = gr::serializeBlock(gr::globalPluginLoader(), subgraph, gr::BlockSerializationFlags::Settings);
        const property_map* parameters = savedBlock.at(std::pmr::string(gr::serialization_fields::BLOCK_PARAMETERS)).get_if<property_map>();
        expect(fatal(parameters != nullptr));
        expect(parameters->at(std::pmr::string("name")) == gr::pmt::Value(std::string("inner"))) << "a save writes the old name " << oldName << " while the new one waits";
        expect(fatal(scheduler.changeStateTo(gr::lifecycle::State::REQUESTED_STOP).has_value()));
    };

    "a rename through the model that the block's check refuses returns the refusal and keeps the old name"_test = [] {
        gr::Graph                             flow;
        auto&                                 gain  = flow.emplaceBlock<Gain>({{"name", std::string("kept")}});
        const std::shared_ptr<gr::BlockModel> model = flow.blocks()[0];
        gain.refuseNames                            = true;

        const std::expected<gr::settings::Values, Rejected> refused = model->setName("renamed");
        expect(fatal(!refused.has_value())) << "a refused rename reported success";
        expect(eq(refused.error().size(), 1UZ));
        expect(eq(refused.error().front().key, std::string("name")));
        expect(eq(refused.error().front().reason, std::string("the device keeps its name")));
        expect(eq(model->get<std::string>("name").value_or(""), std::string("kept"))) << "a read returns the refused name";
        expect(eq(model->name(), std::string_view("kept")));

        gain.refuseNames                                            = false;
        const std::expected<gr::settings::Values, Rejected> renamed = model->setName("renamed");
        expect(renamed.has_value()) << textOf(renamed);
        expect(eq(model->get<std::string>("name").value_or(""), std::string("renamed")));
    };
};

const suite<"the start and the stop of a block's settings"> startTests = [] {
    "a start hook that throws fails the start, and the block's stop hook runs once"_test = [] {
        gr::Graph flow;
        auto&     source = flow.emplaceBlock<Ones>();
        auto&     gain   = flow.emplaceBlock<Gain>();
        auto&     sink   = flow.emplaceBlock<Record>();
        expect(fatal(flow.connect<"out", "in">(source, gain).has_value()));
        expect(fatal(flow.connect<"out", "in">(gain, sink).has_value()));
        gain.startThrow = "no device answers";

        gr::scheduler::Simple<> scheduler;
        expect(fatal(scheduler.exchange(std::move(flow)).has_value()));
        const std::expected<void, gr::Error> ran = scheduler.runAndWait();
        expect(fatal(!ran.has_value()));
        expect(ran.error().message.contains("no device answers")) << ran.error().message;
        expect(eq(gain.nStarts.load(), 1));
        expect(eq(gain.nStops.load(), 1)) << "the stop hook of a failed start must run once";
        const Rejected refusals = FrameworkAccess::runRefusals(gr::settings::detail::settingsOf(gain));
        expect(fatal(eq(refusals.size(), 1UZ)));
        expect(refusals.front().reason.contains("no device answers")) << refusals.front().reason;
    };

    "the start's check of the whole state refuses a start, and no start hook runs"_test = [] {
        gr::Graph flow;
        auto&     source = flow.emplaceBlock<Ones>();
        auto&     gain   = flow.emplaceBlock<Gain>();
        auto&     sink   = flow.emplaceBlock<Record>();
        expect(fatal(flow.connect<"out", "in">(source, gain).has_value()));
        expect(fatal(flow.connect<"out", "in">(gain, sink).has_value()));
        gain.refuseEmptyCode = true;

        gr::scheduler::Simple<> scheduler;
        expect(fatal(scheduler.exchange(std::move(flow)).has_value()));
        const std::expected<void, gr::Error> ran = scheduler.runAndWait();
        expect(fatal(!ran.has_value())) << "a block whose state its check refuses started";
        expect(ran.error().message.contains("code")) << ran.error().message;
        expect(eq(gain.nStarts.load(), 0)) << "a refused start ran the start hook";
        expect(sink.samples.empty());

        expect(gain.set("code", std::string("A1")).has_value());
        expect(fatal(scheduler.runAndWait().has_value())) << "the next start after the fix must run";
        expect(eq(gain.nStarts.load(), 1));
        expect(eq(sink.samples.size(), 64UZ));
    };

    "a caller's apply at the call never overlaps the apply of a work call on another thread"_test = [] {
        // with no graph and stopped settings a set applies at the call, and each work call applies what is pending
        Applier block;
        block.callerThread = std::this_thread::get_id();
        block.init(std::make_shared<gr::Sequence>());
        std::atomic<bool> done{false};
        std::thread       worker([&] {
            while (!done.load()) {
                std::ignore = block.work();
            }
        });
        // a set runs its apply here only when it wins the apply lock, so each iteration sets until one does
        // the 200 contested applies can show an overlap but not force one, since the lock under test forbids it
        const auto  deadline  = std::chrono::steady_clock::now() + 20s;
        int         nIterated = 0;
        std::size_t nSets     = 0UZ;
        for (; nIterated < 200 && std::chrono::steady_clock::now() < deadline; ++nIterated) {
            const int callerApplies = block.nCallerApplies.load();
            while (block.nCallerApplies.load() == callerApplies && std::chrono::steady_clock::now() < deadline) {
                ++nSets;
                std::ignore = block.set("gain", static_cast<float>(1UZ + nSets % 7UZ));
            }
        }
        done = true;
        worker.join();
        expect(!block.overlapped.load()) << "a caller's apply ran beside the work call's";
        expect(eq(nIterated, 200)) << "the iterations did not finish before the deadline";
        expect(ge(block.nCallerApplies.load(), 200)) << "a caller's set ran its apply on the caller's thread too rarely";
    };

    "a resume applies the changes held while paused before any worker calls the block again"_test = [] {
        gr::Graph flow;
        auto&     block = flow.emplaceBlock<Applier>();
        block.readsGain = true;
        gr::SchedulerWrapper<gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::multiThreaded>> scheduler;
        scheduler.setGraph(std::move(flow));
        expect(fatal(scheduler.start().has_value()));
        expect(fatal(waitUntil([&] { return FrameworkAccess::isRunning(gr::settings::detail::settingsOf(block)) && block.nWorkCalls.load() > 0UZ; })));
        auto& top = scheduler.blockRef();
        for (int i = 0; i < 100; ++i) {
            expect(fatal(top.changeStateTo(gr::lifecycle::State::REQUESTED_PAUSE).has_value()));
            expect(fatal(top.state() == gr::lifecycle::State::PAUSED));
            const std::expected<gr::settings::Values, Rejected> held = block.set("gain", static_cast<float>(2 + i % 5), gr::settings::noWait);
            expect(fatal(held.has_value())) << textOf(held);
            const std::size_t callsBefore = block.nWorkCalls.load();
            expect(fatal(top.changeStateTo(gr::lifecycle::State::RUNNING).has_value()));
            expect(fatal(waitUntil([&] { return block.nWorkCalls.load() > callsBefore; })));
        }
        scheduler.stop();
        expect(!block.overlapped.load()) << "the resume's apply ran beside a work call";
        expect(ge(block.nApplies.load(), 100));
    };
};

// whether each worker of a running blocking scheduler waits in its park on the graph's progress
template<typename TScheduler>
[[nodiscard]] bool allWorkersParked(const TScheduler& scheduler) {
    const std::shared_ptr<gr::scheduler::JobLists> jobLists = scheduler.jobs();
    const std::size_t                              nWorkers = gr::scheduler::isMultiThreaded(TScheduler::executionPolicy()) ? (jobLists ? jobLists->size() : 0UZ) : 1UZ;
    return nWorkers > 0UZ && static_cast<std::size_t>(scheduler.graph().progress().nTimedWaiters()) >= nWorkers;
}

template<typename TScheduler>
void waitingSetReturnsAfterTheWake(bool nested) {
    gr::Graph flow;
    auto&     source = flow.emplaceBlock<Ones>({{"n_samples", 0U}});
    auto&     gain   = flow.emplaceBlock<Gain>();
    auto&     sink   = flow.emplaceBlock<Record>();
    expect(fatal(flow.connect<"out", "in">(source, gain).has_value()));
    expect(fatal(flow.connect<"out", "in">(gain, sink).has_value()));

    // an idle worker parks for up to timeout_ms, and the watchdog ends a park each watchdog_timeout; both outlast the call
    const property_map               parks{{"timeout_ms", 30000U}, {"watchdog_timeout", 30000U}};
    gr::SchedulerWrapper<TScheduler> scheduler(parks);
    const TScheduler*                runsGain = std::addressof(scheduler.blockRef());
    if (nested) {
        auto inner = std::make_shared<gr::SchedulerWrapper<TScheduler>>(parks);
        inner->setGraph(std::move(flow));
        runsGain = std::addressof(inner->blockRef());
        gr::Graph outer;
        outer.addBlock(gr::SchedulerModel::asBlockModelPtr(inner));
        scheduler.setGraph(std::move(outer));
    } else {
        scheduler.setGraph(std::move(flow));
    }
    expect(fatal(scheduler.start().has_value()));
    expect(fatal(waitUntil([&] { return FrameworkAccess::isRunning(gr::settings::detail::settingsOf(gain)); })));
    // the workers find nothing to do and park, so only the set's wake can end the park of the gain's worker in time
    expect(fatal(waitUntil([runsGain] { return allWorkersParked(*runsGain); }))) << "the workers running the gain did not park";

    const auto                                          calledAt = std::chrono::steady_clock::now();
    const std::expected<gr::settings::Values, Rejected> applied  = gain.set("gain", 4.f, 3000ms);
    const auto                                          took     = std::chrono::steady_clock::now() - calledAt;
    expect(applied.has_value()) << textOf(applied);
    expect(applied.has_value() && applied->empty()) << "the change came back pending";
    expect(lt(took, 1000ms)) << std::format("the call returned after {} ms, at its timeout", std::chrono::duration_cast<std::chrono::milliseconds>(took).count());
    expect(eq(gain.settings.gain, 4.f));
    source.finish = true;
    scheduler.stop();
}

const suite<"the scheduler wakes for a block's settings"> wakeTests = [] {
    using enum gr::scheduler::ExecutionPolicy;
    "a waiting set on a running block returns applied after the wake, on each scheduler kind"_test = [] {
        waitingSetReturnsAfterTheWake<gr::scheduler::Simple<singleThreadedBlocking>>(false);
        waitingSetReturnsAfterTheWake<gr::scheduler::Simple<multiThreadedBlocking>>(false);
        waitingSetReturnsAfterTheWake<gr::scheduler::BreadthFirst<multiThreadedBlocking>>(false);
        waitingSetReturnsAfterTheWake<gr::scheduler::DepthFirst<multiThreadedBlocking>>(false);
    };

    "a waiting set on a block of a nested scheduler returns applied after the wake"_test = [] {
        waitingSetReturnsAfterTheWake<gr::scheduler::Simple<singleThreadedBlocking>>(true);
        waitingSetReturnsAfterTheWake<gr::scheduler::Simple<multiThreadedBlocking>>(true);
    };

    "a set from a worker of any scheduler does not wait"_test = [] {
        gr::Graph flow;
        auto&     source = flow.emplaceBlock<Ones>({{"n_samples", 1U}}); // the caller's input is silent, so a sample to call on comes from a source that emits once
        auto&     caller = flow.emplaceBlock<Caller>();
        auto&     gain   = flow.emplaceBlock<Gain>();
        auto&     sink   = flow.emplaceBlock<Record>();
        expect(fatal(flow.connect<"out", "in">(source, caller).has_value()));
        expect(fatal(flow.connect<"out", "in">(caller, gain).has_value()));
        expect(fatal(flow.connect<"out", "in">(gain, sink).has_value()));

        gr::Graph otherFlow;
        auto&     otherSource = otherFlow.emplaceBlock<Ones>({{"n_samples", 1000000000U}});
        auto&     otherGain   = otherFlow.emplaceBlock<Gain>();
        auto&     otherSink   = otherFlow.emplaceBlock<Record>();
        expect(fatal(otherFlow.connect<"out", "in">(otherSource, otherGain).has_value()));
        expect(fatal(otherFlow.connect<"out", "in">(otherGain, otherSink).has_value()));
        caller.target = std::addressof(gr::settings::detail::settingsOf(otherGain));

        gr::SchedulerWrapper<gr::scheduler::Simple<multiThreaded>> other;
        other.setGraph(std::move(otherFlow));
        expect(fatal(other.start().has_value()));
        expect(fatal(waitUntil([&] { return FrameworkAccess::isRunning(gr::settings::detail::settingsOf(otherGain)); })));

        gr::SchedulerWrapper<gr::scheduler::Simple<multiThreaded>> scheduler;
        scheduler.setGraph(std::move(flow));
        expect(fatal(scheduler.start().has_value()));
        expect(fatal(waitUntil([&] { return caller.called.load(std::memory_order_acquire); })));
        expect(fatal(caller.result->has_value())) << textOf(*caller.result);
        expect(eq((*caller.result)->at("gain").reason, std::string(gr::settings::kPendingOnWorker))) << "a worker's call does not wait, and its result says so";
        expect(lt(caller.took, 500ms)) << "a worker waited for another block's apply";
        scheduler.stop();
        other.stop();
        std::ignore = gain;
    };
};

const suite<"notifications reach a reader"> notificationTests = [] {
    "a reader installed at the scheduler receives a block's failed apply and may read the block"_test = [] {
        gr::Graph flow;
        auto&     source = flow.emplaceBlock<Ones>({{"n_samples", 0U}});
        auto&     gain   = flow.emplaceBlock<Gain>();
        auto&     sink   = flow.emplaceBlock<Record>();
        expect(fatal(flow.connect<"out", "in">(source, gain).has_value()));
        expect(fatal(flow.connect<"out", "in">(gain, sink).has_value()));
        gain.failAbove = 10.f;

        gr::SchedulerWrapper<gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::singleThreadedBlocking>> scheduler;
        scheduler.setGraph(std::move(flow));
        std::mutex                              lock;
        std::vector<gr::settings::Notification> received;
        std::optional<float>                    readInside;
        scheduler.blockRef().installReader([&](const gr::settings::Notification& notification) {
            const std::expected<float, Rejected> read = gain.get<float>("gain");
            std::lock_guard                      guard(lock);
            received.push_back(notification);
            readInside = read ? std::optional(*read) : std::nullopt;
        });
        expect(fatal(scheduler.start().has_value()));
        expect(fatal(waitUntil([&] { return FrameworkAccess::isRunning(gr::settings::detail::settingsOf(gain)); })));

        const std::expected<gr::settings::Values, Rejected> refused = gain.set("gain", 20.f);
        expect(fatal(refused.has_value() && refused->contains("gain") && refused->at("gain").isFailed())) << "a gain the device refuses came back as applied";
        expect(waitUntil([&] {
            std::lock_guard guard(lock);
            return std::ranges::any_of(received, [](const gr::settings::Notification& notification) { return notification.key == "gain" && notification.what == gr::settings::NotificationKind::failed; });
        })) << "no failure reached the reader";
        {
            std::lock_guard guard(lock);
            expect(readInside.has_value() && *readInside == 1.f) << "the reader could not read the block";
        }
        source.finish = true;
        scheduler.stop();
    };

    "a reader that requests a stop at an error notification ends the run, and the notification names the block and the key"_test = [] {
        gr::Graph flow;
        auto&     source = flow.emplaceBlock<Ones>({{"n_samples", 0U}});
        auto&     gain   = flow.emplaceBlock<Gain>();
        auto&     sink   = flow.emplaceBlock<Record>();
        expect(fatal(flow.connect<"out", "in">(source, gain).has_value()));
        expect(fatal(flow.connect<"out", "in">(gain, sink).has_value()));
        gain.failAbove = 10.f;

        gr::SchedulerWrapper<gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::singleThreadedBlocking>> scheduler;
        scheduler.setGraph(std::move(flow));
        auto&                                     runner = scheduler.blockRef();
        std::mutex                                lock;
        std::optional<gr::settings::Notification> failure;
        runner.installReader([&](const gr::settings::Notification& notification) {
            if (notification.level != gr::settings::NotificationLevel::error) {
                return;
            }
            {
                std::lock_guard guard(lock);
                failure.emplace(notification);
            }
            runner.requestStop();
        });
        expect(fatal(scheduler.start().has_value()));
        expect(fatal(waitUntil([&] { return FrameworkAccess::isRunning(gr::settings::detail::settingsOf(gain)); })));

        std::ignore = gain.set("gain", 20.f);
        expect(waitUntil([&] { return runner.state() == gr::lifecycle::State::STOPPED; })) << "the reader's stop request leaves the run going";
        {
            std::lock_guard guard(lock);
            expect(fatal(failure.has_value())) << "no error reached the reader";
            expect(eq(failure->block, std::string(gain.unique_name)));
            expect(eq(failure->key, std::string("gain")));
        }
        source.finish = true;
        scheduler.stop();
    };

    "failures of one key before a delivery reach the reader as one, with the count it replaced"_test = [] {
        gr::Graph flow;
        auto&     source = flow.emplaceBlock<Ones>();
        auto&     gain   = flow.emplaceBlock<Gain>();
        auto&     sink   = flow.emplaceBlock<Record>();
        expect(fatal(flow.connect<"out", "in">(source, gain).has_value()));
        expect(fatal(flow.connect<"out", "in">(gain, sink).has_value()));

        gr::scheduler::Simple<> scheduler;
        expect(fatal(scheduler.exchange(std::move(flow)).has_value()));
        std::vector<gr::settings::Notification> received;
        scheduler.installReader([&](const gr::settings::Notification& notification) { received.push_back(notification); });
        for (const float value : {2.f, 3.f, 4.f}) {
            gr::settings::detail::FrameworkAccess::fail(gr::settings::detail::settingsOf(gain), "gain", std::format("the device fell back to {}", value));
        }
        expect(received.empty()) << "a stopped scheduler delivered before its run";
        expect(fatal(scheduler.runAndWait().has_value()));
        const auto failures = std::ranges::count_if(received, [](const gr::settings::Notification& notification) { return notification.key == "gain"; });
        expect(fatal(eq(failures, 1))) << "the queue kept more than the latest failure of the key";
        const auto latest = std::ranges::find_if(received, [](const gr::settings::Notification& notification) { return notification.key == "gain"; });
        expect(eq(latest->nReplaced, 2UZ));
        expect(latest->reason.contains("4")) << latest->reason;
    };

    "a block a message removes from a running scheduler notifies its stop hook's failure to the reader there"_test = [] {
        CerrCapture captured;
        gr::Graph   flow;
        auto&       source = flow.emplaceBlock<Ones>({{"n_samples", 0U}});
        auto&       gain   = flow.emplaceBlock<Gain>();
        auto&       sink   = flow.emplaceBlock<Record>();
        expect(fatal(flow.connect<"out", "in">(source, gain).has_value()));
        expect(fatal(flow.connect<"out", "in">(gain, sink).has_value()));
        gain.failGainAtStop = true;
        const std::string gainName(gain.unique_name);

        gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::multiThreaded> scheduler;
        gr::MsgPortOut                                                       toScheduler;
        expect(fatal(toScheduler.connect(scheduler.msgIn).has_value()));
        expect(fatal(scheduler.exchange(std::move(flow)).has_value()));
        std::mutex                              lock;
        std::vector<gr::settings::Notification> received;
        scheduler.installReader([&](const gr::settings::Notification& notification) {
            std::lock_guard guard(lock);
            received.push_back(notification);
        });
        std::thread runner([&scheduler] { std::ignore = scheduler.runAndWait(); });
        expect(fatal(waitUntil([&] { return FrameworkAccess::isRunning(gr::settings::detail::settingsOf(gain)); })));
        gr::sendMessage<gr::message::Command::Set>(toScheduler, std::string(scheduler.unique_name), gr::scheduler::property::kRemoveBlock, property_map{{"uniqueName", gainName}});
        const bool reached = waitUntil([&] {
            std::lock_guard guard(lock);
            return std::ranges::any_of(received, [&gainName](const gr::settings::Notification& notification) { return notification.block == gainName && notification.what == gr::settings::NotificationKind::failed; });
        });
        source.finish      = true;
        std::ignore        = scheduler.changeStateTo(gr::lifecycle::State::REQUESTED_STOP);
        runner.join();
        expect(reached) << "the removed block's stop hook failure did not reach the reader";
        expect(eq(captured.lines("the device lost its lock"), 0UZ)) << captured.str();
    };

    "a stop delivers what a nested scheduler's stop hook raised to the reader at the top"_test = [] {
        gr::Graph flow;
        auto&     source = flow.emplaceBlock<Ones>({{"n_samples", 0U}});
        auto&     gain   = flow.emplaceBlock<Gain>();
        auto&     sink   = flow.emplaceBlock<Record>();
        expect(fatal(flow.connect<"out", "in">(source, gain).has_value()));
        expect(fatal(flow.connect<"out", "in">(gain, sink).has_value()));

        auto inner = std::make_shared<gr::SchedulerWrapper<gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::multiThreaded>>>();
        inner->setGraph(std::move(flow));
        gr::Graph outer;
        outer.addBlock(gr::SchedulerModel::asBlockModelPtr(inner));
        // driven through its lifecycle alone, the top runs no message pass of its own after the stop
        gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::multiThreaded> scheduler;
        expect(fatal(scheduler.exchange(std::move(outer)).has_value()));
        std::mutex                              lock;
        std::vector<gr::settings::Notification> received;
        scheduler.installReader([&](const gr::settings::Notification& notification) {
            std::lock_guard guard(lock);
            received.push_back(notification);
        });
        expect(fatal(scheduler.changeStateTo(gr::lifecycle::State::INITIALISED).has_value()));
        expect(fatal(scheduler.changeStateTo(gr::lifecycle::State::RUNNING).has_value()));
        expect(fatal(waitUntil([&] { return FrameworkAccess::isRunning(gr::settings::detail::settingsOf(gain)); })));
        gain.failGainAtStop = true; // the failure arises after the workers' last message pass
        expect(fatal(scheduler.changeStateTo(gr::lifecycle::State::REQUESTED_STOP).has_value()));
        std::lock_guard guard(lock);
        expect(std::ranges::any_of(received, [](const gr::settings::Notification& notification) { return notification.key == "gain" && notification.reason == "the device lost its lock"; })) << "the nested block's failure had not reached the top's reader when the stop returned";
    };
};

const suite<"a block publishes its own values"> publishTests = [] {
    "a value published by key for a plain setting is read with its reason beside the value set, which the field keeps"_test = [] {
        Publisher block;
        startAlone(block);
        std::ignore = block.work();
        std::thread device([&block] { gr::settings::detail::FrameworkAccess::publish(gr::settings::detail::settingsOf(block), "gain", 2.5f, "the device rounds the gain"); });
        device.join();
        const std::expected<Entry, Rejected> read = block.get("gain");
        expect(fatal(read.has_value()));
        expect(read->value == gr::pmt::Value(2.5f)) << "a read takes the published value at the call";
        expect(eq(read->reason, std::string("the device rounds the gain")));
        std::ignore = block.work();
        expect(eq(block.settings.gain, 1.f)) << "the block's field keeps the value set";
        const std::expected<gr::settings::Values, Rejected> same = block.set("gain", 1.f, gr::settings::noWait);
        expect(fatal(same.has_value()));
        std::ignore = block.work();
        expect(eq(block.get("gain")->reason, std::string())) << "a write of the key ends the published value";
        expect(eq(block.get<float>("gain").value_or(0.f), 1.f));
        stopAlone(block);
    };

    "a plain setting written outside the apply hook is reported at the stop, and takes back its value in force"_test = [] {
        Publisher block;
        block.writeDirectly = true;
        startAlone(block);
        std::ignore = block.work();
        expect(eq(block.get<float>("level").value_or(1.f), 0.f)) << "in force keeps the value set during the run";
        CerrCapture captured;
        stopAlone(block);
        expect(eq(captured.lines("'level'"), 1UZ)) << captured.str();
        expect(eq(captured.lines("outside applySettings()"), 1UZ)) << captured.str();
        expect(captured.str().contains("error")) << captured.str();
        expect(eq(block.get<float>("level").value_or(1.f), 0.f)) << "in force keeps its value after the stop";
        expect(eq(block.settings.level, 0.f)) << "the member takes back the value in force";
    };

    "a running block destroyed without a stop reads no member, and a stop before the destruction reports"_test = [] {
        CerrCapture captured;
        for (const bool stopFirst : {true, false}) {
            Labeled block;
            block.init(std::make_shared<gr::Sequence>());
            expect(fatal(block.changeStateTo(gr::lifecycle::State::RUNNING).has_value()));
            block.settings.label = "unpublished";
            if (stopFirst) {
                expect(fatal(block.changeStateTo(gr::lifecycle::State::REQUESTED_STOP).has_value()));
                expect(fatal(block.changeStateTo(gr::lifecycle::State::STOPPED).has_value()));
            }
        }
        // a read of the destroyed member would find the unpublished label and report it a second time
        expect(eq(captured.lines("outside applySettings()"), 1UZ)) << captured.str();
    };

    "a block that writes its Status and no setting raises no report at the stop"_test = [] {
        Publisher block;
        startAlone(block);
        std::ignore = block.work();
        CerrCapture captured;
        stopAlone(block);
        expect(eq(captured.lines("outside applySettings()"), 0UZ)) << captured.str();
    };

    "a value another thread published after the last work call is no report at the stop"_test = [] {
        Publisher block;
        startAlone(block);
        std::ignore = block.work();
        std::thread device([&block] { gr::settings::detail::FrameworkAccess::publish(gr::settings::detail::settingsOf(block), "gain", 2.5f); });
        device.join();
        CerrCapture captured;
        stopAlone(block);
        expect(eq(captured.lines("'gain'"), 0UZ)) << captured.str();
    };

    "a value another thread publishes after the stop is read"_test = [] {
        Publisher block;
        startAlone(block);
        std::ignore = block.work();
        stopAlone(block);
        std::thread device([&block] { gr::settings::detail::FrameworkAccess::publish(gr::settings::detail::settingsOf(block), "gain", 9.f); });
        device.join();
        expect(eq(block.get<float>("gain").value_or(0.f), 9.f)) << "the report after the stop is lost";
    };
};

const suite<"a block assigns a setting in its apply hook"> assignedTests = [] {
    "a latch the hook clears takes the same value again as a change, and a write at its value in force as none"_test = [] {
        Latch block;
        startAlone(block);
        std::ignore             = block.work();
        const int changesBefore = block.captureChanges;
        expect(block.set("capture", true, gr::settings::noWait).has_value());
        std::ignore = block.work();
        expect(eq(block.captures, 1));
        expect(!block.get<bool>("capture").value_or(true)) << "the hook cleared the latch";
        expect(block.set("capture", false, gr::settings::noWait).has_value());
        std::ignore = block.work();
        expect(eq(block.captureChanges, changesBefore + 1)) << "a write at the value in force runs no hook";
        expect(block.set("capture", true, gr::settings::noWait).has_value());
        std::ignore = block.work();
        expect(eq(block.captures, 2)) << "the cleared latch takes the value last set as a change";
        expect(eq(block.captureChanges, changesBefore + 2));
        stopAlone(block);
    };

    "a hook that takes a setting back by assigning the setting of old keeps the setting's flag"_test = [] {
        TakesBack block;
        block.init(std::make_shared<gr::Sequence>());
        const std::expected<gr::settings::Values, Rejected> taken = block.set("gain", 2.f);
        expect(fatal(taken.has_value())) << textOf(taken);
        expect(fatal(block.changedAfterTakeBack.has_value())) << "the hook did not see the new gain";
        expect(*block.changedAfterTakeBack) << "the assignment from old took the flag of old";
        expect(block.copyChanged.value_or(false)) << "a copy of the settings lost the flag of its source";
        expect(eq(block.get<float>("gain").value_or(0.f), 1.f)) << "the hook took the gain back";
    };
};

const suite<"a block's publish and its reports"> publishReportTests = [] {
    "a plain setting written outside the apply hook is reported at the next apply, and takes back its value in force"_test = [] {
        Publisher block;
        block.writeDirectly = true;
        startAlone(block);
        std::ignore = block.work();
        CerrCapture captured;
        expect(block.set("gain", 2.f, gr::settings::noWait).has_value());
        block.writeDirectly = false; // the next work call applies the set and writes nothing
        std::ignore         = block.work();
        expect(eq(block.settings.gain, 2.f)) << "the work call applied the set";
        expect(eq(captured.lines("'level'"), 1UZ)) << captured.str();
        expect(eq(captured.lines("outside applySettings()"), 1UZ)) << captured.str();
        expect(eq(block.get<float>("level").value_or(1.f), 0.f)) << "in force keeps its value";
        expect(eq(block.settings.level, 0.f)) << "the member takes back the value in force";
        stopAlone(block);
        expect(eq(captured.lines("outside applySettings()"), 1UZ)) << "the stop reports nothing more: " << captured.str();
    };

    "a publish of a key the block does not declare is an error notification naming it"_test = [] {
        Publisher block;
        startAlone(block);
        CerrCapture captured;
        gr::settings::detail::FrameworkAccess::publish(gr::settings::detail::settingsOf(block), "cuont", gr::Size_t{1U});
        expect(eq(captured.lines("cuont"), 1UZ)) << captured.str();
        expect(eq(captured.lines("must name a setting the block declares"), 1UZ)) << captured.str();
        expect(!block.get()->contains("cuont"));
        stopAlone(block);
    };

    "a publish of a value the field cannot hold is an error notification, and in force keeps its value"_test = [] {
        Publisher block;
        startAlone(block);
        CerrCapture captured;
        gr::settings::detail::FrameworkAccess::publish(gr::settings::detail::settingsOf(block), "trigger", std::string("seven"));
        expect(eq(captured.lines("'trigger'"), 1UZ)) << captured.str();
        expect(captured.str().contains("error")) << captured.str();
        expect(eq(block.get<gr::Size_t>("trigger").value_or(1U), 0U)) << "in force keeps its value";
        stopAlone(block);
        expect(eq(captured.lines("outside applySettings()"), 0UZ)) << captured.str();
    };

    "a publish from the block's work call while a host thread applies at the call is read"_test = [] {
        Stepping block;
        block.init(std::make_shared<gr::Sequence>());
        CerrCapture captured;
        block.inApply = [&block] {
            std::thread worker([&block] {
                const gr::settings::detail::OwnThread workCall(gr::settings::detail::settingsOf(block)); // the mark of the block's work call
                FrameworkAccess::publish(gr::settings::detail::settingsOf(block), "level", 2.5f, "the device reports its level");
            });
            worker.join();
        };
        const std::expected<gr::settings::Values, Rejected> applied = block.set("gain", 2.f);
        block.inApply                                               = {};
        expect(fatal(applied.has_value())) << textOf(applied);
        expect(eq(captured.lines("during the apply"), 0UZ)) << captured.str();
        expect(eq(block.get<float>("level").value_or(0.f), 2.5f)) << "the publish of the other thread was refused";
    };
};

const suite<"a setting assigned before the setup"> assignedBeforeSetupTests = [] {
    "a setting a host assigns before the setup holds the block not ready until a call gives it"_test = [] {
        Publisher block;
        block.settings.gain                       = 3.f;
        const std::expected<void, Rejected> ready = block.ready();
        expect(fatal(!ready.has_value()));
        expect(eq(gr::settings::describeRejected(ready.error()), std::string("gain: assigned directly; give it at construction or call set()")));
        const std::expected<gr::settings::Values, Rejected> set = block.set("gain", 3.f);
        expect(fatal(set.has_value())) << textOf(set);
        expect(block.ready().has_value());
        expect(eq(block.get<float>("gain").value_or(0.f), 3.f));
    };

    "a setting given at construction, or assigned its default value, is no direct assignment"_test = [] {
        Publisher block({{"gain", 3.f}});
        block.settings.gain                       = 5.f;
        block.settings.level                      = 0.f;
        const std::expected<void, Rejected> ready = block.ready();
        expect(fatal(ready.has_value())) << gr::settings::describeRejected(ready.error_or(Rejected{}));
        expect(eq(block.get<float>("gain").value_or(0.f), 3.f)) << "the value given wins";
    };

    "a constructor that assigns a setting holds its block not ready, and one given the value at construction is ready"_test = [] {
        Preset                              assigning;
        const std::expected<void, Rejected> ready = assigning.ready();
        expect(fatal(!ready.has_value()));
        expect(eq(gr::settings::describeRejected(ready.error()), std::string("gain: assigned directly; give it at construction or call set()")));

        Preset given({{"gain", 4.f}});
        expect(given.ready().has_value());
    };
};

const suite<"a block's own calls on its settings"> ownCallTests = [] {
    "a block's set, set with noWait and resetDefaults are refused whole from its start hook, work function and stop hook, and its get reads"_test = [] {
        OwnCaller block;
        block.init(std::make_shared<gr::Sequence>());
        expect(fatal(block.changeStateTo(gr::lifecycle::State::RUNNING).has_value()));
        std::ignore = block.work();
        std::ignore = block.work();
        expect(fatal(block.changeStateTo(gr::lifecycle::State::REQUESTED_STOP).has_value()));
        expect(fatal(block.changeStateTo(gr::lifecycle::State::STOPPED).has_value()));
        for (const auto& [place, calls] : {std::pair{"start", &block.inStart}, std::pair{"work", &block.inWork}, std::pair{"stop", &block.inStop}}) {
            expect(fatal(eq(calls->refused.size(), 3UZ))) << place;
            for (const std::expected<gr::settings::Values, Rejected>& call : calls->refused) {
                expect(fatal(!call.has_value())) << "a block's own call was taken in its" << place;
                expect(fatal(eq(call.error().size(), 1UZ))) << place;
                expect(call.error().front().reason.contains("applySettings")) << call.error().front().reason;
            }
            expect(calls->read.has_value() && calls->read->value_or(0.f) == 1.f) << "get reads in the" << place;
        }
        expect(eq(block.settings.gain, 1.f)) << "nothing of the refused calls was merged";
        expect(eq(block.get<float>("gain").value_or(0.f), 1.f));
    };
};

const suite<"a running block and the update policies"> policyTests = [] {
    "on a running block a Stopped key is refused, and an unannotated key applies within one work call"_test = [] {
        Policies block;
        block.init(std::make_shared<gr::Sequence>());
        expect(fatal(block.changeStateTo(gr::lifecycle::State::RUNNING).has_value()));
        const std::expected<gr::settings::Values, Rejected> atStart = block.set("at_start", 2.f, gr::settings::noWait);
        expect(fatal(!atStart.has_value())) << "a running block took a Stopped key";
        expect(fatal(!atStart.error().empty()));
        expect(eq(atStart.error().front().reason, std::string("must be changed while the graph is stopped")));
        expect(block.set("plain", 2.f, gr::settings::noWait).has_value());
        expect(eq(block.settings.plain, 1.f)) << "a running block applies at its work call";
        std::ignore = block.work();
        expect(eq(block.settings.plain, 2.f));
        expect(eq(block.settings.at_start.value, 1.f));
        const property_map& metadata = gr::settings::detail::declarationTable<Policies>().keyMetadata;
        expect(!metadata.contains("plain::update_policy") && !metadata.contains("plain::access")) << "a key that declares nothing exports neither";
        expect(metadata.contains("at_start::update_policy"));
        expect(fatal(block.changeStateTo(gr::lifecycle::State::REQUESTED_STOP).has_value()));
        expect(fatal(block.changeStateTo(gr::lifecycle::State::STOPPED).has_value()));
    };
};

const suite<"the scheduler's own settings"> schedulerSettingsTests = [] {
    "a running scheduler applies its settings timeout at the call and refuses a key that waits for a stop"_test = [] {
        gr::Graph flow;
        auto&     source = flow.emplaceBlock<Ones>({{"n_samples", 0U}});
        auto&     sink   = flow.emplaceBlock<Record>();
        expect(fatal(flow.connect<"out", "in">(source, sink).has_value()));
        gr::SchedulerWrapper<gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::multiThreaded>> scheduler;
        scheduler.setGraph(std::move(flow));
        expect(fatal(scheduler.start().has_value()));
        expect(fatal(waitUntil([&] { return FrameworkAccess::isRunning(gr::settings::detail::settingsOf(scheduler)); })));

        const std::expected<gr::settings::Values, Rejected> timeout = scheduler.set("settings_timeout_ms", 250U);
        expect(timeout.has_value() && timeout->empty()) << textOf(timeout);
        expect(eq(scheduler.blockRef().settings.settings_timeout_ms.value, 250U));
        expect(eq(FrameworkAccess::schedulerLink(gr::settings::detail::settingsOf(source))->defaultTimeout(), 250ms));

        const std::expected<gr::settings::Values, Rejected> atStart = scheduler.set("timeout_ms", 50U);
        expect(fatal(!atStart.has_value())) << "a running scheduler took a key that waits for a stop";
        expect(fatal(!atStart.error().empty()));
        expect(eq(atStart.error().front().reason, std::string("must be changed while the graph is stopped")));
        source.finish = true;
        scheduler.stop();
    };

    "a running single-threaded scheduler takes a Set message of its settings timeout"_test = [] {
        gr::Graph flow;
        auto&     source = flow.emplaceBlock<Ones>({{"n_samples", 0U}});
        auto&     sink   = flow.emplaceBlock<Record>();
        expect(fatal(flow.connect<"out", "in">(source, sink).has_value()));
        gr::scheduler::Simple<> scheduler;
        gr::MsgPortOut          toScheduler;
        gr::MsgPortIn           fromScheduler;
        expect(fatal(toScheduler.connect(scheduler.msgIn).has_value()));
        expect(fatal(scheduler.msgOut.connect(fromScheduler).has_value()));
        expect(fatal(scheduler.exchange(std::move(flow)).has_value()));
        std::thread runner([&scheduler] { std::ignore = scheduler.runAndWait(); });
        expect(waitUntil([&scheduler] { return FrameworkAccess::isRunning(gr::settings::detail::settingsOf(scheduler)); })) << "the scheduler does not run";

        gr::sendMessage<gr::message::Command::Set>(toScheduler, std::string(scheduler.unique_name), gr::block::property::kSetting, property_map{{"settings_timeout_ms", 250U}});
        const bool applied = waitUntil([&scheduler] { return scheduler.get<gr::Size_t>("settings_timeout_ms").value_or(0U) == 250U; });
        source.finish      = true;
        runner.join();
        expect(applied) << "the running scheduler did not apply the Set";
        expect(eq(errorMessages(fromScheduler, "publish"), 0UZ)) << "the scheduler refused the Set as a call on its own settings";
    };

    "a nested scheduler's settings timeout and level follow its parent's while its own hold none"_test = [] {
        gr::Graph flow;
        auto&     source = flow.emplaceBlock<Ones>();
        auto&     sink   = flow.emplaceBlock<Record>();
        expect(fatal(flow.connect<"out", "in">(source, sink).has_value()));
        auto inner = std::make_shared<gr::SchedulerWrapper<gr::scheduler::Simple<>>>();
        inner->setGraph(std::move(flow));
        gr::Graph outer;
        outer.addBlock(gr::SchedulerModel::asBlockModelPtr(inner));
        gr::scheduler::Simple<> scheduler;
        expect(fatal(scheduler.exchange(std::move(outer)).has_value()));

        const gr::settings::detail::SchedulerLink* link = FrameworkAccess::schedulerLink(gr::settings::detail::settingsOf(source));
        expect(fatal(link != nullptr));
        expect(eq(link->defaultTimeout(), 1000ms)) << "the top's default of 0 waits 1 s";
        expect(scheduler.set({{"settings_timeout_ms", 50U}, {"notification_level", std::string("info")}}).has_value());
        expect(eq(link->defaultTimeout(), 50ms));
        expect(link->level() == gr::settings::NotificationLevel::info);
        expect(inner->set({{"settings_timeout_ms", 70U}, {"notification_level", std::string("error")}}).has_value());
        expect(eq(link->defaultTimeout(), 70ms));
        expect(link->level() == gr::settings::NotificationLevel::error);
        expect(inner->set({{"settings_timeout_ms", 0U}, {"notification_level", std::string()}}).has_value());
        expect(eq(link->defaultTimeout(), 50ms)) << "a nested timeout set back to 0 inherits again";
        expect(link->level() == gr::settings::NotificationLevel::info);
    };

    "a scheduler's pool name is empty by default, and a name no pool has is reported and reads back empty"_test = [] {
        gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::multiThreaded> scheduler;
        expect(eq(scheduler.get<std::string>("poolName").value_or("?"), std::string()));
        const std::expected<gr::settings::Values, Rejected> unknown = scheduler.set("poolName", std::string("no such pool"));
        expect(fatal(unknown.has_value())) << textOf(unknown);
        expect(eq(scheduler.get<std::string>("poolName").value_or("?"), std::string())) << "an unknown pool name stayed in force";
        const std::expected<gr::settings::Values, Rejected> known = scheduler.set("poolName", std::string(gr::thread_pool::kDefaultIoPoolId));
        expect(known.has_value() && known->empty()) << textOf(known);
        expect(eq(scheduler.get<std::string>("poolName").value_or("?"), std::string(gr::thread_pool::kDefaultIoPoolId)));
    };
};

const suite<"the cases of a run's outcome, its delivery and its pool"> outcomeTests = [] {
    using enum gr::scheduler::ExecutionPolicy;
    using gr::settings::Notification;

    "a key the start hook fails is a failed value, and a read lists it no more"_test = [] {
        Chain chain({{"code", std::string("A1")}});
        chain.gain.failCodeWithoutValue = true;
        gr::SchedulerWrapper<gr::scheduler::Simple<multiThreaded>> scheduler;
        scheduler.setGraph(std::move(chain.flow));
        expect(fatal(scheduler.start().has_value()));
        expect(fatal(waitUntil([&] { return FrameworkAccess::isRunning(gr::settings::detail::settingsOf(chain.gain)); })));
        const gr::settings::Values outcome = FrameworkAccess::runOutcome(gr::settings::detail::settingsOf(chain.gain));
        expect(fatal(outcome.contains("code")));
        expect(outcome.at("code").state == Entry::State::failed);
        expect(outcome.at("code").reason.contains("no code")) << outcome.at("code").reason;
        const std::expected<gr::settings::Values, Rejected> all = chain.gain.get();
        expect(fatal(all.has_value()));
        expect(!all->contains("code")) << "a read lists a key the device lacks";
        chain.source.finish = true;
        scheduler.stop();
    };

    "a stop hook that throws after a failed start leaves the start's error, and no notification names it"_test = [] {
        Chain chain;
        chain.gain.startThrow = "no device answers";
        chain.gain.stopThrow  = "the device would not close";
        gr::MsgPortIn           messages; // outlives the scheduler that writes to it
        gr::scheduler::Simple<> scheduler;
        expect(fatal(scheduler.exchange(std::move(chain.flow)).has_value()));
        std::vector<Notification> received;
        scheduler.installReader([&](const Notification& notification) { received.push_back(notification); });
        expect(fatal(scheduler.msgOut.connect(messages).has_value()));
        const std::expected<void, gr::Error> ran = scheduler.runAndWait();
        expect(fatal(!ran.has_value()));
        expect(ran.error().message.contains("no device answers")) << ran.error().message;
        expect(eq(chain.gain.nStops.load(), 1));
        expect(eq(errorMessages(messages, "the device would not close"), 1UZ)) << "the stop hook's throw did not reach the error message";
        expect(std::ranges::none_of(received, [](const Notification& notification) { return notification.reason.contains("would not close"); })) << "a settings notification named the stop hook's throw";
    };

    "an inherited off marks a nested block's notification quiet, which a host's reader still receives"_test = [] {
        Chain chain;
        chain.gain.failAbove = 10.f;
        auto inner           = std::make_shared<gr::SchedulerWrapper<gr::scheduler::Simple<multiThreaded>>>();
        inner->setGraph(std::move(chain.flow));
        expect(fatal(inner->set("notification_level", std::string("off")).has_value()));
        gr::Graph outer;
        outer.addBlock(gr::SchedulerModel::asBlockModelPtr(inner));
        CerrCapture                                                captured; // swapped in before the workers start
        gr::SchedulerWrapper<gr::scheduler::Simple<multiThreaded>> scheduler;
        scheduler.setGraph(std::move(outer));
        expect(fatal(scheduler.start().has_value()));
        expect(fatal(waitUntil([&] { return FrameworkAccess::isRunning(gr::settings::detail::settingsOf(chain.gain)); })));

        expect(gr::testing::everyKeySent(property_map{{"gain", 20.f}}, chain.gain.set("gain", 20.f)).at("gain").isFailed());
        std::mutex                lock;
        std::vector<Notification> received;
        scheduler.blockRef().installReader([&](const Notification& notification) {
            std::lock_guard guard(lock);
            received.push_back(notification);
        });
        expect(gr::testing::everyKeySent(property_map{{"gain", 30.f}}, chain.gain.set("gain", 30.f)).at("gain").isFailed());
        expect(fatal(waitUntil([&] {
            std::lock_guard guard(lock);
            return std::ranges::any_of(received, [](const Notification& notification) { return notification.key == "gain"; });
        }))) << "the host's reader received no failure of the nested block";
        {
            std::lock_guard guard(lock);
            expect(std::ranges::all_of(received, [](const Notification& notification) { return notification.key != "gain" || notification.quiet; })) << "a nested block's failure under an inherited off was not marked quiet";
        }
        chain.source.finish = true;
        scheduler.stop();
        expect(eq(captured.lines("'gain'"), 0UZ)) << captured.str();
    };

    "readers run one at a time while a multi-threaded run delivers and stops"_test = [] {
        gr::Graph both;
        auto&     leftSource  = both.emplaceBlock<Ones>({{"n_samples", 0U}});
        auto&     leftGain    = both.emplaceBlock<Gain>();
        auto&     leftSink    = both.emplaceBlock<Record>();
        auto&     rightSource = both.emplaceBlock<Ones>({{"n_samples", 0U}});
        auto&     rightGain   = both.emplaceBlock<Gain>();
        auto&     rightSink   = both.emplaceBlock<Record>();
        expect(fatal(both.connect<"out", "in">(leftSource, leftGain).has_value() && both.connect<"out", "in">(leftGain, leftSink).has_value()));
        expect(fatal(both.connect<"out", "in">(rightSource, rightGain).has_value() && both.connect<"out", "in">(rightGain, rightSink).has_value()));
        gr::SchedulerWrapper<gr::scheduler::Simple<multiThreaded>> scheduler;
        scheduler.setGraph(std::move(both));
        std::atomic<int>         inReader{0};
        std::atomic<bool>        overlapped{false};
        std::atomic<std::size_t> nRead{0UZ};
        scheduler.blockRef().installReader([&](const Notification& /*notification*/) {
            if (inReader.fetch_add(1) != 0) {
                overlapped = true;
            }
            std::this_thread::sleep_for(50us); // holds the reader open for a reader the delivery keeps out, which no barrier can await
            nRead.fetch_add(1UZ);
            inReader.fetch_sub(1);
        });
        for (int run = 0; run < 20; ++run) {
            expect(fatal(scheduler.start().has_value()));
            expect(fatal(waitUntil([&] { return FrameworkAccess::isRunning(gr::settings::detail::settingsOf(leftGain)) && FrameworkAccess::isRunning(gr::settings::detail::settingsOf(rightGain)); })));
            for (int i = 0; i < 50; ++i) { // the workers deliver these while they run, and the stop what is left as they retire
                gr::settings::detail::FrameworkAccess::fail(gr::settings::detail::settingsOf(i % 2 == 0 ? leftGain : rightGain), "gain", std::format("the device fell back to {}", i));
            }
            scheduler.stop();
        }
        expect(!overlapped.load()) << "two readers ran at once";
        expect(gt(nRead.load(), 0UZ));
    };

    "a running scheduler applies a notification level at the call"_test = [] {
        Chain                                                      chain;
        gr::SchedulerWrapper<gr::scheduler::Simple<multiThreaded>> scheduler;
        scheduler.setGraph(std::move(chain.flow));
        expect(fatal(scheduler.start().has_value()));
        expect(fatal(waitUntil([&] { return FrameworkAccess::isRunning(gr::settings::detail::settingsOf(scheduler)); })));
        const std::expected<gr::settings::Values, Rejected> level = scheduler.set("notification_level", std::string("error"));
        expect(level.has_value() && level->empty()) << textOf(level);
        expect(FrameworkAccess::schedulerLink(gr::settings::detail::settingsOf(chain.gain))->level() == gr::settings::NotificationLevel::error);
        chain.source.finish = true;
        scheduler.stop();
    };

    "a worker's own publish on a run whose workers are busy wakes no worker of its scheduler"_test = [] {
        gr::Graph flow;
        auto&     source  = flow.emplaceBlock<Ones>({{"n_samples", 2000000000U}});
        auto&     counter = flow.emplaceBlock<Counter>();
        auto&     sink    = flow.emplaceBlock<Drop>();
        expect(fatal(flow.connect<"out", "in">(source, counter).has_value()));
        expect(fatal(flow.connect<"out", "in">(counter, sink).has_value()));
        gr::SchedulerWrapper<Probe<gr::scheduler::Simple<multiThreadedBlocking>>> scheduler;
        scheduler.setGraph(std::move(flow));
        expect(fatal(scheduler.start().has_value()));
        expect(fatal(waitUntil([&] { return counter.nWorkCalls.load() > 0UZ; })));
        const std::size_t wakesBefore = scheduler.blockRef().nWakes();
        const std::size_t callsBefore = counter.nWorkCalls.load();
        expect(fatal(waitUntil([&] { return counter.nWorkCalls.load() >= callsBefore + 1000UZ; })));
        const std::size_t nWakes = scheduler.blockRef().nWakes() - wakesBefore;
        scheduler.stop();
        expect(lt(nWakes, 100UZ)) << std::format("{} wakes for 1000 publishes from the block's own worker", nWakes);
    };

    "a top scheduler with no pool named runs on the CPU pool, a nested one on the IO pool unless it names one"_test = [] {
        const auto finiteChain = [] {
            gr::Graph flow;
            auto&     source = flow.emplaceBlock<Ones>();
            auto&     sink   = flow.emplaceBlock<Record>();
            expect(fatal(flow.connect<"out", "in">(source, sink).has_value()));
            return flow;
        };
        Probe<gr::scheduler::Simple<multiThreaded>> top;
        expect(fatal(top.exchange(finiteChain()).has_value()));
        expect(fatal(top.runAndWait().has_value()));
        expect(eq(top.poolInUse(), std::string(gr::thread_pool::kDefaultCpuPoolId)));

        for (const bool named : {false, true}) {
            auto inner = std::make_shared<gr::SchedulerWrapper<Probe<gr::scheduler::Simple<>>>>();
            inner->setGraph(finiteChain());
            if (named) {
                expect(fatal(inner->set("poolName", std::string(gr::thread_pool::kDefaultCpuPoolId)).has_value()));
            }
            gr::Graph outer;
            outer.addBlock(gr::SchedulerModel::asBlockModelPtr(inner));
            gr::scheduler::Simple<> scheduler;
            expect(fatal(scheduler.exchange(std::move(outer)).has_value()));
            expect(fatal(scheduler.runAndWait().has_value()));
            expect(eq(inner->blockRef().poolInUse(), std::string(named ? gr::thread_pool::kDefaultCpuPoolId : gr::thread_pool::kDefaultIoPoolId))) << std::format("named: {}", named);
        }
    };

    "an unknown pool name is one error message naming it, and the scheduler nested afterward runs on the IO pool"_test = [] {
        gr::Graph flow;
        auto&     source = flow.emplaceBlock<Ones>();
        auto&     sink   = flow.emplaceBlock<Record>();
        expect(fatal(flow.connect<"out", "in">(source, sink).has_value()));
        auto inner = std::make_shared<gr::SchedulerWrapper<Probe<gr::scheduler::Simple<>>>>();
        inner->setGraph(std::move(flow));
        {
            gr::MsgPortIn messages;
            expect(fatal(inner->blockRef().msgOut.connect(messages).has_value()));
            const std::expected<gr::settings::Values, Rejected> unknown = inner->set("poolName", std::string("no such pool"));
            expect(fatal(unknown.has_value())) << textOf(unknown);
            expect(eq(errorMessages(messages, "no such pool"), 1UZ));
            expect(eq(inner->get<std::string>("poolName").value_or("?"), std::string()));
            expect(fatal(inner->blockRef().msgOut.disconnect().has_value()));
        }
        gr::Graph outer;
        outer.addBlock(gr::SchedulerModel::asBlockModelPtr(inner));
        gr::scheduler::Simple<> scheduler;
        expect(fatal(scheduler.exchange(std::move(outer)).has_value()));
        expect(fatal(scheduler.runAndWait().has_value()));
        expect(eq(inner->blockRef().poolInUse(), std::string(gr::thread_pool::kDefaultIoPoolId)));
    };
};

} // namespace qa_settings_lifecycle

int main() { /* tests are statically registered */ }
