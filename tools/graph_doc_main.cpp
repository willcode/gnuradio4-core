#include <filesystem>
#include <fstream>
#include <print>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "DocOptions.hpp"
#include "GraphDoc.hpp"

namespace {

constexpr std::string_view kUsage = R"(gnuradio_4_0_graph_doc - describe a GNU Radio 4 flowgraph as Markdown or HTML

Usage: gnuradio_4_0_graph_doc [options] <graph.yaml>

  --format md|html   output format (default: md)
  --output <file>    write the document to <file> (default: standard output)
  --title <text>     document title (default: the name of the input file)
  --help, -h         this text

Reads the GRC YAML dialect the framework's own importer reads: blocks with their
parameters, connections, and SUBGRAPH entries with their nested graphs and exported
ports, to any depth. The document carries a summary, a table of blocks, a table of
connections and a mermaid flowchart for every graph level.

No block is instantiated and no registry is consulted, so a graph whose blocks this
build does not provide is described just as well as one it does.

The HTML is a single self-contained page: its style sheet is embedded and it loads
nothing over the network.
)";

} // namespace

int main(int argc, char** argv) {
    using namespace gr::tools;

    const std::vector<std::string_view> arguments = argumentsOf(argc, argv);
    CommonOptions                       options;
    std::string                         inputPath;
    std::string                         error;

    for (std::size_t index = 0UZ; index < arguments.size();) {
        switch (readCommonOption(arguments, index, options, error)) {
        case OptionResult::Taken: continue;
        case OptionResult::Failed: std::println(stderr, "gnuradio_4_0_graph_doc: {}", error); return 2;
        case OptionResult::NotMine: break;
        }
        const std::string_view argument = arguments[index];
        if (argument.starts_with("-") && argument != "-") {
            std::println(stderr, "gnuradio_4_0_graph_doc: unknown option '{}'", argument);
            return 2;
        }
        if (!inputPath.empty()) {
            std::println(stderr, "gnuradio_4_0_graph_doc: more than one input file given ('{}' and '{}')", inputPath, argument);
            return 2;
        }
        inputPath.assign(argument);
        ++index;
    }

    if (options.help) {
        std::print("{}", kUsage);
        return 0;
    }
    if (inputPath.empty()) {
        std::println(stderr, "gnuradio_4_0_graph_doc: no input file given; --help describes the options");
        return 2;
    }

    std::ifstream input(inputPath, std::ios::binary);
    if (!input) {
        std::println(stderr, "gnuradio_4_0_graph_doc: cannot read '{}'", inputPath);
        return 2;
    }
    std::ostringstream contents;
    contents << input.rdbuf();

    const auto level = graphdoc::read(contents.str());
    if (!level.has_value()) {
        std::println(stderr, "gnuradio_4_0_graph_doc: '{}' is not a valid graph document: {}", inputPath, level.error());
        return 1;
    }

    if (options.title.empty()) {
        options.title = std::filesystem::path(inputPath).filename().string();
    }

    const auto written = writeDocument(options.output, graphdoc::render(*level, options.format, options.title));
    if (!written.has_value()) {
        std::println(stderr, "gnuradio_4_0_graph_doc: {}", written.error());
        return 2;
    }
    return 0;
}
