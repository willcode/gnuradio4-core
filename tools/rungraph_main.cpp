// rungraph - load a graph file, run it, and report what the run was asked to report.
//
// No block, setting or connection is named in this file. The graph file names the blocks, the plugin directories supply
// them through a loader of the program's own, gr::RuntimeGraph loads the graph and gr::Runtime runs it. A chain that
// changes needs no program rebuilt.

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <format>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <print>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <gnuradio-4.0/BlockLookup.hpp>
#include <gnuradio-4.0/BlockRegistry.hpp>
#include <gnuradio-4.0/PluginLoader.hpp>
#include <gnuradio-4.0/Runtime.hpp>
#include <gnuradio-4.0/YamlPmt.hpp>
#include <gnuradio-4.0/formatter/ValueFormatter.hpp>

namespace {

constexpr std::string_view kProgram = "rungraph";

constexpr std::string_view kUsage = R"(rungraph - run a GNU Radio 4 graph file

Usage: rungraph --graph <file> [options]

  --graph <file>     the graph to run, in the GRC YAML dialect; - reads it from standard input
  --plugin-dir <dir> a directory to load plugins and block libraries from; repeatable
  --seconds <s>      stop the graph after <s> seconds; without it the run ends when the graph does
  --show <name>      print the settings of the block named <name> when the run ends; repeatable
  --set, -s <key>=<value>
                     set one setting before the run; repeatable
  --verbose          list what each plugin directory loaded and the keys it brought
  --help, -h         this text

The blocks come from the directories named by --plugin-dir, from GNURADIO4_PLUGIN_DIRECTORIES,
the colon-separated list the framework's own plugin loader reads, and from the plugin directory
of this installation, which is always searched. A directory named twice is searched once. A block
type two plugins supply comes from the one searched first.

A settings map holds what the last refresh put there, so the settings --show prints are read
after the run has ended and the block has been asked to refresh them: a counter a block keeps
as a readable member is then current as of the last sample it processed.

--show and the <block> of --set name a block by the unique_name the graph file gives it, or by its
name when no other block carries that name.

A bare key of --set names a setting of the scheduler, and a key of the form <block>.<key> names a
setting of that block. The split is at the last dot before the '=', so
a block name may hold a dot and a setting key never does. rungraph reads the value the way a graph
file's parameter value is read, so a type tag applies: -s timeout_ms=50, -s
'shift.frequency_shift=!!float32 -100000'. One --set carries one setting, and a value that holds a
second key is refused. The last --set of a key wins.

SIGINT and SIGTERM stop the graph as a --seconds bound does. The first error a block or the
scheduler reports stops it too.

Exit status is 0 when the run stopped cleanly, 1 when a plugin directory could not be searched,
when the graph could not be read, loaded or run, when a block or the scheduler reported an error,
and when a --set or --show names a block or a block setting the graph does not hold, and 2 when the
command line could not be used, a scheduler setting the scheduler does not declare or refuses
included.
)";

// how often the wait for the end of the run looks at the signal flag, the bound and the errors the graph reports
constexpr std::chrono::milliseconds kPollInterval{20};

std::atomic<bool> gStopRequested{false};

extern "C" void onSignal(int) { gStopRequested.store(true, std::memory_order_relaxed); }

// one --set
struct Setting {
    std::string block; // the block the setting belongs to; empty names the scheduler
    std::string key;
    std::string value; // the text after the '=', read as a graph file's parameter value is read
};

struct Options {
    std::string              graph; // the graph file, or "-" for standard input
    std::vector<std::string> pluginDirectories;
    std::vector<std::string> show;          // the blocks whose settings are printed when the run ends
    std::vector<Setting>     settings;      // in command-line order, so that the last of a key wins
    double                   seconds = 0.0; // 0 runs until the graph ends or a signal arrives
    bool                     verbose = false;
    bool                     help    = false;
};

// a run bound, or nothing when the text is not one positive number
[[nodiscard]] std::optional<double> secondsOf(std::string_view text) {
    double     value  = 0.0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || !(value > 0.0)) {
        return std::nullopt;
    }
    return value;
}

