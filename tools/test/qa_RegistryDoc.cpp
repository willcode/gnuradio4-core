#include <boost/ut.hpp>

#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <gnuradio-4.0/Block.hpp>
#include <gnuradio-4.0/BlockRegistry.hpp>
#include <gnuradio-4.0/PluginLoader.hpp>

#include "RegistryDoc.hpp"

/**
 * The registry document: everything in it comes from the running program.
 *
 * The document is built over a registry the test fills itself, so the shape of what it must
 * report is known exactly: two blocks whose ports and settings are declared here, a factory that
 * refuses to construct, core's own test plugins, and a plugin directory that does not exist.
 */
namespace qa_registrydoc {

using namespace gr;

enum class DocWaveform { sine, square, triangle };

struct DocScale : Block<DocScale> {
    using Description = Doc<"multiplies its input by a gain">;

    PortIn<float>  in;
    PortOut<float> out;

    Annotated<float, "gain", Visible, Doc<"linear voltage gain">, Unit<"dB">, Limits<0.f, 10.f>> gain     = 1.0f;
    Annotated<DocWaveform, "waveform", Doc<"shape the gain was calibrated for">>                 waveform = DocWaveform::sine;
    Annotated<std::string, "label">                                                              label    = "unnamed";
    gr::Size_t                                                                                   count    = 0U;

    GR_MAKE_REFLECTABLE(DocScale, in, out, gain, waveform, label, count);

    explicit DocScale(property_map init = {}) : Block<DocScale>(std::move(init)) {}

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return gain * value; }
};

/// a block whose inputs are one collection port, addressed "inputs#0" and "inputs#1"
struct DocCombiner : Block<DocCombiner> {
    std::vector<PortIn<float>> inputs;
    PortOut<float>             out;

    Annotated<gr::Size_t, "number of inputs", Limits<1U, 8U>> n_inputs = 2U;

    GR_MAKE_REFLECTABLE(DocCombiner, inputs, out, n_inputs);

    explicit DocCombiner(property_map init = {}) : Block<DocCombiner>(std::move(init)) { inputs.resize(n_inputs); }

    template<InputSpanLike TInSpan>
    work::Status processBulk(const std::span<TInSpan>& inSpans, OutputSpanLike auto& outSpan) const {
        std::ranges::copy(inSpans[0], outSpan.begin());
        for (std::size_t port = 1UZ; port < inSpans.size(); ++port) {
            std::ranges::transform(outSpan, inSpans[port], outSpan.begin(), std::plus<float>{});
        }
        return work::Status::OK;
    }
};

/// two revisions of one key, the older one carrying both status flags
struct DocFilterV1 : Block<DocFilterV1> {
    using Description = Doc<"the first revision of the filter">;

    static constexpr gr::block::Status  status{.deprecated = true, .experimental = true};
    static constexpr gr::block::Version version = 1U;

    PortIn<float>  in;
    PortOut<float> out;

    GR_MAKE_REFLECTABLE(DocFilterV1, in, out);

    explicit DocFilterV1(property_map init = {}) : Block<DocFilterV1>(std::move(init)) {}

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value; }
};

struct DocFilterV2 : Block<DocFilterV2> {
    using Description = Doc<"the second revision of the filter">;

    static constexpr gr::block::Version version = 2U;

    PortIn<float>  in;
    PortOut<float> out;

    GR_MAKE_REFLECTABLE(DocFilterV2, in, out);

    explicit DocFilterV2(property_map init = {}) : Block<DocFilterV2>(std::move(init)) {}

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value; }
};

constexpr std::string_view kVersionedKey   = "doc::filter";
constexpr std::string_view kRefusingKey    = "doc::refuses_to_construct";
constexpr std::string_view kRefusalText    = "this factory refuses to construct anything";
constexpr std::string_view kMissingPlugins = "/gnuradio4-doctools-directory-that-does-not-exist";

std::unique_ptr<BlockModel> refusingFactory(property_map) { throw gr::exception(std::string(kRefusalText)); }

template<typename TBlock>
std::unique_ptr<BlockModel> makeDocBlock(property_map params) {
    return std::make_unique<BlockWrapper<TBlock>>(std::move(params));
}

/// the loader keeps a pointer to the registry beside it, so the two live and die together here
struct Fixture {
    BlockRegistry            registry;
    SchedulerRegistry        schedulerRegistry;
    std::vector<std::string> directories;
    PluginLoader             loader;

