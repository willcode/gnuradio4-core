#include <boost/ut.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#ifndef _WIN32
#include <sys/wait.h>
#endif

/**
 * grinfo, driven as the program a caller runs.
 *
 * The tool's contract is its exit status and what it prints, and neither is visible from inside the process, so
 * every case here runs the built executable over core's own test plugins and test block libraries: the framework
 * report, the block listing, one block in detail, a name nothing is registered under, and a command line that
 * cannot be used. The shape of the report is part of that contract - no line wider than a terminal, one entry per
 * block however many instantiations it has, and a JSON document that carries no null - so each is pinned here too.
 */
namespace qa_grinfo {

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
    std::string command = std::format("\"{}\"", GR_TOOLS_GRINFO);
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

// one JSON document: every bracket closed in order outside a string, and nothing left open at the end
[[nodiscard]] bool isOneJsonDocument(std::string_view text) {
    std::size_t depth    = 0UZ;
    bool        inString = false;
    bool        escaped  = false;
    bool        opened   = false;
    for (const char character : text) {
        if (inString) {
            if (escaped) {
                escaped = false;
            } else if (character == '\\') {
                escaped = true;
            } else if (character == '"') {
                inString = false;
            }
            continue;
        }
        switch (character) {
        case '"': inString = true; break;
        case '{':
        case '[':
            ++depth;
            opened = true;
            break;
        case '}':
        case ']':
            if (depth == 0UZ) {
                return false;
            }
            --depth;
            break;
        default: break;
        }
    }
    return opened && depth == 0UZ && !inString;
}

// the report is written for a terminal, so this is the number every one of its lines has to stay under
constexpr std::size_t kWidth = 80UZ;

[[nodiscard]] std::size_t widestLine(std::string_view text) {
    std::size_t widest = 0UZ;
    for (std::size_t start = 0UZ; start <= text.size();) {
        const std::size_t end = std::min(text.find('\n', start), text.size());
        widest                = std::max(widest, end - start);
        start                 = end + 1UZ;
    }
    return widest;
}

[[nodiscard]] std::size_t occurrences(std::string_view text, std::string_view needle) {
    std::size_t found = 0UZ;
    for (std::size_t at = text.find(needle); at != std::string_view::npos; at = text.find(needle, at + needle.size())) {
        ++found;
    }
    return found;
}

// what a reader sees of a path the report had to cut: the tool keeps a path's tail, which is what tells one
// directory from another
[[nodiscard]] std::string_view tailOf(std::string_view path, std::size_t count) { return path.size() <= count ? path : path.substr(path.size() - count); }

// whether a line of the report is the fact `name` with `value`: the name, the padding that aligns the values, the value
[[nodiscard]] bool hasFact(std::string_view text, std::string_view name, std::string_view value) {
    for (std::size_t start = 0UZ; start < text.size();) {
        const std::size_t end  = std::min(text.find('\n', start), text.size());
        std::string_view  line = text.substr(start, end - start);
        start                  = end + 1UZ;
        line.remove_prefix(std::min(line.find_first_not_of(' '), line.size()));
        if (!line.starts_with(name) || !line.ends_with(value) || line.size() < name.size() + value.size() + 2UZ) {
            continue;
        }
        const std::string_view padding = line.substr(name.size(), line.size() - name.size() - value.size());
        if (padding.find_first_not_of(' ') == std::string_view::npos) {
            return true;
        }
    }
    return false;
}

// the text of the first JSON object that follows `"name": `, braces included; empty when there is none
[[nodiscard]] std::string_view jsonObject(std::string_view text, std::string_view name) {
    const std::string key   = std::format("\"{}\": {{", name);
    const std::size_t begin = text.find(key);
    if (begin == std::string_view::npos) {
        return {};
    }
    const std::size_t end = text.find('}', begin);
    return end == std::string_view::npos ? std::string_view{} : text.substr(begin + key.size() - 1UZ, end - begin - key.size() + 2UZ);
}

#ifdef GR_TOOLS_CORE_TEST_PLUGINS
// the two directories core's own tests build: one of plugins, one of shared objects that register blocks without
// being plugins
[[nodiscard]] std::vector<std::string> overTestDirectories(const std::vector<std::string>& arguments) {
    std::vector<std::string> all(arguments);
    all.emplace_back("--plugin-dir");
    all.emplace_back(GR_TOOLS_CORE_TEST_PLUGINS);
    all.emplace_back("--plugin-dir");
    all.emplace_back(GR_TOOLS_TEST_BLOCK_LIBRARY);
    return all;
}
#endif

} // namespace qa_grinfo

