#include <boost/ut.hpp>

#include <algorithm>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <gnuradio-4.0/BlockRegistry.hpp>
#include <gnuradio-4.0/Graph.hpp>
#include <gnuradio-4.0/Graph_yaml_importer.hpp>
#include <gnuradio-4.0/PluginLoader.hpp>

#include "build_configure.hpp"

namespace qa_plugin_registration {

constexpr std::string_view kVersionedKey = "test::versioned";

[[nodiscard]] inline std::string versionedPluginDirectory() { return std::string(TESTS_BINARY_PATH) + "/versioned_plugin"; }

/// the `version` key of the first block entry in a saved graph, or nothing when that entry carries none
[[nodiscard]] inline std::optional<gr::pmt::Value> savedBlockVersion(std::string_view dump) {
    const auto parsed = gr::pmt::yaml::deserialize(dump);
    boost::ut::expect(boost::ut::fatal(parsed.has_value())) << "a dump this writer produced must parse";
    const auto  blocks = parsed->find("blocks");
    const auto* list   = blocks == parsed->cend() ? nullptr : blocks->second.get_if<gr::Tensor<gr::pmt::Value>>();
    boost::ut::expect(boost::ut::fatal(list != nullptr && list->begin() != list->end())) << "the dump lists its block";
    const auto* entry = list->begin()->get_if<gr::property_map>();
    boost::ut::expect(boost::ut::fatal(entry != nullptr)) << "a block entry is a map";
    const auto version = entry->find("version");
    return version == entry->cend() ? std::nullopt : std::optional<gr::pmt::Value>{version->second};
}

} // namespace qa_plugin_registration

const boost::ut::suite<"PluginRegistration"> pluginRegistrationTests = [] {
    using namespace boost::ut;
    using namespace qa_plugin_registration;

    "a plugin registers into the instance its own header declares"_test = [] {
        gr::BlockRegistry              registry;
        gr::SchedulerRegistry          schedulerRegistry;
        const std::vector<std::string> pluginDirectories{std::string(TESTS_BINARY_PATH) + "/plugins"};
        gr::PluginLoader               loader(registry, schedulerRegistry, pluginDirectories);

        for (const auto& [file, reason] : loader.failedPlugins()) {
            expect(file.find("Good") == std::string::npos) << std::format("{} failed to load: {}", file, reason);
        }

        const std::vector<std::string> available = loader.availableBlocks();
        for (const std::string_view name : {"good::identity<float32>", "good::identity<float64>", "good::cout_sink<float32>", "good::fixed_source<float64>"}) {
            expect(std::ranges::find(available, name) != available.end()) << std::format("{} is not provided by any loaded plugin", name);
        }
        expect(loader.instantiate("good::identity<float32>") != nullptr);
    };

    "a pin selects among the versions a plugin registered under one name"_test = [] {
        gr::BlockRegistry              registry;
        gr::SchedulerRegistry          schedulerRegistry;
        const std::vector<std::string> pluginDirectories{versionedPluginDirectory()};
        gr::PluginLoader               loader(registry, schedulerRegistry, pluginDirectories);

        for (const auto& [file, reason] : loader.failedPlugins()) {
            expect(false) << std::format("{} failed to load: {}", file, reason);
        }
        const std::vector<gr::block::Version> versions = loader.blockVersions(kVersionedKey);
        expect(fatal(eq(versions.size(), 2UZ))) << "the loader reports the versions a plugin holds";
        expect(eq(versions.front(), gr::block::Version{1U}));
        expect(eq(versions.back(), gr::block::Version{2U}));

        const std::shared_ptr<gr::BlockModel> newest = loader.instantiate(kVersionedKey);
        expect(fatal(newest != nullptr));
        expect(eq(newest->version(), gr::block::Version{2U})) << "an unpinned create takes the newest the plugin holds";
        expect(!newest->pinnedVersion().has_value());

        const auto older = loader.instantiatePinnedOrError(kVersionedKey, 1U);
        expect(fatal(older.has_value())) << (older.has_value() ? std::string{} : older.error().message);
        expect(eq(std::string((*older)->typeName()), std::string("good::VersionedFirst")));
        expect((*older)->pinnedVersion() == std::optional<gr::block::Version>{1U});

        const auto newer = loader.instantiatePinnedOrError(kVersionedKey, 2U);
        expect(fatal(newer.has_value())) << (newer.has_value() ? std::string{} : newer.error().message);
        expect(eq(std::string((*newer)->typeName()), std::string("good::VersionedSecond")));
        expect((*newer)->pinnedVersion() == std::optional<gr::block::Version>{2U});

        const auto missing = loader.instantiatePinnedOrError(kVersionedKey, 9U);
        expect(fatal(!missing.has_value())) << "a version the plugin does not hold is refused";
        expect(missing.error().message.contains("version 9")) << missing.error().message;
        expect(missing.error().message.contains("1, 2")) << missing.error().message << "the versions the plugin holds are named";
    };

    "a plugin block keeps its version across a GRC round trip"_test = [] {
        gr::BlockRegistry              registry;
        gr::SchedulerRegistry          schedulerRegistry;
        const std::vector<std::string> pluginDirectories{versionedPluginDirectory()};
        gr::PluginLoader               loader(registry, schedulerRegistry, pluginDirectories);

        gr::Graph flow(loader);
        flow.addBlock(loader.instantiate(kVersionedKey));
        const std::string unpinnedDump = gr::saveGrc(loader, flow);
        expect(!savedBlockVersion(unpinnedDump).has_value()) << unpinnedDump << "an unpinned instance keeps taking the newest";

        const auto unpinnedBack = gr::loadGrc(loader, unpinnedDump);
        expect(fatal(eq(unpinnedBack->blocks().size(), 1UZ)));
        expect(eq(unpinnedBack->blocks().front()->version(), gr::block::Version{2U}));

        const auto pinned = loader.instantiatePinnedOrError(kVersionedKey, 1U);
        expect(fatal(pinned.has_value())) << (pinned.has_value() ? std::string{} : pinned.error().message);
        gr::Graph pinnedFlow(loader);
        pinnedFlow.addBlock(*pinned);
        const std::string                   pinnedDump   = gr::saveGrc(loader, pinnedFlow);
        const std::optional<gr::pmt::Value> savedVersion = savedBlockVersion(pinnedDump);
        expect(savedVersion.has_value() && savedVersion->value_or(gr::block::Version{}) == gr::block::Version{1U}) << pinnedDump;

        const auto pinnedBack = gr::loadGrc(loader, pinnedDump);
        expect(fatal(eq(pinnedBack->blocks().size(), 1UZ)));
        expect(eq(pinnedBack->blocks().front()->version(), gr::block::Version{1U}));
        expect(pinnedBack->blocks().front()->pinnedVersion() == std::optional<gr::block::Version>{1U});
    };
};

int main() { /* not needed for UT */ }