    Fixture()
        : directories{
#ifdef GR_TOOLS_CORE_TEST_PLUGINS
              std::string(GR_TOOLS_CORE_TEST_PLUGINS),
#endif
              std::string(kMissingPlugins)},
          loader(registry, schedulerRegistry, directories) {
        std::ignore = registry.insert<DocScale>("=doc::scale");
        std::ignore = registry.insert<DocCombiner>("=doc::combiner");
        std::ignore = registry.insert(kRefusingKey, "", &refusingFactory);
        std::ignore = registry.insert(kVersionedKey, "", &makeDocBlock<DocFilterV1>, block::versionOf<DocFilterV1>(), block::statusOf<DocFilterV1>());
        std::ignore = registry.insert(kVersionedKey, "", &makeDocBlock<DocFilterV2>, block::versionOf<DocFilterV2>(), block::statusOf<DocFilterV2>());
    }

    [[nodiscard]] std::string document(gr::tools::Format format) {
        const gr::tools::registrydoc::Inputs inputs{.loader = loader, .pluginDirectories = directories, .coreVersion = GR_TOOLS_CORE_VERSION};
        return gr::tools::registrydoc::render(inputs, format, "Registry fixture");
    }
};

/// one fixture for the whole binary: the plugins are opened once
[[nodiscard]] Fixture& fixture() {
    static Fixture instance;
    return instance;
}

#ifdef GR_TOOLS_TEST_BLOCK_LIBRARY
constexpr std::string_view kLibraryKey = "test::library_doubler";

/**
 * @brief The block-library case, over the registries a block library actually registers into.
 *
 * A shared object that is not a plugin reaches `gr::globalBlockRegistry()` from its static
 * initializers, not whichever registry a loader was handed, so this fixture is built over the
 * process-wide pair -- which is also how the command-line tool is built.
 */
struct GlobalFixture {
    std::vector<std::string> directories;
    PluginLoader             loader;

    GlobalFixture()
        : directories{
#ifdef GR_TOOLS_CORE_TEST_PLUGINS
              std::string(GR_TOOLS_CORE_TEST_PLUGINS),
#endif
              std::string(GR_TOOLS_TEST_BLOCK_LIBRARY)},
          loader(gr::globalBlockRegistry(), gr::globalSchedulerRegistry(), directories) {
    }

    [[nodiscard]] std::string document(gr::tools::Format format) {
        const gr::tools::registrydoc::Inputs inputs{.loader = loader, .pluginDirectories = directories, .coreVersion = GR_TOOLS_CORE_VERSION};
        return gr::tools::registrydoc::render(inputs, format, "Block library fixture");
    }
};

/// exactly one loader opens the block-library directory, so the library is opened once
[[nodiscard]] GlobalFixture& blockLibraryFixture() {
    static GlobalFixture instance;
    return instance;
}
#endif

} // namespace qa_registrydoc

