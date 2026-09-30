#include <boost/ut.hpp>

#include <algorithm>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <gnuradio-4.0/BlockRegistry.hpp>
#include <gnuradio-4.0/DeviceListing.hpp>
#include <gnuradio-4.0/PluginLoader.hpp>

#include "build_configure.hpp"

/**
 * A plugin and a block library register device listings of fixed test units. The loader lists the units of every
 * family they register. The block library registers into the process's registry and stays mapped. The cases therefore
 * share one loader over its directory, and the first case loads it.
 */
namespace qa_device_listing {

using namespace gr;

[[nodiscard]] std::string listingDirectory() { return std::string(TESTS_BINARY_PATH) + "/device_listing"; }

[[nodiscard]] std::string duplicateDirectory() { return std::string(TESTS_BINARY_PATH) + "/device_listing_duplicate"; }

[[nodiscard]] std::string staleDirectory() { return std::string(TESTS_BINARY_PATH) + "/device_listing_stale"; }

[[nodiscard]] std::string pluginDirectory() { return std::string(TESTS_BINARY_PATH) + "/plugins"; }

[[nodiscard]] PluginLoader& listingLoader() {
    static PluginLoader loader(gr::globalBlockRegistry(), gr::globalSchedulerRegistry(), std::vector<std::string>{listingDirectory()});
    return loader;
}

[[nodiscard]] const ListedFamily* familyNamed(const DeviceList& list, std::string_view family) {
    const auto found = std::ranges::find(list.families, family, &ListedFamily::family);
    return found == list.families.end() ? nullptr : std::addressof(*found);
}

[[nodiscard]] std::vector<ListedUnit> unitsOf(const DeviceList& list, std::string_view family) {
    std::vector<ListedUnit> units;
    std::ranges::copy_if(list.units, std::back_inserter(units), [family](const ListedUnit& unit) { return unit.family == family; });
    return units;
}

[[nodiscard]] const RefusedDeviceListing* refusalOf(const std::vector<RefusedDeviceListing>& refused, std::string_view family) {
    const auto found = std::ranges::find(refused, family, &RefusedDeviceListing::family);
    return found == refused.end() ? nullptr : std::addressof(*found);
}

std::vector<DeviceUnit> firstUnits() { return {{"first", "fake:1"}}; }

std::vector<DeviceUnit> secondUnits() { return {{"second", "fake:2"}}; }

} // namespace qa_device_listing

