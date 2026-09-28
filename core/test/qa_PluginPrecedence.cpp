#include <boost/ut.hpp>

#include <algorithm>
#include <bit>
#include <cstdlib>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <gnuradio-4.0/Block.hpp>
#include <gnuradio-4.0/BlockRegistry.hpp>
#include <gnuradio-4.0/PluginLoader.hpp>

#include "build_configure.hpp"

/**
 * Which registration answers for a key that several files offer. The search order is the block registry, which holds
 * the program's own blocks and then the block libraries in the order they load, then the plugins in the order they
 * load. The first registration of a key at a version in that order holds it, a later one is refused and listed with
 * the file that holds it, and a different version of the key coexists with it.
 */
namespace qa_plugin_precedence {

using namespace gr;

constexpr std::string_view kPluginKey  = "test::precedence";
constexpr std::string_view kLibraryKey = "test::precedence_library";

/// a block the program registers under the key the precedence plugins offer
struct Framework : gr::Block<Framework> {
    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(Framework, in, out);

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value; }
};

const int kAddressInThisProgram = 0;

[[nodiscard]] std::string directoryOf(std::string_view fixture) { return std::format("{}/{}", TESTS_BINARY_PATH, fixture); }

/// the file of the loaded plugin whose path names `fixture`
[[nodiscard]] std::string pluginFile(const PluginLoader& loader, std::string_view fixture) {
    const auto found = std::ranges::find_if(loader.pluginFiles(), [fixture](const PluginLoader::PluginFile& plugin) { return plugin.file.contains(fixture); });
    boost::ut::expect(boost::ut::fatal(found != loader.pluginFiles().end())) << std::format("{} did not load as a plugin", fixture);
    return found->file;
}

/// the loaded block library whose path names `fixture`
[[nodiscard]] const PluginLoader::BlockLibrary& blockLibrary(const PluginLoader& loader, std::string_view fixture) {
    const auto found = std::ranges::find_if(loader.blockLibraries(), [fixture](const PluginLoader::BlockLibrary& library) { return library.file.contains(fixture); });
    boost::ut::expect(boost::ut::fatal(found != loader.blockLibraries().end())) << std::format("{} did not load as a block library", fixture);
    return *found;
}

/// the refused registrations of `key`
[[nodiscard]] std::vector<RefusedRegistration> refusalsOf(const PluginLoader& loader, std::string_view key) {
    std::vector<RefusedRegistration> refused = loader.refusedRegistrations();
    std::erase_if(refused, [key](const RefusedRegistration& refusal) { return refusal.key != key; });
    return refused;
}

} // namespace qa_plugin_precedence

