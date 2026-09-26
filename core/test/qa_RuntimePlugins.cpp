#include <boost/ut.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <vector>

#include <gnuradio-4.0/PluginLoader.hpp>
#include <gnuradio-4.0/Runtime.hpp>

#include "build_configure.hpp"

/**
 * RuntimeGraph::loadPlugins and RuntimeGraph::fromYaml over core's own test plugins and block libraries, through the
 * process's loader and through a loader the program builds.
 *
 * The process's loader is built on first use from GNURADIO4_PLUGIN_DIRECTORIES. The suite sets that variable to the
 * block-library directory before any case runs, so that loader searches the block libraries at its construction, and a
 * case's own call searches the plugin directory.
 */
namespace qa_runtime_plugins {

constexpr std::string_view kDoublerKey = "test::library_doubler";
constexpr std::string_view kTriplerKey = "test::library_tripler";
constexpr std::string_view kSourceKey  = "good::fixed_source<float32>";

// the scheduler good_math_plugin supplies
constexpr std::string_view kPluginSchedulerName = "GoodMathScheduler";

// one block the plugin directory supplies
constexpr std::string_view kSourceDocument = "blocks:\n  - id: good::fixed_source<float32>\n    parameters:\n      name: source\n";

[[nodiscard]] std::string blockLibraryDirectory() { return std::string(TESTS_BINARY_PATH) + "/block_library"; }

[[nodiscard]] std::string pluginDirectory() { return std::string(TESTS_BINARY_PATH) + "/plugins"; }

[[nodiscard]] std::string skippedNamesDirectory() { return std::string(TESTS_BINARY_PATH) + "/block_library_skipped"; }

[[nodiscard]] std::string missingDirectory() { return std::string(TESTS_BINARY_PATH) + "/no_such_plugin_directory"; }

[[nodiscard]] std::string unreadableDirectory() { return std::string(TESTS_BINARY_PATH) + "/unreadable_plugin_directory"; }

[[nodiscard]] bool holds(const std::vector<std::string>& list, std::string_view entry) { return std::ranges::contains(list, entry); }

[[nodiscard]] bool anyContains(const std::vector<std::string>& files, std::string_view part) {
    return std::ranges::any_of(files, [part](const std::string& file) { return file.contains(part); });
}

// a loader over no directory, registering into the process's registries as the process's loader does
[[nodiscard]] std::shared_ptr<gr::PluginLoader> emptyLoader() { return std::make_shared<gr::PluginLoader>(gr::globalBlockRegistry(), gr::globalSchedulerRegistry(), std::span<const std::string>{}); }

} // namespace qa_runtime_plugins

