#include <boost/ut.hpp>

#include <array>
#include <cstdio>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#ifndef _WIN32
#include <sys/wait.h>
#endif

/**
 * rungraph, driven as the program a caller runs.
 *
 * The tool's contract is its exit status and what it prints, and neither is visible from inside the process, so
 * every case here runs the built executable: a graph that would not end by itself, bounded by --seconds; the
 * settings --show prints when the run is over; a scheduler setting and a block setting taken and each refused; a
 * recipe composite's exported parameter taken and its interior's setting refused; a command line that cannot be used;
 * and a graph file that cannot be read.
 */
namespace qa_rungraph {

#ifdef _WIN32
constexpr auto openPipe  = _popen;
constexpr auto closePipe = _pclose;
#else
constexpr auto openPipe  = popen;
constexpr auto closePipe = pclose;
#endif

struct Result {
    int         exitCode = -1;
    std::string output; // standard output and standard error together, in the order the run wrote them
};

// runs the tool with `arguments` and collects what it wrote and the status it exited with
[[nodiscard]] Result run(const std::vector<std::string>& arguments) {
    std::string command = std::format("\"{}\"", GR_TOOLS_RUNGRAPH);
    for (const std::string& argument : arguments) {
        command += std::format(" \"{}\"", argument);
    }
    command += " 2>&1";

    Result     result;
    std::FILE* pipe = openPipe(command.c_str(), "r");
    if (pipe == nullptr) {
        return result;
    }
    std::array<char, 4096UZ> buffer{};
    while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        result.output.append(buffer.data());
    }
    const int status = closePipe(pipe);
#ifdef _WIN32
    result.exitCode = status;
#else
    result.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
    return result;
}

#ifdef GR_TOOLS_CORE_TEST_PLUGINS
constexpr std::string_view kGraphFile{GR_TOOLS_TEST_ASSETS "/source_to_sink.yaml"};
constexpr std::string_view kSettingsChainFile{GR_TOOLS_TEST_ASSETS "/settings_chain.yaml"};
constexpr std::string_view kStartChainFile{GR_TOOLS_TEST_ASSETS "/start_chain.yaml"};
constexpr std::string_view kUnnamedResourceFile{GR_TOOLS_TEST_ASSETS "/unnamed_resource_chain.yaml"};

// the graph, the plugins that supply its blocks, and a bound short enough for a test
[[nodiscard]] std::vector<std::string> boundedRun() { return {"--graph", std::string(kGraphFile), "--plugin-dir", GR_TOOLS_CORE_TEST_PLUGINS, "--seconds", "0.5"}; }

// the same, over the chain whose two middle blocks open a resource when they start
[[nodiscard]] std::vector<std::string> startChainRun() { return {"--graph", std::string(kStartChainFile), "--plugin-dir", GR_TOOLS_CORE_TEST_PLUGINS, "--seconds", "0.5"}; }

// the same, over a chain whose middle block names no resource and fails to start
[[nodiscard]] std::vector<std::string> unnamedResourceRun() { return {"--graph", std::string(kUnnamedResourceFile), "--plugin-dir", GR_TOOLS_CORE_TEST_PLUGINS, "--seconds", "0.5"}; }

// the same, over the four-block chain the settings cases set a value on
[[nodiscard]] std::vector<std::string> settingsChainRun() { return {"--graph", std::string(kSettingsChainFile), "--plugin-dir", GR_TOOLS_CORE_TEST_PLUGINS, "--seconds", "0.5"}; }

constexpr std::string_view kRecipeChainFile{GR_TOOLS_TEST_ASSETS "/recipe_chain.yaml"};
constexpr std::string_view kRecipeDirectory{GR_TOOLS_TEST_ASSETS "/recipes"};