// one <key>=<value>, or nothing when the text is not one; a key carrying a dot names a block and the key under it
[[nodiscard]] std::optional<Setting> settingOf(std::string_view text) {
    const std::size_t assignment = text.find('=');
    if (assignment == std::string_view::npos || assignment == 0UZ) {
        return std::nullopt;
    }
    const std::string_view target = text.substr(0UZ, assignment);
    const std::string_view value  = text.substr(assignment + 1UZ);
    const std::size_t      dot    = target.rfind('.');
    if (dot == std::string_view::npos) {
        return Setting{std::string(), std::string(target), std::string(value)};
    }
    if (dot == 0UZ || dot + 1UZ == target.size()) {
        return std::nullopt;
    }
    return Setting{std::string(target.substr(0UZ, dot)), std::string(target.substr(dot + 1UZ)), std::string(value)};
}

// the command line, or nothing when it cannot be used; every refusal is reported as it is found
[[nodiscard]] std::optional<Options> parse(std::span<const std::string_view> arguments) {
    Options options;
    for (std::size_t index = 0UZ; index < arguments.size();) {
        const std::string_view argument = arguments[index];
        if (argument == "--help" || argument == "-h") {
            options.help = true;
            ++index;
            continue;
        }
        if (argument == "--verbose") {
            options.verbose = true;
            ++index;
            continue;
        }
        // the option is recognized before its value is asked for, so that an unknown option in the last position is
        // reported as unknown rather than as one missing a value
        if (argument != "--graph" && argument != "--plugin-dir" && argument != "--show" && argument != "--seconds" && argument != "--set" && argument != "-s") {
            std::println(stderr, "{}: unknown option '{}'", kProgram, argument);
            return std::nullopt;
        }
        if (index + 1UZ >= arguments.size()) {
            std::println(stderr, "{}: {} needs a value", kProgram, argument);
            return std::nullopt;
        }
        const std::string_view value = arguments[index + 1UZ];
        index += 2UZ;
        if (argument == "--graph") {
            options.graph.assign(value);
        } else if (argument == "--plugin-dir") {
            options.pluginDirectories.emplace_back(value);
        } else if (argument == "--show") {
            options.show.emplace_back(value);
        } else if (argument == "--set" || argument == "-s") {
            std::optional<Setting> setting = settingOf(value);
            if (!setting.has_value()) {
                std::println(stderr, "{}: --set takes <key>=<value> or <block>.<key>=<value>, not '{}'", kProgram, value);
                return std::nullopt;
            }
            options.settings.push_back(std::move(*setting));
        } else {
            const std::optional<double> seconds = secondsOf(value);
            if (!seconds.has_value()) {
                std::println(stderr, "{}: --seconds takes one positive number of seconds, not '{}'", kProgram, value);
                return std::nullopt;
            }
            options.seconds = *seconds;
        }
    }
    if (!options.help && options.graph.empty()) {
        std::println(stderr, "{}: --graph names the graph file to run", kProgram);
        return std::nullopt;
    }
    return options;
}

// the graph file's text, or nothing when it could not be read
[[nodiscard]] std::optional<std::string> readGraph(const std::string& path) {
    std::ostringstream text;
    if (path == "-") {
        text << std::cin.rdbuf();
        return text.str();
    }
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        std::println(stderr, "{}: {} could not be read", kProgram, path);
        return std::nullopt;
    }
    text << file.rdbuf();
    return text.str();
}

// Searches the directories --plugin-dir names, the environment's and the installation's, in that order, with `loader`,
// and returns what each held. A directory the search could not read is reported, and the answer is then nothing.
[[nodiscard]] std::optional<std::vector<gr::RuntimePluginDirectory>> loadPlugins(gr::PluginLoader& loader, const std::vector<std::string>& fromCommandLine) {
    std::vector<std::string> paths;
    for (const gr::tools::Directory& directory : gr::tools::searchDirectories(fromCommandLine, gr::installedPluginDirectory())) {
        paths.push_back(directory.path);
    }
    std::vector<gr::RuntimePluginDirectory> loaded = gr::RuntimeGraph::loadPlugins(loader, paths);
    bool                                    read   = true;
    for (const gr::RuntimePluginDirectory& directory : loaded) {
        if (!directory.error.empty()) {
            std::println(stderr, "{}: {} could not be searched: {}", kProgram, directory.directory, directory.error);
            read = false;
        }
    }
    if (!read) {
        return std::nullopt;
    }
    return loaded;
}

