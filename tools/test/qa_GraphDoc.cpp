#include <boost/ut.hpp>

#include <array>
#include <cstddef>
#include <format>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

#include "GraphDoc.hpp"

/**
 * The flowgraph document: what the reader must be able to find in it.
 *
 * The two pinned documents in `assets/` are compared as whole files by ctest, which catches any
 * change to the layout. The assertions here are the ones a diff cannot make: that every block,
 * every connection and every level of the fixture reached the document at all, that the nesting
 * put them in the right section, and that the HTML page stands alone.
 */
namespace qa_graphdoc {

using namespace gr::tools;

[[nodiscard]] std::string fixture() {
    std::ifstream      input(std::string(GR_TOOLS_TEST_ASSETS) + "/nested_graph.yaml", std::ios::binary);
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

[[nodiscard]] std::size_t occurrences(std::string_view haystack, std::string_view needle) {
    std::size_t count = 0UZ;
    for (std::size_t at = haystack.find(needle); at != std::string_view::npos; at = haystack.find(needle, at + needle.size())) {
        ++count;
    }
    return count;
}

/// the `x` the element beginning at `at` is drawn at, or -1 where it states none
[[nodiscard]] double leftEdgeOf(std::string_view document, std::size_t at) {
    const std::size_t attribute = document.find(" x=\"", at);
    if (attribute == std::string_view::npos) {
        return -1.0;
    }
    const std::size_t begin = attribute + 4UZ;
    return std::stod(std::string(document.substr(begin, document.find('"', begin) - begin)));
}

[[nodiscard]] graphdoc::Level readFixture() {
    auto level = graphdoc::read(fixture());
    if (!level.has_value()) {
        throw std::runtime_error(level.error());
    }
    return std::move(*level);
}

/// every block name and type, and both ends of every connection, at every level of the fixture
constexpr std::array<std::string_view, 7> kBlockNames{"source", "front_end", "sink", "pre_gain", "inner_chain", "fine_gain", "combiner"};
constexpr std::array<std::string_view, 4> kBlockTypes{"qa::RampSource", "qa::Scale", "qa::SumInputs", "qa::RecordingSink"};
constexpr std::array<std::string_view, 8> kConnectionEnds{"source_1", "front_end", "front_end", "sink", "pre_gain", "inner_chain", "fine_gain", "combiner"};

} // namespace qa_graphdoc

const boost::ut::suite<"GraphDoc"> graphDocTests = [] {
    using namespace boost::ut;
    using namespace gr::tools;
    using namespace qa_graphdoc;

    "the fixture parses into three graph levels"_test = [] {
        const graphdoc::Level level = readFixture();
        expect(eq(level.blocks.size(), 3UZ));
        expect(eq(level.connections.size(), 2UZ));
        expect(eq(graphdoc::countSubgraphs(level), 2UZ));
        expect(eq(level.metadata.size(), 5UZ)) << "definition_metadata carries five entries";
        expect(eq(level.uninterpretedKeys.size(), 1UZ)) << "graph_version is not a key the reader interprets";
    };

    "the document names every block and every connection"_test = [] {
        const std::string document = graphdoc::render(readFixture(), Format::Markdown, "Nested graph fixture");
        for (const std::string_view name : kBlockNames) {
            expect(document.find(name) != std::string::npos) << std::format("block '{}' is missing from the document", name);
        }
        for (const std::string_view type : kBlockTypes) {
            expect(document.find(type) != std::string::npos) << std::format("type '{}' is missing from the document", type);
        }
        for (const std::string_view endpoint : kConnectionEnds) {
            expect(document.find(endpoint) != std::string::npos) << std::format("connection endpoint '{}' is missing from the document", endpoint);
        }
        expect(document.find("4096") != std::string::npos) << "the minimum buffer size of the second connection is missing";
        expect(document.find("[context fast @ 0]") != std::string::npos) << "the context parameters of the sink are missing";
    };

    "each subgraph has its own section, exported ports and diagram"_test = [] {
        const std::string document = graphdoc::render(readFixture(), Format::Markdown, "Nested graph fixture");
        const std::size_t outer    = document.find("## Subgraph: front_end");
        const std::size_t inner    = document.find("### Subgraph: front_end / inner_chain");
        expect(outer != std::string::npos) << "the outer subgraph has no section";
        expect(inner != std::string::npos) << "the inner subgraph has no section of its own";
        expect(outer < inner) << "the inner section must nest inside the outer one";
        expect(document.find("fine_gain") > outer) << "an inner block is documented at the top level";
        expect(eq(occurrences(document, "Exported ports"), 2UZ)) << "both subgraphs export ports";
        expect(document.find("gr::scheduler::Simple") != std::string::npos) << "the scheduler of the managed subgraph is missing";
        expect(eq(occurrences(document, "```mermaid"), 3UZ)) << "one diagram per graph level";
    };

    "a composite's exported parameters are read and documented"_test = [] {
        const graphdoc::Level level = readFixture();
        expect(eq(level.blocks[1].exportedParameters.size(), 2UZ)) << "the outer subgraph declares two exported parameters";
        expect(eq(level.blocks[1].uninterpretedKeys.size(), 0UZ)) << "exported_parameters is interpreted on a composite entry";
        expect(!level.blocks[1].exportedParameters[0].required) << "a declaration with a default is not required";
        expect(level.blocks[1].exportedParameters[1].required) << "a declaration without a default is required at instantiation";

        for (const Format format : {Format::Markdown, Format::Html}) {
            const std::string document = graphdoc::render(level, format, "Nested graph fixture");
            expect(document.find("Exported parameters") != std::string::npos) << "the declarations have no table";
            for (const std::string_view fragment : {"front_gain", "float32", "channel_rate", "float64", "required", "the scale the front end applies"}) {
                expect(document.find(fragment) != std::string::npos) << std::format("'{}' is missing from the document", fragment);
            }
        }
    };

    "a block's uninterpreted keys are documented with the block"_test = [] {
        const graphdoc::Level level = readFixture();
        expect(eq(level.blocks[2].uninterpretedKeys.size(), 1UZ)) << "layout_hint is not a key the reader interprets";

        for (const Format format : {Format::Markdown, Format::Html}) {
            const std::string document = graphdoc::render(level, format, "Nested graph fixture");
            expect(document.find("Other block keys") != std::string::npos) << "a block's unknown keys have no table";
            expect(document.find("layout_hint") != std::string::npos) << "the key itself is missing";
            expect(document.find("bottom-right") != std::string::npos) << "the value of the key is missing";
        }
    };

    "a pinned version is interpreted and shown; an entry that pins none says nothing"_test = [] {
        const auto pinned = graphdoc::read("blocks:\n  - id: qa::Scale\n    name: front\n    version: 2\n");
        expect(pinned.has_value());
        expect(eq(pinned->blocks[0].pinnedVersion, std::string("2")));
        expect(pinned->blocks[0].uninterpretedKeys.empty()) << "`version` is a key the reader interprets";
        expect(graphdoc::render(*pinned, Format::Markdown, "t").find("version 2 pinned") != std::string::npos);

        const auto unpinned = graphdoc::read("blocks:\n  - id: qa::Scale\n    name: front\n");
        expect(unpinned.has_value());
        expect(unpinned->blocks[0].pinnedVersion.empty());
        expect(graphdoc::render(*unpinned, Format::Markdown, "t").find("pinned") == std::string::npos);
    };

    "exported_parameters on a plain block stays uninterpreted"_test = [] {
        const auto level = graphdoc::read("blocks:\n  - id: qa::Scale\n    exported_parameters:\n      - name: gain\n        type: float32\n");
        expect(level.has_value());
        expect(level->blocks[0].exportedParameters.empty()) << "only the entry carrying `graph` declares what a definition exports";
        expect(eq(level->blocks[0].uninterpretedKeys.size(), 1UZ)) << "the key is reported rather than silently dropped";
        expect(graphdoc::render(*level, Format::Markdown, "t").find("exported_parameters") != std::string::npos);
    };

    "the diagram labels nodes and edges"_test = [] {
        const std::string diagram = graphdoc::diagramOf(readFixture(), "g");
        expect(diagram.starts_with("flowchart LR\n"));
        expect(diagram.find("source<br/>qa::RampSource") != std::string::npos) << "a node carries the block name and its type";
        expect(diagram.find("[[\"front_end<br/>SUBGRAPH\"]]") != std::string::npos) << "a subgraph node uses the subroutine shape";
        expect(eq(occurrences(diagram, " --> "), 2UZ)) << "one edge per connection of the level";
        expect(diagram.find("-- \"0 to 0\" -->") != std::string::npos) << "an edge is labeled with its ports";
    };

    "the HTML page carries no external reference"_test = [] {
        const std::string document = graphdoc::render(readFixture(), Format::Html, "Nested graph fixture");
        for (const std::string_view forbidden : {"<script", "<link", "<img", "http://", "https://", "@import"}) {
            expect(document.find(forbidden) == std::string::npos) << std::format("the page reaches outside itself: '{}'", forbidden);
        }
        expect(document.starts_with("<!DOCTYPE html>"));
        expect(document.find("<svg class=\"flowgraph\"") != std::string::npos) << "the diagram is drawn into the page";
        expect(document.find("&lt;") != std::string::npos || document.find("&amp;") != std::string::npos) << "the page escapes its content";
    };

    "the HTML draws one picture per level, one node per block and one edge per connection"_test = [] {
        const std::string document = graphdoc::render(readFixture(), Format::Html, "Nested graph fixture");
        expect(eq(occurrences(document, "<svg class=\"flowgraph\""), 3UZ)) << "one drawing per graph level";
        expect(eq(occurrences(document, "<rect class=\"node"), 7UZ)) << "one node per block of every level";
        expect(eq(occurrences(document, "<path class=\"edge\""), 4UZ)) << "one edge per connection of every level";
        expect(eq(occurrences(document, "<pre class=\"mermaid\""), 0UZ)) << "the page carries no diagram as text";
        expect(document.find("viewBox=\"0 0 ") != std::string::npos) << "the drawing scales with the page";
        expect(document.find("<rect class=\"inner\"") != std::string::npos) << "the subgraph node carries a second border";
        expect(document.find(">front_end<") != std::string::npos) << "a node is labeled with the block's name";
        expect(document.find(">RampSource<") != std::string::npos) << "and with its type";
    };

    "the diagram stands before the blocks of its level in both formats"_test = [] {
        const std::string markdown = graphdoc::render(readFixture(), Format::Markdown, "Nested graph fixture");
        expect(markdown.find("## Diagram") < markdown.find("## Blocks")) << "the Markdown diagram follows the summary and leads the tables";
        expect(markdown.find("## Summary") < markdown.find("## Diagram"));
        expect(markdown.find("### Diagram") < markdown.find("### Blocks")) << "and so does a subgraph's";

        const std::string html = graphdoc::render(readFixture(), Format::Html, "Nested graph fixture");
        expect(html.find("<svg class=\"flowgraph\"") < html.find("<h2>Blocks</h2>"));
        expect(html.find("<h2>Summary</h2>") < html.find("<svg class=\"flowgraph\""));
    };

    "the same input yields the same bytes"_test = [] {
        const graphdoc::Level level = readFixture();
        expect(eq(graphdoc::render(level, Format::Markdown, "t"), graphdoc::render(level, Format::Markdown, "t")));
        expect(eq(graphdoc::render(level, Format::Html, "t"), graphdoc::render(level, Format::Html, "t")));
    };

    "a document with no blocks is empty and well formed"_test = [] {
        const auto level = graphdoc::read("something_else: 1\n");
        expect(level.has_value()) << "a graph with no blocks key is not an error";
        const std::string document = graphdoc::render(*level, Format::Markdown, "empty");
        expect(document.find("This graph level holds no blocks.") != std::string::npos);
        expect(document.find("This graph level holds no connections.") != std::string::npos);
        expect(eq(occurrences(document, "```mermaid"), 1UZ));
    };

    "a file that is not the dialect is reported"_test = [] {
        const auto level = graphdoc::read("value: !!int32 not-a-number\n");
        expect(!level.has_value()) << "a value that fails its own type tag must be reported";
        expect(!level.error().empty());
    };

    "a connection to a block the level does not hold is still drawn"_test = [] {
        const auto level = graphdoc::read("blocks:\n  - id: qa::Scale\n    parameters:\n      name: only\nconnections:\n  - [only, 0, missing, 0]\n");
        expect(level.has_value());
        const std::string diagram = graphdoc::diagramOf(*level, "g");
        expect(diagram.find("(unresolved)") != std::string::npos) << "the missing end is drawn and marked";
        expect(diagram.find("classDef unresolved") != std::string::npos);

        const std::string drawing = graphdoc::svgOf(*level, "g");
        expect(eq(occurrences(drawing, "<rect class=\"node"), 2UZ)) << "the missing end is a node of the drawing too";
        expect(drawing.find("<rect class=\"node unresolved\"") != std::string::npos) << "and it is drawn dashed";
        expect(drawing.find(">(unresolved)<") != std::string::npos);
    };

    "the drawing ranks a chain left to right and breaks a cycle at the edge that closes it"_test = [] {
        const auto chain = graphdoc::read("blocks:\n  - id: qa::Scale\n    parameters:\n      name: first\n  - id: qa::Scale\n    parameters:\n      name: second\nconnections:\n  - [first, out, second, in]\n");
        expect(chain.has_value());
        const std::string drawn = graphdoc::svgOf(*chain, "g");
        const std::size_t firstNode  = drawn.find("<rect class=\"node\"");
        const std::size_t secondNode = drawn.find("<rect class=\"node\"", firstNode + 1UZ);
        expect(firstNode != std::string::npos && secondNode != std::string::npos);
        expect(leftEdgeOf(drawn, firstNode) < leftEdgeOf(drawn, secondNode)) << "the destination stands to the right of its source";

        const auto loop = graphdoc::read("blocks:\n  - id: qa::Scale\n    parameters:\n      name: first\n  - id: qa::Scale\n    parameters:\n      name: second\nconnections:\n  - [first, out, second, in]\n  - [second, out, first, in]\n");
        expect(loop.has_value());
        const std::string cycle      = graphdoc::svgOf(*loop, "g");
        const std::size_t cycleFirst = cycle.find("<rect class=\"node\"");
        const std::size_t cycleAfter = cycle.find("<rect class=\"node\"", cycleFirst + 1UZ);
        expect(leftEdgeOf(cycle, cycleFirst) < leftEdgeOf(cycle, cycleAfter)) << "the edge that closes the cycle does not rank its destination";
        expect(eq(occurrences(cycle, "<path class=\"edge\""), 2UZ)) << "both connections are still drawn";
    };
};

int main() { /* not needed for UT */ }