const boost::ut::suite<"RuntimePlugins"> runtimePluginTests = [] {
    using namespace boost::ut;
    using namespace qa_runtime_plugins;

    expect(fatal(::setenv("GNURADIO4_PLUGIN_DIRECTORIES", blockLibraryDirectory().c_str(), 1) == 0));

    "a graph and its scheduler come from the loader the program built, which the graph and its runtime keep"_test = [] {
        expect(fatal(!holds(gr::RuntimeGraph::availableBlockTypes(), kSourceKey))) << "the process's loader holds the plugin directory already";

        std::shared_ptr<gr::PluginLoader>             loader  = emptyLoader();
        const std::vector<gr::RuntimePluginDirectory> reports = gr::RuntimeGraph::loadPlugins(*loader, std::vector<std::string>{pluginDirectory()});
        expect(fatal(eq(reports.size(), 1UZ)));
        expect(holds(reports.front().blockTypes, kSourceKey)) << "the program's loader does not report the plugin's block type";

        const auto throughProcess = gr::RuntimeGraph::fromYaml(kSourceDocument);
        expect(fatal(!throughProcess.has_value())) << "the process's loader resolved a block only the program's loader holds";
        expect(throughProcess.error().message.contains(kSourceKey)) << "the refusal does not name the block type: " << throughProcess.error().message;

        const std::vector<std::string> schedulers = loader->availableSchedulers();
        const auto                     scheduler  = std::ranges::find_if(schedulers, [](const std::string& name) { return name.contains(kPluginSchedulerName); });
        expect(fatal(scheduler != schedulers.end())) << "the program's loader holds no plugin scheduler";
        expect(!holds(gr::RuntimeGraph::availableSchedulerTypes(), *scheduler)) << "the process's loader holds the plugin scheduler";
        expect(!gr::Runtime::create(gr::RuntimeGraph{}, *scheduler).has_value()) << "the process's loader built the plugin scheduler";
        expect(gr::Runtime::create(gr::RuntimeGraph{loader}, *scheduler).has_value()) << "a graph over the program's loader does not reach its scheduler";

        const std::weak_ptr<gr::PluginLoader> watched = loader;
        {
            auto graph = gr::RuntimeGraph::fromYaml(std::move(loader), kSourceDocument);
            expect(fatal(graph.has_value())) << (graph ? std::string{} : graph.error().message);
            expect(graph->find("source").has_value()) << "the loaded block is not in the graph";
            expect(!watched.expired()) << "the graph does not keep its loader";

            auto runtime = gr::Runtime::create(std::move(*graph), *scheduler);
            expect(fatal(runtime.has_value())) << (runtime ? std::string{} : runtime.error().message);
            expect(!watched.expired()) << "the runtime does not keep its graph's loader";
        }
        expect(watched.expired()) << "the loader outlives the graph and the runtime that held it";
    };

    "each directory's report lists what the loader holds from it, from the construction's search and from the call's"_test = [] {
        const std::vector<std::string> before = gr::RuntimeGraph::availableBlockTypes();
        expect(holds(before, kDoublerKey)) << "the construction did not search the environment's directory";
        expect(!holds(before, kSourceKey)) << "the plugin directory was searched before the call";

        const std::vector<std::string>                directories{blockLibraryDirectory(), pluginDirectory(), skippedNamesDirectory(), missingDirectory()};
        const std::vector<gr::RuntimePluginDirectory> reports = gr::RuntimeGraph::loadPlugins(directories);
        expect(fatal(eq(reports.size(), directories.size())));
        for (std::size_t i = 0UZ; i < directories.size(); ++i) {
            expect(eq(reports[i].directory, directories[i])) << "the reports are not in the order the directories were given";
            expect(reports[i].error.empty()) << reports[i].error;
        }

        const gr::RuntimePluginDirectory& libraries = reports[0];
        expect(fatal(eq(libraries.blockLibraries.size(), 2UZ))) << "the libraries the loader found at its construction are not reported";
        for (const gr::RuntimePluginDirectory::BlockLibrary& library : libraries.blockLibraries) {
            expect(library.nBlockRegistrations > 0UZ) << library.file;
        }
        expect(holds(libraries.blockTypes, kDoublerKey) && holds(libraries.blockTypes, kTriplerKey)) << "a block library's keys are not reported";
        expect(libraries.plugins.empty() && libraries.failed.empty() && libraries.skipped.empty());

        const gr::RuntimePluginDirectory& plugins = reports[1];
        expect(anyContains(plugins.plugins, "GoodBasePlugin")) << "a plugin that loaded is not reported";
        expect(std::ranges::any_of(plugins.failed, [](const auto& failure) { return failure.first.contains("bad_plugin") && !failure.second.empty(); })) << "a plugin that did not load is not reported with its reason";
        expect(holds(plugins.blockTypes, kSourceKey)) << "a plugin's block types are not reported";
        expect(!holds(plugins.blockTypes, kDoublerKey)) << "another directory's block type is reported";
        expect(plugins.blockLibraries.empty());

        expect(anyContains(reports[2].skipped, "libnotopened.so.1")) << "a file the scan did not open is not reported";
        expect(reports[3].plugins.empty() && reports[3].blockLibraries.empty() && reports[3].failed.empty() && reports[3].skipped.empty() && reports[3].blockTypes.empty()) << "a directory that does not exist reports files";

        const std::vector<gr::RuntimePluginDirectory> again = gr::RuntimeGraph::loadPlugins(std::vector<std::string>{pluginDirectory()});
        expect(fatal(eq(again.size(), 1UZ)));
        expect(again.front().plugins == plugins.plugins && again.front().blockTypes == plugins.blockTypes) << "a second search of a directory changes its report";

        expect(holds(gr::RuntimeGraph::availableBlockTypes(), kSourceKey)) << "a reported block type is not available";
    };

    "a directory the search cannot read is reported with the reason, and the search goes on"_test = [] {
        namespace fs = std::filesystem;
        const fs::path unreadable(unreadableDirectory());
        std::ignore = fs::create_directory(unreadable);
        fs::permissions(unreadable, fs::perms::none);

        std::shared_ptr<gr::PluginLoader>             loader  = emptyLoader();
        const std::vector<gr::RuntimePluginDirectory> reports = gr::RuntimeGraph::loadPlugins(*loader, std::vector<std::string>{unreadable.string(), pluginDirectory()});

        std::error_code ignored;
        fs::permissions(unreadable, fs::perms::owner_all, ignored);
        fs::remove(unreadable, ignored);

        expect(fatal(eq(reports.size(), 2UZ)));
        expect(!reports[0].error.empty()) << "an unreadable directory reports no error";
        expect(reports[0].plugins.empty() && reports[0].blockTypes.empty());
        expect(reports[1].error.empty()) << reports[1].error;
        expect(anyContains(reports[1].plugins, "GoodBasePlugin")) << "the search stopped at the unreadable directory";
    };

    "a graph document reaches the block types a loaded directory supplies"_test = [] {
        std::ignore      = gr::RuntimeGraph::loadPlugins(std::vector<std::string>{pluginDirectory()});
        const auto graph = gr::RuntimeGraph::fromYaml(kSourceDocument);
        expect(fatal(graph.has_value())) << (graph ? std::string{} : graph.error().message);
        expect(graph->find("source").has_value()) << "the loaded block is not in the graph";
    };

    "the library names the plugin directory of the installation it was built for"_test = [] { expect(eq(gr::installedPluginDirectory(), std::string_view(GR_TEST_INSTALLED_PLUGIN_DIRECTORY))); };
};

int main() { /* not needed for UT */ }