const boost::ut::suite<"DeviceListing"> deviceListingTests = [] {
    using namespace boost::ut;
    using namespace qa_device_listing;

    "the first registration of a family word holds it, and another function under the word is refused"_test = [] {
        DeviceListingRegistry registry;
        expect(registry.add({.family = "fake", .opens = false, .receive = true, .transmit = false, .listUnits = firstUnits}));
        expect(!registry.add({.family = "fake", .opens = false, .receive = true, .transmit = false, .listUnits = firstUnits})) << "the same function again adds nothing";
        expect(that % registry.refused().empty()) << "and is not refused";

        expect(!registry.add({.family = "fake", .opens = true, .receive = false, .transmit = true, .listUnits = secondUnits}));
        expect(fatal(eq(registry.refused().size(), 1UZ)));
        expect(eq(registry.refused().front().family, std::string("fake")));
        expect(registry.refused().front().refused == &secondUnits) << "the refusal names the function refused";
        expect(registry.refused().front().holder == &firstUnits) << "and the function that holds the word";

        expect(eq(registry.generation(), 1UZ));
        expect(fatal(eq(registry.listings().size(), 1UZ)));
        expect(registry.listings().front().listUnits == &firstUnits) << "the holder keeps the word";
        expect(!registry.listings().front().opens) << "and its flags";
    };

    "the loader lists the families of a plugin and of a block library, and leaves out the one whose listing opens"_test = [] {
        PluginLoader& loader = listingLoader();
        expect(that % loader.failedPlugins().empty()) << "neither file fails to load";
        expect(eq(loader.plugins().size(), 1UZ)) << "the plugin loads as a plugin";
        expect(fatal(eq(loader.blockLibraries().size(), 1UZ))) << "a library that registers a listing and no block is kept mapped";
        expect(eq(loader.blockLibraries().front().nDeviceListings, 1UZ));
        expect(eq(loader.blockLibraries().front().nBlockRegistrations, 0UZ));

        const DeviceList list = loader.listDevices();
        expect(std::ranges::is_sorted(list.families, {}, &ListedFamily::family)) << "the families are ordered by word";
        for (const std::string_view word : {"fakeempty", "fakelibrary", "fakeopening", "fakeplugin", "fakethrowing"}) {
            expect(familyNamed(list, word) != nullptr) << word << " is reported";
        }

        const ListedFamily* opening = familyNamed(list, "fakeopening");
        expect(fatal(opening != nullptr));
        expect(opening->opens);
        expect(!opening->listed) << "the default call does not ask a family whose listing opens a unit";
        expect(eq(opening->reason, std::string("its listing opens a unit, and the call does not allow that")));
        expect(that % unitsOf(list, "fakeopening").empty());

        const ListedFamily* plugin = familyNamed(list, "fakeplugin");
        expect(fatal(plugin != nullptr));
        expect(plugin->listed && !plugin->opens);
        expect(that % plugin->reason.empty()) << "a family that lists units carries no reason";
        const std::vector<ListedUnit> pluginUnits = unitsOf(list, "fakeplugin");
        expect(fatal(eq(pluginUnits.size(), 3UZ)));
        expect(eq(pluginUnits[0].label, std::string("Fake unit A")));
        expect(eq(pluginUnits[0].selector, std::string("fakeplugin:serial=FAKE0001"))) << "the selector as the family spelled it";
        expect(pluginUnits[0].receive && pluginUnits[0].transmit) << "the directions of the family";
        expect(eq(pluginUnits[1].selector, std::string("fakeplugin:serial=FAKE0002")));
        expect(eq(pluginUnits[2].label, std::string("Fake unit without a serial")));
        expect(that % pluginUnits[2].selector.empty()) << "a unit listed without a selector keeps its label";

        const ListedFamily* library = familyNamed(list, "fakelibrary");
        expect(fatal(library != nullptr));
        expect(library->listed);
        const std::vector<ListedUnit> libraryUnits = unitsOf(list, "fakelibrary");
        expect(fatal(eq(libraryUnits.size(), 1UZ)));
        expect(eq(libraryUnits[0].selector, std::string("fakelibrary:index=0")));
        expect(libraryUnits[0].receive && !libraryUnits[0].transmit);

        const ListedFamily* throwing = familyNamed(list, "fakethrowing");
        expect(fatal(throwing != nullptr));
        expect(!throwing->listed) << "a family whose listing throws is not listed";
        expect(eq(throwing->reason, std::string("the fake bus does not answer"))) << "and carries the exception's message";

        const ListedFamily* empty = familyNamed(list, "fakeempty");
        expect(fatal(empty != nullptr));
        expect(empty->listed);
        expect(eq(empty->reason, std::string("the listing found no unit")));
        expect(that % unitsOf(list, "fakeempty").empty());

        expect(that % loader.refusedDeviceListings().empty()) << "no family word is registered twice";
    };

    "with opening allowed the family whose listing opens a unit is asked"_test = [] {
        const DeviceList list = listingLoader().listDevices(Opening::Allowed);

        const ListedFamily* opening = familyNamed(list, "fakeopening");
        expect(fatal(opening != nullptr));
        expect(opening->opens && opening->listed);
        expect(that % opening->reason.empty());
        const std::vector<ListedUnit> units = unitsOf(list, "fakeopening");
        expect(fatal(eq(units.size(), 1UZ)));
        expect(eq(units[0].selector, std::string("fakeopening:bus=1")));
        expect(units[0].receive && !units[0].transmit);
        expect(eq(unitsOf(list, "fakeplugin").size(), 3UZ)) << "the other families are listed as before";
    };

    "a second registration of a family word is refused, and names the file that offered it and the holder"_test = [] {
        PluginLoader& loader = listingLoader();
        loader.loadDirectories(std::vector<std::string>{duplicateDirectory()});
        expect(that % loader.failedPlugins().empty()) << "a refused listing fails no file";
        expect(eq(loader.plugins().size(), 2UZ));
        expect(fatal(eq(loader.blockLibraries().size(), 2UZ))) << "a library whose listing was refused is kept mapped";
        expect(eq(loader.blockLibraries().back().nDeviceListings, 0UZ));

        const std::vector<RefusedDeviceListing> refused = loader.refusedDeviceListings();
        expect(eq(refused.size(), 2UZ));
        const RefusedDeviceListing* library = refusalOf(refused, "fakelibrary");
        expect(fatal(library != nullptr));
        expect(library->file.ends_with("/device_listing_duplicate/libdevice_listing_duplicate_library.so")) << library->file;
        expect(library->holder.ends_with("/device_listing/libdevice_listing_library.so")) << library->holder;
        const RefusedDeviceListing* plugin = refusalOf(refused, "fakeplugin");
        expect(fatal(plugin != nullptr));
        expect(plugin->file.ends_with("/device_listing_duplicate/libdevice_listing_duplicate_plugin.so")) << plugin->file;
        expect(plugin->holder.ends_with("/device_listing/libdevice_listing_plugin.so")) << plugin->holder;

        const DeviceList              list        = loader.listDevices();
        const std::vector<ListedUnit> pluginUnits = unitsOf(list, "fakeplugin");
        expect(fatal(eq(pluginUnits.size(), 3UZ))) << "the holder answers for the word";
        expect(eq(pluginUnits[0].selector, std::string("fakeplugin:serial=FAKE0001")));
        const std::vector<ListedUnit> libraryUnits = unitsOf(list, "fakelibrary");
        expect(fatal(eq(libraryUnits.size(), 1UZ)));
        expect(eq(libraryUnits[0].selector, std::string("fakelibrary:index=0")));
        expect(eq(std::ranges::count(list.families, std::string("fakeplugin"), &ListedFamily::family), 1L)) << "the word is reported once";
    };

    "a plugin that registers no listing loads, lists nothing and is not refused"_test = [] {
        const std::vector<std::string> directories{pluginDirectory()};
        PluginLoader                   loader(gr::globalBlockRegistry(), gr::globalSchedulerRegistry(), directories);

        expect(fatal(ge(loader.plugins().size(), 1UZ))) << "the plugin directory holds plugins that load";
        for (const PluginHandler& plugin : loader.plugins()) {
            expect(that % plugin->deviceListings().empty()) << plugin->metadata.plugin_name;
        }
        for (const auto& [file, reason] : loader.failedPlugins()) {
            expect(file.contains("bad_plugin")) << file << " failed: " << reason;
        }
        expect(eq(loader.listDevices(Opening::Allowed).families.size(), gr::globalDeviceListingRegistry().listings().size())) << "only the process's registry reports a family";
        expect(that % loader.refusedDeviceListings().empty());
    };

    "a library refused for its scheduler's plugin ABI version leaves no device listing behind"_test = [] {
        const std::size_t              generationBefore = gr::globalDeviceListingRegistry().generation();
        const std::vector<std::string> directories{staleDirectory()};
        PluginLoader                   loader(gr::globalBlockRegistry(), gr::globalSchedulerRegistry(), directories);

        expect(fatal(eq(loader.failedPlugins().size(), 1UZ))) << "the library is refused";
        const auto& [file, reason] = *loader.failedPlugins().begin();
        expect(file.ends_with("/device_listing_stale/libdevice_listing_stale_library.so")) << file;
        expect(reason.contains("plugin ABI version 1")) << reason;
        expect(that % loader.blockLibraries().empty()) << "it is not kept mapped";

        expect(std::ranges::none_of(gr::globalDeviceListingRegistry().listings(), [](const DeviceListing& listing) { return listing.family == "fakestale"; })) << "its listing left the process's registry";
        expect(eq(gr::globalDeviceListingRegistry().generation(), generationBefore)) << "the registry holds as many words as before the load";
        const DeviceList list = loader.listDevices(Opening::Allowed);
        expect(familyNamed(list, "fakestale") == nullptr) << "the listing asks no function of the closed file";
        expect(that % loader.refusedDeviceListings().empty());
    };
};

int main() { /* not needed for UT */ }
