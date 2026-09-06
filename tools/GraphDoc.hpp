#ifndef GNURADIO_TOOLS_GRAPHDOC_HPP
#define GNURADIO_TOOLS_GRAPHDOC_HPP

#include <algorithm>
#include <array>
#include <expected>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gnuradio-4.0/Tag.hpp>
#include <gnuradio-4.0/Value.hpp>
#include <gnuradio-4.0/YamlPmt.hpp>
#include <gnuradio-4.0/formatter/ValueFormatter.hpp>

#include "DocWriter.hpp"

namespace gr::tools::graphdoc {

/// The block-entry keys the reader interprets; anything else a file carries is listed as an
/// uninterpreted key rather than dropped, so a document never hides part of its input.
inline constexpr std::array<std::string_view, 9> kKnownBlockKeys{"id", "name", "unique_name", "block_category", "meta_information", "parameters", "ctx_parameters", "scheduler", "graph"};

/// A composite entry -- the one carrying `graph` -- additionally declares what the definition
/// exports, which `gr::detail::readRecipeDeclarations` reads from that entry and from no other.
/// The key is therefore interpreted only there; on a plain block it stays uninterpreted, which is
/// what it is to the loader as well.
inline constexpr auto kKnownSubgraphKeys = [] {
    std::array<std::string_view, kKnownBlockKeys.size() + 1UZ> keys{};
    std::ranges::copy(kKnownBlockKeys, keys.begin());
    keys.back() = "exported_parameters";
    return keys;
}();

/// the same for a graph level
inline constexpr std::array<std::string_view, 4> kKnownGraphKeys{"blocks", "connections", "exported_ports", "definition_metadata"};

[[nodiscard]] inline const property_map* mapOf(const pmt::Value& value) noexcept { return value.get_if<property_map>(); }

[[nodiscard]] inline const Tensor<pmt::Value>* listOf(const pmt::Value& value) noexcept { return value.get_if<Tensor<pmt::Value>>(); }

/// the string a value holds, or nothing: a non-string value yields a view with no data
[[nodiscard]] inline std::optional<std::string> stringOf(const pmt::Value& value) {
    const std::string_view view = value.value_or(std::string_view{});
    if (view.data() == nullptr) {
        return std::nullopt;
    }
    return std::string(view);
}

[[nodiscard]] inline const pmt::Value* entryOf(const property_map& map, std::string_view key) noexcept {
    const auto it = map.find(key);
    return it == map.cend() ? nullptr : &it->second;
}

/**
 * @brief One line of text for a value, with map keys in sorted order.
 *
 * `gr::pmt::to_string` walks a map in the hash order the container happens to have, which
 * differs between runs; a document that is compared against a pinned expectation needs the
 * same bytes every time, so maps are spelled here and everything else is delegated.
 */
[[nodiscard]] inline std::string valueText(const pmt::Value& value) {
    if (const property_map* map = mapOf(value); map != nullptr) {
        std::vector<std::string> entries;
        entries.reserve(map->size());
        for (const auto& [key, entry] : *map) {
            entries.emplace_back(std::format("{}: {}", std::string_view(key), valueText(entry)));
        }
        std::ranges::sort(entries);
        std::string out = "{";
        for (std::size_t i = 0UZ; i < entries.size(); ++i) {
            if (i != 0UZ) {
                out += ", ";
            }
            out += entries[i];
        }
        out += "}";
        return out;
    }
    return gr::pmt::to_string(value);
}

/// the sorted `key: value` lines of a map
[[nodiscard]] inline std::string mapLines(const property_map& map) {
    std::vector<std::string> lines;
    lines.reserve(map.size());
    for (const auto& [key, value] : map) {
        lines.emplace_back(std::format("{}: {}", std::string_view(key), valueText(value)));
    }
    std::ranges::sort(lines);
    std::string out;
    for (std::size_t i = 0UZ; i < lines.size(); ++i) {
        if (i != 0UZ) {
            out += "\n";
        }
        out += lines[i];
    }
    return out;
}

/// how a connection spells one of its two ends: an index, a name, or an index and a sub-index
[[nodiscard]] inline std::string portText(const pmt::Value& value) {
    if (const Tensor<pmt::Value>* pair = listOf(value); pair != nullptr) {
        std::string out;
        for (std::size_t i = 0UZ; i < pair->size(); ++i) {
            if (i != 0UZ) {
                out += ".";
            }
            out += valueText((*pair)[i]);
        }
        return out;
    }
    if (const std::optional<std::string> name = stringOf(value); name.has_value()) {
        return *name;
    }
    return valueText(value);
}

struct NamedText {
    std::string name;
    std::string value;
};

struct Connection {
    std::string sourceBlock;
    std::string sourcePort;
    std::string destinationBlock;
    std::string destinationPort;
    std::string minBufferSize; ///< empty unless the connection carries a fifth element
};

struct ExportedPort {
    std::string block;
    std::string direction;
    std::string internalName;
    std::string exportedName;
};

/// One entry of a composite's `exported_parameters` list, spelled as `gr::recipe::ParameterDeclaration`
/// carries it: a name, a dialect type word, an optional default and an optional `doc` line. A
/// declaration that states no default is required at instantiation, which is what `required` says.
struct ExportedParameter {
    std::string name;
    std::string type;
    std::string defaultValue; ///< empty unless the declaration states one
    std::string doc;
    bool        required = true;
};

struct Block;

/// one graph level: what a file's top level and a subgraph's `graph` map both look like
struct Level {
    std::vector<Block>        blocks;
    std::vector<Connection>   connections;
    std::vector<ExportedPort> exportedPorts;
    std::vector<NamedText>    metadata;          ///< `definition_metadata`, sorted
    std::vector<NamedText>    uninterpretedKeys; ///< keys outside kKnownGraphKeys
};

struct Block {
    std::string            type; ///< the `id` field; "SUBGRAPH" for a nested graph
    std::string            name;
    std::string            uniqueName;
    std::string            schedulerId; ///< empty unless the subgraph is scheduler-managed
    std::string            parameters;  ///< sorted `key: value` lines
    std::string            metaInformation;
    std::vector<NamedText> contexts;          ///< one entry per `ctx_parameters` context
    std::vector<NamedText> uninterpretedKeys; ///< keys outside kKnownBlockKeys, or kKnownSubgraphKeys for a composite

