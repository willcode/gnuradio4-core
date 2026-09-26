#ifndef GNURADIO_TOOLS_BLOCKLOOKUP_HPP
#define GNURADIO_TOOLS_BLOCKLOOKUP_HPP

// The lookups a tool makes through the plugin loader without running a block.

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include <gnuradio-4.0/BlockAttributes.hpp>
#include <gnuradio-4.0/BlockModel.hpp>
#include <gnuradio-4.0/BlockRegistry.hpp>
#include <gnuradio-4.0/PluginLoader.hpp>
#include <gnuradio-4.0/Settings.hpp>

namespace gr::tools {

/// one directory a tool searches for blocks, and whether it is a directory at all
struct Directory {
    std::string path;
    std::string origin; // option, environment, or installation
    bool        present = false;
};

/**
 * @brief The directories a tool searches for blocks, in the order it searches them.
 *
 * The command line comes first, then the colon-separated list in GNURADIO4_PLUGIN_DIRECTORIES,
 * then `installed`, which is the plugin directory of the installation the caller was built for and
 * is always searched. A directory named twice is searched once.
 */
[[nodiscard]] inline std::vector<Directory> searchDirectories(std::span<const std::string> fromCommandLine, std::string_view installed) {
    std::vector<Directory> directories;
    auto                   add = [&directories](std::string_view path, std::string_view origin) {
        if (path.empty() || std::ranges::any_of(directories, [path](const Directory& held) { return held.path == path; })) {
            return;
        }
        std::error_code ignored;
        directories.push_back({.path = std::string(path), .origin = std::string(origin), .present = std::filesystem::is_directory(path, ignored)});
    };
    for (const std::string& directory : fromCommandLine) {
        add(directory, "option");
    }
    if (const char* environment = std::getenv("GNURADIO4_PLUGIN_DIRECTORIES"); environment != nullptr) {
        const std::string_view list(environment);
        for (std::size_t start = 0UZ; start < list.size();) {
            const std::size_t separator = list.find(':', start);
            const std::size_t end       = separator == std::string_view::npos ? list.size() : separator;
            add(list.substr(start, end - start), "environment");
            start = end + 1UZ;
        }
    }
    add(installed, "installation");
    return directories;
}

/// one output port of a block, as the block declares it
struct OutputPort {
    std::string name;
    std::string dataType;
};

/**
 * @brief The type of the items a block's output port carries, read from an instance of the block.
 *
 * A registry key is all a graph file gives, and only a block itself knows what its ports carry, so
 * the answer comes from a default-constructed instance: `settings().init()` copies the annotations
 * into the meta information and fills the port descriptions, and the block is never started. A key
 * no registry holds, a factory that refuses, and a port name the block does not declare each answer
 * with an empty string. The caller then prints no type at all, never a guess.
 *
 * Every key is read once and kept, because a graph names the same block on many connections.
 */
class OutputPortTypes {
    PluginLoader&                                               _loader;
    std::map<std::string, std::vector<OutputPort>, std::less<>> _byKey;

public:
    explicit OutputPortTypes(PluginLoader& loader) : _loader(loader) {}

