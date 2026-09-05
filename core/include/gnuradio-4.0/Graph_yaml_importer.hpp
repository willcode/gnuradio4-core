#ifndef GNURADIO_GRAPH_YAML_IMPORTER_H
#define GNURADIO_GRAPH_YAML_IMPORTER_H

#include <array>
#include <limits>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <utility>

#include <gnuradio-4.0/meta/indirect.hpp>

#include <gnuradio-4.0/RecipeParameters.hpp>
#include <gnuradio-4.0/YamlPmt.hpp>

#include "BlockModel.hpp"
#include "Graph.hpp"
#include "PluginLoader.hpp"

namespace gr {

namespace detail {

template<typename T>
inline std::expected<T, gr::Error> getProperty(const gr::property_map& map, std::string_view propertyName) {
    auto it = map.find(propertyName);
    if (it == map.cend()) {
        return std::unexpected(gr::Error(std::format("Missing field {} in YAML object", propertyName)));
    }

    if constexpr (std::is_same_v<T, std::string>) {
        auto value = it->second.value_or(std::string_view{});
        if (value.data() != nullptr) {
            return std::string(value);
        }
    } else {
        // the non-terminating form, so that the incorrect-type report below is reachable for every T
        auto value = checked_access_ptr<const T, false>{it->second.get_if<T>()};
        if (value != nullptr) {
            return *value;
        }
    }

    return std::unexpected(gr::Error(std::format("Field {} in YAML object {} has an incorrect type {}:{} instead of {}", propertyName, map, it->second.value_type(), it->second.container_type(), gr::meta::type_name<T>())));
}

template<typename T>
inline std::expected<T, gr::Error> getProperty(const gr::property_map& map, std::string_view propertyName, const auto&... propertySubNames)
requires(sizeof...(propertySubNames) > 0)
{
    static_assert((std::is_convertible_v<decltype(propertySubNames), std::string_view> && ...));
    auto it = map.find(propertyName);
    if (it == map.cend()) {
        return std::unexpected(gr::Error(std::format("Missing field {} in YAML object", propertyName)));
    }

    auto value = checked_access_ptr<const gr::property_map, false>{it->second.get_if<gr::property_map>()};
    if (value == nullptr) {
        return std::unexpected(gr::Error(std::format("Field {} in YAML object has an incorrect type {}:{} instead of gr::property_map", propertyName, it->second.value_type(), it->second.container_type())));
    }

    return getProperty<T>(*value, propertySubNames...);
}

/**
 * @brief The `version` a block entry pins, or nothing when it pins none.
 *
 * A block entry without the key takes the newest registered version. Any integral spelling the YAML reader
 * produced is accepted; anything else, or a value outside a version number's range, is reported as a defect.
 *
 * The map is whichever one carries the entry: a block of a graph file, or the data of a message that emplaces
 * or replaces one at runtime. A message handler must not throw, so the defect is returned rather than thrown
 * and the file loader is the one that turns it into an exception.
 */
[[nodiscard]] inline std::expected<std::optional<block::Version>, gr::Error> pinnedVersionOrError(const gr::property_map& grcBlock, std::string_view blockType) {
    const auto it = grcBlock.find("version");
    if (it == grcBlock.cend()) {
        return std::optional<block::Version>{};
    }

    std::optional<block::Version> pinned;
    pmt::ValueVisitor([&pinned]<typename TValue>(const TValue& value) {
        if constexpr (std::is_integral_v<TValue> && !std::is_same_v<TValue, bool>) {
            if (std::cmp_greater_equal(value, 0) && std::cmp_less_equal(value, std::numeric_limits<block::Version>::max())) {
                pinned = static_cast<block::Version>(value);
            }
        }
    }).visit(it->second);

    if (!pinned.has_value()) {
        return std::unexpected(gr::Error(std::format("Block of type '{}' pins a version that is not a version number", blockType)));
    }
    return pinned;
}

[[nodiscard]] inline std::optional<block::Version> pinnedVersionOf(const gr::property_map& grcBlock, std::string_view blockType) {
    const std::expected<std::optional<block::Version>, gr::Error> pinned = pinnedVersionOrError(grcBlock, blockType);
    if (!pinned.has_value()) {
        throw gr::exception(pinned.error().message);
    }
    return *pinned;
}

template<typename T>
T getOrThrow(std::expected<T, gr::Error>&& expectedValue, std::source_location location = std::source_location::current()) {
    if (!expectedValue) {
        throw gr::exception(std::format("Got an error {}, caller {}:{}", expectedValue.error().message, location.file_name(), location.line()));
    } else {
        return *expectedValue;
    }
}

/**
 * The blocks one graph level produced, indexed by the two names a YAML file may address them by.
 *
 * `unique_name` identifies a block within a file; `name` is a label and may repeat, so a repeated name
 * is recorded as ambiguous and reported instead of silently resolving to the last block that carried it.
 */
struct LoadedBlocks {
    std::map<std::string, std::shared_ptr<BlockModel>, std::less<>> byUniqueName;
    std::map<std::string, std::shared_ptr<BlockModel>, std::less<>> byName;
    std::set<std::string, std::less<>>                              ambiguousNames;

