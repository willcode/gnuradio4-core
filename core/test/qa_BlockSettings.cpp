#include <boost/ut.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <expected>
#include <format>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <gnuradio-4.0/Block.hpp>
#include <gnuradio-4.0/Port.hpp>
#include <gnuradio-4.0/Settings.hpp>

#include "settings_utils.hpp"

// operator delete below frees what malloc gave operator new
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif

namespace {

// the allocation count of each thread that turned `countAllocations` on
std::atomic<std::size_t> gAllocations{0UZ};
thread_local bool        countAllocations = false;

} // namespace

void* operator new(std::size_t size) {
    if (countAllocations) {
        gAllocations.fetch_add(1UZ, std::memory_order_relaxed);
    }
    if (void* memory = std::malloc(size == 0UZ ? 1UZ : size); memory != nullptr) {
        return memory;
    }
    throw std::bad_alloc();
}

void* operator new(std::size_t size, std::align_val_t alignment) {
    if (countAllocations) {
        gAllocations.fetch_add(1UZ, std::memory_order_relaxed);
    }
    const std::size_t align = static_cast<std::size_t>(alignment);
    if (void* memory = std::aligned_alloc(align, (size + align - 1UZ) / align * align + (size == 0UZ ? align : 0UZ)); memory != nullptr) {
        return memory;
    }
    throw std::bad_alloc();
}

void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete(void* memory, std::align_val_t) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t, std::align_val_t) noexcept { std::free(memory); }

namespace qa_block_settings {

using namespace boost::ut;
using namespace std::chrono_literals;
using gr::property_map;
using gr::settings::BlockSettingsOf;
using gr::settings::Rejected;
using gr::settings::Rejection;
using gr::settings::Values;
using gr::settings::detail::BlockState;
using gr::testing::CerrCapture;
using PmtValue = gr::pmt::Value;
using Entry    = gr::settings::Value;

// the framework's side of a block's settings, which a test drives in place of a graph and a scheduler
template<typename TBlock>
struct Driven : BlockSettingsOf<TBlock> {
    using BlockSettingsOf<TBlock>::BlockSettingsOf;
    using BlockSettingsOf<TBlock>::moveState;
    using BlockSettingsOf<TBlock>::markStarting;
    using BlockSettingsOf<TBlock>::attachGraph;
    using BlockSettingsOf<TBlock>::attachScheduler;
    using BlockSettingsOf<TBlock>::takeForwarded;
    using BlockSettingsOf<TBlock>::tagEnters;
    using BlockSettingsOf<TBlock>::savedValues;
    using BlockSettingsOf<TBlock>::publish;
    using BlockSettingsOf<TBlock>::fail;
    using BlockSettingsOf<TBlock>::isGiven;
};

struct FakeGraph : gr::settings::detail::GraphLink {
    std::size_t connected = 0UZ; // the ports of each collection below this index hold an edge
    int         refreshes = 0;

    std::optional<std::string> edgeAtOrPast(std::string_view collection, std::size_t count) const override { return count < connected ? std::optional<std::string>(std::format("{}#{}", collection, count)) : std::nullopt; }
    void                       refreshPorts() override { ++refreshes; }
};

// a scheduler as a block's settings see it; the test thread alone pushes into it
struct FakeScheduler : gr::settings::detail::SchedulerLink {
    std::atomic<int>                               wakes{0};
    std::vector<gr::settings::Notification>        held{};
    bool                                           worker = false;
    std::optional<gr::settings::NotificationLevel> inherited{};
    std::chrono::milliseconds                      timeout{1000};

    void                                           wake() override { wakes.fetch_add(1, std::memory_order_relaxed); }
    void                                           push(gr::settings::Notification notification) override { held.push_back(std::move(notification)); }
    bool                                           isWorkerThread() const override { return worker; }
    std::optional<gr::settings::NotificationLevel> level() const override { return inherited; }
    std::chrono::milliseconds                      defaultTimeout() const override { return timeout; }
};

// a scheduler at level info whose delivery prints each notification at once, as the default reader prints
struct InfoScheduler : FakeScheduler {
    InfoScheduler() { inherited = gr::settings::NotificationLevel::info; }

    void push(gr::settings::Notification notification) override { gr::settings::detail::printNotification(notification); }
};

struct Plain {
    std::vector<gr::PortIn<float>> inputs{2UZ};

    struct Settings {
        gr::Annotated<float, gr::Limits<gr::Min<0.f>, gr::Max<50.f>>> gain       = 1.f;
        double                                                        ppm        = 0.0;
        std::int32_t                                                  a          = 0;
        std::int32_t                                                  b          = 0;
        std::vector<float>                                            taps       = {1.f};
        bool                                                          enabled    = true;
        gr::Annotated<gr::Size_t, gr::StoppedOnly>                    burst_size = 16U;
        gr::Annotated<gr::Size_t, gr::PortCountFor<"inputs">>         n_inputs   = 2U;

        GR_MAKE_REFLECTABLE(Settings, gain, ppm, a, b, taps, enabled, burst_size, n_inputs);
    } settings;

    struct Status {
        float level = 0.f;

        GR_MAKE_REFLECTABLE(Status, level);
    } status;

    GR_MAKE_REFLECTABLE(Plain, inputs, settings, status);

    std::string              unique_name = "plain#0";
    gr::SettingsBase*        core        = nullptr;
    int                      applyCalls  = 0;
    int                      resetCalls  = 0;
    mutable int              checkCalls  = 0;
    std::vector<std::string> lastChanged{}; // the settings the hook's last call found changed from `old`
    float                    refuseGainAbove = 1e9f;
    std::string              failKey{}; // the hook fails this setting when `failWhen` changed, else when it changed
    std::string              failWhen{};
    bool                     failOnAnotherThread = false; // the hook makes its fail call from a thread it starts and joins
    std::string              throwText{};
    std::string              checkThrowText{};
    std::string              resetThrowText{};
    bool                     roundGain     = false;
    bool                     publishInHook = false; // the hook publishes the gain and the ppm when the gain changed
    bool                     deriveB       = false;
    bool                     forwardGain   = false;
    bool                     stageBWhenA   = false;
    double                   oldPpm        = -1.0; // `old.ppm` at the hook's last call
    std::optional<float>     assignGain{};         // the hook assigns this gain when `assignGainWhen` changed
    std::string              assignGainWhen = "gain";

    std::vector<std::expected<Values, Rejected>>         ownCalls{}; // the hook's settings calls on its own block
    std::optional<std::expected<std::int32_t, Rejected>> ownRead{};

    std::optional<gr::Refused> checkSettings(const Settings& s) const {
        ++checkCalls;
        if (!checkThrowText.empty()) {
            throw std::runtime_error(checkThrowText);
        }
        if (s.gain > refuseGainAbove) {
            return gr::Refused(s.gain, "the hook refuses this gain");
        }
        return {};
    }

    // assigns `b` and the gain where the test asks it to, to pin how core takes a value the hook writes
    void applySettings(Settings& s, const Settings& old) {
        ++applyCalls;
        oldPpm             = old.ppm;
        lastChanged        = changedFrom(old);
        const auto changed = [this](std::string_view key) { return std::ranges::contains(lastChanged, key); };
        if (!throwText.empty()) {
            throw std::runtime_error(throwText);
        }
        if (publishInHook && changed("gain")) {
            gr::settings::detail::FrameworkAccess::publish(*core, "gain", PmtValue(7.f), "the device runs 7");
            gr::settings::detail::FrameworkAccess::publish(*core, "ppm", PmtValue(7.0), "the device runs 7");
        }
        if (roundGain && changed("gain")) {
            s.gain = std::round(s.gain.value);
        }
        if (deriveB && changed("a")) {
            s.b = s.a * 2;
        }
        if (forwardGain && changed("gain")) {
            gr::settings::detail::FrameworkAccess::forwardTag(*core, "gain", PmtValue(s.gain.value));
        }
        if (stageBWhenA && changed("a")) {
            ownCalls.push_back(core->set("b", 1));
            ownCalls.push_back(core->set(property_map{{"b", 1}}, gr::settings::noWait));
            ownCalls.push_back(core->resetDefaults(gr::settings::noWait));
            ownRead = core->get<std::int32_t>("a");
        }
        if (assignGain && changed(assignGainWhen)) {
            s.gain = *assignGain;
        }
        if (!failKey.empty() && changed(failWhen.empty() ? failKey : failWhen)) {
            const auto failNow = [this] { gr::settings::detail::FrameworkAccess::fail(*core, failKey, "the device refuses it"); };
            if (failOnAnotherThread) {
                std::thread(failNow).join();
            } else {
                failNow();
            }
        }
    }

    [[nodiscard]] std::vector<std::string> changedFrom(const Settings& old) const {
        std::vector<std::string> names;
        const auto               note = [&names](std::string_view name, bool moved) {
            if (moved) {
                names.emplace_back(name);
            }
        };
        note("a", settings.a != old.a);
        note("b", settings.b != old.b);
        note("burst_size", settings.burst_size.value != old.burst_size.value);
        note("enabled", settings.enabled != old.enabled);
        note("gain", settings.gain.value != old.gain.value);
        note("n_inputs", settings.n_inputs.value != old.n_inputs.value);
        note("ppm", settings.ppm != old.ppm);
        note("taps", settings.taps != old.taps);
        return names;
    }

    void reset() {
        ++resetCalls;
        if (!resetThrowText.empty()) {
            throw std::runtime_error(resetThrowText);
        }
    }
};

struct Radio {
    std::vector<gr::PortOut<float>> outputs{2UZ};

    struct Settings {
        gr::Annotated<gr::Size_t, gr::StoppedOnly> burst_size = 16U;

        GR_MAKE_REFLECTABLE(Settings, burst_size);
    } settings;

    GR_MAKE_REFLECTABLE(Radio, outputs, settings);

    property_map                                  describedSettings;
    gr::SettingsBase*                             core       = nullptr;
    int                                           applyCalls = 0;
    std::vector<std::string>                      lastChanged{};
    property_map                                  describedBefore{}; // the described values at the hook's last call that kept them
    std::string                                   failKey{};         // the hook throws when this key changed
    bool                                          readLnaInHook = false;
    std::optional<std::expected<Entry, Rejected>> lnaInHook{};

    static property_map settingsDescription() {
        return property_map{
            {"version", 1},
            {"block_type", "qa::Radio"},
            {"settings",
                property_map{
                    {"sample_rate", property_map{{"type", "float64"}, {"default", 10.0e6}, {"minimum", 2.0e6}, {"maximum", 20.0e6}}},
                    {"n_channels", property_map{{"type", "uint32"}, {"default", 2}, {"port_count_for", "outputs"}}},
                    {"temperature", property_map{{"type", "float64"}, {"access", "read_only"}}},
                }},
            {"groups", property_map{{"channel",
                           property_map{
                               {"count", "n_channels"},
                               {"bare", true},
                               {"settings",
                                   property_map{
                                       {"frequency", property_map{{"type", "float64"}, {"default", 100.0e6}, {"minimum", 1.0e6}, {"maximum", 6.0e9}}},
                                       {"lna", property_map{{"type", "float64"}, {"minimum", 0.0}, {"maximum", 40.0}}},
                                       {"rssi", property_map{{"type", "float32"}, {"access", "read_only"}}},
                                   }},
                           }}}},
        };
    }