// what the directories hold: the files that loaded, the files that did not, and the block keys they brought
void reportPlugins(const std::vector<gr::RuntimePluginDirectory>& directories) {
    std::vector<std::string> brought;
    for (const gr::RuntimePluginDirectory& directory : directories) {
        std::println(stderr, "{}: searching {}", kProgram, directory.directory);
        for (const gr::RuntimePluginDirectory::BlockLibrary& library : directory.blockLibraries) {
            std::println(stderr, "{}: loaded {} ({} block registration(s))", kProgram, library.file, library.nBlockRegistrations);
        }
        for (const auto& [file, reason] : directory.failed) {
            std::println(stderr, "{}: {} did not load: {}", kProgram, file, reason);
        }
        for (const std::string& file : directory.skipped) {
            std::println(stderr, "{}: {} was not opened; its name only reads as a shared object", kProgram, file);
        }
        std::ranges::copy(directory.blockTypes, std::back_inserter(brought));
    }
    std::ranges::sort(brought);
    brought.erase(std::ranges::unique(brought).begin(), brought.end());
    std::println(stderr, "{}: the load brought {} block key(s):", kProgram, brought.size());
    for (const std::string& key : brought) {
        std::println(stderr, "{}:   {}", kProgram, key);
    }
}

// One block's readable settings, one `label: key = value` line each, in key order, where `label` is the name --show
// gave.
//
// A settings map holds what the last refresh put there, and the framework refreshes one when a settings change is
// applied. A block whose readable members move while it runs reports what it started with until it is asked. The run
// is over by the time this is called, and a member is then read as the block left it.
void showSettings(std::string_view label, gr::BlockHandle& block) {
    block.updateActiveParameters();
    std::vector<std::pair<std::string, std::string>> lines;
    for (const auto& [key, value] : block.get()) {
        lines.emplace_back(std::string(key.begin(), key.end()), std::format("{}", value));
    }
    std::ranges::sort(lines);
    for (const auto& [key, value] : lines) {
        std::println("{}: {} = {}", label, key, value);
    }
    std::fflush(stdout);
}

// the scheduler's settings, and the settings of each block named, which the graph file is read with
struct StagedSettings {
    gr::property_map                                     scheduler;
    std::map<std::string, gr::property_map, std::less<>> blocks;
};

// The settings the --set arguments stand for, or nothing when one of the values cannot be read or stands for more
// than the key it was given for.
//
// Each pair becomes a one-entry YAML document and the framework's own reader gives the value its type, so the text
// after the '=' means here what the same text means as a parameter of a graph file: a bare `true` is a boolean, a bare
// number an integer, and a tagged `!!float32 1.5` a float. The document has to come back holding that one key alone:
// text carrying a second line would otherwise reach the run as a setting the caller never wrote. A later setting of a
// key overwrites an earlier one.
[[nodiscard]] std::optional<StagedSettings> stagedSettingsOf(const std::vector<Setting>& settings) {
    StagedSettings staged;
    for (const Setting& setting : settings) {
        const auto parsed = gr::pmt::yaml::deserialize(std::format("{}: {}", setting.key, setting.value));
        if (!parsed.has_value()) {
            std::println(stderr, "{}: the value of --set {} could not be read: {}", kProgram, setting.key, parsed.error().message);
            return std::nullopt;
        }
        if (parsed->size() != 1UZ || std::string_view(parsed->begin()->first) != setting.key) {
            std::println(stderr, "{}: --set takes one value for one key, and the value of --set {} holds more than one key", kProgram, setting.key);
            return std::nullopt;
        }
        gr::property_map* target = setting.block.empty() ? std::addressof(staged.scheduler) : std::addressof(staged.blocks[setting.block]);
        for (const auto& [parsedKey, parsedValue] : *parsed) {
            target->insert_or_assign(parsedKey, parsedValue);
        }
    }
    return staged;
}

