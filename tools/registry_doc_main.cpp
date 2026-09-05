#include <cstdlib>
#include <print>
#include <string>
#include <string_view>
#include <vector>

#include <gnuradio-4.0/BlockRegistry.hpp>
#include <gnuradio-4.0/PluginLoader.hpp>

#include "DocOptions.hpp"
#include "RegistryDoc.hpp"

namespace {

constexpr std::string_view kUsage = R"(gnuradio_4_0_registry_doc - describe the registered blocks as Markdown or HTML

Usage: gnuradio_4_0_registry_doc [options]

  --plugin-dir <dir> a directory to load plugins and block definitions from; repeatable
  --format md|html   output format (default: md)
  --output <file>    write the document to <file> (default: standard output)
  --title <text>     document title
  --help, -h         this text

With no --plugin-dir, the directories come from GNURADIO4_PLUGIN_DIRECTORIES, the
colon-separated list the framework's own plugin loader reads. A directory that does not
exist is named in the document and is not an error.

A directory is scanned for names whose final extension is the platform's shared-object
extension and that resolve to a regular file; a symlink is followed and loads like the
file it points at. A soname-style name such as libfoo.so.1, and a symlink that does not
resolve, are not opened, and the document lists them so the omission is visible.

A shared object with no plugin interface that registers its blocks as it loads is
documented as a block library and is kept mapped for the run: its registry entries hold
factory pointers into its own code.

Everything documented is read from the running program: the block registry linked into
it, the plugins loaded from those directories, and a default-constructed instance of each
registered key. No source file is read and no block list is built in.

The HTML is a single self-contained page: its style sheet is embedded and it loads
nothing over the network.
)";

[[nodiscard]] std::vector<std::string> directoriesFromEnvironment() {
    std::vector<std::string> directories;
    const char*              value = std::getenv("GNURADIO4_PLUGIN_DIRECTORIES");
    if (value == nullptr) {
        return directories;
    }
    const std::string_view list(value);
    for (std::size_t start = 0UZ; start < list.size();) {
        const std::size_t separator = list.find(':', start);
        const std::size_t end       = separator == std::string_view::npos ? list.size() : separator;
        if (end > start) {
            directories.emplace_back(list.substr(start, end - start));
        }
        start = end + 1UZ;
    }
    return directories;
}

} // namespace

int main(int argc, char** argv) {
    using namespace gr::tools;

    const std::vector<std::string_view> arguments = argumentsOf(argc, argv);
    CommonOptions                       options;
    std::vector<std::string>            pluginDirectories;
    std::string                         error;

    for (std::size_t index = 0UZ; index < arguments.size();) {
        switch (readCommonOption(arguments, index, options, error)) {
        case OptionResult::Taken: continue;
        case OptionResult::Failed: std::println(stderr, "gnuradio_4_0_registry_doc: {}", error); return 2;
        case OptionResult::NotMine: break;
        }
        const std::string_view argument = arguments[index];
        if (argument == "--plugin-dir") {
            if (index + 1UZ >= arguments.size()) {
                std::println(stderr, "gnuradio_4_0_registry_doc: --plugin-dir needs a value");
                return 2;
            }
            pluginDirectories.emplace_back(arguments[index + 1UZ]);
            index += 2UZ;
            continue;
        }
        std::println(stderr, "gnuradio_4_0_registry_doc: unknown option '{}'", argument);
        return 2;
    }

    if (options.help) {
        std::print("{}", kUsage);
        return 0;
    }
    if (pluginDirectories.empty()) {
        pluginDirectories = directoriesFromEnvironment();
    }
    if (options.title.empty()) {
        options.title = "GNU Radio 4 block registry";
    }

    gr::PluginLoader loader(gr::globalBlockRegistry(), gr::globalSchedulerRegistry(), pluginDirectories);

    const registrydoc::Inputs inputs{.loader = loader, .pluginDirectories = pluginDirectories, .coreVersion = GR_TOOLS_CORE_VERSION};
    const auto                written = writeDocument(options.output, registrydoc::render(inputs, options.format, options.title));
    if (!written.has_value()) {
        std::println(stderr, "gnuradio_4_0_registry_doc: {}", written.error());
        return 2;
    }
    return 0;
}