    void add(std::string_view uniqueName, std::string_view name, std::shared_ptr<BlockModel> block) {
        if (!uniqueName.empty()) {
            byUniqueName.insert_or_assign(std::string(uniqueName), block);
        }
        if (!name.empty() && !byName.try_emplace(std::string(name), std::move(block)).second) {
            ambiguousNames.emplace(name);
        }
    }

    [[nodiscard]] std::expected<std::shared_ptr<BlockModel>, gr::Error> find(std::string_view key) const {
        if (const auto it = byUniqueName.find(key); it != byUniqueName.cend()) {
            return it->second;
        }
        if (ambiguousNames.contains(key)) {
            return std::unexpected(gr::Error(std::format("'{}' is the name of more than one block, address it by its unique_name", key)));
        }
        if (const auto it = byName.find(key); it != byName.cend()) {
            return it->second;
        }
        return std::unexpected(gr::Error(std::format("Unknown block '{}'", key)));
    }
};

inline LoadedBlocks loadGraphFromMap(PluginLoader& loader, gr::Graph& resultGraph, gr::property_map yaml, std::source_location location = std::source_location::current()) {
    LoadedBlocks createdBlocks;

    Tensor<pmt::Value> blks;
    if (auto it = yaml.find("blocks"); it != yaml.end()) {
        if (const auto blkRef = checked_access_ptr<Tensor<pmt::Value>, false>{it->second.get_if<Tensor<pmt::Value>>()}; blkRef != nullptr) {
            blks = *blkRef;
        }
    }

    for (const auto& blk : blks) {
        const auto _grcBlock = checked_access_ptr<const property_map, false>{blk.get_if<property_map>()};
        if (_grcBlock == nullptr) {
            continue;
        }
        const auto& grcBlock = *_grcBlock;

        // the type decides which fields are required, so it is read first
        const auto blockType       = getOrThrow(getProperty<std::string>(grcBlock, "id"sv));
        const bool isSubgraph      = blockType == "SUBGRAPH";
        const auto blockUniqueName = getProperty<std::string>(grcBlock, "unique_name"sv).value_or(std::string{});
        const auto blockName       = [&] {
            auto fromParameters = getProperty<std::string>(grcBlock, "parameters"sv, "name"sv);
            if (fromParameters.has_value() || !isSubgraph) {
                return getOrThrow(std::move(fromParameters));
            }
            // subgraphs written before the parameters key carry their name at the top level
            return getProperty<std::string>(grcBlock, "name"sv).value_or(std::string{});
        }();

        // the block's own entries win: a block regenerates what it says about itself when it is
        // constructed, so the file only contributes the keys the block does not regenerate
        auto restoreMetaInformation = [&grcBlock](BlockModel& createdBlock) {
            const auto metaIt = grcBlock.find("meta_information");
            if (metaIt == grcBlock.cend()) {
                return;
            }
            const auto meta = checked_access_ptr<const property_map, false>{metaIt->second.get_if<property_map>()};
            if (meta == nullptr) {
                return;
            }
            for (const auto& [key, value] : *meta) {
                createdBlock.metaInformation().try_emplace(key, value);
            }
        };

        if (isSubgraph) {
            auto loadGraph = [&grcBlock, &loader, &location, &blockName, &blockType](auto graphWrapper) {
                // checked_access_ptr terminates on a null unless not_null is turned off, so the
                // non-terminating form is what keeps the report below reachable: a subgraph whose
                // graph field is present but not a map is a defect in the document and is named as one
                const auto _graphData = checked_access_ptr<const property_map, false>{grcBlock.at("graph").get_if<property_map>()};
                if (_graphData == nullptr) {
                    throw gr::exception(std::format("Unable to create block '{}' of type '{}': graph is not a map", blockName, blockType));
                }
                const auto&        graphData    = *_graphData;
                gr::Graph&         graph        = *graphWrapper->graph();
                const LoadedBlocks innerBlocks  = loadGraphFromMap(loader, graph, graphData);
                const auto         exportedIt   = graphData.find("exported_ports");
                const auto         exportedList = exportedIt == graphData.cend() ? Tensor<pmt::Value>() : exportedIt->second.value_or(Tensor<pmt::Value>());
                for (const auto& exportedPort_ : exportedList) {
                    auto exportedPort = checked_access_ptr<const Tensor<pmt::Value>, false>{exportedPort_.get_if<Tensor<pmt::Value>>()};
                    if (exportedPort == nullptr) {
                        throw gr::exception("Unable to parse exported port (not a list)");
                    }
                    if (exportedPort->size() != 4) {
                        throw gr::exception(std::format("Unable to parse exported port ({} instead of 4 elements)", exportedPort->size()));
                    }

                    const auto requiredBlockName   = (*exportedPort)[0].value_or(std::string_view{});
                    const auto portDirectionString = (*exportedPort)[1].value_or(std::string_view{});
                    const auto internalPortName    = (*exportedPort)[2].value_or(std::string_view{});
                    const auto exportedPortName    = (*exportedPort)[3].value_or(std::string_view{});
                    if (requiredBlockName.data() == nullptr || portDirectionString.data() == nullptr || internalPortName.data() == nullptr || exportedPortName.data() == nullptr) {
                        throw gr::exception(std::format("Required fields for exported ports missing"));
                    }

                    // the writer names the inner block by unique_name, a hand-written file by name
                    const auto innerBlock = innerBlocks.find(requiredBlockName);
                    if (!innerBlock.has_value()) {
                        throw gr::exception(std::format("{} in:\n{}", innerBlock.error().message, gr::graph::format(graph)), location);
                    }
                    const std::string innerUniqueName{innerBlock.value()->uniqueName()};

                    if (auto result = graphWrapper->exportPort(true,                                       //
                            innerUniqueName,                                                               //
                            portDirectionString == "INPUT" ? PortDirection::INPUT : PortDirection::OUTPUT, //
                            internalPortName,                                                              //
                            exportedPortName);
                        !result.has_value()) {
                        throw result.error();
                    }
                }
            };

            auto       schedulerIt = grcBlock.find("scheduler");
            const bool isManaged   = schedulerIt != grcBlock.end();

            if (isManaged) {
                auto schedulerPmt = checked_access_ptr<const property_map, false>{schedulerIt->second.get_if<property_map>()};
                if (schedulerPmt == nullptr) {
                    throw gr::exception(std::format("scheduler is not a property_map"));
                }
                auto schedulerId = getOrThrow(getProperty<std::string>(*schedulerPmt, "id"sv));

                property_map schedulerParams;
                if (auto paramsIt = schedulerPmt->find("parameters"); paramsIt != schedulerPmt->end()) {
                    if (const auto params = checked_access_ptr<const property_map, false>{paramsIt->second.get_if<property_map>()}; params != nullptr) {
                        schedulerParams = *params;
                    }
                }

                auto scheduler = loader.instantiateScheduler(schedulerId, schedulerParams);
                if (!scheduler) {
                    throw gr::exception(std::format("Unable to create scheduler of type '{}'", schedulerId));
                }

                auto schedulerBlock = SchedulerModel::asBlockModelPtr(scheduler);
                resultGraph.addBlock(schedulerBlock);
                schedulerBlock->setName(blockName);
                restoreMetaInformation(*schedulerBlock);
                createdBlocks.add(blockUniqueName, blockName, schedulerBlock);

                loadGraph(schedulerBlock);

            } else {
                const std::shared_ptr<BlockModel>& subGraph = resultGraph.addBlock(std::make_shared<GraphWrapper<gr::Graph>>(gr::Graph(loader)));
                subGraph->setName(blockName);
                restoreMetaInformation(*subGraph);
                createdBlocks.add(blockUniqueName, blockName, subGraph);

                loadGraph(static_cast<GraphWrapper<gr::Graph>*>(subGraph.get()));
            }
        } else {
            // no `version` key means the newest registered version
            const std::optional<block::Version> pinnedVersion = pinnedVersionOf(grcBlock, blockType);
            std::shared_ptr<BlockModel>         currentBlock;
            if (pinnedVersion.has_value()) {
                const auto instantiated = loader.instantiatePinnedOrError(blockType, *pinnedVersion);
                if (!instantiated.has_value()) {
                    throw gr::exception(std::format("Unable to create block '{}' of type '{}': {}", blockName, blockType, instantiated.error().message));
                }
                currentBlock = *instantiated;
            } else {
                currentBlock = loader.instantiate(blockType);
            }
            if (!currentBlock) {
                throw gr::exception(std::format("Unable to create block of type '{}'", blockType));
            }

            // This sets the previously read "name" field for the block
            currentBlock->setName(blockName);

            const auto parametersPmt = grcBlock.at("parameters");
            if (const auto parameters = checked_access_ptr{parametersPmt.get_if<property_map>()}; parameters != nullptr) {
                currentBlock->settings().loadParametersFromPropertyMap(*parameters);
            } else {
                currentBlock->settings().loadParametersFromPropertyMap({});
            }

            if (auto it = grcBlock.find("ctx_parameters"); it != grcBlock.end()) {
                // as with the graph field above, the null tests below are reachable only because the
                // pointers they test are the non-terminating form
                const auto parametersCtx = checked_access_ptr<const Tensor<pmt::Value>, false>{it->second.get_if<Tensor<pmt::Value>>()};
                if (parametersCtx == nullptr) {
                    throw gr::exception(std::format("Unable to create block '{}' of type '{}': ctx_parameters is not a list", blockName, blockType));
                }

                for (const auto& ctxPmt : *parametersCtx) {
                    const auto ctxPar = checked_access_ptr<const property_map, false>{ctxPmt.get_if<property_map>()};
                    if (ctxPar == nullptr) {
                        throw gr::exception(std::format("Unable to create block '{}' of type '{}': a ctx_parameters entry is not a map", blockName, blockType));
                    }

                    const auto ctxName       = ctxPar->at(gr::tag::CONTEXT.shortKey()).value_or(std::string_view{});
                    const auto ctxTime       = checked_access_ptr<const std::uint64_t, false>{ctxPar->at(gr::tag::CONTEXT_TIME.shortKey()).get_if<std::uint64_t>()};
                    const auto ctxParameters = checked_access_ptr<const property_map, false>{ctxPar->at("parameters").get_if<property_map>()};
                    if (ctxName.data() == nullptr || ctxTime == nullptr || ctxParameters == nullptr) {
                        throw gr::exception(std::format("Unable to create block '{}' of type '{}': a ctx_parameters entry needs a context, a context_time and a parameters map", blockName, blockType));
                    }

                    currentBlock->settings().loadParametersFromPropertyMap(*ctxParameters, SettingsCtx{*ctxTime, ctxName});
                }
            }

            if (const auto failed = currentBlock->settings().activateContext(); failed == std::nullopt) {
                throw gr::exception("Settings for context could not be activated");
            }

            restoreMetaInformation(*currentBlock);
            createdBlocks.add(blockUniqueName, blockName, resultGraph.addBlock(std::move(currentBlock)));
        }
    } // for blocks

    Tensor<pmt::Value> connections;
    if (auto it = yaml.find("connections"); it != yaml.end()) {
        if (const auto connRef = checked_access_ptr<Tensor<pmt::Value>, false>{it->second.get_if<Tensor<pmt::Value>>()}; connRef != nullptr) {
            connections = *connRef;
        }
    }

    for (const auto& conn : connections) {
        const auto _connection = checked_access_ptr<const Tensor<pmt::Value>, false>{conn.get_if<Tensor<pmt::Value>>()};
        if (_connection == nullptr) {
            throw gr::exception("Unable to parse connection (not a list)");
        }
        if (_connection->size() < 4) {
            throw gr::exception(std::format("Unable to parse connection ({} instead of >=4 elements)", _connection->size()));
        }
        const auto& connection = *_connection;

        auto parseBlockPort = [&](const pmt::Value& blockField, const pmt::Value& portField) {
            const auto blockName = blockField.value_or(std::string_view{});
            if (blockName.empty()) {
                throw gr::exception(std::format("Invalid blockField"));
            }
            auto block = createdBlocks.find(blockName);
            if (!block.has_value()) {
                throw gr::exception(block.error().message);
            }

            struct result {
                std::shared_ptr<BlockModel> block;
                PortDefinition              port_definition;
            };

            if (const auto portFields = checked_access_ptr<const Tensor<pmt::Value>, false>{portField.template get_if<Tensor<pmt::Value>>()}; portFields != nullptr) {
                if (portFields->size() != 2) {
                    throw gr::exception(std::format("Port definition has invalid length ({} instead of 2)", portFields->size()));
                }
                const auto index    = checked_access_ptr<const std::int64_t, false>{portFields->at(0).template get_if<std::int64_t>()};
                const auto subIndex = checked_access_ptr<const std::int64_t, false>{portFields->at(1).template get_if<std::int64_t>()};
                if (index == nullptr || subIndex == nullptr) {
                    throw gr::exception(std::format("Port definition missing values"));
                }

                return result{*block, {static_cast<std::size_t>(*index), static_cast<std::size_t>(*subIndex)}};

            } else if (const auto portFieldString = portField.value_or(std::string_view{}); portFieldString.data()) {
                return result{*block, {std::string(portFieldString)}};

            } else {
                const auto index = checked_access_ptr<const std::int64_t, false>{portField.template get_if<std::int64_t>()};
                if (index == nullptr) {
                    throw gr::exception(std::format("Port definition missing values"));
                }
                return result{*block, {static_cast<std::size_t>(*index)}};
            }
        };

        auto src = parseBlockPort(connection[0], connection[1]);
        auto dst = parseBlockPort(connection[2], connection[3]);

        if (connection.size() == 4) {
            if (auto r = resultGraph.connect(src.block, src.port_definition, dst.block, dst.port_definition, EdgeParameters{.minBufferSize = undefined_size, .weight = graph::defaultWeight, .name = graph::defaultEdgeName}, location); !r) {
                throw gr::exception(std::format("connection failed: {}", r.error().message));
            }
        } else {
            std::size_t minBufferSize{};
            pmt::ValueVisitor([&minBufferSize]<typename TValue>(const TValue& value) {
                if constexpr (std::is_same_v<TValue, std::size_t>) {
                    minBufferSize = value;
                } else if constexpr (std::is_integral_v<TValue>) {
                    minBufferSize = static_cast<std::size_t>(value);
                } else {
                    minBufferSize = std::numeric_limits<std::size_t>::max();
                }
            }).visit(connection[4]);

            if (auto r = resultGraph.connect(src.block, src.port_definition, dst.block, dst.port_definition, EdgeParameters{.minBufferSize = minBufferSize, .weight = graph::defaultWeight, .name = graph::defaultEdgeName}, location); !r) {
                throw gr::exception(std::format("connection failed: {}", r.error().message));
            }
        }
    } // for connections

    return createdBlocks;
}

inline gr::property_map saveGraphToMap(PluginLoader& loader, const gr::Graph& rootGraph) {
    property_map result;

    {
        const std::size_t  nBlocks = gr::graph::countBlocks<gr::block::Category::NormalBlock>(rootGraph);
        Tensor<pmt::Value> serializedBlocks;
        serializedBlocks.reserve(nBlocks);
        gr::graph::forEachBlock<gr::block::Category::NormalBlock>(rootGraph, [&serializedBlocks, &loader](const std::shared_ptr<BlockModel>& block) { serializedBlocks.emplace_back(serializeBlock(loader, block, BlockSerializationFlags::All & (~BlockSerializationFlags::Ports))); });
        result["blocks"] = std::move(serializedBlocks);
    }

    {
        const std::size_t  nEdges = gr::graph::countEdges<block::Category::NormalBlock>(rootGraph);
        Tensor<pmt::Value> serializedConnections;
        serializedConnections.reserve(nEdges);
        graph::forEachEdge<block::Category::NormalBlock>(rootGraph, [&serializedConnections](const Edge& edge) { // NormalBlock -> perhaps can be modelled to 'ALL' for a cleaner sub-graph handling
            Tensor<pmt::Value> seq;
            seq.reserve(7);

            auto writePortDefinition = [&](const auto& definition) {
                if (auto* idx = std::get_if<PortDefinition::IndexBased>(&definition.definition)) {
                    if (idx->subIndex != meta::invalid_index) {
                        Tensor<pmt::Value> seqPort;
                        seqPort.reserve(2);
                        seqPort.push_back(std::int64_t(idx->topLevel));
                        seqPort.push_back(std::int64_t(idx->subIndex));
                        seq.push_back(std::move(seqPort));
                    } else {
                        seq.push_back(std::int64_t(idx->topLevel));
                    }
                } else {
                    auto& str = std::get<PortDefinition::StringBased>(definition.definition);
                    seq.push_back(str.name);
                }
            };

            // an edge names its ends by unique_name: a name may repeat, and two blocks sharing one lose an edge on load
            seq.push_back(std::string(edge.sourceBlock()->uniqueName()));
            writePortDefinition(edge.sourcePortDefinition());

            seq.push_back(std::string(edge.destinationBlock()->uniqueName()));
            writePortDefinition(edge.destinationPortDefinition());

            if (edge.minBufferSize() != std::numeric_limits<std::size_t>::max()) {
                seq.push_back(static_cast<gr::Size_t>(edge.minBufferSize()));
            }

            serializedConnections.emplace_back(std::move(seq));
        });
        result["connections"] = std::move(serializedConnections);
    }

    return result;
}

} // namespace detail

inline gr::meta::indirect<gr::Graph> loadGrc(PluginLoader& loader, std::string_view yamlSrc, std::source_location location = std::source_location::current()) {
    gr::meta::indirect<gr::Graph> resultGraph{loader};
    const auto                    yaml = pmt::yaml::deserialize(yamlSrc);
    if (!yaml) {
        throw gr::exception(std::format("Could not parse yaml: {}:{}\n{}", yaml.error().message, yaml.error().line, yamlSrc));
    }

    detail::loadGraphFromMap(loader, *resultGraph, *yaml, location);
    return resultGraph;
}

/// The key order a GRC document is emitted in: identity first, structure last, so a reader
/// skimming a block sees what it is before how it is configured. Every map in the document is
/// emitted with these keys first and the remainder lexicographically, which also makes the
/// output deterministic (the underlying map's own iteration order is a hash artifact).
inline constexpr std::array<std::string_view, 13> grcYamlKeyOrder{"id", "version", "name", "unique_name", "block_category", "meta_information", "parameters", "ctx_parameters", "scheduler", "exported_ports", "blocks", "connections", "graph"};

inline std::string saveGrc(PluginLoader& loader, const gr::Graph& rootGraph) { return pmt::yaml::serialize(detail::saveGraphToMap(loader, rootGraph), grcYamlKeyOrder); }

namespace detail {

/// the exported_parameters list of a definition's SUBGRAPH entry, as declarations
inline std::expected<std::vector<recipe::ParameterDeclaration>, gr::Error> readRecipeDeclarations(const property_map& blockEntry) {
    std::vector<recipe::ParameterDeclaration> declarations;
    const auto                                listIt = blockEntry.find("exported_parameters");
    if (listIt == blockEntry.end()) {
        return declarations;
    }
    const auto* list = listIt->second.get_if<Tensor<pmt::Value>>();
    if (list == nullptr) {
        return std::unexpected(gr::Error("recipe_expression_parse: exported_parameters is not a list"));
    }
    for (const auto& entryValue : *list) {
        const auto* entry = entryValue.get_if<property_map>();
        if (entry == nullptr) {
            return std::unexpected(gr::Error("recipe_expression_parse: an exported_parameters entry is not a map"));
        }
        recipe::ParameterDeclaration declaration;
        for (const auto& [key, value] : *entry) {
            const std::string_view keyView(key);
            const std::string_view valueView = value.value_or(std::string_view{});
            if (keyView == "name" && !valueView.empty()) {
                declaration.name.assign(valueView);
            } else if (keyView == "type" && !valueView.empty()) {
                declaration.type.assign(valueView);
            } else if (keyView == "default") {
                declaration.defaultValue = value;
            } else if (keyView == "doc" && !valueView.empty()) {
                declaration.doc.assign(valueView);
            }
        }
        if (declaration.name.empty() || declaration.type.empty()) {
            return std::unexpected(gr::Error("recipe_expression_parse: an exported parameter needs a name and a type"));
        }
        declarations.push_back(std::move(declaration));
    }
    return declarations;
}

/// walks block entries at any nesting depth: a parameters value spelled "=expr" is evaluated
/// against the recipe's declarations and replaced with the result; "\=text" unescapes to the
/// literal "=text". Below the composite's own level each evaluated expression is also
/// collected as a live binding, addressed by the block-name path from the interior downward,
/// so a later parameter change can re-evaluate and re-stage it.
// GCC's -Wnull-dereference mis-traces the inlined std::string copies through push_back here
// (the sources are guarded non-null); same known false positive qa_MemoryAllocators suppresses
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnull-dereference"
inline std::expected<void, gr::Error> evaluateRecipeExpressions(Tensor<pmt::Value>& blocks, std::span<const recipe::ParameterDeclaration> declarations, std::span<const pmt::Value> values, //
    std::vector<recipe::Binding>& outBindings, std::vector<std::string>& namePath, bool insideComposite) {
    for (std::size_t index = 0; index < blocks.size(); ++index) {
        auto* blockEntry = blocks[index].get_if<property_map>();
        if (blockEntry == nullptr) {
            continue;
        }
        std::string blockName;
        if (const auto parametersIt = blockEntry->find("parameters"); parametersIt != blockEntry->end()) {
            if (auto* parameters = parametersIt->second.get_if<property_map>(); parameters != nullptr) {
                if (const auto nameIt = parameters->find("name"); nameIt != parameters->end()) {
                    if (const std::string_view nameView = nameIt->second.value_or(std::string_view{}); !nameView.empty()) {
                        blockName.assign(nameView);
                    }
                }
                for (auto& [key, value] : *parameters) {
                    const auto* text = value.get_if<std::pmr::string>();
                    if (text == nullptr) {
                        continue;
                    }
                    const std::string_view textView(*text);
                    // a leading run of backslashes before '=' loses exactly one backslash, so
                    // every literal spelling stays expressible: "\=x" -> "=x", "\\=x" -> "\=x"
                    const std::size_t backslashes = textView.find_first_not_of('\\');
                    if (backslashes != std::string_view::npos && backslashes > 0 && textView[backslashes] == '=') {
                        value = std::pmr::string(textView.substr(1));
                        continue;
                    }
                    if (!textView.starts_with("=")) {
                        continue;
                    }
                    // `=name` naming a string, boolean or vector parameter hands that value through unchanged. Those
                    // types carry no arithmetic, so there is no grammar over them: the whole of what a recipe
                    // can do with one is substitute it, and anything else stays the numeric expression path.
                    const std::string_view     reference = textView.substr(1);
                    std::optional<std::size_t> substituted;
                    for (std::size_t declared = 0UZ; declared < declarations.size(); ++declared) {
                        if (std::string_view(declarations[declared].name) == reference && recipe::detail::substitutedTypeWord(declarations[declared].type)) {
                            substituted = declared;
                            break;
                        }
                    }
                    if (substituted.has_value()) {
                        if (insideComposite) {
                            if (blockName.empty()) {
                                return std::unexpected(gr::Error(std::format("recipe_expression_parse: the block holding '={}' needs a name so the parameter can re-substitute live", recipe::detail::printableEcho(reference))));
                            }
                            std::vector<std::string> path = namePath;
                            path.push_back(blockName);
                            outBindings.push_back({.namePath = std::move(path), .settingKey = std::string(key), .expression = {}, .substituted = substituted});
                        }
                        value = values[*substituted];
                        continue;
                    }

                    auto expression = recipe::parseExpression(reference, declarations);
                    if (!expression.has_value()) {
                        return std::unexpected(expression.error());
                    }
                    auto result = recipe::evaluate(*expression, values);
                    if (!result.has_value()) {
                        return std::unexpected(result.error());
                    }
                    if (insideComposite) {
                        if (blockName.empty()) {
                            return std::unexpected(gr::Error(std::format("recipe_expression_parse: the block holding '={}' needs a name so the expression can re-evaluate live", recipe::detail::printableEcho(expression->source))));
                        }
                        std::vector<std::string> path = namePath;
                        path.push_back(blockName);
                        outBindings.push_back({.namePath = std::move(path), .settingKey = std::string(key), .expression = *expression, .substituted = std::nullopt});
                    }
                    value = std::move(*result);
                }
            }
        }
        if (const auto graphIt = blockEntry->find("graph"); graphIt != blockEntry->end()) {
            if (auto* graphMap = graphIt->second.get_if<property_map>(); graphMap != nullptr) {
                if (const auto interiorIt = graphMap->find("blocks"); interiorIt != graphMap->end()) {
                    if (auto* interior = interiorIt->second.get_if<Tensor<pmt::Value>>(); interior != nullptr) {
                        // descending into the composite's interior starts the bindable region;
                        // one level further down the interior block's name joins the path
                        if (insideComposite) {
                            namePath.push_back(blockName);
                        }
                        auto walked = evaluateRecipeExpressions(*interior, declarations, values, outBindings, namePath, true);
                        if (insideComposite) {
                            namePath.pop_back();
                        }
                        if (!walked.has_value()) {
                            return walked;
                        }
                    }
                }
            }
        }
    }
    return {};
}
#pragma GCC diagnostic pop

} // namespace detail

inline std::expected<std::shared_ptr<gr::BlockModel>, gr::Error> detail::instantiateBlockFromYamlDefinition(PluginLoader& loader, const detail::YamlDefinitionsLoader::Definition& def, const property_map& parameters) noexcept {
    try {
        // the definition is rewritten, not consumed: expressions are evaluated against the
        // declarations overlaid with the caller's parameters before the graph loader runs,
        // so the loader sees only the literal dialect it always saw
        property_map definition = def.definition;

        std::vector<recipe::ParameterDeclaration> declarations;
        Tensor<pmt::Value>*                       blocksList = nullptr;
        if (const auto blocksIt = definition.find("blocks"); blocksIt != definition.end()) {
            blocksList = blocksIt->second.get_if<Tensor<pmt::Value>>();
        }
        if (blocksList != nullptr) {
            for (std::size_t index = 0; index < blocksList->size(); ++index) {
                const auto* blockEntry = (*blocksList)[index].get_if<property_map>();
                if (blockEntry != nullptr && blockEntry->contains("graph")) {
                    auto read = readRecipeDeclarations(*blockEntry);
                    if (!read.has_value()) {
                        return std::unexpected(read.error());
                    }
                    declarations = std::move(*read);
                    break;
                }
            }
        }

        if (auto valid = recipe::validateDeclarations(declarations); !valid.has_value()) {
            return std::unexpected(valid.error());
        }
        auto values = recipe::resolveParameters(declarations, parameters);
        if (!values.has_value()) {
            return std::unexpected(values.error());
        }
        std::vector<recipe::Binding> bindings;
        if (blocksList != nullptr) {
            std::vector<std::string> namePath;
            if (auto evaluatedAll = evaluateRecipeExpressions(*blocksList, declarations, *values, bindings, namePath, false); !evaluatedAll.has_value()) {
                return std::unexpected(evaluatedAll.error());
            }
        }

        gr::Graph tempGraph;
        detail::loadGraphFromMap(loader, tempGraph, definition);
        auto blocks = tempGraph.blocks();
        if (blocks.empty()) {
            return std::unexpected(gr::Error{"YAML definition produced no blocks"});
        }
        if (!declarations.empty()) {
            // the live half of the ruled contract: parameter changes on the composite
            // re-evaluate and re-stage. A scheduler-managed composite has no GraphWrapper to
            // carry the bindings; its parameters are instantiation-time only.
            if (auto* wrapper = dynamic_cast<GraphWrapper<gr::Graph>*>(blocks.front().get()); wrapper != nullptr) {
                if (auto attached = wrapper->attachRecipeBindings({.declarations = std::move(declarations), .bindings = std::move(bindings), .values = std::move(*values)}); !attached.has_value()) {
                    return std::unexpected(attached.error());
                }
            }
        }
        return blocks.front();
    } catch (const gr::exception& e) {
        return std::unexpected(gr::Error{e});
    } catch (const std::exception& e) {
        return std::unexpected(gr::Error{e});
    }
}

} // namespace gr

#endif // include guard