    void applySettings([[maybe_unused]] const Settings& s) {
        ++applyCalls;
        lastChanged.clear();
        for (const auto& [key, value] : describedSettings) {
            if (const auto before = describedBefore.find(key); before == describedBefore.end() || !(before->second == value)) {
                lastChanged.emplace_back(key);
            }
        }
        if (readLnaInHook && std::ranges::contains(lastChanged, "channel.0.lna")) {
            lnaInHook = core->get("channel.0.lna");
        }
        if (!failKey.empty() && std::ranges::contains(lastChanged, failKey)) {
            throw std::runtime_error("the device refuses it");
        }
        describedBefore = describedSettings;
    }
};

// a block with a map setting, which may hold not-a-number inside
struct Recorded {
    struct Settings {
        property_map records{};
        std::int32_t a = 0;

        GR_MAKE_REFLECTABLE(Settings, records, a);
    } settings;

    GR_MAKE_REFLECTABLE(Recorded, settings);

    void applySettings([[maybe_unused]] const Settings& s) {}
};

// a scheduler-style block group whose key applies at the call while it runs
struct Tuner {
    struct Settings {
        double frequency = 100e6;

        GR_MAKE_REFLECTABLE(Settings, frequency);
    } settings;

    GR_MAKE_REFLECTABLE(Tuner, settings);

    static constexpr gr::block::Category               blockCategory = gr::block::Category::ScheduledBlockGroup;
    static constexpr std::array<std::string_view, 1UZ> kSettingsAtCall{"frequency"};

    int applyCalls = 0;

    void applySettings([[maybe_unused]] const Settings& s) { ++applyCalls; }
};

struct Faulty {
    property_map describedSettings;

    static property_map settingsDescription() { return property_map{{"version", 1}, {"block_type", "qa::Faulty"}, {"settings", property_map{{"gain", property_map{{"type", "float64"}, {"minimun", 0.0}}}}}}; }
};

// a block whose hook counts its calls in atomics, which a thread reads while another applies
struct Counter {
    struct Settings {
        std::int64_t a      = 0;
        std::int64_t b      = 0;
        std::int64_t c      = 0;
        std::int64_t shared = 0;

        GR_MAKE_REFLECTABLE(Settings, a, b, c, shared);
    } settings;

    GR_MAKE_REFLECTABLE(Counter, settings);

    std::atomic<int> applyCalls{0};
    std::atomic<int> callsSettingA{0};

    void applySettings(const Settings& s, const Settings& old) {
        applyCalls.fetch_add(1, std::memory_order_relaxed);
        if (s.a != old.a) {
            callsSettingA.fetch_add(1, std::memory_order_relaxed);
        }
    }
};

// a block whose settings hook takes the lock its control thread holds while it publishes
struct Guarded {
    struct Settings {
        float        gain = 1.f;
        std::int32_t a    = 0;

        GR_MAKE_REFLECTABLE(Settings, gain, a);
    } settings;

    GR_MAKE_REFLECTABLE(Guarded, settings);

    std::mutex        device;
    std::atomic<bool> inHook{false};
    bool              waitInHook = false;

    void applySettings(const Settings& s, const Settings& old) {
        if (waitInHook && s.a != old.a) {
            inHook.store(true);
            inHook.notify_all();
            std::lock_guard lock(device);
        }
    }
};

// a block whose check holds `min` at or below `max`, and whose device runs a `max` of 10 at most
struct Bounds {
    struct Settings {
        std::int32_t min = 0;
        std::int32_t max = 10;

        GR_MAKE_REFLECTABLE(Settings, min, max);
    } settings;

    GR_MAKE_REFLECTABLE(Bounds, settings);

    gr::SettingsBase* core       = nullptr;
    int               applyCalls = 0;

    std::optional<gr::Refused> checkSettings(const Settings& s) const {
        if (s.min > s.max) {
            return gr::Refused(s.min, "must not exceed max");
        }
        return {};
    }

    void applySettings(const Settings& s, const Settings& old) {
        ++applyCalls;
        if (s.max != old.max && s.max > 10) {
            gr::settings::detail::FrameworkAccess::fail(*core, "max", "runs 10 at most");
        }
    }
};

struct OldHook {
    void settingsChanged(const property_map&, const property_map&) {}
};
struct TemplatedOldHook {
    template<typename T>
    void settingsChanged(const T&, const T&) {}
};
struct MutableCheck {
    struct Settings {
        float gain = 1.f;

        GR_MAKE_REFLECTABLE(Settings, gain);
    } settings;

    GR_MAKE_REFLECTABLE(MutableCheck, settings);

    std::optional<gr::Refused> checkSettings(const Settings&) { return {}; }
};
struct FinalOldHook final {
    void settingsChanged(const property_map&, const property_map&) {}
};

static_assert(gr::settings::HasSettingsApply<OldHook> && gr::settings::HasSettingsApply<TemplatedOldHook>, "the older hook is a settings hook");
static_assert(gr::settings::detail::NamesCheckSettings<MutableCheck> && !gr::settings::HasSettingsCheck<MutableCheck>);
static_assert(gr::settings::HasSettingsApply<Plain> && gr::settings::HasSettingsCheck<Plain>);
static_assert(gr::settings::HasSettingsApply<FinalOldHook>, "a final block type takes the older hook");

template<typename TBlock>
struct Fixture {
    TBlock         block{};
    Driven<TBlock> settings;