const boost::ut::suite<"GrInfo"> grInfoTests = [] {
    using namespace boost::ut;
    using namespace qa_grinfo;

    "a command line that cannot be used is refused with the usage text"_test = [] {
        const Result unknownOption = run({"--no-such-option"});
        expect(eq(unknownOption.exitCode, 2)) << unknownOption.output;
        expect(unknownOption.output.contains("unknown option '--no-such-option'")) << unknownOption.output;
        expect(unknownOption.output.contains("Usage: grinfo")) << unknownOption.output;

        const Result unknownCommand = run({"blocks-please"});
        expect(eq(unknownCommand.exitCode, 2)) << unknownCommand.output;
        expect(unknownCommand.output.contains("unknown command 'blocks-please'")) << unknownCommand.output;

        const Result withoutName = run({"block"});
        expect(eq(withoutName.exitCode, 2)) << withoutName.output;
        expect(withoutName.output.contains("block needs the name of a block")) << withoutName.output;

        const Result withoutValue = run({"--plugin-dir"});
        expect(eq(withoutValue.exitCode, 2)) << withoutValue.output;
        expect(withoutValue.output.contains("--plugin-dir needs a value")) << withoutValue.output;
    };

    "a plugin directory that cannot be searched ends the run"_test = [] {
        const Result missing = run({"--plugin-dir", "/gnuradio4-plugin-directory-that-does-not-exist"});
        expect(eq(missing.exitCode, 1)) << missing.output;
        expect(missing.output.contains("could not be searched")) << missing.output;
    };

    "--help is not an error"_test = [] {
        const Result help = run({"--help"});
        expect(eq(help.exitCode, 0)) << help.output;
        expect(help.output.contains("Usage: grinfo")) << help.output;
    };

#ifdef GR_TOOLS_CORE_TEST_PLUGINS
    "version names the directories searched and what they held"_test = [] {
        const Result version = run(overTestDirectories({"version"}));
        expect(eq(version.exitCode, 0)) << version.output;
        expect(version.output.contains(tailOf(GR_TOOLS_CORE_TEST_PLUGINS, 30UZ))) << "the directory it was given" << version.output;
        expect(version.output.contains(tailOf(GR_TOOLS_TEST_BLOCK_LIBRARY, 30UZ))) << "the directory it was given" << version.output;
        expect(version.output.contains("block-library")) << "a shared object that registers without being a plugin" << version.output;
        expect(version.output.contains("block_library")) << "and the file it is" << version.output;
        expect(version.output.contains("not-loaded")) << "the plugin whose ABI version does not match" << version.output;
        expect(version.output.contains("Good Math Plugin")) << "a plugin that did load" << version.output;
        expect(version.output.contains("block keys")) << version.output;
    };

    "a library is listed by its own name under the directory that held it"_test = [] {
        const Result version = run(overTestDirectories({"version"}));
        expect(eq(version.exitCode, 0)) << version.output;
        expect(version.output.contains("\n    libblock_library.so")) << "the file by its name alone, indented under its directory" << version.output;
        expect(eq(occurrences(version.output, GR_TOOLS_TEST_BLOCK_LIBRARY "/libblock_library.so"), 0UZ)) << "and not by a path repeated on every line" << version.output;
    };

    "version --json is one document carrying the shape a reader relies on"_test = [] {
        const Result version = run(overTestDirectories({"version", "--json"}));
        expect(eq(version.exitCode, 0)) << version.output;
        expect(isOneJsonDocument(version.output)) << version.output;
        expect(version.output.contains("\"schema\": 2")) << version.output;
        for (const std::string_view key : {"\"framework\"", "\"directories\"", "\"libraries\"", "\"plugins\"", "\"schedulers\"", "\"totals\"", "\"blockKeys\""}) {
            expect(version.output.contains(key)) << key << version.output;
        }
        expect(version.output.contains(GR_TOOLS_CORE_TEST_PLUGINS)) << "a path the text report cut is whole here" << version.output;
        expect(version.output.contains("\"origin\": \"option\"")) << "the directory came from the command line" << version.output;
        expect(version.output.contains("\"kind\": \"block-library\"")) << "the kinds are an enumeration" << version.output;
    };

    "blocks lists a block under the file that registered it and under its family"_test = [] {
        const Result blocks = run(overTestDirectories({"blocks"}));
        expect(eq(blocks.exitCode, 0)) << blocks.output;
        expect(blocks.output.contains("block_library")) << "the file the block came from" << blocks.output;
        expect(blocks.output.contains("gr::testing")) << "its family" << blocks.output;
        expect(blocks.output.contains("LibraryDoubler")) << "its name" << blocks.output;
        expect(blocks.output.contains("good")) << "the family of the test plugins' blocks" << blocks.output;
        expect(blocks.output.contains("fixed_source")) << "a block a plugin brought" << blocks.output;
        expect(blocks.output.contains("<float32>")) << "the instantiations collapsed onto the block's line" << blocks.output;
    };

    "blocks --json is one document shaped by library, family and block"_test = [] {
        const Result blocks = run(overTestDirectories({"blocks", "--json"}));
        expect(eq(blocks.exitCode, 0)) << blocks.output;
        expect(isOneJsonDocument(blocks.output)) << blocks.output;
        expect(blocks.output.contains("\"schema\": 2")) << blocks.output;
        for (const std::string_view key : {"\"libraries\"", "\"families\"", "\"instantiations\"", "\"LibraryDoubler\""}) {
            expect(blocks.output.contains(key)) << key << blocks.output;
        }
    };

    "block on a bare name prints its ports and its settings"_test = [] {
        const Result block = run(overTestDirectories({"block", "LibraryDoubler"}));
        expect(eq(block.exitCode, 0)) << block.output;
        expect(block.output.contains("gr::testing::LibraryDoubler")) << block.output;
        expect(block.output.contains("block_library")) << "the file it came from" << block.output;
        expect(block.output.contains("input")) << "its input port" << block.output;
        expect(block.output.contains("output")) << "its output port" << block.output;
        expect(block.output.contains("float32")) << "the type the ports carry" << block.output;
        expect(block.output.contains("extra_gain")) << "its setting" << block.output;
        expect(block.output.contains("dB")) << "the unit the annotation carries" << block.output;
        expect(block.output.contains("doubles its input")) << "the description the block declares" << block.output;
    };

    "the settings the framework declares on every block are behind an option"_test = [] {
        const Result block = run(overTestDirectories({"block", "LibraryDoubler"}));
        expect(eq(block.exitCode, 0)) << block.output;
        expect(!block.output.contains("unique_name")) << "a framework setting is not in the block's own table" << block.output;
        expect(block.output.contains("--all-settings")) << "and the report says where it is" << block.output;

        const Result all = run(overTestDirectories({"block", "LibraryDoubler", "--all-settings"}));
        expect(eq(all.exitCode, 0)) << all.output;
        expect(all.output.contains("unique_name")) << all.output;
        expect(all.output.contains("extra_gain")) << "the block's own settings are still there" << all.output;
    };

    "a block of several instantiations is one entry written in its type parameters"_test = [] {
        const Result block = run(overTestDirectories({"block", "convert"}));
        expect(eq(block.exitCode, 0)) << block.output;
        expect(block.output.contains("good::convert<T1, T2>")) << "named by its parameters" << block.output;
        expect(block.output.contains("<float64, float32>")) << "which are listed once" << block.output;
        expect(block.output.contains("<float32, float64>")) << block.output;
        expect(eq(occurrences(block.output, "direction"), 1UZ)) << "one port table for every instantiation" << block.output;
        expect(block.output.contains("T1")) << "the input port carries the first parameter" << block.output;
        expect(block.output.contains("T2")) << "and the output port the second" << block.output;
    };

    "block --json carries the settings with their defaults typed"_test = [] {
        const Result block = run(overTestDirectories({"block", "LibraryDoubler", "--json"}));
        expect(eq(block.exitCode, 0)) << block.output;
        expect(isOneJsonDocument(block.output)) << block.output;
        expect(block.output.contains("\"command\": \"block\"")) << block.output;
        expect(block.output.contains("\"query\": \"LibraryDoubler\"")) << block.output;
        expect(block.output.contains("\"name\": \"extra_gain\"")) << block.output;
        expect(block.output.contains("\"default\": 1")) << "a number is a number, not a string" << block.output;
        expect(block.output.contains("\"unit\": \"dB\"")) << block.output;
        expect(block.output.contains("\"framework\": false")) << "a setting of the block's own" << block.output;
        expect(!block.output.contains(": null")) << "a field the framework holds nothing in is left out" << block.output;
    };

    "block --json keys every instantiation in full, which the text report does not"_test = [] {
        const Result block = run(overTestDirectories({"block", "convert", "--json"}));
        expect(eq(block.exitCode, 0)) << block.output;
        expect(isOneJsonDocument(block.output)) << block.output;
        expect(block.output.contains("\"key\": \"good::convert<float64, float32>\"")) << block.output;
        expect(block.output.contains("\"key\": \"good::convert<float32, float64>\"")) << block.output;
        expect(block.output.contains("\"parameters\": [")) << "split into one entry per parameter" << block.output;
        expect(eq(occurrences(block.output, "\"dataType\": \"float64\""), 2UZ)) << "the concrete type of each port of each key" << block.output;
    };

    "a block that declares nothing prints no label line, and the role its stream ports read"_test = [] {
        const Result block = run(overTestDirectories({"block", "LibraryDoubler"}));
        expect(eq(block.exitCode, 0)) << block.output;
        expect(hasFact(block.output, "version", "1")) << "the instrument: a fact line is found where the report has one" << block.output;
        expect(hasFact(block.output, "role", "processor, read from the stream ports")) << "every block has its read role" << block.output;
        expect(!block.output.contains("/")) << "no class/word line" << block.output;

        const Result json = run(overTestDirectories({"block", "LibraryDoubler", "--json"}));
        expect(eq(json.exitCode, 0)) << json.output;
        expect(json.output.contains("\"labels\": []")) << "the array is present and empty" << json.output;
        expect(json.output.contains("\"role\": \"processor\"")) << json.output;
    };
#endif

#ifdef GR_TOOLS_VERSIONED_PLUGIN
    "a block that declares labels prints each with its meaning, then the role its ports read"_test = [] {
        const Result block = run({"block", "test::versioned", "--plugin-dir", GR_TOOLS_VERSIONED_PLUGIN});
        expect(eq(block.exitCode, 0)) << block.output;
        expect(hasFact(block.output, "family/versioned", "Blocks of the versioned test plugin.")) << "the meaning the plugin's vocabulary carries across the boundary" << block.output;
        expect(hasFact(block.output, "holds/device", "Opens a hardware unit attached to the host.")) << block.output;
        expect(hasFact(block.output, "holds/testbus", "Opens the test plugin's own bus.")) << block.output;
        expect(hasFact(block.output, "status/experimental", "Its interface or its numerics may still change.")) << block.output;
        expect(hasFact(block.output, "role", "processor, read from the stream ports")) << "holding a device makes no transceiver" << block.output;
        expect(hasFact(block.output, "version", "2")) << "the newest revision" << block.output;
        expect(!block.output.contains("\n  note ")) << "no role is declared, so nothing contradicts the ports" << block.output;

        const std::array<std::size_t, 4> lines{block.output.find("\n  family/versioned "), block.output.find("\n  holds/device "), block.output.find("\n  role "), block.output.find("\n  version ")};
        expect(lines.back() != std::string::npos && std::ranges::is_sorted(lines)) << "the labels in class order, then the role and the version" << block.output;
    };

    "a declared role the stream ports contradict is printed with one note, and nothing is refused"_test = [] {
        const Result block = run({"block", "test::mislabeled", "--plugin-dir", GR_TOOLS_VERSIONED_PLUGIN});
        expect(eq(block.exitCode, 0)) << block.output;
        expect(hasFact(block.output, "role/source", "Brings signals from the world into the graph across a boundary")) << block.output;
        expect(hasFact(block.output, "note", "role/source declared; the stream ports read consumer")) << block.output;
        expect(eq(occurrences(block.output, "\n  note "), 1UZ)) << block.output;

        const Result agreeing = run({"block", "test::antenna", "--plugin-dir", GR_TOOLS_VERSIONED_PLUGIN});
        expect(eq(agreeing.exitCode, 0)) << agreeing.output;
        expect(hasFact(agreeing.output, "role", "notation, read from the stream ports")) << "no stream port and plane/notation" << agreeing.output;
        expect(!agreeing.output.contains("\n  note ")) << agreeing.output;
    };

    "block --json carries the labels with their meanings and the read role"_test = [] {
        const Result block = run({"block", "test::versioned", "--json", "--plugin-dir", GR_TOOLS_VERSIONED_PLUGIN});
        expect(eq(block.exitCode, 0)) << block.output;
        expect(isOneJsonDocument(block.output)) << block.output;
        expect(block.output.contains("\"label\": \"family/versioned\"")) << block.output;
        expect(block.output.contains("\"Blocks of the versioned test plugin.\"")) << block.output;
        expect(block.output.contains("\"known\": true")) << block.output;
        expect(block.output.contains("\"role\": \"processor\"")) << block.output;
        expect(block.output.contains("\"version\": 2")) << "a number, not a string" << block.output;
        expect(!block.output.contains("\"roleNote\"")) << block.output;

        const Result mislabeled = run({"block", "test::mislabeled", "--json", "--plugin-dir", GR_TOOLS_VERSIONED_PLUGIN});
        expect(mislabeled.output.contains("\"roleNote\": \"role/source declared; the stream ports read consumer\"")) << mislabeled.output;
    };

    "blocks --label keeps the blocks whose newest version carries every label named"_test = [] {
        const Result all = run(overTestDirectories({"blocks", "--plugin-dir", GR_TOOLS_VERSIONED_PLUGIN}));
        expect(eq(all.exitCode, 0)) << all.output;
        expect(all.output.contains("VersionedFirst")) << "the instrument: without the option every block is listed" << all.output;
        expect(all.output.contains("LibraryDoubler")) << all.output;
        expect(!all.output.contains("blocks carrying")) << "no naming line without the option" << all.output;

        const Result devices = run(overTestDirectories({"blocks", "--label", "holds/device", "--plugin-dir", GR_TOOLS_VERSIONED_PLUGIN}));
        expect(eq(devices.exitCode, 0)) << devices.output;
        expect(devices.output.starts_with("blocks carrying holds/device\n")) << "the first line names the narrowing" << devices.output;
        expect(hasFact(devices.output, "block keys", "2")) << "the totals count the kept keys" << devices.output;
        expect(hasFact(devices.output, "block families", "2")) << devices.output;
        expect(hasFact(devices.output, "libraries", "1")) << "the one plugin that holds a kept key" << devices.output;
        expect(devices.output.contains("\n      versioned\n")) << "the key whose newest version holds a device" << devices.output;
        expect(devices.output.contains("VersionedSecond")) << "the type behind it, registered under its own name" << devices.output;
        expect(!devices.output.contains("VersionedFirst")) << devices.output;
        expect(!devices.output.contains("LibraryDoubler")) << devices.output;

        const Result both = run(overTestDirectories({"blocks", "--label", "holds/device", "--label", "status/deprecated", "--plugin-dir", GR_TOOLS_VERSIONED_PLUGIN}));
        expect(eq(both.exitCode, 0)) << both.output;
        expect(both.output.starts_with("blocks carrying holds/device, status/deprecated\n")) << both.output;
        expect(hasFact(both.output, "block keys", "0")) << "every label named must hold" << both.output;

        const Result sources = run(overTestDirectories({"blocks", "--label", "role/source", "--json", "--plugin-dir", GR_TOOLS_VERSIONED_PLUGIN}));
        expect(eq(sources.exitCode, 0)) << sources.output;
        expect(isOneJsonDocument(sources.output)) << sources.output;
        expect(sources.output.contains("\"mislabeled\"")) << "a declared role counts whatever the ports read" << sources.output;
        expect(jsonObject(sources.output, "totals").contains("\"blockKeys\": 2")) << sources.output;

        const Result processors = run(overTestDirectories({"blocks", "--label", "role/processor", "--plugin-dir", GR_TOOLS_VERSIONED_PLUGIN}));
        expect(eq(processors.exitCode, 0)) << processors.output;
        expect(processors.output.contains("LibraryDoubler")) << "a block that declares nothing matches the role its ports read" << processors.output;
        expect(processors.output.contains("\n      versioned\n")) << "a declaring type without a role matches the role its map reads" << processors.output;
        expect(!processors.output.contains("mislabeled")) << "a declared role counts before the one the ports read" << processors.output;

        const Result outside = run(overTestDirectories({"blocks", "--label", "holds/gpib", "--plugin-dir", GR_TOOLS_VERSIONED_PLUGIN}));
        expect(eq(outside.exitCode, 0)) << outside.output;
        expect(outside.output.contains("\nholds/gpib is outside the loaded vocabulary\n")) << outside.output;
        expect(hasFact(outside.output, "block keys", "0")) << outside.output;
    };

    "a malformed label is refused with the usage text"_test = [] {
        const Result refused = run({"blocks", "--label", "rf", "--plugin-dir", GR_TOOLS_VERSIONED_PLUGIN});
        expect(eq(refused.exitCode, 2)) << refused.output;
        expect(refused.output.contains("--label 'rf' is not class/word with a class of family, role, plane")) << refused.output;
        expect(refused.output.contains("Usage: grinfo")) << refused.output;

        const Result upper = run({"blocks", "--label", "emits/RF"});
        expect(eq(upper.exitCode, 2)) << upper.output;
        expect(upper.output.contains("a word of a lower-case letter")) << upper.output;

        const Result withoutValue = run({"blocks", "--label"});
        expect(eq(withoutValue.exitCode, 2)) << withoutValue.output;
        expect(withoutValue.output.contains("--label needs a value")) << withoutValue.output;
    };

    "labels prints the vocabulary by class with each word's meaning"_test = [] {
        const Result labels = run({"labels", "--plugin-dir", GR_TOOLS_VERSIONED_PLUGIN});
        expect(eq(labels.exitCode, 0)) << labels.output;
        expect(labels.output.starts_with("family\n")) << labels.output;
        expect(hasFact(labels.output, "versioned", "Blocks of the versioned test plugin.")) << "a plugin's word" << labels.output;
        expect(hasFact(labels.output, "testbus", "Opens the test plugin's own bus.")) << labels.output;
        expect(hasFact(labels.output, "rf", "Radio-frequency energy, radiated or conducted. (physical)")) << "core's words, a physical medium marked" << labels.output;
        expect(hasFact(labels.output, "storage", "Data at rest in a file, a disk or a database.")) << labels.output;
        const std::array<std::size_t, 8> classes{labels.output.find("family\n"), labels.output.find("\nrole\n"), labels.output.find("\nplane\n"), labels.output.find("\nholds\n"), labels.output.find("\nemits\n"), labels.output.find("\ningests\n"), labels.output.find("\ncompute\n"), labels.output.find("\nstatus\n")};
        expect(classes.back() != std::string::npos && std::ranges::is_sorted(classes)) << "the classes in their order" << labels.output;

        const Result json = run({"labels", "--json", "--plugin-dir", GR_TOOLS_VERSIONED_PLUGIN});
        expect(eq(json.exitCode, 0)) << json.output;
        expect(isOneJsonDocument(json.output)) << json.output;
        expect(json.output.contains("\"label\": \"holds/testbus\"")) << json.output;
        expect(json.output.contains("\"physical\": true")) << json.output;
    };
#endif

#ifdef GR_TOOLS_CORE_TEST_PLUGINS
    "no line of any report is wider than a terminal"_test = [] {
        for (const std::vector<std::string>& arguments : {std::vector<std::string>{"version"}, {"blocks"}, {"blocks", "--verbose"}, {"block", "convert"}, {"block", "LibraryDoubler", "--all-settings"}}) {
            const Result report = run(overTestDirectories(arguments));
            expect(eq(report.exitCode, 0)) << report.output;
            expect(le(widestLine(report.output), kWidth)) << arguments.front() << report.output;
        }
    };
#endif

    "--help is no wider than a terminal either"_test = [] {
        const Result help = run({"--help"});
        expect(eq(help.exitCode, 0)) << help.output;
        expect(le(widestLine(help.output), kWidth)) << help.output;
    };

    "a name nothing is registered under is refused"_test = [] {
        const Result missing = run({"block", "NoSuchBlockIsRegistered"});
        expect(eq(missing.exitCode, 1)) << missing.output;
        expect(missing.output.contains("no block named NoSuchBlockIsRegistered is registered")) << missing.output;
    };
};

int main() { /* not needed for UT */ }