// the graph of one recipe composite, the plugins its interior blocks come from and the directory its recipe is read
// from; the graph ends by itself, and the bound only guards a run whose derived count never arrived
[[nodiscard]] std::vector<std::string> recipeChainRun() { return {"--graph", std::string(kRecipeChainFile), "--plugin-dir", GR_TOOLS_CORE_TEST_PLUGINS, "--plugin-dir", std::string(kRecipeDirectory), "--seconds", "5"}; }

constexpr std::string_view kRecipeStartFile{GR_TOOLS_TEST_ASSETS "/recipe_start_chain.yaml"};

// the same, over the composite whose interior block opens the resource the composite's parameter names
[[nodiscard]] std::vector<std::string> recipeStartRun() { return {"--graph", std::string(kRecipeStartFile), "--plugin-dir", GR_TOOLS_CORE_TEST_PLUGINS, "--plugin-dir", std::string(kRecipeDirectory), "--seconds", "5"}; }
#endif

} // namespace qa_rungraph

const boost::ut::suite<"RunGraph"> runGraphTests = [] {
    using namespace boost::ut;
    using namespace qa_rungraph;

    "a command line that cannot be used is refused with the usage text"_test = [] {
        const Result unknown = run({"--no-such-option"});
        expect(eq(unknown.exitCode, 2)) << unknown.output;
        expect(unknown.output.contains("unknown option '--no-such-option'")) << unknown.output;
        expect(unknown.output.contains("Usage: rungraph")) << unknown.output;

        const Result withoutArguments = run({});
        expect(eq(withoutArguments.exitCode, 2)) << withoutArguments.output;
        expect(withoutArguments.output.contains("Usage: rungraph")) << withoutArguments.output;

        const Result withoutValue = run({"--graph"});
        expect(eq(withoutValue.exitCode, 2)) << withoutValue.output;
        expect(withoutValue.output.contains("--graph needs a value")) << withoutValue.output;
    };

    "a scheduler setting the scheduler does not declare is refused"_test = [] {
        const Result refused = run({"--graph", "unread.yaml", "--set", "no_such_setting=1"});
        expect(eq(refused.exitCode, 2)) << refused.output;
        expect(refused.output.contains("the scheduler declares no setting named 'no_such_setting'")) << refused.output;
        expect(!refused.output.contains("unread.yaml")) << "the command line is judged before the graph is read" << refused.output;

        const Result withoutValue = run({"--graph", "unread.yaml", "-s", "timeout_ms"});
        expect(eq(withoutValue.exitCode, 2)) << withoutValue.output;
        expect(withoutValue.output.contains("--set takes <key>=<value>")) << withoutValue.output;
    };

    "a --set whose value holds a second key is refused"_test = [] {
        const Result schedulerSetting = run({"--graph", "unread.yaml", "--set", "timeout_ms=10\nno_such_setting: 1"});
        expect(eq(schedulerSetting.exitCode, 2)) << schedulerSetting.output;
        expect(schedulerSetting.output.contains("the value of --set timeout_ms holds more than one key")) << schedulerSetting.output;
        expect(!schedulerSetting.output.contains("no_such_setting")) << "the second key never reaches the scheduler" << schedulerSetting.output;
        expect(!schedulerSetting.output.contains("unread.yaml")) << "the refusal comes before the graph is read" << schedulerSetting.output;

        const Result blockSetting = run({"--graph", "unread.yaml", "--set", "src.count=1\nother: 2"});
        expect(eq(blockSetting.exitCode, 2)) << blockSetting.output;
        expect(blockSetting.output.contains("the value of --set count holds more than one key")) << blockSetting.output;
        expect(!blockSetting.output.contains("unread.yaml")) << blockSetting.output;
    };

    "a graph file that cannot be read ends the run before anything is loaded"_test = [] {
        const Result missing = run({"--graph", "/gnuradio4-graph-file-that-does-not-exist.yaml"});
        expect(eq(missing.exitCode, 1)) << missing.output;
        expect(missing.output.contains("could not be read")) << missing.output;
    };

#ifdef GR_TOOLS_CORE_TEST_PLUGINS
    "a run of a graph that does not end by itself is bounded by --seconds and stops cleanly"_test = [] {
        const Result bounded = run(boundedRun());
        expect(eq(bounded.exitCode, 0)) << bounded.output;
        expect(bounded.output.contains("the graph was stopped before it ended")) << bounded.output;
    };

    "--show prints the settings of the block it names, and of no other"_test = [] {
        std::vector<std::string> arguments = boundedRun();
        arguments.emplace_back("--show");
        arguments.emplace_back("source");

        const Result shown = run(arguments);
        expect(eq(shown.exitCode, 0)) << shown.output;
        expect(shown.output.contains("source: event_count = ")) << "the block's own setting is reported" << shown.output;
        expect(!shown.output.contains("sink:")) << "only the block --show named is reported" << shown.output;
    };

    "--set reaches a block's own setting, and --show reads it back"_test = [] {
        std::vector<std::string> arguments = settingsChainRun();
        arguments.emplace_back("--set");
        arguments.emplace_back("source.event_count=1000");
        arguments.emplace_back("--show");
        arguments.emplace_back("source");

        const Result set = run(arguments);
        expect(eq(set.exitCode, 0)) << set.output;
        expect(set.output.contains("source: event_count = 1000")) << set.output;
        expect(set.output.contains("the graph ended on its own")) << "the bound the setting carries stopped the run" << set.output;
    };

    "--set gives a block the value its start() reads"_test = [] {
        std::vector<std::string> arguments = startChainRun();
        arguments.insert(arguments.end(), {"--set", "first.resource=named-on-the-command-line", "--show", "first"});

        const Result started = run(arguments);
        expect(eq(started.exitCode, 0)) << started.output;
        expect(started.output.contains(R"(first: resource_at_start = "named-on-the-command-line")")) << "start() read the value the command line set, not the graph file's" << started.output;
        expect(started.output.contains("the graph was stopped before it ended")) << started.output;
    };

    "a block whose start fails holds every --set value, and --show reads each back"_test = [] {
        std::vector<std::string> arguments = unnamedResourceRun();
        arguments.insert(arguments.end(), {"--set", "first.sample_rate=2000", "--set", "first.gain=0.5", "--set", "source.event_count=1000", "--show", "first", "--show", "source"});

        const Result failed = run(arguments);
        expect(eq(failed.exitCode, 1)) << failed.output;
        expect(failed.output.contains("no resource to open")) << "start() failed on the empty resource the graph file gives the block" << failed.output;
        expect(failed.output.contains("first: sample_rate = 2000")) << failed.output;
        expect(failed.output.contains("first: gain = 0.5")) << failed.output;
        expect(failed.output.contains("source: event_count = 1000")) << "the value set on a second block is held as well" << failed.output;
    };

    // a --set value is read where the graph file's own value is read, so a value the block refuses is refused as the
    // graph file's would be: a value outside the block's limits by the framework's own line, and a value its
    // settingsChanged() throws on when the scheduler initializes the block, before start() runs
    "a --set value the block refuses is refused as the graph file's value would be"_test = [] {
        std::vector<std::string> outOfLimits = startChainRun();
        outOfLimits.insert(outOfLimits.end(), {"--set", "first.gain=2", "--show", "first"});

        const Result refusedByLimits = run(outOfLimits);
        expect(refusedByLimits.output.contains("Failed to validate field 'gain' with value '2.000000'")) << refusedByLimits.output;
        expect(!refusedByLimits.output.contains("first: gain = 2")) << "the block keeps a value inside its limits" << refusedByLimits.output;

        std::vector<std::string> refusedBySettingsChanged = startChainRun();
        refusedBySettingsChanged.insert(refusedBySettingsChanged.end(), {"--set", "first.sample_rate=-1"});

        const Result refusedByBlock = run(refusedBySettingsChanged);
        expect(refusedByBlock.output.contains("init() throws: sample_rate -1 is not positive")) << refusedByBlock.output;
    };

    "a --set value the block forwards reaches the block downstream in place of the graph file's"_test = [] {
        std::vector<std::string> arguments = startChainRun();
        arguments.insert(arguments.end(), {"--set", "first.sample_rate=2000", "--set", "source.event_count=1000", "--show", "second"});

        const Result forwarded = run(arguments);
        expect(eq(forwarded.exitCode, 0)) << forwarded.output;
        expect(forwarded.output.contains("second: sample_rate = 2000")) << "the graph file gives the upstream block 1000" << forwarded.output;
        expect(forwarded.output.contains("the graph ended on its own")) << "the run ended after every sample had passed the downstream block" << forwarded.output;
    };

    "a block, or a block setting, the graph does not hold is refused"_test = [] {
        std::vector<std::string> unknownKey = settingsChainRun();
        unknownKey.emplace_back("-s");
        unknownKey.emplace_back("source.no_such_key=1");

        const Result refusedKey = run(unknownKey);
        expect(eq(refusedKey.exitCode, 1)) << refusedKey.output;
        expect(refusedKey.output.contains("block 'source' of type 'good::fixed_source<float32>' declares no setting named 'no_such_key'; the nearest are")) << refusedKey.output;

        std::vector<std::string> unknownBlock = settingsChainRun();
        unknownBlock.emplace_back("-s");
        unknownBlock.emplace_back("no_such_block.event_count=1");

        const Result refusedBlock = run(unknownBlock);
        expect(eq(refusedBlock.exitCode, 1)) << refusedBlock.output;
        expect(refusedBlock.output.contains("settings are given for block 'no_such_block', and the graph holds no block of that name")) << refusedBlock.output;
    };

    // the composite's one parameter derives a pair of interior counts: the sink reports the value of its count-th
    // sample, and the source ends the run at twice the count
    "--set reaches a recipe composite's exported parameter, the derived pair follows it, and --show reads it back"_test = [] {
        std::vector<std::string> asFiled = recipeChainRun();
        asFiled.emplace_back("--show");
        asFiled.emplace_back("counted");

        const Result filed = run(asFiled);
        expect(eq(filed.exitCode, 0)) << filed.output;
        expect(filed.output.contains("counted: count = 100\n")) << "the file's value is in force" << filed.output;
        expect(filed.output.contains("last value was: 100\n")) << "the sink counts to the file's value" << filed.output;
        expect(filed.output.contains("the graph ended on its own")) << filed.output;

        std::vector<std::string> changed = recipeChainRun();
        changed.emplace_back("--set");
        changed.emplace_back("counted.count=1000");
        changed.emplace_back("--show");
        changed.emplace_back("counted");

        const Result set = run(changed);
        expect(eq(set.exitCode, 0)) << set.output;
        expect(set.output.contains("counted: count = 1000\n")) << "--show reads back the value --set put in force" << set.output;
        expect(set.output.contains("last value was: 1000\n")) << "the sink's derived count followed the parameter" << set.output;
        expect(!set.output.contains("last value was: 100\n")) << "the file's value did not reach the sink" << set.output;
        expect(set.output.contains("the graph ended on its own")) << "the source's derived count followed it too" << set.output;
    };

    // the graph file names no resource, and the interior block refuses to start without one
    "--set gives a recipe composite's interior block the derived value its start() reads"_test = [] {
        std::vector<std::string> arguments = recipeStartRun();
        arguments.insert(arguments.end(), {"--set", "started.device=named-on-the-command-line", "--show", "started"});

        const Result started = run(arguments);
        expect(eq(started.exitCode, 0)) << started.output;
        expect(!started.output.contains("no resource to open")) << "the interior block's start() read the value derived from --set, not the graph file's empty one" << started.output;
        expect(started.output.contains(R"(started: device = "named-on-the-command-line")")) << started.output;
        expect(started.output.contains("the graph ended on its own")) << "the interior source ended the run" << started.output;
    };

    "a derived value a recipe composite's interior block refuses is refused as the graph file's would be"_test = [] {
        std::vector<std::string> arguments = recipeStartRun();
        arguments.insert(arguments.end(), {"--set", "started.device=named-on-the-command-line", "--set", "started.level=!!float32 2"});

        const Result refused = run(arguments);
        expect(refused.output.contains("Failed to validate field 'gain' with value '2")) << "the interior block's limits refuse the derived gain" << refused.output;
    };

    "a recipe composite refuses a name its recipe does not export, and a value its recipe cannot derive from"_test = [] {
        for (const std::string_view key : {"no_such_key", "event_count"}) {
            std::vector<std::string> arguments = recipeChainRun();
            arguments.emplace_back("-s");
            arguments.emplace_back(std::format("counted.{}=1", key));

            const Result refused = run(arguments);
            expect(eq(refused.exitCode, 1)) << refused.output;
            expect(refused.output.contains(std::format("block 'counted' of type 'qa::CountedChain' declares no setting named '{}'", key))) << "an interior block's setting is not the composite's" << refused.output;
        }

        std::vector<std::string> text = recipeChainRun();
        text.emplace_back("-s");
        text.emplace_back("counted.count=many");

        const Result underived = run(text);
        expect(eq(underived.exitCode, 1)) << underived.output;
        expect(underived.output.contains("Unable to create block 'counted' of type 'qa::CountedChain'")) << underived.output;
        expect(underived.output.contains("recipe_expression_conversion")) << "the refusal names the recipe's reason" << underived.output;
    };

    "a value that does not convert is refused by its reason alone, without the source location of the refusal"_test = [] {
        const Result schedulerRefused = run({"--graph", "unread.yaml", "--set", "timeout_ms=many"});
        expect(eq(schedulerRefused.exitCode, 2)) << schedulerRefused.output;
        expect(schedulerRefused.output.contains("a scheduler setting could not be applied")) << schedulerRefused.output;
        expect(!schedulerRefused.output.contains(".cpp:") && !schedulerRefused.output.contains(".hpp:")) << "a scheduler setting's refusal names no file and line of the library" << schedulerRefused.output;

        std::vector<std::string> member = settingsChainRun();
        member.emplace_back("-s");
        member.emplace_back("source.event_count=many");

        const Result memberRefused = run(member);
        expect(eq(memberRefused.exitCode, 1)) << memberRefused.output;
        expect(memberRefused.output.contains("Unable to create block 'source'")) << memberRefused.output;
        expect(!memberRefused.output.contains(".cpp:") && !memberRefused.output.contains(".hpp:")) << "a member's refusal names no file and line of the library" << memberRefused.output;

        std::vector<std::string> parameter = recipeChainRun();
        parameter.emplace_back("-s");
        parameter.emplace_back("counted.count=many");

        const Result parameterRefused = run(parameter);
        expect(eq(parameterRefused.exitCode, 1)) << parameterRefused.output;
        expect(parameterRefused.output.contains("Unable to create block 'counted'")) << parameterRefused.output;
        expect(!parameterRefused.output.contains(".cpp:") && !parameterRefused.output.contains(".hpp:")) << "nor does an exported parameter's" << parameterRefused.output;
    };

    "a block name the graph does not hold is refused"_test = [] {
        std::vector<std::string> arguments = boundedRun();
        arguments.emplace_back("--show");
        arguments.emplace_back("no_such_block");

        const Result refused = run(arguments);
        expect(eq(refused.exitCode, 1)) << refused.output;
        expect(refused.output.contains("no block named no_such_block")) << refused.output;
    };
#endif
};

int main() { /* not needed for UT */ }