    explicit Fixture(property_map constructedWith = {}) : settings(block, std::move(constructedWith)) {
        if constexpr (requires { block.core; }) {
            block.core = &settings;
        }
    }
};

[[nodiscard]] bool rejectedNames(const std::expected<Values, Rejected>& result, std::string_view key) {
    return !result.has_value() && std::ranges::any_of(result.error(), [&](const Rejection& entry) { return entry.key == key; });
}

// the reason the result gives for a key whose apply failed, or nothing
[[nodiscard]] std::optional<std::string> failedReason(const std::expected<Values, Rejected>& result, std::string_view key) {
    if (!result.has_value()) {
        return std::nullopt;
    }
    const auto it = result->find(key);
    return it != result->end() && it->second.isFailed() ? std::optional(it->second.reason) : std::nullopt;
}

[[nodiscard]] bool failedNames(const std::expected<Values, Rejected>& result, std::string_view key) { return failedReason(result, key).has_value(); }

[[nodiscard]] std::expected<Values, Rejected> setOne(gr::SettingsBase& settings, std::string_view key, PmtValue value, gr::settings::Timeout timeout = std::nullopt) { return settings.set(key, std::move(value), timeout); }

// the wait of a call that a case ends by an apply or a stop, far beyond the time any of them takes
constexpr std::chrono::milliseconds kLongWait{60000};

// the worker's side: applies until the block's hook has run `calls` times
template<typename TBlock>
void applyUntilHookCalls(Fixture<TBlock>& fixture, int calls) {
    while (fixture.block.applyCalls < calls) {
        std::ignore = gr::testing::applyStagedForTest(fixture.settings);
        std::this_thread::yield();
    }
}

const suite<"BlockSettings: the check"> checkTests = [] {
    "a double sent to a float key takes the nearest float"_test = [] {
        Fixture<Plain> fixture;
        expect(setOne(fixture.settings, "gain", 2000000.0 / 1e5).has_value());
        expect(eq(fixture.settings.get<double>("gain").value_or(0.0), 20.0));
        expect(setOne(fixture.settings, "gain", 10.1).has_value());
        expect(eq(fixture.settings.get<float>("gain").value_or(0.f), static_cast<float>(10.1)));
    };

    "an integer sent to a floating-point key converts"_test = [] {
        Fixture<Plain> fixture;
        expect(setOne(fixture.settings, "ppm", std::int32_t{7}).has_value());
        expect(eq(fixture.settings.get<double>("ppm").value_or(0.0), 7.0));
    };

    "a text converts to a described numeric type"_test = [] {
        Fixture<Radio> fixture;
        expect(setOne(fixture.settings, "sample_rate", "5e6").has_value());
        const auto it = fixture.block.describedSettings.find("sample_rate");
        expect(fatal(it != fixture.block.describedSettings.end()));
        expect(it->second == PmtValue(5e6));
        expect(rejectedNames(setOne(fixture.settings, "sample_rate", "5e6x"), "sample_rate"));
    };

    "a list converts element by element and any other pair refuses naming the type it must have"_test = [] {
        Fixture<Plain> fixture;
        expect(setOne(fixture.settings, "taps", std::vector<double>{0.1, 2.5}).has_value());
        expect(fatal(eq(fixture.block.settings.taps.size(), 2UZ)));
        expect(eq(fixture.block.settings.taps[0], static_cast<float>(0.1)));
        const auto refused = setOne(fixture.settings, "enabled", "yes");
        expect(fatal(rejectedNames(refused, "enabled")));
        expect(eq(refused.error().front().reason, std::string("must be true, false, 1 or 0")));
        const auto wrongKind = setOne(fixture.settings, "enabled", std::int32_t{1});
        expect(fatal(rejectedNames(wrongKind, "enabled")));
        expect(eq(wrongKind.error().front().reason, std::string("must be of type bool")));
        expect(setOne(fixture.settings, "enabled", "FALSE").has_value());
        expect(!fixture.block.settings.enabled);
    };

    "an undeclared key refuses the call and the hook sees no write"_test = [] {
        Fixture<Plain> fixture;
        expect(fixture.settings.check({}).has_value());
        const int  calls   = fixture.block.applyCalls;
        const auto refused = setOne(fixture.settings, "rx_gain", 1.0);
        expect(fatal(!refused.has_value()));
        expect(eq(refused.error().size(), 1UZ));
        expect(rejectedNames(refused, "rx_gain"));
        expect(eq(fixture.block.applyCalls, calls));
    };

    "a misspelled member key, a nested map, a member past its count and a leading zero are undeclared"_test = [] {
        Fixture<Radio> fixture;
        for (std::string_view key : {"channel.0.lnb", "channel.2.frequency", "channel.01.frequency", "channel"}) {
            const PmtValue value  = key == "channel" ? PmtValue(property_map{{"0", property_map{{"frequency", 1e8}}}}) : PmtValue(1e8);
            const auto     result = setOne(fixture.settings, key, value);
            expect(fatal(!result.has_value() && !result.error().empty())) << key;
            expect(rejectedNames(result, key)) << key;
            expect(result.error().front().reason == "must name a setting the block declares") << key;
        }
    };

    "both spellings of one key refuse with different values and pass with one"_test = [] {
        Fixture<Radio> fixture;
        const auto     refused = fixture.settings.set(property_map{{"frequency", 144e6}, {"channel.0.frequency", 145e6}});
        expect(fatal(!refused.has_value()));
        expect(eq(refused.error().front().reason, std::string("must take one value in a call")));
        expect(fixture.settings.set(property_map{{"frequency", 144e6}, {"channel.0.frequency", 144e6}}).has_value());
        expect(eq(fixture.settings.get<double>("channel.0.frequency").value_or(0.0), 144e6));
    };

    "limits refuse a changed value"_test = [] {
        Fixture<Plain> plain;
        expect(rejectedNames(setOne(plain.settings, "gain", 60.f), "gain"));
        expect(eq(plain.block.settings.gain.value, 1.f));
        Fixture<Radio> radio;
        expect(rejectedNames(setOne(radio.settings, "channel.1.frequency", 7e9), "channel.1.frequency"));
    };

    "a read-only key reads, and a call that names it is refused whole"_test = [] {
        Fixture<Radio> fixture;
        expect(fixture.settings.check({}).has_value());
        fixture.settings.publish("temperature", PmtValue(41.5));
        expect(eq(fixture.settings.get<double>("temperature").value_or(0.0), 41.5));
        const auto result = fixture.settings.set(property_map{{"temperature", 1.0}, {"sample_rate", 5e6}});
        expect(fatal(rejectedNames(result, "temperature")));
        expect(eq(result.error().front().reason, std::string("is read-only")));
        expect(eq(fixture.settings.get<double>("sample_rate").value_or(0.0), 10e6)) << "a refused call writes nothing";
        expect(eq(fixture.settings.get<double>("temperature").value_or(0.0), 41.5));
        const auto read = fixture.settings.get();
        expect(fatal(read.has_value()));
        expect(rejectedNames(fixture.settings.set(gr::settings::heldValues(read)), "temperature")) << "a read set back whole names its read-only keys";
        expect(fixture.settings.set(gr::testing::writableValues(fixture.settings, read)).has_value()) << "a read set back without its read-only keys";
    };

    "a block declares no notification level"_test = [] {
        Fixture<Plain> fixture;
        expect(rejectedNames(setOne(fixture.settings, "notification_level", "info"), "notification_level"));
    };

    "a count and a member of the new count in one call"_test = [] {
        Fixture<Radio> fixture;
        expect(fixture.settings.set(property_map{{"n_channels", 3U}, {"channel.2.frequency", 200e6}}).has_value());
        expect(eq(fixture.settings.get<double>("channel.2.frequency").value_or(0.0), 200e6));
    };

    "an at-start key and a port key are refused while the block runs"_test = [] {
        Fixture<Plain> fixture;
        expect(fixture.settings.check({}).has_value());
        fixture.settings.moveState(BlockState::running);
        const auto atStart = setOne(fixture.settings, "burst_size", 32U);
        expect(fatal(rejectedNames(atStart, "burst_size")));
        expect(eq(atStart.error().front().reason, std::string("must be changed while the graph is stopped")));
        const auto port = setOne(fixture.settings, "n_inputs", 3U);
        expect(fatal(rejectedNames(port, "n_inputs")));
        expect(eq(port.error().front().reason, std::string("must be changed while the graph is stopped")));
        expect(setOne(fixture.settings, "burst_size", 16U, gr::settings::noWait).has_value()) << "a key at its value in force passes";
    };

    "a port key is refused while an edge holds a removed port, and applies at the call once none does"_test = [] {
        Fixture<Plain> fixture;
        FakeGraph      graph;
        graph.connected = 2UZ;
        fixture.settings.attachGraph(&graph);
        expect(fixture.settings.check({}).has_value());
        const auto refused = setOne(fixture.settings, "n_inputs", 1U);
        expect(fatal(rejectedNames(refused, "n_inputs")));
        expect(eq(refused.error().front().reason, std::string("must keep the port inputs#1, which an edge holds")));
        graph.connected = 1UZ;
        expect(setOne(fixture.settings, "n_inputs", 1U).has_value());
        expect(eq(fixture.block.settings.n_inputs, 1U));
        expect(eq(graph.refreshes, 1));
    };

    "a call that fails the check takes no key"_test = [] {
        Fixture<Plain> fixture;
        expect(!fixture.settings.set(property_map{{"ppm", 9.0}, {"rx_gain", 1.0}}).has_value());
        expect(eq(fixture.block.settings.ppm, 0.0));
    };

    "the check hook refuses a changed key"_test = [] {
        Fixture<Plain> fixture;
        fixture.block.refuseGainAbove = 40.f;
        const auto refused            = setOne(fixture.settings, "gain", 45.f);
        expect(fatal(rejectedNames(refused, "gain"))) << "the core makes every entry of the check invalid";
        expect(refused.error().front().reason == "the hook refuses this gain");
        expect(eq(fixture.block.settings.gain.value, 1.f));
    };

    "a key sent at its pending value is no change and runs no check hook"_test = [] {
        Fixture<Plain> fixture;
        FakeGraph      graph;
        fixture.settings.attachGraph(&graph);
        expect(fixture.settings.check({}).has_value());
        expect(setOne(fixture.settings, "gain", 5.f).has_value());
        const int checks              = fixture.block.checkCalls;
        fixture.block.refuseGainAbove = 4.f;
        expect(setOne(fixture.settings, "gain", 5.f).has_value());
        expect(eq(fixture.block.checkCalls, checks));
    };

    "check refuses as set would and stores nothing"_test = [] {
        Fixture<Plain> fixture;
        const auto     refused = fixture.settings.check(property_map{{"gain", 99.f}});
        expect(fatal(!refused.has_value()));
        expect(eq(refused.error().front().key, std::string("gain")));
        expect(fixture.settings.check(property_map{{"gain", 5.f}}).has_value());
        expect(eq(fixture.settings.get<float>("gain").value_or(0.f), 1.f));
        expect(!fixture.settings.isGiven("gain"));
    };

    "a throwing check hook refuses the call with its text, against the key the call changes"_test = [] {
        Fixture<Plain> fixture;
        expect(fixture.settings.check({}).has_value());
        fixture.block.checkThrowText = "the hook lost its device";
        const auto refused           = setOne(fixture.settings, "gain", 5.f);
        expect(fatal(rejectedNames(refused, "gain")));
        expect(eq(refused.error().size(), 1UZ));
        expect(refused.error().front().reason == "the hook lost its device");
        expect(eq(fixture.block.settings.gain.value, 1.f));
    };

    "a lowered count displaces a pending member and its given mark"_test = [] {
        Fixture<Radio> fixture;
        FakeGraph      graph;
        fixture.settings.attachGraph(&graph);
        expect(fixture.settings.check({}).has_value());
        expect(setOne(fixture.settings, "channel.1.frequency", 150e6).has_value());
        expect(fixture.settings.isGiven("channel.1.frequency"));
        expect(fixture.settings.tagEnters(property_map{{"n_channels", 1U}}, false).empty()) << "a tag stages the count";
        expect(!fixture.settings.isGiven("channel.1.frequency")) << "the pending member leaves before any apply";
        expect(gr::testing::applyStagedForTest(fixture.settings).empty());
        expect(std::ranges::find(fixture.block.lastChanged, "channel.1.frequency") == fixture.block.lastChanged.end());
        expect(!fixture.block.describedSettings.contains("channel.1.frequency"));
    };

    "a starting block refuses a changed at-start key and passes one at its value in force"_test = [] {
        Fixture<Plain> fixture;
        expect(fixture.settings.check({}).has_value());
        fixture.settings.markStarting(true);
        const auto refused = setOne(fixture.settings, "burst_size", 32U);
        expect(fatal(rejectedNames(refused, "burst_size")));
        expect(eq(refused.error().front().reason, std::string("must be changed while the graph is stopped")));
        expect(setOne(fixture.settings, "burst_size", 16U).has_value());
        fixture.settings.markStarting(false);
        expect(setOne(fixture.settings, "burst_size", 32U).has_value());
        expect(eq(fixture.block.settings.burst_size, 32U));
    };

    "a misspelled constructed key refuses the first call"_test = [] {
        Fixture<Plain> fixture(property_map{{"aa", 1}});
        const auto     refused = setOne(fixture.settings, "b", 2);
        expect(rejectedNames(refused, "aa"));
        expect(eq(fixture.block.applyCalls, 0));
    };

    "a faulty description refuses the first call"_test = [] {
        Fixture<Faulty> fixture;
        const auto      refused = setOne(fixture.settings, "gain", 1.0);
        expect(fatal(!refused.has_value()));
        expect(eq(refused.error().size(), 1UZ));
        expect(refused.error().front().key.empty());
        expect(refused.error().front().reason.contains("minimun")) << refused.error().front().reason;
        expect(!fixture.settings.check({}).has_value()) << "the next call refuses again";
    };
};

const suite<"BlockSettings: the apply"> applyTests = [] {
    "a block no graph holds applies at the call"_test = [] {
        Fixture<Plain> fixture(property_map{{"a", 5}});
        const auto     result = setOne(fixture.settings, "b", 7);
        expect(fatal(result.has_value()));
        expect(result->empty()) << "every key is in force as sent";
        expect(eq(fixture.block.settings.a, 5));
        expect(eq(fixture.block.settings.b, 7));
        expect(eq(fixture.block.applyCalls, 2)) << "INIT and the call";
    };

    "at INIT the hook's old holds the declared defaults, so a constructed value differs from it and a constructed default does not"_test = [] {
        Fixture<Plain> fixture(property_map{{"a", 5}, {"b", 0}});
        expect(fixture.settings.check({}).has_value());
        expect(eq(fixture.block.applyCalls, 1));
        expect(fixture.block.lastChanged == std::vector<std::string>{"a"});
        expect(eq(fixture.block.oldPpm, 0.0));
        expect(setOne(fixture.settings, "ppm", 1.0).has_value());
        expect(fixture.block.lastChanged == std::vector<std::string>{"ppm"});
        expect(eq(fixture.block.oldPpm, 0.0)) << "old holds the value before the call";
        expect(setOne(fixture.settings, "ppm", 2.0).has_value());
        expect(eq(fixture.block.oldPpm, 1.0));
    };

    "a block built with no values runs the check of the whole state at INIT, so a default its rule refuses fails it"_test = [] {
        Fixture<Plain> fixture;
        fixture.block.refuseGainAbove                   = 0.5f;
        const std::expected<void, Rejected> initialized = fixture.settings.check({});
        expect(fatal(!initialized.has_value()));
        expect(std::ranges::any_of(initialized.error(), [](const Rejection& entry) { return entry.key == "gain"; }));
        expect(eq(fixture.block.applyCalls, 0)) << "a refused initialization applies nothing";
    };

    "with no constructed value the hook still runs once at INIT, with old equal to the settings"_test = [] {
        Fixture<Plain> fixture;
        expect(fixture.settings.check({}).has_value());
        expect(eq(fixture.block.applyCalls, 1));
        expect(fixture.block.lastChanged.empty());
    };

    "a map get returned passes and changes nothing"_test = [] {
        Fixture<Plain> fixture(property_map{{"a", 5}});
        expect(fixture.settings.check({}).has_value());
        const auto read = fixture.settings.get();
        expect(fatal(read.has_value()));
        expect(read->contains("level")) << "a status key reads";
        expect(fixture.settings.set(gr::testing::writableValues(fixture.settings, read)).has_value());
        expect(eq(fixture.block.applyCalls, 1));
        expect(eq(fixture.block.settings.a, 5));
    };

    "two calls before one apply apply together"_test = [] {
        Fixture<Plain> fixture;
        FakeGraph      graph;
        fixture.settings.attachGraph(&graph);
        expect(fixture.settings.check({}).has_value());
        expect(setOne(fixture.settings, "a", 1).has_value());
        expect(setOne(fixture.settings, "b", 2).has_value());
        expect(eq(fixture.block.applyCalls, 1)) << "a stopped block a graph holds keeps the calls pending";
        expect(gr::testing::applyStagedForTest(fixture.settings).empty());
        expect(eq(fixture.block.applyCalls, 2));
        expect(fixture.block.lastChanged == std::vector<std::string>{"a", "b"});
    };

    "a key the hook fails takes back the call's other keys, the value the hook assigned among them, each failed naming it"_test = [] {
        Fixture<Plain> fixture;
        expect(fixture.settings.check({}).has_value());
        fixture.block.failKey   = "ppm";
        fixture.block.roundGain = true;
        const auto result       = fixture.settings.set(property_map{{"gain", 2.6f}, {"ppm", 3.0}});
        expect(fatal(result.has_value())) << "a failed key leaves the call's result whole";
        expect(eq(failedReason(result, "ppm").value_or(""), std::string("the device refuses it")));
        expect(eq(failedReason(result, "gain").value_or(""), std::string("ppm: the device refuses it"))) << gr::testing::textOf(result);
        expect(eq(fixture.block.settings.gain.value, 1.f)) << "the field takes back its value before the batch, not the one the hook assigned";
        expect(eq(fixture.settings.get<float>("gain").value_or(0.f), 1.f));
    };

    "a throwing hook fails every key the call changed"_test = [] {
        Fixture<Plain> fixture;
        expect(fixture.settings.check({}).has_value());
        fixture.block.throwText = "the device is gone";
        const auto result       = fixture.settings.set(property_map{{"gain", 5.f}, {"ppm", 3.0}});
        expect(fatal(result.has_value()));
        expect(eq(result->size(), 2UZ));
        expect(std::ranges::all_of(*result, [](const auto& entry) { return entry.second.isFailed() && entry.second.reason == "the device is gone"; }));
        expect(eq(fixture.block.settings.ppm, 0.0));
    };

    "a throw fails the changed keys alone, and a key sent at its value stays given and saved"_test = [] {
        InfoScheduler  scheduler;
        Fixture<Plain> fixture;
        fixture.settings.attachScheduler(&scheduler);
        expect(fixture.settings.check({}).has_value());
        expect(setOne(fixture.settings, "gain", 2.f).has_value());
        fixture.block.throwText = "the device is not open";
        CerrCapture captured;
        const auto  result = fixture.settings.set(property_map{{"gain", 2.f}, {"ppm", 3.0}});
        expect(fatal(result.has_value()));
        expect(eq(failedReason(result, "ppm").value_or(""), std::string("the device is not open")));
        expect(!result->contains("gain")) << "the key sent at its value is in force as sent";
        expect(eq(fixture.block.settings.gain.value, 2.f));
        expect(eq(fixture.block.settings.ppm, 0.0));
        expect(eq(captured.lines("'gain'"), 0UZ)) << captured.str();
        expect(eq(captured.lines("'ppm'"), 1UZ)) << captured.str();
        expect(fixture.settings.isGiven("gain"));
        expect(!fixture.settings.isGiven("ppm"));
        const property_map saved = fixture.settings.savedValues();
        expect(saved.contains("gain") && saved.at("gain") == PmtValue(2.f)) << "a saved graph keeps the gain";
        expect(!saved.contains("ppm"));
    };

    "a key the hook fails with no value takes back every key of its batch, unrecorded and announced once each, as after a throw"_test = [] {
        InfoScheduler  scheduler;
        Fixture<Plain> fixture;
        fixture.settings.attachScheduler(&scheduler);
        expect(fixture.settings.check({}).has_value());
        fixture.block.failKey = "ppm";
        CerrCapture captured;
        const auto  result = fixture.settings.set(property_map{{"ppm", 7.0}, {"a", 3}});
        expect(eq(failedReason(result, "ppm").value_or(""), std::string("the device refuses it")));
        expect(eq(failedReason(result, "a").value_or(""), std::string("ppm: the device refuses it"))) << gr::testing::textOf(result);
        expect(eq(fixture.block.settings.ppm, 0.0)) << "the field takes back its earlier value";
        expect(eq(fixture.block.settings.a, 0)) << "the batch's other field takes back its earlier value";
        expect(eq(fixture.settings.get<double>("ppm").value_or(-1.0), 0.0)) << "in force takes back its earlier value";
        expect(eq(fixture.settings.get<std::int32_t>("a").value_or(-1), 0));
        expect(eq(captured.lines("'ppm'"), 1UZ)) << captured.str();
        expect(eq(captured.lines("failed 'ppm'"), 1UZ)) << captured.str();
        expect(eq(captured.lines("failed 'a': ppm: the device refuses it"), 1UZ)) << captured.str();
        expect(eq(captured.lines("changed"), 0UZ)) << captured.str();
        expect(!fixture.settings.isGiven("ppm"));
        expect(!fixture.settings.isGiven("a"));
        const property_map saved = fixture.settings.savedValues();
        expect(!saved.contains("ppm")) << "a saved graph does not keep the failed value";
        expect(!saved.contains("a")) << "a saved graph does not keep the batch's other key";
    };

    "a key the hook fails takes back a rule's other key a second call sent in the same batch"_test = [] {
        InfoScheduler   scheduler;
        Fixture<Bounds> fixture;
        fixture.settings.attachScheduler(&scheduler);
        expect(fatal(fixture.settings.check({}).has_value()));
        expect(fatal(setOne(fixture.settings, "max", 10).has_value())) << "max is given at its value in force";
        fixture.settings.moveState(BlockState::running);
        const int                       appliesBefore = fixture.block.applyCalls;
        CerrCapture                     captured;
        std::expected<Values, Rejected> raised;
        std::expected<Values, Rejected> lowered;
        std::thread                     first([&] { raised = setOne(fixture.settings, "max", 20, kLongWait); });
        const bool                      firstWaits = gr::testing::waitUntil([&] { return gr::settings::detail::FrameworkAccess::hasWork(fixture.settings); });
        std::thread                     second([&] { lowered = setOne(fixture.settings, "min", 15, kLongWait); });
        const bool                      secondWaits = gr::testing::waitUntil([&] { return fixture.settings.isGiven("min"); });
        std::ignore                                 = gr::testing::applyStagedForTest(fixture.settings);
        first.join();
        second.join();
        expect(fatal(firstWaits && secondWaits)) << "both calls wait for one apply";
        expect(eq(fixture.block.applyCalls, appliesBefore + 1)) << "one batch";
        expect(eq(failedReason(raised, "max").value_or(""), std::string("runs 10 at most"))) << gr::testing::textOf(raised);
        expect(eq(failedReason(lowered, "min").value_or(""), std::string("max: runs 10 at most"))) << gr::testing::textOf(lowered);
        expect(eq(fixture.block.settings.min, 0));
        expect(eq(fixture.block.settings.max, 10));
        expect(eq(fixture.settings.get<std::int32_t>("min").value_or(-1), 0));
        expect(eq(fixture.settings.get<std::int32_t>("max").value_or(-1), 10));
        expect(eq(captured.lines("failed 'max': runs 10 at most"), 1UZ)) << captured.str();
        expect(eq(captured.lines("failed 'min': max: runs 10 at most"), 1UZ)) << captured.str();
        expect(eq(captured.lines("changed"), 0UZ)) << captured.str();
        const property_map saved = fixture.settings.savedValues();
        expect(saved.contains("max") && saved.at("max") == PmtValue(std::int32_t{10})) << "a save writes the max given before the batch";
        expect(!saved.contains("min"));

        fixture.settings.moveState(BlockState::stopped);
        expect(gr::settings::detail::FrameworkAccess::checkStart(fixture.settings).empty()) << "the next start finds the rule held";
        expect(rejectedNames(setOne(fixture.settings, "min", 15), "min")) << "a later call is judged against the max in force";
        const auto later = setOne(fixture.settings, "min", 5);
        expect(fatal(later.has_value()));
        expect(later->empty()) << gr::testing::textOf(later);
        expect(eq(fixture.settings.get<std::int32_t>("min").value_or(-1), 5));
    };

    "a setting or status value the hook fails outside its batch takes back the key the call changed, failed under that name"_test = [] {
        for (const std::string otherKey : {"ppm", "level"}) {
            InfoScheduler  scheduler;
            Fixture<Plain> fixture;
            fixture.settings.attachScheduler(&scheduler);
            expect(fixture.settings.check({}).has_value());
            fixture.block.failKey  = otherKey;
            fixture.block.failWhen = "gain";
            CerrCapture       captured;
            const auto        result = setOne(fixture.settings, "gain", 5.f);
            const std::string reason = std::format("{}: the device refuses it", otherKey);
            expect(fatal(result.has_value())) << otherKey;
            expect(eq(result->size(), 1UZ)) << gr::testing::textOf(result);
            expect(eq(failedReason(result, "gain").value_or(""), reason));
            expect(eq(fixture.block.settings.gain.value, 1.f)) << "the field takes back its earlier value";
            expect(eq(fixture.settings.get<float>("gain").value_or(0.f), 1.f)) << "in force takes back its earlier value";
            expect(eq(captured.lines(std::format("failed 'gain': {}", reason)), 1UZ)) << captured.str();
            expect(eq(captured.lines(std::format("'{}'", otherKey)), 0UZ)) << captured.str();
            expect(eq(captured.lines("changed 'gain'"), 0UZ)) << captured.str();
            expect(!fixture.settings.isGiven("gain"));
            expect(!fixture.settings.savedValues().contains("gain"));

            fixture.block.failKey.clear();
            const auto again = setOne(fixture.settings, "gain", 5.f);
            expect(fatal(again.has_value()));
            expect(again->empty()) << "a second call takes: " << gr::testing::textOf(again);
            expect(eq(fixture.settings.get<float>("gain").value_or(0.f), 5.f));
        }
    };

    "a key the hook fails outside its batch takes back every key the call changed, each failed under that name"_test = [] {
        InfoScheduler  scheduler;
        Fixture<Plain> fixture;
        fixture.settings.attachScheduler(&scheduler);
        expect(fixture.settings.check({}).has_value());
        fixture.block.failKey  = "ppm";
        fixture.block.failWhen = "a";
        CerrCapture captured;
        const auto  result = fixture.settings.set(property_map{{"gain", 5.f}, {"a", 3}});
        expect(fatal(result.has_value()));
        expect(eq(result->size(), 2UZ)) << gr::testing::textOf(result);
        expect(eq(failedReason(result, "gain").value_or(""), std::string("ppm: the device refuses it")));
        expect(eq(failedReason(result, "a").value_or(""), std::string("ppm: the device refuses it")));
        expect(eq(fixture.settings.get<float>("gain").value_or(0.f), 1.f));
        expect(eq(fixture.settings.get<std::int32_t>("a").value_or(-1), 0));
        expect(eq(captured.lines("failed 'gain': ppm: the device refuses it"), 1UZ)) << captured.str();
        expect(eq(captured.lines("failed 'a': ppm: the device refuses it"), 1UZ)) << captured.str();
        expect(eq(captured.lines("changed"), 0UZ)) << captured.str();
        expect(eq(fixture.settings.get<double>("ppm").value_or(-1.0), 0.0));
    };

    "at the setup every key is in the batch, so a key the hook fails refuses the block under its own name"_test = [] {
        Fixture<Plain> fixture(property_map{{"a", 5}});
        fixture.block.failKey  = "gain";
        fixture.block.failWhen = "a";
        const auto refused     = setOne(fixture.settings, "b", 1);
        expect(fatal(rejectedNames(refused, "gain")));
        expect(eq(refused.error().size(), 1UZ));
        expect(eq(refused.error().front().reason, std::string("the device refuses it")));
        fixture.block.failKey.clear();
        expect(setOne(fixture.settings, "b", 1).has_value());
        expect(eq(fixture.block.settings.a, 5));
        expect(eq(fixture.block.settings.b, 1));
    };

    "a key the hook fails from another thread fails after the apply, and the call's key takes"_test = [] {
        InfoScheduler  scheduler;
        Fixture<Plain> fixture;
        fixture.settings.attachScheduler(&scheduler);
        expect(fixture.settings.check({}).has_value());
        fixture.block.failKey             = "ppm";
        fixture.block.failWhen            = "gain";
        fixture.block.failOnAnotherThread = true;
        CerrCapture captured;
        const auto  result = setOne(fixture.settings, "gain", 5.f);
        expect(fatal(result.has_value()));
        expect(result->empty()) << gr::testing::textOf(result);
        expect(eq(fixture.settings.get<float>("gain").value_or(0.f), 5.f));
        expect(eq(captured.lines("failed 'ppm': the device refuses it"), 1UZ)) << captured.str();
        expect(eq(captured.lines("failed 'gain'"), 0UZ)) << captured.str();
    };

    "a value the hook assigns outside the key's limits fails the key, which takes back its earlier value"_test = [] {
        Fixture<Plain> fixture;
        expect(fixture.settings.check({}).has_value());
        fixture.block.assignGain = 60.f;
        const auto        result = setOne(fixture.settings, "gain", 5.f);
        const std::string reason = failedReason(result, "gain").value_or("");
        expect(reason.contains("the block assigned 60") && reason.contains("must be")) << reason;
        expect(eq(fixture.block.settings.gain.value, 1.f));
        expect(eq(fixture.settings.get<float>("gain").value_or(0.f), 1.f));
        expect(!fixture.settings.savedValues().contains("gain"));

        fixture.block.assignGain = 40.f;
        const auto inside        = setOne(fixture.settings, "gain", 5.f);
        expect(fatal(inside.has_value()));
        expect(inside->contains("gain") && inside->at("gain").isValue()) << "a value inside the limits is the one a host reads";
        expect(eq(fixture.settings.get<float>("gain").value_or(0.f), 40.f));
    };

    "a value the hook assigns outside the key's limits takes back the batch's other key, failed naming it"_test = [] {
        InfoScheduler  scheduler;
        Fixture<Plain> fixture;
        fixture.settings.attachScheduler(&scheduler);
        expect(fixture.settings.check({}).has_value());
        fixture.block.assignGain = 60.f;
        CerrCapture       captured;
        const auto        result = fixture.settings.set(property_map{{"gain", 5.f}, {"ppm", 3.0}});
        const std::string reason = failedReason(result, "gain").value_or("");
        expect(reason.starts_with("the block assigned 60") && reason.contains("must be")) << reason;
        expect(eq(failedReason(result, "ppm").value_or(""), std::format("gain: {}", reason))) << gr::testing::textOf(result);
        expect(eq(fixture.block.settings.gain.value, 1.f));
        expect(eq(fixture.block.settings.ppm, 0.0));
        expect(eq(fixture.settings.get<double>("ppm").value_or(-1.0), 0.0));
        expect(eq(captured.lines("failed 'gain': the block assigned 60"), 1UZ)) << captured.str();
        expect(eq(captured.lines("failed 'ppm': gain: the block assigned 60"), 1UZ)) << captured.str();
        expect(eq(captured.lines("changed"), 0UZ)) << captured.str();
        expect(!fixture.settings.savedValues().contains("ppm"));
    };

    "a value the hook assigns outside the limits of a key outside its batch takes back the key sent, failed naming it"_test = [] {
        InfoScheduler  scheduler;
        Fixture<Plain> fixture;
        fixture.settings.attachScheduler(&scheduler);
        expect(fixture.settings.check({}).has_value());
        fixture.block.assignGain     = -2.f;
        fixture.block.assignGainWhen = "ppm";
        CerrCapture captured;
        const auto  result = setOne(fixture.settings, "ppm", 3.0);
        expect(fatal(result.has_value()));
        expect(eq(result->size(), 1UZ)) << gr::testing::textOf(result);
        const std::string reason = failedReason(result, "ppm").value_or("");
        expect(reason.starts_with("gain: the block assigned -2") && reason.contains("must be")) << reason;
        expect(eq(fixture.block.settings.gain.value, 1.f));
        expect(eq(fixture.settings.get<float>("gain").value_or(0.f), 1.f));
        expect(eq(fixture.settings.get<double>("ppm").value_or(-1.0), 0.0));
        expect(eq(captured.lines("failed 'ppm': gain: the block assigned -2"), 1UZ)) << captured.str();
        expect(eq(captured.lines("'gain'"), 0UZ)) << captured.str();
    };

    "a map setting that holds not-a-number is the same as itself at each apply"_test = [] {
        Fixture<Recorded> fixture(property_map{{"records", property_map{{"x", std::numeric_limits<double>::quiet_NaN()}, {"y", 1.0}}}});
        expect(fixture.settings.check({}).has_value());
        CerrCapture captured;
        expect(setOne(fixture.settings, "a", 1).has_value());
        expect(setOne(fixture.settings, "a", 2).has_value());
        expect(eq(captured.lines("outside applySettings()"), 0UZ)) << captured.str();
        expect(setOne(fixture.settings, "records", property_map{{"x", std::numeric_limits<double>::quiet_NaN()}, {"y", 1.0}}).value_or(Values{}).empty()) << "the map sent at its own value is in force as sent";
    };

    "a failed count keeps its members, and a failed higher count adds none"_test = [] {
        Fixture<Radio> fixture;
        expect(fixture.settings.check({}).has_value());
        fixture.block.failKey = "n_channels";
        expect(failedNames(setOne(fixture.settings, "n_channels", 1U), "n_channels"));
        expect(eq(fixture.settings.get<double>("channel.1.frequency").value_or(0.0), 100e6));
        expect(failedNames(setOne(fixture.settings, "n_channels", 3U), "n_channels"));
        expect(!fixture.block.describedSettings.contains("channel.2.frequency"));
        expect(!fixture.settings.get("channel.2.frequency").has_value());
    };

    "a lowered count drops its members, and raised again they take their defaults"_test = [] {
        Fixture<Radio> fixture;
        expect(setOne(fixture.settings, "channel.1.frequency", 150e6).has_value());
        expect(fixture.settings.isGiven("channel.1.frequency"));
        expect(setOne(fixture.settings, "n_channels", 1U).has_value());
        expect(!fixture.settings.isGiven("channel.1.frequency"));
        const auto all = fixture.settings.get();
        expect(fatal(all.has_value()));
        expect(std::ranges::none_of(*all, [](const auto& entry) { return entry.first.starts_with("channel.1."); }));
        expect(setOne(fixture.settings, "n_channels", 2U).has_value());
        expect(eq(fixture.settings.get<double>("channel.1.frequency").value_or(0.0), 100e6));
    };

    "a key with no default is given once a caller sets it"_test = [] {
        Fixture<Radio> fixture;
        FakeGraph      graph;
        fixture.settings.attachGraph(&graph);
        expect(fixture.settings.check({}).has_value());
        expect(!fixture.settings.isGiven("channel.0.lna"));
        expect(setOne(fixture.settings, "channel.0.lna", 16.0).has_value());
        expect(fixture.settings.isGiven("channel.0.lna")) << "a key waiting for the start is given";
        expect(!fixture.settings.isGiven("burst_size"));
        expect(gr::testing::applyStagedForTest(fixture.settings).empty());
        expect(eq(fixture.settings.get<double>("channel.0.lna").value_or(0.0), 16.0));
    };

    "an apply notifies each key it changed at info, with the reason"_test = [] {
        InfoScheduler  scheduler;
        Fixture<Plain> fixture;
        fixture.settings.attachScheduler(&scheduler);
        fixture.block.roundGain = true;
        fixture.block.deriveB   = true;
        expect(fixture.settings.check({}).has_value());
        CerrCapture captured;
        expect(fixture.settings.set(property_map{{"gain", 2.4f}, {"ppm", 3.0}, {"a", 4}}).has_value());
        expect(eq(captured.lines("changed 'gain': a call set the value"), 1UZ)) << "a key of the batch the hook assigned: " << captured.str();
        expect(eq(captured.lines("changed 'ppm': a call set the value"), 1UZ));
        expect(eq(captured.lines("changed 'a': a call set the value"), 1UZ));
        expect(eq(captured.lines("'b'"), 0UZ)) << "a setting the hook assigns is notified to no one";
        expect(eq(fixture.settings.get<std::int32_t>("b").value_or(0), 8)) << "a read returns the value the hook assigned";
        expect(eq(captured.lines("plain#0"), 3UZ));
    };

    "a value the hook assigns to a key of its batch comes back in place of the value sent"_test = [] {
        Fixture<Plain> fixture;
        fixture.block.roundGain = true;
        const auto result       = setOne(fixture.settings, "gain", 2.4f);
        expect(fatal(result.has_value()));
        const Entry& entry = result->at("gain");
        expect(entry.isValue());
        expect(eq(entry.as<float>().value_or(0.f), 2.f));
        expect(eq(fixture.settings.get<float>("gain").value_or(0.f), 2.f));
    };

    "a publish during the apply changes no key and notifies one failure for each, telling the block to assign"_test = [] {
        InfoScheduler  scheduler;
        Fixture<Plain> fixture;
        fixture.settings.attachScheduler(&scheduler);
        fixture.block.publishInHook = true;
        expect(fixture.settings.check({}).has_value());
        CerrCapture captured;
        const auto  result = setOne(fixture.settings, "gain", 3.f);
        expect(fatal(result.has_value()));
        expect(result->empty()) << "the gain sent is in force: " << gr::testing::textOf(result);
        expect(eq(fixture.settings.get<float>("gain").value_or(0.f), 3.f)) << "a key of the batch keeps the value sent";
        expect(eq(fixture.settings.get<double>("ppm").value_or(-1.0), 0.0)) << "a key outside the batch keeps its value";
        expect(fixture.settings.get("gain").value().reason.empty());
        const std::string_view refusal = "publish changes nothing during the apply: assign the setting in applySettings()";
        expect(eq(captured.lines(std::format("failed 'gain': {}", refusal)), 1UZ)) << captured.str();
        expect(eq(captured.lines(std::format("failed 'ppm': {}", refusal)), 1UZ)) << captured.str();
        expect(eq(captured.lines("the device runs 7"), 0UZ)) << captured.str();
        expect(eq(captured.lines("changed 'gain': a call set the value"), 1UZ)) << captured.str();
    };

    "a write at the value sent differs from the value the hook assigned, so it reaches the check and the hook"_test = [] {
        Fixture<Plain> fixture;
        fixture.block.roundGain = true;
        expect(fixture.settings.check({}).has_value());
        for (int sent = 1; sent <= 2; ++sent) {
            const int  applied = fixture.block.applyCalls;
            const int  checked = fixture.block.checkCalls;
            const auto result  = setOne(fixture.settings, "gain", 2.4f);
            expect(fatal(result.has_value()));
            expect(eq(fixture.block.checkCalls, checked + 1)) << std::format("set {}: the check ran", sent);
            expect(eq(fixture.block.applyCalls, applied + 1)) << std::format("set {}: the hook ran", sent);
            expect(eq(fixture.settings.get<float>("gain").value_or(0.f), 2.f)) << std::format("set {}: a read returns the value assigned", sent);
        }
        const int applied = fixture.block.applyCalls;
        expect(setOne(fixture.settings, "gain", 2.f).has_value());
        expect(eq(fixture.block.applyCalls, applied)) << "a write at the value assigned changes nothing";
        expect(eq(fixture.settings.get<float>("gain").value_or(0.f), 2.f));
    };

    "the forwarded settings the hook names wait for the next output"_test = [] {
        Fixture<Plain> fixture;
        fixture.block.forwardGain = true;
        expect(setOne(fixture.settings, "gain", 3.f).has_value());
        const property_map forwarded = fixture.settings.takeForwarded();
        expect(fatal(forwarded.contains("gain")));
        expect(forwarded.at("gain") == PmtValue(3.f));
        expect(fixture.settings.takeForwarded().empty());
    };

    "the hook's forwards at INIT are dropped, and a later change forwards"_test = [] {
        Fixture<Plain> fixture(property_map{{"gain", 2.f}});
        fixture.block.forwardGain = true;
        expect(gr::testing::applyStagedForTest(fixture.settings).empty());
        expect(fatal(std::ranges::contains(fixture.block.lastChanged, std::string("gain")))) << "INIT showed the hook no changed gain";
        expect(fixture.settings.takeForwarded().empty()) << "the value the block was built with went downstream";

        expect(setOne(fixture.settings, "gain", 3.f).has_value());
        const property_map forwarded = fixture.settings.takeForwarded();
        expect(fatal(forwarded.contains("gain"))) << "a later change forwarded nothing";
        expect(forwarded.at("gain") == PmtValue(3.f));
    };

    "a failed apply at INIT fails the first call, and the next call runs INIT again"_test = [] {
        Fixture<Plain> fixture(property_map{{"a", 5}});
        fixture.block.failKey = "a";
        expect(rejectedNames(setOne(fixture.settings, "b", 1), "a")) << "a failed INIT refuses the call, whose own keys take no effect";
        expect(eq(fixture.block.settings.a, 0));
        fixture.block.failKey.clear();
        expect(setOne(fixture.settings, "b", 1).has_value());
        expect(eq(fixture.block.settings.a, 5));
    };

    "a work call on a fresh block alone runs INIT"_test = [] {
        Fixture<Plain> fixture(property_map{{"a", 5}});
        expect(gr::testing::applyStagedForTest(fixture.settings).empty());
        expect(eq(fixture.block.settings.a, 5));
        expect(eq(fixture.block.applyCalls, 1));
        expect(std::ranges::contains(fixture.block.lastChanged, std::string("a")));
    };

    "a failed INIT leaves a block alone due for its first use"_test = [] {
        Fixture<Plain> fixture(property_map{{"a", 5}});
        fixture.block.failKey = "a";
        expect(fatal(eq(gr::testing::applyStagedForTest(fixture.settings).size(), 1UZ)));
        expect(eq(fixture.block.applyCalls, 1));
        const Rejected again = gr::testing::applyStagedForTest(fixture.settings);
        expect(fatal(eq(again.size(), 1UZ))) << "the next work call runs INIT again";
        expect(again.front().key == "a");
        expect(eq(fixture.block.applyCalls, 2));
        fixture.block.failKey.clear();
        expect(gr::testing::applyStagedForTest(fixture.settings).empty());
        expect(eq(fixture.block.settings.a, 5));
    };

    "a settings hook's set and resetDefaults on its own block are refused whole, and its get reads"_test = [] {
        for (const bool atInit : {true, false}) {
            Fixture<Plain> fixture(atInit ? property_map{{"a", 5}} : property_map{});
            fixture.block.stageBWhenA = atInit;
            expect(fixture.settings.check({}).has_value());
            if (!atInit) {
                fixture.block.stageBWhenA = true;
                expect(setOne(fixture.settings, "a", 5).has_value());
            }
            expect(fatal(eq(fixture.block.ownCalls.size(), 3UZ))) << atInit;
            for (const std::expected<Values, Rejected>& call : fixture.block.ownCalls) {
                expect(fatal(!call.has_value())) << "a block's own call was taken" << atInit;
                expect(call.error().front().reason.contains("applySettings")) << call.error().front().reason;
                expect(call.error().front().reason.contains("applySettings")) << call.error().front().reason;
            }
            expect(fixture.block.ownRead.has_value() && fixture.block.ownRead->value_or(0) == 5) << "get on the own thread reads";
            expect(eq(fixture.block.settings.b, 0));
            expect(!fixture.settings.isGiven("b")) << "nothing of the refused call stays";
            expect(gr::testing::applyStagedForTest(fixture.settings).empty());
            expect(eq(fixture.block.settings.b, 0));
        }
    };

    "a call on another block's settings from a hook is no own call"_test = [] {
        Fixture<Plain> other;
        Fixture<Plain> fixture;
        expect(fixture.settings.check({}).has_value());
        const gr::settings::detail::OwnThread ownThread(fixture.settings);
        expect(setOne(other.settings, "b", 1).has_value());
        expect(eq(other.block.settings.b, 1));
        expect(!setOne(fixture.settings, "b", 1).has_value()) << "the marked block's own call";
    };

    "a throwing reset hook is notified and the reset goes on"_test = [] {
        Fixture<Plain> fixture;
        expect(setOne(fixture.settings, "gain", 5.f).has_value());
        fixture.block.resetThrowText = "the device did not reset";
        CerrCapture captured;
        expect(fixture.settings.resetDefaults().has_value());
        expect(eq(captured.lines("error: plain#0 failed"), 1UZ)) << captured.str();
        expect(eq(captured.lines("the device did not reset"), 1UZ));
        expect(eq(fixture.block.settings.gain.value, 1.f));
    };

    "a publish notifies a change at info"_test = [] {
        InfoScheduler  scheduler;
        Fixture<Plain> fixture;
        fixture.settings.attachScheduler(&scheduler);
        expect(fixture.settings.check({}).has_value());
        CerrCapture captured;
        fixture.settings.publish("level", PmtValue(3.f));
        expect(eq(captured.lines("changed 'level': the block published the value"), 1UZ)) << captured.str();
        expect(eq(fixture.settings.get<float>("level").value_or(0.f), 3.f));
    };

    "a publish on the block's own thread copies no map while no check shares it"_test = [] {
        Fixture<Plain> fixture;
        expect(fixture.settings.check({}).has_value());
        const gr::settings::detail::OwnThread ownThread(fixture.settings);
        fixture.settings.publish("level", PmtValue(3.f));
        gAllocations     = 0UZ;
        countAllocations = true;
        fixture.settings.publish("level", PmtValue(4.f));
        countAllocations = false;
        expect(eq(gAllocations.load(), 0UZ));
        expect(eq(fixture.settings.get<float>("level").value_or(0.f), 4.f));
    };

    "a scheduler's key declared at the call applies at the call while it runs"_test = [] {
        Fixture<Tuner> fixture;
        expect(fixture.settings.check({}).has_value());
        fixture.settings.moveState(BlockState::running);
        const auto result = setOne(fixture.settings, "frequency", 101e6, gr::settings::noWait);
        expect(fatal(result.has_value()));
        expect(result->empty()) << "in force as sent";
        expect(eq(fixture.block.settings.frequency, 101e6));
        expect(eq(fixture.block.applyCalls, 2));
    };

    "an apply with nothing pending allocates nothing"_test = [] {
        Fixture<Plain> fixture;
        expect(fixture.settings.check({}).has_value());
        expect(gr::testing::applyStagedForTest(fixture.settings).empty());
        gAllocations     = 0UZ;
        countAllocations = true;
        for (int pass = 0; pass < 100; ++pass) {
            std::ignore = gr::testing::applyStagedForTest(fixture.settings);
        }
        countAllocations = false;
        expect(eq(gAllocations.load(), 0UZ));
    };

    "a block alone with no level prints each failure on its own line"_test = [] {
        Fixture<Plain> fixture;
        expect(fixture.settings.check({}).has_value());
        fixture.block.failKey = "gain";
        CerrCapture captured;
        for (int value = 2; value < 12; ++value) {
            std::ignore = setOne(fixture.settings, "gain", static_cast<float>(value));
        }
        expect(eq(captured.lines("error: plain#0 failed 'gain': the device refuses it"), 10UZ)) << captured.str();
    };

    "the core holds no rule for two keys of one value"_test = [] {
        Fixture<Plain> fixture;
        expect(fixture.settings.set(property_map{{"a", 3}, {"b", 6}}).has_value());
        expect(fixture.settings.isGiven("a") && fixture.settings.isGiven("b"));
    };

    "a control thread publishes while it holds the lock the settings hook waits for"_test = [] {
        Fixture<Guarded> fixture;
        expect(fatal(fixture.settings.check({}).has_value()));
        fixture.block.waitInHook = true;
        std::unique_lock held(fixture.block.device);
        std::thread      caller([&fixture] { std::ignore = setOne(fixture.settings, "a", 3); });
        fixture.block.inHook.wait(false);
        fixture.settings.publish("gain", PmtValue(2.5f)); // returns while the hook waits for the lock this thread holds
        expect(eq(fixture.settings.get<float>("gain").value_or(0.f), 2.5f)) << "a read takes the published value at the call";
        held.unlock();
        caller.join();
        expect(eq(fixture.block.settings.a, 3));
        expect(eq(fixture.block.settings.gain, 1.f)) << "the field keeps the value set";
        expect(eq(fixture.settings.get<float>("gain").value_or(0.f), 2.5f)) << "the apply of another key loses the published value";
    };

    "a save holds the keys a caller gave, not a derived or a published value"_test = [] {
        Fixture<Plain> fixture;
        expect(fatal(fixture.settings.check({}).has_value()));
        fixture.block.deriveB = true;
        expect(fatal(setOne(fixture.settings, "a", 3).has_value()));
        std::thread device([&fixture] { fixture.settings.publish("ppm", PmtValue(2.0)); });
        device.join();
        expect(fatal(eq(fixture.settings.get<std::int32_t>("b").value_or(0), 6))) << "the hook derives no value";
        expect(fatal(eq(fixture.settings.get<double>("ppm").value_or(0.0), 2.0))) << "the published value is not in force";
        const property_map saved = fixture.settings.savedValues();
        expect(saved.contains("a")) << "a save leaves out a key a caller gave";
        expect(!saved.contains("b")) << "a save holds a value the hook derived";
        expect(!saved.contains("ppm")) << "a save holds a value the device published";
    };
};

const suite<"BlockSettings: waiting calls"> waitTests = [] {
    "noWait and a negative timeout answer pending on a running block until its next work call, in every form"_test = [] {
        Fixture<Plain> fixture;
        expect(fixture.settings.check({}).has_value());
        fixture.settings.moveState(BlockState::running);
        const Values                                           read{{"gain", Entry{.value = PmtValue(5.f)}}};
        const std::array<std::expected<Values, Rejected>, 3UZ> results{
            fixture.settings.set(property_map{{"gain", 5.f}}, gr::settings::noWait),
            fixture.settings.set(gr::settings::heldValues(read), gr::settings::noWait),
            fixture.settings.set("gain", PmtValue(5.f), -1ms),
        };
        for (const std::expected<Values, Rejected>& result : results) {
            expect(fatal(result.has_value()));
            expect(result->at("gain") == Entry{.state = Entry::State::pending, .value = {}, .reason = std::string(gr::settings::kPendingOnWorker)}) << "a call that does not wait names no timeout";
        }
    };

    "a deadline ends a wait with timeout, and a stop with not running"_test = [] {
        Fixture<Plain> fixture;
        expect(fixture.settings.check({}).has_value());
        fixture.settings.moveState(BlockState::running);
        const std::expected<Values, Rejected> timedOut = setOne(fixture.settings, "gain", 3.f, 20ms); // no work call applies it
        expect(fatal(timedOut.has_value()));
        expect(timedOut->at("gain").reason == gr::settings::kPendingTimeout);
        applyUntilHookCalls(fixture, 2);
        expect(fatal(!gr::settings::detail::FrameworkAccess::hasWork(fixture.settings)));
        std::expected<Values, Rejected> stopped;
        std::thread                     second([&] { stopped = setOne(fixture.settings, "gain", 4.f, kLongWait); });
        while (!gr::settings::detail::FrameworkAccess::hasWork(fixture.settings)) { // the call merged its key, and waits
            std::this_thread::yield();
        }
        fixture.settings.moveState(BlockState::stopped);
        second.join();
        expect(fatal(stopped.has_value()));
        expect(stopped->at("gain").reason == gr::settings::kPendingNotRunning);
    };

    "a timeout past the clock's range waits with no limit"_test = [] {
        Fixture<Plain> fixture;
        expect(fixture.settings.check({}).has_value());
        fixture.settings.moveState(BlockState::running);
        std::expected<Values, Rejected> result;
        std::atomic<bool>               returned{false};
        // about 584 years: in nanoseconds it wraps to just below zero, so a deadline formed from it lies in the past
        constexpr std::chrono::milliseconds pastRange{18'446'744'073'709};
        std::thread                         caller([&] {
            result = setOne(fixture.settings, "gain", 2.f, pastRange);
            returned.store(true, std::memory_order_release);
        });
        // a call that returned at once would do so within this bound; one that waits never does before the apply
        const auto watchedUntil = std::chrono::steady_clock::now() + 200ms;
        while (!returned.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < watchedUntil) {
            std::this_thread::yield();
        }
        expect(!returned.load(std::memory_order_acquire)) << "the call waits for the apply";
        applyUntilHookCalls(fixture, 2);
        caller.join();
        expect(fatal(result.has_value()));
        expect(result->empty()) << "no timeout: in force as sent";
    };

    "a call that names no timeout waits the link's default, else one second"_test = [] {
        Fixture<Plain> fixture;
        expect(fixture.settings.check({}).has_value());
        fixture.settings.moveState(BlockState::running);
        const auto alone = std::chrono::steady_clock::now();
        const auto first = setOne(fixture.settings, "gain", 2.f);
        expect(std::chrono::steady_clock::now() - alone >= gr::settings::kDefaultTimeout);
        expect(fatal(first.has_value()));
        expect(first->at("gain").reason == gr::settings::kPendingTimeout);
        FakeScheduler scheduler;
        scheduler.timeout = 1200ms;
        fixture.settings.attachScheduler(&scheduler);
        const auto linked = std::chrono::steady_clock::now();
        const auto second = setOne(fixture.settings, "gain", 3.f);
        expect(std::chrono::steady_clock::now() - linked >= 1200ms);
        expect(fatal(second.has_value()));
        expect(second->at("gain").reason == gr::settings::kPendingTimeout);
    };

    "a scheduler link takes the notifications, wakes for a running block, and keeps a worker from waiting"_test = [] {
        Fixture<Plain> fixture;
        FakeScheduler  scheduler;
        fixture.settings.attachScheduler(&scheduler);
        expect(fixture.settings.check({}).has_value());
        fixture.block.throwText = "the device refuses it";
        CerrCapture captured;
        expect(failedNames(setOne(fixture.settings, "gain", 5.f), "gain"));
        expect(fatal(eq(scheduler.held.size(), 1UZ)));
        expect(scheduler.held.front().key == "gain" && scheduler.held.front().what == gr::settings::NotificationKind::failed && !scheduler.held.front().quiet);
        expect(eq(captured.lines("failed"), 0UZ)) << "the default reader prints nothing for a block a scheduler holds";
        fixture.block.throwText.clear();
        fixture.settings.moveState(BlockState::running);
        const int wakes = scheduler.wakes.load();
        expect(setOne(fixture.settings, "gain", 6.f, gr::settings::noWait).has_value());
        expect(eq(scheduler.wakes.load(), wakes + 1));
        scheduler.worker      = true;
        const auto fromWorker = setOne(fixture.settings, "ppm", 2.0, kLongWait);
        expect(fatal(fromWorker.has_value()));
        expect(fromWorker->at("ppm").reason == gr::settings::kPendingOnWorker) << "a worker never waits, and its call says so";
        expect(gr::testing::applyStagedForTest(fixture.settings).empty());
        scheduler.inherited     = gr::settings::NotificationLevel::off;
        fixture.block.throwText = "the device refuses it";
        expect(setOne(fixture.settings, "a", 1, gr::settings::noWait).has_value());
        expect(fatal(eq(gr::testing::applyStagedForTest(fixture.settings).size(), 1UZ)));
        expect(fatal(eq(scheduler.held.size(), 2UZ)));
        expect(scheduler.held.back().key == "a" && scheduler.held.back().quiet) << "an inherited off marks it quiet";
    };

    "several threads set distinct and shared keys of a running block while it applies"_test = [] {
        Counter                block;
        Driven<Counter>        settings(block);
        constexpr std::int64_t kSets = 200;
        std::atomic<bool>      done{false};
        std::atomic<int>       refused{0};
        expect(fatal(settings.check({}).has_value()));
        settings.moveState(BlockState::running);
        std::thread              worker([&] {
            while (!done.load(std::memory_order_acquire)) {
                std::ignore = gr::testing::applyStagedForTest(settings);
                std::this_thread::yield();
            }
        });
        std::vector<std::thread> callers;
        for (const std::string_view key : {"a", "b", "c"}) {
            callers.emplace_back([&settings, &refused, key] {
                for (std::int64_t value = 1; value <= kSets; ++value) {
                    const property_map changes{{std::pmr::string(key), value}, {"shared", value}};
                    if (!settings.set(changes, kLongWait).has_value()) {
                        refused.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
        }
        for (std::thread& caller : callers) {
            caller.join();
        }
        done.store(true, std::memory_order_release);
        worker.join();
        expect(eq(refused.load(), 0));
        for (const std::string_view key : {"a", "b", "c", "shared"}) {
            expect(eq(settings.get<std::int64_t>(key).value_or(0), kSets)) << key;
        }
        expect(eq(block.settings.a, kSets));
    };

    "two threads making a first call at once initialize each block once"_test = [] {
        constexpr std::size_t                         kBlocks = 50UZ;
        std::vector<std::unique_ptr<Counter>>         blocks;
        std::vector<std::unique_ptr<Driven<Counter>>> settings;
        for (std::size_t index = 0UZ; index < kBlocks; ++index) {
            blocks.push_back(std::make_unique<Counter>());
            settings.push_back(std::make_unique<Driven<Counter>>(*blocks.back(), property_map{{"a", 5}}));
        }
        std::atomic<int> refused{0};
        const auto       firstCalls = [&](std::string_view key) {
            for (const auto& each : settings) {
                if (!each->set(key, PmtValue(std::int64_t{7})).has_value()) {
                    refused.fetch_add(1, std::memory_order_relaxed);
                }
            }
        };
        std::thread one(firstCalls, "b");
        std::thread two(firstCalls, "c");
        one.join();
        two.join();
        expect(eq(refused.load(), 0));
        // two calls that apply at once on one block may share one apply, so the INIT apply, the only one that writes `a`, is counted
        expect(std::ranges::all_of(blocks, [](const auto& block) { return block->callsSettingA.load() == 1 && block->settings.a == 5 && block->settings.b == 7 && block->settings.c == 7; }));
    };
};

const suite<"BlockSettings: reading"> readTests = [] {
    "a read before INIT gives the constructed values and runs no hook"_test = [] {
        Fixture<Plain> fixture(property_map{{"a", 5}});
        expect(eq(fixture.settings.get<std::int32_t>("a").value_or(0), 5));
        expect(eq(fixture.block.settings.a, 0));
        expect(eq(fixture.block.applyCalls, 0));
        Fixture<Radio> radio;
        const auto     all = radio.settings.get();
        expect(fatal(all.has_value()));
        for (std::string_view key : {"sample_rate", "n_channels", "channel.0.frequency", "channel.1.frequency", "burst_size"}) {
            expect(all->contains(key)) << key;
        }
        expect(!all->contains("channel.0.lna") && !all->contains("temperature") && !all->contains("channel.0.rssi"));
        expect(radio.block.describedSettings.empty()) << "a read writes nothing";
        expect(eq(radio.block.applyCalls, 0));
    };

    "a stopped block a graph holds reads the waiting value, and a restore keeps it"_test = [] {
        Fixture<Plain> fixture;
        FakeGraph      graph;
        fixture.settings.attachGraph(&graph);
        expect(fixture.settings.check({}).has_value());
        const auto staged = setOne(fixture.settings, "gain", 4.f);
        expect(fatal(staged.has_value()));
        expect(staged->at("gain").reason == gr::settings::kPendingNotRunning);
        expect(eq(fixture.settings.get<float>("gain").value_or(0.f), 4.f));
        expect(eq(fixture.block.settings.gain.value, 1.f));
        expect(fixture.settings.set(gr::testing::writableValues(fixture.settings, fixture.settings.get())).has_value());
        expect(eq(fixture.settings.get<float>("gain").value_or(0.f), 4.f));
        expect(fixture.settings.isGiven("gain"));
    };

    "members past a pending count are left out of a read"_test = [] {
        Fixture<Radio> fixture;
        FakeGraph      graph;
        fixture.settings.attachGraph(&graph);
        expect(fixture.settings.check({}).has_value());
        expect(fixture.settings.tagEnters(property_map{{"n_channels", 1U}}, false).empty()) << "a tag stages the count";
        const auto all = fixture.settings.get();
        expect(fatal(all.has_value()));
        expect(std::ranges::none_of(*all, [](const auto& entry) { return entry.first.starts_with("channel.1."); }));
        expect(!fixture.settings.isGiven("n_channels")) << "a tag gives no key";
    };

    "a key with no value reads pending on a running block and refuses on a block alone"_test = [] {
        Fixture<Radio> fixture;
        const auto     none = fixture.settings.get("channel.0.lna");
        expect(fatal(!none.has_value()));
        expect(none.error().front().reason == "must name a setting that holds a value");
        expect(fixture.settings.check({}).has_value());
        fixture.settings.moveState(BlockState::running);
        expect(setOne(fixture.settings, "channel.1.lna", 10.0, gr::settings::noWait).has_value());
        const auto pending = fixture.settings.get("channel.1.lna");
        expect(fatal(pending.has_value()));
        expect(pending->isPending() && pending->reason.empty()) << "a read waits for nothing, so its pending entry gives no cause";
    };

    "a key the batch under way applies on a running block reads pending"_test = [] {
        Fixture<Radio> fixture;
        fixture.block.readLnaInHook = true;
        expect(fixture.settings.check({}).has_value());
        fixture.settings.moveState(BlockState::running);
        expect(setOne(fixture.settings, "channel.0.lna", 16.0, gr::settings::noWait).has_value());
        expect(gr::testing::applyStagedForTest(fixture.settings).empty());
        expect(fatal(fixture.block.lnaInHook.has_value()));
        expect(fatal(fixture.block.lnaInHook->has_value()));
        expect((*fixture.block.lnaInHook)->isPending() && (*fixture.block.lnaInHook)->reason.empty());
    };

    "a read waits for an INIT under way on another thread"_test = [] {
        for (int round = 0; round < 20; ++round) {
            Fixture<Radio>    radio;
            Fixture<Plain>    plain(property_map{{"a", 3}});
            std::atomic<bool> started{false};
            plain.block.deriveB = true;
            std::thread initializer([&] {
                started.store(true, std::memory_order_release);
                std::ignore = radio.settings.check({});
                std::ignore = plain.settings.check({});
            });
            while (!started.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (int read = 0; read < 50; ++read) {
                expect(radio.settings.get().has_value());
                expect(plain.settings.get().has_value());
            }
            initializer.join();
            expect(eq(plain.settings.get<std::int32_t>("b").value_or(0), 6));
        }
    };

    "a bare name reads member 0 and comes back bare"_test = [] {
        Fixture<Radio> fixture;
        expect(setOne(fixture.settings, "frequency", 144e6).has_value());
        expect(eq(fixture.settings.get<double>("frequency").value_or(0.0), 144e6));
        expect(eq(fixture.settings.get<double>("channel.0.frequency").value_or(0.0), 144e6));
        const std::array<std::string, 2UZ> keys{"frequency", "channel.0.frequency"};
        const auto                         both = fixture.settings.get(keys);
        expect(fatal(both.has_value()));
        expect(both->contains("frequency") && both->contains("channel.0.frequency"));
        expect(setOne(fixture.settings, "channel.1.frequency", 145e6).has_value());
        expect(fixture.block.lastChanged == std::vector<std::string>{"channel.1.frequency"});
    };

    "a typed read refuses a narrowing and keeps a lossless one"_test = [] {
        Fixture<Plain> fixture;
        expect(setOne(fixture.settings, "ppm", 0.1).has_value());
        expect(eq(fixture.settings.get<double>("ppm").value_or(0.0), 0.1));
        const auto narrowed = fixture.settings.get<float>("ppm");
        expect(fatal(!narrowed.has_value()));
        expect(narrowed.error().front().key == "ppm");
        expect(fixture.settings.get("gain").value().isValue());
        expect(eq(fixture.settings.get<float>("gain").value_or(0.f), 1.f));
        expect(rejectedNames(fixture.settings.get(std::array<std::string, 1UZ>{"rx_gain"}), "rx_gain"));
    };

    "every key sent: failed by a refusal, a failed key, an entry, or the value sent"_test = [] {
        const property_map sent{{"a", 1}, {"b", 2}};
        const Values       refused = gr::testing::everyKeySent(sent, std::unexpected(Rejected{Rejection{.key = "a", .reason = "too big"}}));
        expect(refused.at("a").isFailed() && refused.at("b").isFailed() && refused.at("b").reason.contains("too big"));
        const Values failed = gr::testing::everyKeySent(sent, Values{{"a", Entry{.state = Entry::State::failed, .value = {}, .reason = "no lock"}}});
        expect(failed.at("a").isFailed() && failed.at("a").reason == "no lock");
        expect(failed.at("b").isValue() && failed.at("b").value == PmtValue(2));
        const Values mixed = gr::testing::everyKeySent(sent, Values{{"a", Entry{.value = PmtValue(3), .reason = "rounded"}}});
        expect(mixed.at("a").value == PmtValue(3) && mixed.at("a").reason == "rounded");
        expect(mixed.at("b").value == PmtValue(2));
    };
};

const suite<"BlockSettings: tags"> tagTests = [] {
    "a reset tag makes its defaults given and leaves the keys the tag sets ungiven"_test = [] {
        InfoScheduler  scheduler;
        Fixture<Plain> fixture;
        fixture.settings.attachScheduler(&scheduler);
        FakeGraph graph;
        fixture.settings.attachGraph(&graph);
        expect(fixture.settings.check({}).has_value());
        expect(setOne(fixture.settings, "gain", 5.f).has_value());
        expect(gr::testing::applyStagedForTest(fixture.settings).empty());
        expect(!fixture.settings.isGiven("ppm"));
        CerrCapture captured;
        expect(fixture.settings.tagEnters(property_map{{"a", 3}}, true).empty());
        expect(gr::testing::applyStagedForTest(fixture.settings).empty());
        expect(eq(fixture.block.settings.a, 3));
        expect(eq(fixture.block.settings.gain.value, 1.f));
        expect(fixture.settings.isGiven("ppm")) << "a reset tag's defaults are given";
        expect(!fixture.settings.isGiven("a")) << "a key the tag sets is not";
        expect(fixture.settings.savedValues().at("gain") == PmtValue(1.f));
        expect(eq(captured.lines("changed 'a': a tag set the value"), 1UZ)) << captured.str();
        expect(eq(captured.lines("a tag set the value"), 1UZ));
        expect(eq(fixture.block.resetCalls, 1));
    };

    "a tag at the value in force stages nothing unless it replaces a pending value"_test = [] {
        InfoScheduler  scheduler;
        Fixture<Plain> fixture;
        fixture.settings.attachScheduler(&scheduler);
        FakeGraph graph;
        fixture.settings.attachGraph(&graph);
        expect(fixture.settings.check({}).has_value());
        {
            CerrCapture captured;
            expect(fixture.settings.tagEnters(property_map{{"gain", 1.0}}, false).empty());
            expect(gr::testing::applyStagedForTest(fixture.settings).empty());
            expect(eq(captured.lines("a tag set the value"), 0UZ)) << captured.str();
        }
        expect(setOne(fixture.settings, "gain", 5.f).has_value());
        expect(fixture.settings.tagEnters(property_map{{"gain", 1.0}}, false).empty());
        expect(gr::testing::applyStagedForTest(fixture.settings).empty());
        expect(eq(fixture.block.settings.gain.value, 1.f));
    };

    "a tag's refused value is notified and stages nothing"_test = [] {
        Fixture<Plain> fixture;
        expect(fixture.settings.check({}).has_value());
        CerrCapture    captured;
        const Rejected refused = fixture.settings.tagEnters(property_map{{"gain", 60.0}}, false);
        expect(fatal(eq(refused.size(), 1UZ)));
        expect(refused.front().key == "gain");
        expect(eq(captured.lines("error: plain#0 failed 'gain': the tag's value is refused"), 1UZ)) << captured.str();
        expect(gr::testing::applyStagedForTest(fixture.settings).empty());
        expect(eq(fixture.block.settings.gain.value, 1.f));
    };

    "a coupling on a bare name sets member 0's key"_test = [] {
        Fixture<Radio> fixture;
        expect(fatal(fixture.settings.check({}).has_value()));
        expect(fatal(fixture.settings.setTagCouplings(property_map{{"frequency", std::string("rx_freq")}}).has_value()));
        expect(fixture.settings.tagEnters(property_map{{"rx_freq", 144e6}}).empty());
        expect(gr::testing::applyStagedForTest(fixture.settings).empty());
        expect(eq(fixture.settings.get<double>("channel.0.frequency").value_or(0.0), 144e6)) << "the tag misses member 0";
        expect(eq(fixture.settings.get<double>("channel.1.frequency").value_or(0.0), 100e6)) << "the tag reaches member 1";
    };
};

const suite<"BlockSettings: a moved block"> moveTests = [] {
    "the settings move with their block to a new owner"_test = [] {
        Plain         first;
        Driven<Plain> settings(first, property_map{{"a", 5}});
        first.core = &settings;
        FakeGraph graph;
        settings.attachGraph(&graph);
        expect(settings.check({}).has_value());
        expect(setOne(settings, "b", 7).has_value());
        Plain         second = std::move(first);
        Driven<Plain> moved(second, std::move(settings));
        second.core = &moved;
        expect(eq(moved.get<std::int32_t>("b").value_or(0), 7)) << "the waiting value moves";
        expect(moved.isGiven("a") && moved.isGiven("b"));
        expect(gr::testing::applyStagedForTest(moved).empty());
        expect(eq(second.settings.b, 7));
        expect(eq(first.settings.b, 0)) << "the apply writes the new owner's field";
        expect(eq(second.applyCalls, 2));
        expect(setOne(moved, "b", 8).has_value());
        expect(gr::testing::applyStagedForTest(moved).empty());
        expect(eq(second.settings.b, 8));
    };
};

const suite<"BlockSettings: reset"> resetTests = [] {
    "a reset is a set of the stored defaults, then one reset hook"_test = [] {
        Fixture<Plain> fixture;
        expect(setOne(fixture.settings, "gain", 5.f).has_value());
        const int calls = fixture.block.applyCalls;
        expect(fixture.settings.resetDefaults().has_value());
        expect(eq(fixture.block.applyCalls, calls + 1));
        expect(eq(fixture.block.resetCalls, 1));
        expect(eq(fixture.block.settings.gain.value, 1.f));
        expect(fixture.settings.isGiven("gain"));
        expect(fixture.settings.resetDefaults().has_value());
        expect(eq(fixture.block.applyCalls, calls + 1)) << "a reset that changes nothing calls no apply hook";
        expect(eq(fixture.block.resetCalls, 2));
    };

    "a reset returns the values in force when the initialization ended, the constructed ones among them"_test = [] {
        Fixture<Plain> fixture({{"gain", 5.f}});
        expect(setOne(fixture.settings, "gain", 7.f).has_value());
        expect(fixture.settings.resetDefaults().has_value());
        expect(eq(fixture.block.settings.gain.value, 5.f));
    };

    "a reset leaves a setting with no default that no caller gave ungiven"_test = [] {
        Fixture<Radio> fixture;
        expect(fixture.settings.check({}).has_value());
        fixture.settings.publish("channel.0.lna", PmtValue(12.0));
        expect(gr::testing::applyStagedForTest(fixture.settings).empty());
        expect(eq(fixture.settings.get<double>("channel.0.lna").value_or(0.0), 12.0));
        expect(setOne(fixture.settings, "sample_rate", 3e6).has_value());
        expect(fixture.settings.resetDefaults().has_value());
        expect(!fixture.settings.isGiven("channel.0.lna"));
        expect(fixture.settings.isGiven("sample_rate"));
        expect(eq(fixture.settings.get<double>("sample_rate").value_or(0.0), 10e6));
    };
};

} // namespace qa_block_settings

int main() { /* not needed by the UT framework */ }
