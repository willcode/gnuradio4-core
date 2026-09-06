#ifndef GNURADIO_TOOLS_REGISTRYDOC_HPP
#define GNURADIO_TOOLS_REGISTRYDOC_HPP

#include <algorithm>
#include <array>
#include <exception>
#include <filesystem>
#include <format>
#include <limits>
#include <map>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gnuradio-4.0/BlockModel.hpp>
#include <gnuradio-4.0/BlockRegistry.hpp>
#include <gnuradio-4.0/PluginLoader.hpp>
#include <gnuradio-4.0/PluginMetadata.hpp>
#include <gnuradio-4.0/Settings.hpp>
#include <gnuradio-4.0/config.hpp>
#include <gnuradio-4.0/meta/formatter.hpp>

#include "DocWriter.hpp"
#include "GraphDoc.hpp"

namespace gr::tools::registrydoc {

using graphdoc::valueText;

/// what the document is made from: a loader already holding its plugins, and the facts about the
/// build that no runtime object carries
struct Inputs {
    gr::PluginLoader&            loader;
    std::span<const std::string> pluginDirectories;
    std::string_view             coreVersion;
};

#ifdef GR_ENABLE_BLOCK_REGISTRY
inline constexpr std::string_view kRegistryState = "enabled";
#else
inline constexpr std::string_view kRegistryState = "disabled";
#endif

#ifdef INTERNAL_ENABLE_BLOCK_PLUGINS
inline constexpr std::string_view kPluginState = "enabled";
#else
inline constexpr std::string_view kPluginState = "disabled";
#endif

/// the meta-information suffixes `SettingsBase::init()` writes for an annotated member
inline constexpr std::array<std::string_view, 6> kSettingMetaSuffixes{"::description", "::documentation", "::unit", "::visible", "::enum_values", "::enum_type"};

[[nodiscard]] inline std::string metaString(const property_map& meta, std::string_view key) {
    const auto it = meta.find(key);
    if (it == meta.cend()) {
        return {};
    }
    const std::string_view view = it->second.value_or(std::string_view{});
    return view.data() == nullptr ? valueText(it->second) : std::string(view);
}

[[nodiscard]] inline std::string joined(std::span<const std::string> items, std::string_view separator) {
    std::string out;
    for (std::size_t i = 0UZ; i < items.size(); ++i) {
        if (i != 0UZ) {
            out += separator;
        }
        out += items[i];
    }
    return out;
}

[[nodiscard]] inline std::string yesNo(bool value) { return value ? "yes" : "no"; }

/// the qualities a block declares, named; a block that declares none reads as plain, which is the common case
[[nodiscard]] inline std::string statusText(block::Status status) {
    std::vector<std::string> set;
    if (status.deprecated) {
        set.emplace_back("deprecated");
    }
    if (status.experimental) {
        set.emplace_back("experimental");
    }
    return set.empty() ? "none declared" : joined(set, ", ");
}

/// the sample-count bound a port declares; the largest representable count means "no bound"
[[nodiscard]] inline std::string sampleBound(std::size_t count) { //
    return count == std::numeric_limits<std::size_t>::max() ? "unbounded" : std::to_string(count);
}

/// one row of the port table, already spelled for the document
[[nodiscard]] inline std::vector<std::string> portRow(std::string_view direction, std::string_view index, std::string_view name, gr::DynamicPort& port) {
    const port::BitMask mask = port.portMaskInfo();
    return {
        std::string(direction),
        std::string(index),
        std::string(name),
        std::string(port.metaInfo.data_type),
        std::format("{}", port::decodePortType(mask)),
        yesNo(port.isOptional()),
        yesNo(port.isSynchronous()),
        std::string(port.domain()),
        std::format("{} .. {}", sampleBound(port.min_samples), sampleBound(port.max_samples)),
    };
}

inline void collectPorts(BlockModel::DynamicPorts& ports, std::string_view direction, std::vector<std::vector<std::string>>& rows) {
    for (std::size_t i = 0UZ; i < ports.size(); ++i) {
        if (auto* collection = std::get_if<BlockModel::NamedPortCollection>(&ports[i]); collection != nullptr) {
            for (std::size_t j = 0UZ; j < collection->ports.size(); ++j) {
                rows.push_back(portRow(direction, std::format("{}.{}", i, j), std::format("{}#{}", collection->name, j), collection->ports[j]));
            }
            if (collection->ports.empty()) {
                rows.push_back({std::string(direction), std::to_string(i), std::format("{} (empty collection)", collection->name), "", "", "", "", "", ""});
            }
        } else if (auto* port = std::get_if<gr::DynamicPort>(&ports[i]); port != nullptr) {
            rows.push_back(portRow(direction, std::to_string(i), std::string(port->metaInfo.name), *port));
        }
    }
}

/**
 * @brief Documents one registered key from a default-constructed instance of it.
 *
 * `settings().init()` is called rather than the block's full `init()`: it is the call that
 * copies a block's annotations into `meta_information` and stores the defaults, and it needs
 * neither a progress counter nor a thread pool. Anything the instance throws on the way is
 * reported in place of the block's detail, so one uncooperative key does not end the document.
 */
inline void writeBlock(DocWriter& writer, std::string_view key, std::string_view origin, gr::PluginLoader& loader, std::string_view anchor) {
    writer.heading(3UZ, std::string(key), anchor);

    std::shared_ptr<gr::BlockModel> instance;
    std::string                     failure;
    try {
        if (auto owned = loader.registry().create(key, {}); owned) {
            instance = std::shared_ptr<gr::BlockModel>(std::move(owned));
        } else {
            const auto created = loader.instantiateOrError(key, {});
            if (!created.has_value()) {
                failure = created.error().message;
            } else {
                instance = *created;
            }
        }
        if (instance == nullptr && failure.empty()) {
            failure = "nothing of that name could be instantiated";
        }
    } catch (const gr::exception& e) {
        // the message alone: what() appends the throwing source location, which is a path on the
        // machine that built the library and says nothing to the reader of the document
        failure = e.message;
    } catch (const std::exception& e) {
        failure = e.what();
    } catch (...) {
        failure = "an exception that is not a std::exception";
    }

    std::vector<std::string> facts;
    facts.push_back(writer.labeled("Provided by", origin));
    if (instance == nullptr) {
        facts.push_back(writer.labeled("Error", failure));
        writer.rawBullets(facts);
        return;
    }

    std::string settingsFailure;
    try {
        instance->settings().init();
    } catch (const gr::exception& e) {
        settingsFailure = e.message;
    } catch (const std::exception& e) {
        settingsFailure = e.what();
    } catch (...) {
        settingsFailure = "an exception that is not a std::exception";
    }

    facts.push_back(writer.labeled("Type name", std::string(instance->typeName())));
    facts.push_back(writer.labeled("Block category", std::format("{}", instance->blockCategory())));
    facts.push_back(writer.labeled("UI category", std::format("{}", instance->uiCategory())));

    // Facts, not advice: a version and a status are printed where the block has something to say and are
    // left out where it has not, and nothing here refuses, warns about or reorders anything on either.
    const std::vector<block::Version> registeredVersions = loader.registry().versions(key);
    const block::Status               declaredStatus     = instance->status();
    if (instance->version() != block::kDefaultVersion || registeredVersions.size() > 1UZ) {
        facts.push_back(writer.labeled("Version", std::to_string(instance->version())));
    }
    if (declaredStatus.any()) {
        facts.push_back(writer.labeled("Status", statusText(declaredStatus)));
    }
    if (!settingsFailure.empty()) {
        facts.push_back(writer.labeled("Settings error", settingsFailure));
    }
    writer.rawBullets(facts);

    if (registeredVersions.size() > 1UZ) {
        writer.paragraph("More than one version of this key is registered. A caller that names no version gets the newest.");
        std::vector<std::vector<std::string>> versionRows;
        versionRows.reserve(registeredVersions.size());
        for (const block::Version version : registeredVersions) {
            versionRows.push_back({std::to_string(version), statusText(loader.registry().status(key, version).value_or(block::Status{})), version == registeredVersions.back() ? "newest" : ""});
        }
        const std::array<std::string_view, 3> versionHeaders{"Version", "Status", "Taken by default"};
        writer.table(versionHeaders, versionRows);
    }

    const property_map& meta        = instance->metaInformation();
    const std::string   description = metaString(meta, "description");
    if (!description.empty()) {
        writer.paragraph(description);
    }

    std::vector<std::vector<std::string>> portRows;
    collectPorts(instance->dynamicInputPorts(), "input", portRows);
    collectPorts(instance->dynamicOutputPorts(), "output", portRows);
    if (portRows.empty()) {
        writer.paragraph("This block declares no stream ports.");
    } else {
        const std::array<std::string_view, 9> headers{"Direction", "Index", "Name", "Data type", "Port type", "Optional", "Synchronous", "Domain", "Samples per work call"};
        writer.table(headers, portRows);
    }

    const SettingsBase&          settings  = instance->settings();
    const property_map           defaults  = settings.defaultParameters();
    const std::set<std::string>& writable  = settings.writableMembers();
    const std::set<std::string>& forwarded = settings.autoForwardParameters();
    std::set<std::string>        settingKeys(writable.begin(), writable.end());
    for (const auto& [name, _] : defaults) {
        settingKeys.emplace(std::string_view(name));
    }

    std::set<std::string> consumedMetaKeys{"description"};
    if (settingKeys.empty()) {
        writer.paragraph("This block declares no settings.");
    } else {
        std::vector<std::vector<std::string>> settingRows;
        settingRows.reserve(settingKeys.size());
        for (const std::string& name : settingKeys) {
            const auto        defaultIt     = defaults.find(std::string_view(name));
            std::string       type          = defaultIt == defaults.cend() ? std::string{} : gr::pmt::detail::type_name(defaultIt->second);
            const std::string enumValuesKey = name + "::enum_values";
            if (const auto enumValues = meta.find(std::string_view(enumValuesKey)); enumValues != meta.cend()) {
                type += std::format(" (enum {}: {})", metaString(meta, name + "::enum_type"), valueText(enumValues->second));
            }
            for (const std::string_view suffix : kSettingMetaSuffixes) {
                consumedMetaKeys.emplace(name + std::string(suffix));
            }
            settingRows.push_back({
                name,
                defaultIt == defaults.cend() ? "(not stored)" : valueText(defaultIt->second),
                std::move(type),
                yesNo(writable.contains(name)),
                yesNo(forwarded.contains(name)),
                metaString(meta, name + "::unit"),
                metaString(meta, name + "::description"),
                metaString(meta, name + "::documentation"),
            });
        }
        const std::array<std::string_view, 8> headers{"Setting", "Default", "Type", "Writable", "Auto-forwarded", "Unit", "Description", "Documentation"};
        writer.table(headers, settingRows);
    }

    std::vector<graphdoc::NamedText> remainingMeta;
    for (const auto& [name, value] : meta) {
        const std::string entry{std::string_view(name)};
        if (!consumedMetaKeys.contains(entry)) {
            remainingMeta.emplace_back(entry, valueText(value));
        }
    }
    std::ranges::sort(remainingMeta, [](const graphdoc::NamedText& a, const graphdoc::NamedText& b) { return a.name < b.name; });
    if (!remainingMeta.empty()) {
        writer.heading(4UZ, "Other meta information");
        graphdoc::writeNamedTable(writer, "Key", "Value", remainingMeta);
    }
}

inline void writeFramework(DocWriter& writer, const Inputs& inputs) {
    writer.heading(2UZ, "Framework");
    const std::vector<std::vector<std::string>> rows{
        {"Core version", std::string(inputs.coreVersion)},
        {"Block registry", std::string(kRegistryState)},
        {"Plugin system", std::string(kPluginState)},
        {"C++ compiler", std::format("{} {}", CXX_COMPILER_ID, CXX_COMPILER_VERSION)},
    };
    const std::array<std::string_view, 2> headers{"Property", "Value"};
    writer.table(headers, rows);

    writer.heading(3UZ, "Plugin directories");
    if (inputs.pluginDirectories.empty()) {
        writer.paragraph("No plugin directory was given, so only what is linked into this program is documented.");
    } else {
        std::vector<std::vector<std::string>> directoryRows;
        directoryRows.reserve(inputs.pluginDirectories.size());
        for (const std::string& directory : inputs.pluginDirectories) {
            std::error_code   ignored;
            const std::string status = std::filesystem::is_directory(directory, ignored) ? "read" : "not a directory, skipped";
            directoryRows.push_back({directory, status});
        }
        const std::array<std::string_view, 2> directoryHeaders{"Directory", "Status"};
        writer.table(directoryHeaders, directoryRows);
    }
}

inline void writePlugins(DocWriter& writer, gr::PluginLoader& loader) {
    writer.heading(2UZ, "Plugins");
#ifdef INTERNAL_ENABLE_BLOCK_PLUGINS
    std::vector<std::vector<std::string>> rows;
    for (const auto& plugin : loader.plugins()) {
        const gr_plugin_metadata& metadata = plugin->metadata;
        rows.push_back({metadata.plugin_name, metadata.plugin_author, metadata.plugin_license, metadata.plugin_version, std::to_string(plugin->availableBlocks().size()), std::to_string(plugin->availableSchedulers().size())});
    }
    std::ranges::sort(rows, [](const std::vector<std::string>& a, const std::vector<std::string>& b) { return a[0] < b[0]; });
    if (rows.empty()) {
        writer.paragraph("No plugin library was loaded.");
    } else {
        const std::array<std::string_view, 6> headers{"Name", "Author", "License", "Version", "Blocks", "Schedulers"};
        writer.table(headers, rows);
    }

    std::vector<std::vector<std::string>> libraries;
    for (const gr::PluginLoader::BlockLibrary& library : loader.blockLibraries()) {
        libraries.push_back({std::filesystem::path(library.file).filename().string(), std::to_string(library.nBlockRegistrations), std::to_string(library.nSchedulerRegistrations)});
    }
    std::ranges::sort(libraries, [](const std::vector<std::string>& a, const std::vector<std::string>& b) { return a[0] < b[0]; });
    if (!libraries.empty()) {
        writer.heading(3UZ, "Block libraries");
        writer.paragraph("Shared objects that carry no plugin interface. They registered their blocks as they loaded, and are kept mapped for the lifetime of the process.");
        const std::array<std::string_view, 3> headers{"File", "Block registrations", "Scheduler registrations"};
        writer.table(headers, libraries);
    }

    std::vector<std::vector<std::string>> failures;
    for (const auto& [file, reason] : loader.failedPlugins()) {
        failures.push_back({std::filesystem::path(file).filename().string(), reason});
    }
    std::ranges::sort(failures, [](const std::vector<std::string>& a, const std::vector<std::string>& b) { return a[0] < b[0]; });
    if (!failures.empty()) {
        writer.heading(3UZ, "Plugin files that did not load");
        const std::array<std::string_view, 2> headers{"File", "Reason"};
        writer.table(headers, failures);
    }
#else
    static_cast<void>(loader);
    writer.paragraph("This build has no plugin system, so no plugin library can be loaded.");
#endif

    if (!loader.skippedFiles().empty()) {
        std::vector<std::vector<std::string>> skipped;
        skipped.reserve(loader.skippedFiles().size());
        for (const std::string& file : loader.skippedFiles()) {
            skipped.push_back({file});
        }
        std::ranges::sort(skipped, [](const std::vector<std::string>& a, const std::vector<std::string>& b) { return a[0] < b[0]; });
        writer.heading(3UZ, "Files that were not opened");
        writer.paragraph("A plugin directory is scanned for names ending in the platform's shared-object extension that resolve to a regular file. These read as shared objects but do not meet that test: a symlink that does not resolve, or a soname-style name such as libfoo.so.1.");
        const std::array<std::string_view, 1> headers{"File"};
        writer.table(headers, skipped);
    }

    std::vector<std::vector<std::string>> definitions;
    for (const auto& [blockType, definition] : loader.definitionForBlockName()) {
        definitions.push_back({blockType, definition.metadata.plugin_name, definition.metadata.plugin_author, definition.metadata.plugin_license, definition.metadata.plugin_version});
    }
    std::ranges::sort(definitions, [](const std::vector<std::string>& a, const std::vector<std::string>& b) { return a[0] < b[0]; });
    if (!definitions.empty()) {
        writer.heading(3UZ, "Block definitions");
        const std::array<std::string_view, 5> headers{"Block type", "Name", "Author", "License", "Version"};
        writer.table(headers, definitions);
    }
    if (loader.nSkippedAssets() != 0UZ) {
        writer.paragraph(std::format("{} block definitions listed by an index could not be registered; each was reported as it was skipped.", loader.nSkippedAssets()));
    }
}

inline void writeSchedulers(DocWriter& writer, gr::PluginLoader& loader) {
    writer.heading(2UZ, "Schedulers");
    std::vector<std::string> names = loader.availableSchedulers();
    std::ranges::sort(names);
    if (names.empty()) {
        writer.paragraph("No scheduler is registered in this program.");
        return;
    }
    std::vector<std::vector<std::string>> rows;
    rows.reserve(names.size());
    for (const std::string& name : names) {
        rows.push_back({name});
    }
    const std::array<std::string_view, 1> headers{"Key"};
    writer.table(headers, rows);
}

/**
 * @brief The whole document.
 *
 * The key set is the union of the registry's own keys with the names the loaded plugins answer
 * to, which is what `PluginLoader::availableBlocks()` reports. A plugin that registers into the
 * `gr::plugin<>` instance its own header declares contributes nothing to `BlockRegistry::keys()`,
 * and would otherwise be missing from a document whose subject is what the build offers.
 */
[[nodiscard]] inline std::string render(const Inputs& inputs, Format format, std::string title) {
    DocWriter writer(format, std::move(title));

    writeFramework(writer, inputs);
    writePlugins(writer, inputs.loader);
    writeSchedulers(writer, inputs.loader);

    std::vector<std::string> keys = inputs.loader.availableBlocks();
    std::ranges::sort(keys);
    keys.erase(std::ranges::unique(keys).begin(), keys.end());

    writer.heading(2UZ, "Blocks");
    if (keys.empty()) {
        writer.paragraph("No block is registered in this program.");
        return writer.finish();
    }
    writer.paragraph(std::format("{} registered keys.", keys.size()));

    std::vector<std::string> anchors;
    anchors.reserve(keys.size());
    std::vector<std::string> contents;
    contents.reserve(keys.size());
    for (const std::string& key : keys) {
        anchors.push_back(writer.anchor(std::format("block {}", key)));
        contents.push_back(writer.link(key, anchors.back()));
    }
    writer.rawBullets(contents);

    for (std::size_t i = 0UZ; i < keys.size(); ++i) {
        const std::string_view origin = inputs.loader.registry().contains(keys[i]) ? "the block registry" : "a plugin or a block definition";
        writeBlock(writer, keys[i], origin, inputs.loader, anchors[i]);
    }

    return writer.finish();
}

} // namespace gr::tools::registrydoc

#endif // GNURADIO_TOOLS_REGISTRYDOC_HPP