const boost::ut::suite<"PluginPrecedence"> pluginPrecedenceTests = [] {
    using namespace boost::ut;
    using namespace qa_plugin_precedence;

    "of two plugins that offer one key at one version, the first loaded holds it in either order"_test = [] {
        for (const bool firstAhead : {true, false}) {
            const std::string_view ahead  = firstAhead ? "precedence_plugin_first" : "precedence_plugin_second";
            const std::string_view behind = firstAhead ? "precedence_plugin_second" : "precedence_plugin_first";

            gr::BlockRegistry              registry;
            gr::SchedulerRegistry          schedulerRegistry;
            const std::vector<std::string> directories{directoryOf(ahead), directoryOf(behind)};
            PluginLoader                   loader(registry, schedulerRegistry, directories);
            expect(that % loader.failedPlugins().empty()) << "a refused registration fails no file";
            expect(eq(loader.pluginFiles().size(), 2UZ)) << "the plugin whose registration was refused is loaded";

            const std::shared_ptr<BlockModel> block = loader.instantiate(kPluginKey);
            expect(fatal(block != nullptr));
            expect(eq(std::string(block->typeName()), std::string(firstAhead ? "precedence::First" : "precedence::Second"))) << ahead;

            const std::vector<RefusedRegistration> refused = loader.refusedRegistrations();
            expect(fatal(eq(refused.size(), 1UZ))) << "only the shared key collides" << ahead;
            expect(eq(refused.front().key, std::string(kPluginKey)));
            expect(eq(refused.front().version, block::Version{1U}));
            expect(eq(refused.front().file, pluginFile(loader, behind)));
            expect(eq(refused.front().holder, pluginFile(loader, ahead)));
            expect(loader.blockVersions(kPluginKey) == std::vector<block::Version>{1U});
        }
    };

    "a plugin's key at another version coexists, and a caller that names no version gets the newest"_test = [] {
        gr::BlockRegistry              registry;
        gr::SchedulerRegistry          schedulerRegistry;
        const std::vector<std::string> directories{directoryOf("precedence_plugin_newer"), directoryOf("precedence_plugin_first")};
        PluginLoader                   loader(registry, schedulerRegistry, directories);

        expect(that % loader.refusedRegistrations().empty()) << "two versions of one key do not collide";
        expect(loader.blockVersions(kPluginKey) == std::vector<block::Version>{1U, 2U}) << "each plugin's version is listed";

        const std::shared_ptr<BlockModel> newest = loader.instantiate(kPluginKey);
        expect(fatal(newest != nullptr));
        expect(eq(std::string(newest->typeName()), std::string("precedence::Newer")));
        const std::shared_ptr<BlockModel> older = loader.instantiatePinned(kPluginKey, 1U);
        expect(fatal(older != nullptr));
        expect(eq(std::string(older->typeName()), std::string("precedence::First"))) << "the plugin loaded second holds version 1";

        const std::optional<property_map> attributes = loader.blockAttributes(kPluginKey);
        expect(fatal(attributes.has_value()));
        expect(eq(block::attributesFromMap(*attributes).version, block::Version{2U})) << "the newest version's map answers";
    };

    "a plugin that offers a key the program holds at that version is refused, and its other version coexists"_test = [] {
        gr::BlockRegistry     registry;
        gr::SchedulerRegistry schedulerRegistry;
        expect(registry.insert<Framework>(std::format("={}", kPluginKey)));

        const std::vector<std::string> directories{directoryOf("precedence_plugin_first"), directoryOf("precedence_plugin_newer")};
        PluginLoader                   loader(registry, schedulerRegistry, directories);

        const std::vector<RefusedRegistration> refused = loader.refusedRegistrations();
        expect(fatal(eq(refused.size(), 1UZ))) << "the plugin at version 2 is not refused";
        expect(eq(refused.front().key, std::string(kPluginKey)));
        expect(eq(refused.front().file, pluginFile(loader, "precedence_plugin_first")));
        expect(eq(refused.front().holder, gr::detail::fileHoldingCode(&kAddressInThisProgram))) << "the program holds the key";

        const std::shared_ptr<BlockModel> framework = loader.instantiatePinned(kPluginKey, 1U);
        expect(fatal(framework != nullptr));
        expect(eq(std::string(framework->typeName()), gr::meta::type_name<Framework>()));
        const std::shared_ptr<BlockModel> newest = loader.instantiate(kPluginKey);
        expect(fatal(newest != nullptr));
        expect(eq(std::string(newest->typeName()), std::string("precedence::Newer")));
        expect(loader.blockVersions(kPluginKey) == std::vector<block::Version>{1U, 2U});
    };

    "of two block libraries that register one alias at one version, the first mapped holds it, alias and all"_test = [] {
        // GR_TEST_PRECEDENCE_ORDER=reversed maps the second library first
        const char*            order     = std::getenv("GR_TEST_PRECEDENCE_ORDER");
        const bool             reversed  = std::string_view(order == nullptr ? "" : order) == "reversed";
        const std::string_view ahead     = reversed ? "precedence_library_second" : "precedence_library_first";
        const std::string_view behind    = reversed ? "precedence_library_first" : "precedence_library_second";
        const std::string      aheadKey  = reversed ? "gr::testing::PrecedenceSecond" : "gr::testing::PrecedenceFirst";
        const std::string      behindKey = reversed ? "gr::testing::PrecedenceFirst" : "gr::testing::PrecedenceSecond";

        const std::vector<std::string> directories{directoryOf(ahead), directoryOf(behind)};
        PluginLoader                   loader(gr::globalBlockRegistry(), gr::globalSchedulerRegistry(), directories);
        expect(that % loader.failedPlugins().empty()) << "a refused registration fails no file";

        const PluginLoader::BlockLibrary& aheadLibrary  = blockLibrary(loader, ahead);
        const PluginLoader::BlockLibrary& behindLibrary = blockLibrary(loader, behind);
        expect(eq(aheadLibrary.nBlockRegistrations, 2UZ)) << "its type name and its alias";
        expect(eq(behindLibrary.nBlockRegistrations, 1UZ)) << "its type name alone";

        const std::vector<RefusedRegistration> refused = refusalsOf(loader, kLibraryKey);
        expect(fatal(eq(refused.size(), 1UZ)));
        expect(eq(refused.front().version, block::Version{1U}));
        expect(eq(refused.front().file, behindLibrary.file));
        expect(eq(refused.front().holder, aheadLibrary.file));

        std::shared_ptr<BlockModel> held = gr::globalBlockRegistry().create(kLibraryKey, {});
        expect(fatal(held != nullptr));
        expect(eq(std::string(held->typeName()), aheadKey));
        expect(eq(gr::globalBlockRegistry().typeName(held), std::string(kLibraryKey))) << "the refusal leaves the holder's alias";

        std::shared_ptr<BlockModel> other = gr::globalBlockRegistry().create(behindKey, {});
        expect(fatal(other != nullptr)) << "the later library's own type name is registered";
        expect(eq(gr::globalBlockRegistry().typeName(other), behindKey)) << "a refused alias names no block";
    };
};

int main() { /* not needed for UT */ }