// Whether the scheduler takes these settings, asked of one built over an empty graph for the question alone.
//
// A setting the scheduler will not take is a command line problem, and the answer is wanted before the graph file is
// read. Each name is checked against the settings the scheduler declares, and its value is then staged, which converts
// it to its setting's type without applying it.
[[nodiscard]] bool schedulerTakesSettings(const std::shared_ptr<gr::PluginLoader>& loader, const gr::property_map& settings) {
    std::expected<gr::Runtime, gr::RuntimeError> probe = gr::Runtime::create(gr::RuntimeGraph{loader});
    if (!probe.has_value()) {
        std::println(stderr, "{}: the scheduler could not be built: {}", kProgram, probe.error().message);
        return false;
    }
    gr::BlockHandle             scheduler = probe->scheduler();
    const std::set<std::string> declared  = scheduler.writableMembers();
    for (const auto& [key, value] : settings) {
        const std::string name(key.begin(), key.end());
        if (!declared.contains(name)) {
            std::println(stderr, "{}: the scheduler declares no setting named '{}'", kProgram, name);
            return false;
        }
        if (const std::expected<gr::property_map, gr::RuntimeError> staged = scheduler.setStaged(gr::property_map{{key, value}}); !staged.has_value()) {
            std::println(stderr, "{}: the scheduler setting '{}' could not be applied: {}", kProgram, name, staged.error().message);
            return false;
        }
    }
    return true;
}

// the names of the scheduler settings --set gave, quoted and joined for a message
[[nodiscard]] std::string settingNames(const gr::property_map& settings) {
    std::string names;
    for (const auto& [key, value] : settings) {
        names += std::format("{}'{}'", names.empty() ? "" : ", ", std::string_view(key.data(), key.size()));
    }
    return names;
}

// The blocks --show names, or nothing when one of them is not in the graph; each missing name is reported. A handle
// keeps its block when the graph moves into the scheduler.
[[nodiscard]] std::optional<std::vector<gr::BlockHandle>> shownBlocks(const gr::RuntimeGraph& graph, const std::vector<std::string>& names) {
    std::vector<gr::BlockHandle> shown;
    bool                         allFound = true;
    for (const std::string& wanted : names) {
        std::expected<gr::BlockHandle, gr::RuntimeError> found = graph.find(wanted, gr::RuntimeGraph::Recursive::No);
        if (found.has_value()) {
            shown.push_back(std::move(*found));
            continue;
        }
        allFound                 = false;
        const bool nameIsInGraph = std::ranges::any_of(graph.blocks(), [&wanted](const gr::BlockHandle& block) { return block.name() == wanted; });
        if (nameIsInGraph) {
            std::println(stderr, "{}: {}", kProgram, found.error().message);
        } else {
            std::println(stderr, "{}: the graph holds no block named {}", kProgram, wanted);
        }
    }
    if (!allFound) {
        return std::nullopt;
    }
    return shown;
}

// the errors a run reported: the first as the output gives it, and how many there were
struct ReportedErrors {
    std::size_t count = 0UZ;
    std::string first; // a block's error in the words of the run it failed, or the scheduler's own message
};