    /// `port` is the port as a connection spells it: its name, its name and index within a
    /// collection, or its position among the block's output ports with an optional sub-index
    [[nodiscard]] std::string operator()(std::string_view blockKey, std::string_view port) {
        const std::vector<OutputPort>& ports = outputsOf(blockKey);
        if (ports.empty()) {
            return {};
        }

        const std::string_view index = port.substr(0UZ, port.find('.'));
        std::size_t            at    = 0UZ;
        if (const auto [after, failed] = std::from_chars(index.data(), index.data() + index.size(), at); failed == std::errc{} && after == index.data() + index.size()) {
            return at < ports.size() ? ports[at].dataType : std::string{};
        }

        const std::string_view name  = port.substr(0UZ, port.find('#'));
        const auto             named = std::ranges::find(ports, name, &OutputPort::name);
        return named == ports.end() ? std::string{} : named->dataType;
    }

private:
    const std::vector<OutputPort>& outputsOf(std::string_view blockKey) {
        if (const auto held = _byKey.find(blockKey); held != _byKey.end()) {
            return held->second;
        }

        std::shared_ptr<BlockModel> instance;
        try {
            if (const auto made = _loader.instantiateOrError(blockKey, {}); made.has_value()) {
                instance = *made;
            }
        } catch (...) {
            instance = nullptr; // a factory that throws leaves every connection out of it blank
        }

        std::vector<OutputPort> ports;
        if (instance != nullptr) {
            try {
                instance->settings().init();
            } catch (...) {
                // a block that refuses its own defaults still declares its ports
            }
            // a collection's members all carry the type of the collection, so it enters as one entry
            for (auto& entry : instance->dynamicOutputPorts()) {
                if (auto* collection = std::get_if<BlockModel::NamedPortCollection>(&entry); collection != nullptr) {
                    ports.push_back({.name = std::string(collection->name), .dataType = collection->ports.empty() ? std::string{} : std::string(collection->ports.front().metaInfo.data_type)});
                } else if (auto* single = std::get_if<DynamicPort>(&entry); single != nullptr) {
                    ports.push_back({.name = std::string(single->metaInfo.name), .dataType = std::string(single->metaInfo.data_type)});
                }
            }
        }
        return _byKey.emplace(std::string(blockKey), std::move(ports)).first->second;
    }
};

/// one label a block type carries, with what the loaded vocabulary says of it
struct LabelText {
    block::LabelClass        cls{};
    std::string              label;    ///< `class/word`
    std::vector<std::string> meanings; ///< empty for a word outside the loaded vocabulary or declared without a meaning
    bool                     known    = false;
    bool                     physical = false; ///< the vocabulary's reading of an `emits` or `ingests` word, true for one it lacks
};

/// whether `cls` names a medium, the one kind of word the vocabulary reads as physical or not
[[nodiscard]] constexpr bool isMedium(block::LabelClass cls) noexcept { return cls == block::LabelClass::Emits || cls == block::LabelClass::Ingests; }

/// the labels `attributes` carries, in the order of the map, each read against `vocabulary`
[[nodiscard]] inline std::vector<LabelText> labelTexts(const property_map& attributes, const block::Vocabulary& vocabulary) {
    std::vector<LabelText> texts;
    for (const block::LabelRead& label : block::attributesFromMap(attributes, vocabulary).labels) {
        const block::VocabularyEntry* entry = vocabulary.find(label.cls, label.word);
        texts.push_back({.cls = label.cls, .label = label.text(), .meanings = entry == nullptr ? std::vector<std::string>{} : entry->meanings, .known = label.known, .physical = isMedium(label.cls) && vocabulary.physical(label.cls, label.word)});
    }
    return texts;
}

/**
 * @brief The text a tool prints beside a label: its meanings, and a mark for what a reader must not miss.
 *
 * Several meanings are joined by "; ". A word outside the loaded vocabulary reads "(outside the loaded vocabulary)",
 * and an `emits` word the vocabulary reads as physical ends in "(physical)", an unknown one included.
 */
[[nodiscard]] inline std::string meaningText(const LabelText& text) {
    std::string meaning;
    for (const std::string& one : text.meanings) {
        meaning += meaning.empty() ? one : std::format("; {}", one);
    }
    if (!text.known) {
        meaning += meaning.empty() ? "(outside the loaded vocabulary)" : " (outside the loaded vocabulary)";
    }
    if (text.physical && text.cls == block::LabelClass::Emits) {
        meaning += " (physical)";
    }
    return meaning;
}

/// the role the stream ports of `block` read; `isNotation` states whether its labels hold `plane/notation`
[[nodiscard]] inline std::optional<block::Label> portRoleOf(BlockModel& block, bool isNotation) {
    const auto holdsStream = [](BlockModel::DynamicPorts& ports) {
        const auto isStream = [](const DynamicPort& port) { return port::isStream(port.portMaskInfo()); };
        return std::ranges::any_of(ports, [&isStream](const BlockModel::DynamicPortOrCollection& portOrCollection) {
            if (const auto* port = std::get_if<DynamicPort>(&portOrCollection); port != nullptr) {
                return isStream(*port);
            }
            return std::ranges::any_of(std::get<BlockModel::NamedPortCollection>(portOrCollection).ports, isStream);
        });
    };
    return block::roleFromPorts(holdsStream(block.dynamicInputPorts()), holdsStream(block.dynamicOutputPorts()), isNotation);
}

/**
 * @brief The note a tool prints when a declared role contradicts the role the stream ports read, else nothing.
 *
 * A `source` whose ports read `consumer`, a `sink` whose ports read `generator`, and a declared `generator`,
 * `processor`, `consumer` or `notation` the ports read otherwise each contradict them. Nothing is refused.
 */
[[nodiscard]] inline std::optional<std::string> roleNote(std::string_view declared, std::string_view read) {
    namespace role           = block::labels::role;
    const bool readWord      = declared == role::generator.word || declared == role::processor.word || declared == role::consumer.word || declared == role::notation.word;
    const bool contradiction = (declared == role::source.word && read == role::consumer.word) || (declared == role::sink.word && read == role::generator.word) || (readWord && declared != read);
    if (!contradiction) {
        return std::nullopt;
    }
    return std::format("{} declared; the stream ports read {}", block::Label{block::LabelClass::Role, declared}.text(), read.empty() ? std::string_view("no role") : read);
}

/// the declared role word among `attributes`' labels, empty when it declares none
[[nodiscard]] inline std::string declaredRole(const property_map& attributes) {
    const std::vector<std::string_view> roles = block::attributesFromMap(attributes).words(block::LabelClass::Role);
    return roles.empty() ? std::string{} : std::string(roles.front());
}

/**
 * @brief The attributes map of the revision of `blockType` a graph entry takes.
 *
 * `pinnedVersion` is the entry's `version` as the file spells it. An empty `pinnedVersion` selects the newest revision.
 * A `pinnedVersion` that is not a revision number, and a type or a revision the loader does not hold, yield nothing.
 * The function reads the attributes the type registered with and makes no instance of the block.
 */
[[nodiscard]] inline std::optional<property_map> attributesAt(const PluginLoader& loader, std::string_view blockType, std::string_view pinnedVersion) {
    if (pinnedVersion.empty()) {
        return loader.blockAttributes(blockType);
    }
    block::Version version = 0U;
    const char*    end     = pinnedVersion.data() + pinnedVersion.size();
    if (const auto [after, failed] = std::from_chars(pinnedVersion.data(), end, version); failed != std::errc{} || after != end) {
        return std::nullopt;
    }
    return loader.blockAttributes(blockType, version);
}

} // namespace gr::tools

#endif // GNURADIO_TOOLS_BLOCKLOOKUP_HPP