const boost::ut::suite<"RegistryDoc"> registryDocTests = [] {
    using namespace boost::ut;
    using namespace gr::tools;
    using namespace qa_registrydoc;

    "the framework section names the version, the switches and the compiler"_test = [] {
        const std::string document = fixture().document(Format::Markdown);
        expect(document.find(GR_TOOLS_CORE_VERSION) != std::string::npos) << "the core version is missing";
        expect(document.find("Block registry") != std::string::npos);
        expect(document.find("Plugin system") != std::string::npos);
        expect(document.find(CXX_COMPILER_ID) != std::string::npos) << "the compiler is missing";
        expect(document.find(CXX_COMPILER_VERSION) != std::string::npos);
    };

    "every registered key is documented with its ports and its settings"_test = [] {
        const std::string document = fixture().document(Format::Markdown);

        for (const std::string& key : fixture().registry.keys()) {
            expect(document.find(key) != std::string::npos) << std::format("key '{}' is missing from the document", key);
        }
        for (const std::string_view port : {"inputs#0", "inputs#1"}) {
            expect(document.find(port) != std::string::npos) << std::format("port '{}' is missing from the document", port);
        }
        for (const std::string_view setting : {"gain", "waveform", "label", "count", "n_inputs", "input_chunk_size"}) {
            expect(document.find(setting) != std::string::npos) << std::format("setting '{}' is missing from the document", setting);
        }
        expect(document.find("multiplies its input by a gain") != std::string::npos) << "the block description is missing";
        expect(document.find("linear voltage gain") != std::string::npos) << "a setting's description is missing";
        expect(document.find("dB") != std::string::npos) << "a setting's unit is missing";
        expect(document.find("triangle") != std::string::npos) << "an enum setting's values are missing";
        expect(document.find("float32") != std::string::npos) << "a port data type is missing";
    };

    "every registered version of a key is reported, with the flags each declares"_test = [] {
        const std::string document = fixture().document(Format::Markdown);
        expect(document.find(kVersionedKey) != std::string::npos) << "the versioned key is not listed";
        expect(document.find("More than one version of this key is registered") != std::string::npos) << "the version table is missing";
        expect(document.find("deprecated, experimental") != std::string::npos) << "the older revision's flags are missing";
        expect(document.find("the second revision of the filter") != std::string::npos) << "the newest version is what an unversioned caller gets";
        expect(document.find("Taken by default") != std::string::npos);
        expect(document.find("**Status**: deprecated, experimental") == std::string::npos) << "the newest is what the section documents, and it declares no flag";
    };

    "a block whose factory throws is listed with its error"_test = [] {
        const std::string document = fixture().document(Format::Markdown);
        expect(document.find(kRefusingKey) != std::string::npos) << "the refusing key is not listed";
        expect(document.find(kRefusalText) != std::string::npos) << "the refusal is not reported";
        expect(document.find("doc::scale") != std::string::npos) << "one refusing key must not end the document";
    };

    "a plugin directory that does not exist is reported and is not fatal"_test = [] {
        const std::string document = fixture().document(Format::Markdown);
        expect(document.find(kMissingPlugins) != std::string::npos) << "the missing directory is not named";
        expect(document.find("not a directory, skipped") != std::string::npos) << "the missing directory is not marked";
        expect(document.find("## Blocks") != std::string::npos) << "the document is otherwise complete";
    };

#ifdef GR_TOOLS_CORE_TEST_PLUGINS
    "the loaded plugins and their metadata are reported"_test = [] {
        const std::string document = fixture().document(Format::Markdown);
        expect(document.find("Good Math Plugin") != std::string::npos) << "a loaded plugin's metadata is missing";
        expect(document.find("good::multiply") != std::string::npos) << "a plugin-provided block is missing";
        expect(document.find("bad_plugin") != std::string::npos) << "the plugin that fails to load is not named";
        expect(document.find("Plugin files that did not load") != std::string::npos);
    };
#endif

    "the table of contents links every key it lists"_test = [] {
        const std::string markdown = fixture().document(Format::Markdown);
        expect(markdown.find("[doc::scale](#block-doc-scale)") != std::string::npos) << "the Markdown contents entry does not link its section";
        expect(markdown.find("<a id=\"block-doc-scale\"></a>") != std::string::npos) << "the Markdown section carries no anchor";

        const std::string html = fixture().document(Format::Html);
        expect(html.find("<a href=\"#block-doc-scale\">") != std::string::npos) << "the HTML contents entry does not link its section";
        expect(html.find("id=\"block-doc-scale\"") != std::string::npos) << "the HTML section carries no anchor";
    };

#ifdef GR_TOOLS_TEST_BLOCK_LIBRARY
    "a shared object that registered blocks is kept mapped and its blocks stay usable"_test = [] {
        GlobalFixture& withLibrary = blockLibraryFixture();

        const auto& libraries = withLibrary.loader.blockLibraries();
        expect(fatal(eq(libraries.size(), 2UZ))) << "core's two block-library fixtures were not recognized as such";
        for (const gr::PluginLoader::BlockLibrary& library : libraries) {
            expect(library.file.contains("block_library"));
            expect(ge(library.nBlockRegistrations, 1UZ)) << "each library registered at least its own block";
        }

        expect(fatal(gr::globalBlockRegistry().contains(kLibraryKey))) << "the library's block is not in the registry";
        // calling the factory is the point: a library that was unloaded leaves the entry behind with
        // a pointer into code that is no longer mapped, and this is where that jump would happen
        std::unique_ptr<BlockModel> block = gr::globalBlockRegistry().create(kLibraryKey, {});
        expect(fatal(block != nullptr)) << "the library's factory produced nothing";
        block->settings().init();
        expect(eq(block->dynamicInputPorts().size(), 1UZ));
        expect(that % block->settings().defaultParameters().contains("extra_gain"));
    };

    "the document names the block library and documents its blocks"_test = [] {
        const std::string document = blockLibraryFixture().document(Format::Markdown);
        expect(document.find("Block libraries") != std::string::npos) << "the block-library section is missing";
        expect(document.find("block_library") != std::string::npos) << "the library file is not named";
        expect(document.find(kLibraryKey) != std::string::npos) << "the library's block has no section";
        expect(document.find("applied on top of the doubling") != std::string::npos) << "the library block's settings are not described";
        expect(document.find("bad_plugin") != std::string::npos) << "a library that registered nothing is still a failure";
    };
#endif

    "the HTML page carries no external reference"_test = [] {
        const std::string html = fixture().document(Format::Html);
        for (const std::string_view forbidden : {"<script", "<link", "<img", "http://", "https://", "@import"}) {
            expect(html.find(forbidden) == std::string::npos) << std::format("the page reaches outside itself: '{}'", forbidden);
        }
        expect(html.starts_with("<!DOCTYPE html>"));
    };
};

int main() { /* not needed for UT */ }