// Takes the errors the graph has reported since the last call into `errors` and prints the first of the run, one line
// without its source location. The later ones follow from the stop the first causes, and they are counted alone.
void takeReportedErrors(gr::Runtime& runtime, ReportedErrors& errors) {
    const std::string scheduler(runtime.scheduler().uniqueName());
    for (std::vector<gr::RuntimeEvent> events = runtime.pollEvents(); !events.empty(); events = runtime.pollEvents()) {
        for (const gr::RuntimeEvent& event : events) {
            if (!event.isError) {
                continue;
            }
            if (errors.count++ > 0UZ) {
                continue;
            }
            if (event.source == scheduler) {
                std::println(stderr, "{}: the scheduler reports an error on '{}': {}", kProgram, event.endpoint, event.text);
                errors.first = event.text;
            } else {
                errors.first = std::format("block '{}' reports an error on '{}': {}", event.source, event.endpoint, event.text);
                std::println(stderr, "{}: {}", kProgram, errors.first);
            }
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string_view> arguments;
    arguments.reserve(static_cast<std::size_t>(argc > 1 ? argc - 1 : 0));
    for (int i = 1; i < argc; ++i) {
        arguments.emplace_back(argv[i]);
    }

    const std::optional<Options> parsed = parse(arguments);
    if (!parsed.has_value()) {
        std::print(stderr, "{}", kUsage);
        return 2;
    }
    const Options& options = *parsed;
    if (options.help) {
        std::print("{}", kUsage);
        return 0;
    }

    const std::optional<StagedSettings> staged = stagedSettingsOf(options.settings);
    if (!staged.has_value()) {
        std::print(stderr, "{}", kUsage);
        return 2;
    }

    // The program's loader searches its directories before anything builds a graph. The first graph builds the
    // process's loader over GNURADIO4_PLUGIN_DIRECTORIES, and a block library that loader opened first registers
    // nothing when this loader opens it again.
    const std::shared_ptr<gr::PluginLoader>                      loader = std::make_shared<gr::PluginLoader>(gr::globalBlockRegistry(), gr::globalSchedulerRegistry(), std::span<const std::string>{});
    const std::optional<std::vector<gr::RuntimePluginDirectory>> loaded = loadPlugins(*loader, options.pluginDirectories);
    if (!loaded.has_value()) {
        return 1;
    }
    if (options.verbose) {
        reportPlugins(*loaded);
    }

    // the scheduler's settings are settled before the graph is read, so that one it will not take is reported as the
    // command line problem it is rather than after a file has been loaded
    if (!schedulerTakesSettings(loader, staged->scheduler)) {
        std::print(stderr, "{}", kUsage);
        return 2;
    }

    const std::optional<std::string> document = readGraph(options.graph);
    if (!document.has_value()) {
        return 1;
    }

    // each block reads its --set values where it reads the graph file's own, so they are the values its start() sees;
    // the reader refuses a graph file for a key nothing supplies, a setting a block does not declare, a value of the
    // wrong type or a port that is not there, and a --set for a block the file does not hold
    std::expected<gr::RuntimeGraph, gr::RuntimeError> graph = gr::RuntimeGraph::fromYaml(loader, *document, staged->blocks);
    if (!graph.has_value()) {
        std::println(stderr, "{}: {} did not load: {}", kProgram, options.graph, graph.error().message);
        return 1;
    }

    std::optional<std::vector<gr::BlockHandle>> shown = shownBlocks(*graph, options.show);
    if (!shown.has_value()) {
        return 1;
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    std::expected<gr::Runtime, gr::RuntimeError> runtime = gr::Runtime::create(std::move(*graph), gr::Runtime::kDefaultScheduler, staged->scheduler);
    if (!runtime.has_value()) {
        if (staged->scheduler.empty()) {
            std::println(stderr, "{}: the scheduler could not be built: {}", kProgram, runtime.error().message);
        } else {
            std::println(stderr, "{}: the scheduler refused the settings {}: {}", kProgram, settingNames(staged->scheduler), runtime.error().message);
        }
        return 1;
    }
    if (const std::optional<gr::RuntimeError> refused = runtime->start(); refused.has_value()) {
        std::println(stderr, "{}: the graph did not start: {}", kProgram, refused->message);
        return 1;
    }

    const auto     start        = std::chrono::steady_clock::now();
    const auto     boundReached = [&options, start] { return options.seconds > 0.0 && std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() >= options.seconds; };
    ReportedErrors errors;
    bool           endedItself = false;
    while (!endedItself && errors.count == 0UZ && !gStopRequested.load(std::memory_order_relaxed) && !boundReached()) {
        endedItself = runtime->waitFor(kPollInterval);
        takeReportedErrors(*runtime, errors);
    }
    const bool stoppedOnError = errors.count > 0UZ && !endedItself;
    if (!endedItself) {
        // the stop is requested once, and each later call waits for that same stop
        while (!runtime->stopFor(kPollInterval)) {
            takeReportedErrors(*runtime, errors);
        }
    }
    runtime->wait();
    takeReportedErrors(*runtime, errors);
    if (errors.count > 1UZ) {
        std::println(stderr, "{}: {} more error(s) followed the first", kProgram, errors.count - 1UZ);
    }

    // a run the first error failed returns that error as its result, and the line is printed once
    const std::expected<void, gr::RuntimeError> result = runtime->result();
    if (!result.has_value() && result.error().message != errors.first) {
        std::println(stderr, "{}: the graph stopped: {}", kProgram, result.error().message);
    }
    for (std::size_t i = 0UZ; i < shown->size(); ++i) {
        showSettings(options.show[i], (*shown)[i]);
    }
    std::println(stderr, "{}: {}", kProgram, endedItself ? "the graph ended on its own" : (stoppedOnError ? "the graph was stopped on the first error reported" : "the graph was stopped before it ended"));
    return result.has_value() && errors.count == 0UZ ? 0 : 1;
}