    std::vector<ExportedParameter> exportedParameters; ///< the `exported_parameters` a composite entry declares

    std::shared_ptr<Level> interior; ///< the nested graph of a SUBGRAPH entry

    [[nodiscard]] bool isSubgraph() const noexcept { return interior != nullptr; }
};

[[nodiscard]] inline std::vector<NamedText> uninterpretedKeysOf(const property_map& map, std::span<const std::string_view> known) {
    std::vector<NamedText> result;
    for (const auto& [key, value] : map) {
        const std::string_view view(key);
        if (std::ranges::find(known, view) == known.end()) {
            result.emplace_back(std::string(view), valueText(value));
        }
    }
    std::ranges::sort(result, [](const NamedText& a, const NamedText& b) { return a.name < b.name; });
    return result;
}

[[nodiscard]] inline std::vector<NamedText> sortedEntriesOf(const property_map& map) {
    std::vector<NamedText> result;
    result.reserve(map.size());
    for (const auto& [key, value] : map) {
        result.emplace_back(std::string(std::string_view(key)), valueText(value));
    }
    std::ranges::sort(result, [](const NamedText& a, const NamedText& b) { return a.name < b.name; });
    return result;
}

/// The parameters a composite entry declares, in the order the file gives them. A declaration the
/// loader would refuse -- no name, no type, or an entry that is not a map -- is reported as it
/// stands rather than dropped or corrected: the document says what the file says, and the
/// instantiation is where a malformed declaration is refused with a reason.
[[nodiscard]] inline std::vector<ExportedParameter> exportedParametersOf(const property_map& entry) {
    std::vector<ExportedParameter> parameters;

    const pmt::Value* declarations = entryOf(entry, "exported_parameters");
    if (declarations == nullptr) {
        return parameters;
    }
    const Tensor<pmt::Value>* list = listOf(*declarations);
    if (list == nullptr) {
        return parameters;
    }

    for (const pmt::Value& declarationValue : *list) {
        const property_map* declaration = mapOf(declarationValue);
        if (declaration == nullptr) {
            continue;
        }
        ExportedParameter parameter;
        if (const pmt::Value* name = entryOf(*declaration, "name"); name != nullptr) {
            parameter.name = stringOf(*name).value_or(valueText(*name));
        }
        if (const pmt::Value* type = entryOf(*declaration, "type"); type != nullptr) {
            parameter.type = stringOf(*type).value_or(valueText(*type));
        }
        if (const pmt::Value* defaultValue = entryOf(*declaration, "default"); defaultValue != nullptr) {
            parameter.defaultValue = valueText(*defaultValue);
            parameter.required     = false;
        }
        if (const pmt::Value* doc = entryOf(*declaration, "doc"); doc != nullptr) {
            parameter.doc = stringOf(*doc).value_or(valueText(*doc));
        }
        parameters.push_back(std::move(parameter));
    }
    return parameters;
}

[[nodiscard]] Level readLevel(const property_map& map);

[[nodiscard]] inline Block readBlock(const property_map& entry) {
    Block block;
    if (const pmt::Value* id = entryOf(entry, "id"); id != nullptr) {
        block.type = stringOf(*id).value_or(valueText(*id));
    }
    if (const pmt::Value* uniqueName = entryOf(entry, "unique_name"); uniqueName != nullptr) {
        block.uniqueName = stringOf(*uniqueName).value_or(std::string{});
    }

    const property_map* parameters = nullptr;
    if (const pmt::Value* value = entryOf(entry, "parameters"); value != nullptr) {
        parameters = mapOf(*value);
    }
    if (parameters != nullptr) {
        block.parameters = mapLines(*parameters);
        if (const pmt::Value* name = entryOf(*parameters, "name"); name != nullptr) {
            block.name = stringOf(*name).value_or(std::string{});
        }
    }
    // a subgraph written before the parameters key carries its name at the top level
    if (block.name.empty()) {
        if (const pmt::Value* name = entryOf(entry, "name"); name != nullptr) {
            block.name = stringOf(*name).value_or(std::string{});
        }
    }

    if (const pmt::Value* meta = entryOf(entry, "meta_information"); meta != nullptr) {
        if (const property_map* map = mapOf(*meta); map != nullptr) {
            block.metaInformation = mapLines(*map);
        }
    }

    if (const pmt::Value* contexts = entryOf(entry, "ctx_parameters"); contexts != nullptr) {
        if (const Tensor<pmt::Value>* list = listOf(*contexts); list != nullptr) {
            for (const pmt::Value& contextValue : *list) {
                const property_map* context = mapOf(contextValue);
                if (context == nullptr) {
                    continue;
                }
                std::string label;
                if (const pmt::Value* name = entryOf(*context, gr::tag::CONTEXT.shortKey()); name != nullptr) {
                    label = stringOf(*name).value_or(valueText(*name));
                }
                if (const pmt::Value* time = entryOf(*context, gr::tag::CONTEXT_TIME.shortKey()); time != nullptr) {
                    label += std::format(" @ {}", valueText(*time));
                }
                std::string values;
                if (const pmt::Value* parameterValue = entryOf(*context, "parameters"); parameterValue != nullptr) {
                    if (const property_map* map = mapOf(*parameterValue); map != nullptr) {
                        values = mapLines(*map);
                    }
                }
                block.contexts.emplace_back(std::move(label), std::move(values));
            }
        }
    }

    if (const pmt::Value* scheduler = entryOf(entry, "scheduler"); scheduler != nullptr) {
        if (const property_map* map = mapOf(*scheduler); map != nullptr) {
            if (const pmt::Value* id = entryOf(*map, "id"); id != nullptr) {
                block.schedulerId = stringOf(*id).value_or(valueText(*id));
            }
        }
    }

    if (const pmt::Value* graph = entryOf(entry, "graph"); graph != nullptr) {
        if (const property_map* map = mapOf(*graph); map != nullptr) {
            block.interior = std::make_shared<Level>(readLevel(*map));
        }
    }

    if (block.isSubgraph()) {
        block.exportedParameters = exportedParametersOf(entry);
    }
    block.uninterpretedKeys = uninterpretedKeysOf(entry, block.isSubgraph() ? std::span<const std::string_view>(kKnownSubgraphKeys) : std::span<const std::string_view>(kKnownBlockKeys));
    return block;
}

inline Level readLevel(const property_map& map) {
    Level level;

    if (const pmt::Value* blocks = entryOf(map, "blocks"); blocks != nullptr) {
        if (const Tensor<pmt::Value>* list = listOf(*blocks); list != nullptr) {
            for (const pmt::Value& entry : *list) {
                if (const property_map* block = mapOf(entry); block != nullptr) {
                    level.blocks.push_back(readBlock(*block));
                }
            }
        }
    }

    if (const pmt::Value* connections = entryOf(map, "connections"); connections != nullptr) {
        if (const Tensor<pmt::Value>* list = listOf(*connections); list != nullptr) {
            for (const pmt::Value& entry : *list) {
                const Tensor<pmt::Value>* fields = listOf(entry);
                if (fields == nullptr || fields->size() < 4UZ) {
                    continue;
                }
                Connection connection{
                    .sourceBlock      = portText((*fields)[0]),
                    .sourcePort       = portText((*fields)[1]),
                    .destinationBlock = portText((*fields)[2]),
                    .destinationPort  = portText((*fields)[3]),
                    .minBufferSize    = fields->size() > 4UZ ? valueText((*fields)[4]) : std::string{},
                };
                level.connections.push_back(std::move(connection));
            }
        }
    }

    if (const pmt::Value* exported = entryOf(map, "exported_ports"); exported != nullptr) {
        if (const Tensor<pmt::Value>* list = listOf(*exported); list != nullptr) {
            for (const pmt::Value& entry : *list) {
                const Tensor<pmt::Value>* fields = listOf(entry);
                if (fields == nullptr || fields->size() < 4UZ) {
                    continue;
                }
                level.exportedPorts.emplace_back(portText((*fields)[0]), portText((*fields)[1]), portText((*fields)[2]), portText((*fields)[3]));
            }
        }
    }

    if (const pmt::Value* metadata = entryOf(map, "definition_metadata"); metadata != nullptr) {
        if (const property_map* entries = mapOf(*metadata); entries != nullptr) {
            level.metadata = sortedEntriesOf(*entries);
        }
    }

    level.uninterpretedKeys = uninterpretedKeysOf(map, kKnownGraphKeys);
    return level;
}

/// how many subgraphs a level holds, counting every depth below it
[[nodiscard]] inline std::size_t countSubgraphs(const Level& level) {
    std::size_t count = 0UZ;
    for (const Block& block : level.blocks) {
        if (block.isSubgraph()) {
            ++count;
            count += countSubgraphs(*block.interior);
        }
    }
    return count;
}

inline void collectSchedulers(const Level& level, std::vector<std::string>& out) {
    for (const Block& block : level.blocks) {
        if (!block.schedulerId.empty()) {
            out.push_back(block.schedulerId);
        }
        if (block.isSubgraph()) {
            collectSchedulers(*block.interior, out);
        }
    }
}

/// mermaid takes its labels between quotes, so the quote and the entity marker are spelled as entities
[[nodiscard]] inline std::string mermaidLabel(std::string_view label) {
    std::string out;
    out.reserve(label.size());
    for (const char c : label) {
        switch (c) {
        case '"': out += "#quot;"; break;
        case '#': out += "#35;"; break;
        case '\n': out += "<br/>"; break;
        default: out += c; break;
        }
    }
    return out;
}

/**
 * @brief A `flowchart LR` of one graph level.
 *
 * Node identifiers are minted from the level prefix and the block position, so they are stable
 * across runs and unique across nesting depths. An endpoint no block of the level answers to is
 * still drawn, as a node marked unresolved: a connection the document silently omitted would be
 * worse than one whose end is visibly missing.
 */
[[nodiscard]] inline std::string diagramOf(const Level& level, std::string_view prefix) {
    std::map<std::string, std::string, std::less<>> idForName;
    std::string                                     nodes;
    std::string                                     edges;

    for (std::size_t i = 0UZ; i < level.blocks.size(); ++i) {
        const Block&      block = level.blocks[i];
        const std::string id    = std::format("{}b{}", prefix, i);
        const std::string label = mermaidLabel(std::format("{}\n{}", block.name.empty() ? "(unnamed)" : block.name, block.type));
        nodes += block.isSubgraph() ? std::format("    {}[[\"{}\"]]\n", id, label) : std::format("    {}[\"{}\"]\n", id, label);
        if (!block.uniqueName.empty()) {
            idForName.emplace(block.uniqueName, id);
        }
        if (!block.name.empty()) {
            idForName.emplace(block.name, id);
        }
    }

    std::size_t unresolved = 0UZ;
    auto        idOf       = [&](const std::string& blockName) {
        const auto it = idForName.find(blockName);
        if (it != idForName.end()) {
            return it->second;
        }
        const std::string id = std::format("{}x{}", prefix, unresolved++);
        nodes += std::format("    {}(\"{}\"):::unresolved\n", id, mermaidLabel(std::format("{}\n(unresolved)", blockName)));
        idForName.emplace(blockName, id);
        return id;
    };

    for (const Connection& connection : level.connections) {
        const std::string source      = idOf(connection.sourceBlock);
        const std::string destination = idOf(connection.destinationBlock);
        edges += std::format("    {} -- \"{} to {}\" --> {}\n", source, mermaidLabel(connection.sourcePort), mermaidLabel(connection.destinationPort), destination);
    }

    std::string diagram = "flowchart LR\n";
    diagram += nodes;
    diagram += edges;
    if (unresolved != 0UZ) {
        diagram += "    classDef unresolved stroke-dasharray: 4 3\n";
    }
    return diagram;
}

inline void writeNamedTable(DocWriter& writer, std::string_view firstColumn, std::string_view secondColumn, const std::vector<NamedText>& entries) {
    if (entries.empty()) {
        return;
    }
    std::vector<std::vector<std::string>> rows;
    rows.reserve(entries.size());
    for (const NamedText& entry : entries) {
        rows.push_back({entry.name, entry.value});
    }
    const std::array<std::string_view, 2> headers{firstColumn, secondColumn};
    writer.table(headers, rows);
}

inline void writeLevelBody(DocWriter& writer, const Level& level, std::size_t depth, std::string_view prefix) {
    const std::size_t headingLevel = depth + 2UZ;

    writer.heading(headingLevel, "Blocks");
    if (level.blocks.empty()) {
        writer.paragraph("This graph level holds no blocks.");
    } else {
        std::vector<std::vector<std::string>> rows;
        rows.reserve(level.blocks.size());
        for (const Block& block : level.blocks) {
            std::string parameters = block.parameters;
            for (const NamedText& context : block.contexts) {
                if (!parameters.empty()) {
                    parameters += "\n";
                }
                parameters += std::format("[context {}] {}", context.name, context.value);
            }
            std::string kind = block.isSubgraph() ? "subgraph" : "block";
            if (!block.schedulerId.empty()) {
                kind += std::format(", scheduler {}", block.schedulerId);
            }
            rows.push_back({block.name, block.type, kind, block.uniqueName, parameters, block.metaInformation});
        }
        const std::array<std::string_view, 6> headers{"Name", "Type", "Kind", "Unique name", "Parameters", "Meta information"};
        writer.table(headers, rows);
    }

    writer.heading(headingLevel, "Connections");
    if (level.connections.empty()) {
        writer.paragraph("This graph level holds no connections.");
    } else {
        std::vector<std::vector<std::string>> rows;
        rows.reserve(level.connections.size());
        for (const Connection& connection : level.connections) {
            rows.push_back({connection.sourceBlock, connection.sourcePort, connection.destinationBlock, connection.destinationPort, connection.minBufferSize});
        }
        const std::array<std::string_view, 5> headers{"From", "Port", "To", "Port", "Minimum buffer"};
        writer.table(headers, rows);
    }

    writer.heading(headingLevel, "Diagram");
    writer.mermaid(diagramOf(level, prefix));

    if (!level.uninterpretedKeys.empty()) {
        writer.heading(headingLevel, "Other keys");
        writeNamedTable(writer, "Key", "Value", level.uninterpretedKeys);
    }

    // A block's uninterpreted keys belong to that block, so they are named with it. They travel in
    // one table per level, beside the level's own, rather than in a further column of the block
    // table that every graph without such a key would carry empty.
    std::vector<std::vector<std::string>> blockKeys;
    for (std::size_t i = 0UZ; i < level.blocks.size(); ++i) {
        const Block&      block = level.blocks[i];
        const std::string name  = block.name.empty() ? std::format("block {}", i) : block.name;
        for (const NamedText& key : block.uninterpretedKeys) {
            blockKeys.push_back({name, key.name, key.value});
        }
    }
    if (!blockKeys.empty()) {
        writer.heading(headingLevel, "Other block keys");
        const std::array<std::string_view, 3> headers{"Block", "Key", "Value"};
        writer.table(headers, blockKeys);
    }
}

inline void writeSubgraphs(DocWriter& writer, const Level& level, std::size_t depth, std::string_view prefix, std::string_view path) {
    for (std::size_t i = 0UZ; i < level.blocks.size(); ++i) {
        const Block& block = level.blocks[i];
        if (!block.isSubgraph()) {
            continue;
        }
        const std::string name       = block.name.empty() ? std::format("subgraph {}", i) : block.name;
        const std::string nestedPath = path.empty() ? name : std::format("{} / {}", path, name);
        const std::string nestedTag  = std::format("{}s{}", prefix, i);
        writer.heading(depth + 2UZ, std::format("Subgraph: {}", nestedPath), writer.anchor(std::format("subgraph {}", nestedPath)));

        std::vector<std::string> facts;
        facts.push_back(writer.labeled("Type", block.type));
        if (!block.uniqueName.empty()) {
            facts.push_back(writer.labeled("Unique name", block.uniqueName));
        }
        facts.push_back(writer.labeled("Scheduler", block.schedulerId.empty() ? "none, the parent schedules its blocks" : block.schedulerId));
        facts.push_back(writer.labeled("Blocks", std::to_string(block.interior->blocks.size())));
        facts.push_back(writer.labeled("Connections", std::to_string(block.interior->connections.size())));
        writer.rawBullets(facts);

        // the declarations come before the ports because they are what an instantiation must supply:
        // a composite's interior is derived from them before it exists
        if (!block.exportedParameters.empty()) {
            writer.heading(depth + 3UZ, "Exported parameters");
            std::vector<std::vector<std::string>> rows;
            rows.reserve(block.exportedParameters.size());
            for (const ExportedParameter& parameter : block.exportedParameters) {
                rows.push_back({parameter.name, parameter.type, parameter.required ? "required" : parameter.defaultValue, parameter.doc});
            }
            const std::array<std::string_view, 4> headers{"Name", "Type", "Default", "Description"};
            writer.table(headers, rows);
        }

        if (!block.interior->exportedPorts.empty()) {
            writer.heading(depth + 3UZ, "Exported ports");
            std::vector<std::vector<std::string>> rows;
            rows.reserve(block.interior->exportedPorts.size());
            for (const ExportedPort& port : block.interior->exportedPorts) {
                rows.push_back({port.exportedName, port.direction, port.block, port.internalName});
            }
            const std::array<std::string_view, 4> headers{"Exported as", "Direction", "Inner block", "Inner port"};
            writer.table(headers, rows);
        }

        writeLevelBody(writer, *block.interior, depth + 1UZ, nestedTag);
        writeSubgraphs(writer, *block.interior, depth + 1UZ, nestedTag, nestedPath);
    }
}

/// the whole document for a graph already read into a level
[[nodiscard]] inline std::string render(const Level& level, Format format, std::string title) {
    DocWriter writer(format, std::move(title));

    std::vector<std::string> schedulers;
    collectSchedulers(level, schedulers);
    std::ranges::sort(schedulers);
    schedulers.erase(std::ranges::unique(schedulers).begin(), schedulers.end());

    writer.heading(2UZ, "Summary");
    std::vector<std::string> summary;
    summary.push_back(writer.labeled("Blocks at the top level", std::to_string(level.blocks.size())));
    summary.push_back(writer.labeled("Connections at the top level", std::to_string(level.connections.size())));
    summary.push_back(writer.labeled("Subgraphs, all depths", std::to_string(countSubgraphs(level))));
    if (!schedulers.empty()) {
        std::string names;
        for (std::size_t i = 0UZ; i < schedulers.size(); ++i) {
            names += (i == 0UZ ? "" : ", ") + schedulers[i];
        }
        summary.push_back(writer.labeled("Schedulers", names));
    }
    writer.rawBullets(summary);

    if (!level.metadata.empty()) {
        writer.heading(2UZ, "Definition metadata");
        writeNamedTable(writer, "Key", "Value", level.metadata);
    }

    if (!level.exportedPorts.empty()) {
        writer.heading(2UZ, "Exported ports");
        std::vector<std::vector<std::string>> rows;
        rows.reserve(level.exportedPorts.size());
        for (const ExportedPort& port : level.exportedPorts) {
            rows.push_back({port.exportedName, port.direction, port.block, port.internalName});
        }
        const std::array<std::string_view, 4> headers{"Exported as", "Direction", "Inner block", "Inner port"};
        writer.table(headers, rows);
    }

    writeLevelBody(writer, level, 0UZ, "g");
    writeSubgraphs(writer, level, 0UZ, "g", {});

    return writer.finish();
}

/// reads the YAML with the framework's own parser, so the dialect is exactly the importer's
[[nodiscard]] inline std::expected<Level, std::string> read(std::string_view yaml) {
    const auto parsed = gr::pmt::yaml::deserialize(yaml);
    if (!parsed.has_value()) {
        return std::unexpected(std::format("line {}, column {}: {}", parsed.error().line, parsed.error().column, parsed.error().message));
    }
    return readLevel(*parsed);
}

} // namespace gr::tools::graphdoc

#endif // GNURADIO_TOOLS_GRAPHDOC_HPP
