#ifndef GNURADIO_PLUGIN_LOADER_HPP
#define GNURADIO_PLUGIN_LOADER_HPP

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "BlockRegistry.hpp"

#include <gnuradio-4.0/BlockAttributes.hpp>
#include <gnuradio-4.0/PluginMetadata.hpp>
#include <gnuradio-4.0/YamlPmt.hpp>

#ifdef INTERNAL_ENABLE_BLOCK_PLUGINS
#include <dlfcn.h>

#include "Plugin.hpp"
#endif

#include <gnuradio-4.0/Profiler.hpp>

namespace gr {

using namespace std::string_literals;
using namespace std::string_view_literals;

// Forward declaration needed for instantiateBlockFromYamlDefinition before PluginLoader is fully defined.
class PluginLoader;

namespace detail {

using gr::pmt::yaml::ParseError;

template<typename R>
R optionalMapAt(const auto& map, std::string_view key, auto defaultResult) {
    if (auto it = map.find(std::string(key)); it != map.cend()) {
        return it->second;
    } else {
        return defaultResult;
    }
}

inline std::string joinUri(const std::string& base, const std::string& file) {
    return base.empty()          ? file        //
           : base.ends_with('/') ? base + file //
                                 : base + '/' + file;
}

inline bool isRemoteUri(std::string_view uri) noexcept { return uri.starts_with("http://") || uri.starts_with("https://"); }

inline std::expected<std::string, ParseError> readUriToString(std::string_view uri) {
    const auto uriString = std::string(uri);
    if (isRemoteUri(uriString)) {
        return std::unexpected(ParseError{.message = "HTTP(S) asset loading is not available in gnuradio4-core"});
    }

    const auto    path = uriString.starts_with("file://") ? uriString.substr(7) : uriString;
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::unexpected(ParseError{.message = std::format("Failed to read URI {}", uriString)});
    }
    std::ostringstream content;
    content << input.rdbuf();
    return content.str();
}

inline std::string uriToCacheFilename(std::string_view uri) {
    // FNV-1a 64-bit: deterministic across runs, collision-resistant, NAME_MAX-safe.
    constexpr std::uint64_t fnvOffset = 0xcbf29ce484222325ULL;
    constexpr std::uint64_t fnvPrime  = 0x100000001b3ULL;
    std::uint64_t           hash      = fnvOffset;
    for (char c : uri) {
        hash ^= static_cast<std::uint8_t>(c);
        hash *= fnvPrime;
    }
    return std::format("{:016x}", hash);
}

/**
 * @brief The role of the block a definition instantiates, from the stream ports its first block entry exports.
 *
 * A definition instantiates its first block entry, and that entry's graph lists what the result exports under
 * `exported_ports`. An exported entry whose direction is `INPUT` is an input and any other direction an output, as
 * the importer reads them. An entry whose inner port is a message port counts for neither: `registry` creates the
 * inner block with its default settings, and the port of that name states its kind. An entry that does not resolve
 * on such an instance counts as a stream port. The role is `block::roleFromPorts()` of the two port kinds, with
 * `isNotation` for a definition that declares `plane/notation`.
 */
[[nodiscard]] inline std::optional<block::Label> definitionRole(const property_map& definition, bool isNotation, const BlockRegistry& registry) {
    const auto find = []<typename TValue>(const property_map* map, std::string_view key, std::type_identity<TValue>) -> const TValue* {
        if (map == nullptr) {
            return nullptr;
        }
        const auto it = map->find(key);
        return it == map->cend() ? nullptr : it->second.get_if<TValue>();
    };
    const auto text = [](const property_map* map, std::string_view key) -> std::string_view {
        if (map == nullptr) {
            return {};
        }
        const auto it = map->find(key);
        return it == map->cend() ? std::string_view{} : it->second.value_or(std::string_view{});
    };

    const property_map* firstEntry = nullptr;
    if (const auto* blocks = find(&definition, "blocks", std::type_identity<Tensor<pmt::Value>>{}); blocks != nullptr) {
        for (const pmt::Value& entry : *blocks) {
            if (firstEntry = entry.get_if<property_map>(); firstEntry != nullptr) {
                break;
            }
        }
    }
    const property_map*       graph       = find(firstEntry, "graph", std::type_identity<property_map>{});
    const Tensor<pmt::Value>* innerBlocks = find(graph, "blocks", std::type_identity<Tensor<pmt::Value>>{});

    // an exported entry names its inner block by `unique_name`, or by `name` in a hand-written file
    const auto innerTypeOf = [&](std::string_view innerName) -> std::string_view {
        if (innerBlocks == nullptr) {
            return {};
        }
        for (const pmt::Value& entry : *innerBlocks) {
            const auto* inner = entry.get_if<property_map>();
            if (text(inner, "unique_name") == innerName || text(find(inner, "parameters", std::type_identity<property_map>{}), "name") == innerName) {
                return text(inner, "id");
            }
        }
        return {};
    };
    std::map<std::string_view, std::unique_ptr<BlockModel>, std::less<>> created;
    const auto                                                           isMessagePort = [&](std::string_view innerName, bool isInput, std::string_view portName) {
        if (innerName.empty() || portName.empty()) {
            return false;
        }
        auto [it, isNew] = created.try_emplace(innerName);
        if (isNew) {
            it->second = registry.create(innerTypeOf(innerName), property_map{});
        }
        if (it->second == nullptr) {
            return false;
        }
        const std::expected<DynamicPort*, Error> port = isInput ? it->second->dynamicInputPort(portName) : it->second->dynamicOutputPort(portName);
        return port.has_value() && !port::isStream((*port)->portMaskInfo());
    };

    bool hasInputs  = false;
    bool hasOutputs = false;
    if (const auto* ports = find(graph, "exported_ports", std::type_identity<Tensor<pmt::Value>>{}); ports != nullptr) {
        for (const pmt::Value& port : *ports) {
            const auto* fields = port.get_if<Tensor<pmt::Value>>();
            if (fields == nullptr || fields->size() != 4UZ) {
                continue;
            }
            const bool isInput = (*fields)[1].value_or(std::string_view{}) == "INPUT";
            if (isMessagePort((*fields)[0].value_or(std::string_view{}), isInput, (*fields)[2].value_or(std::string_view{}))) {
                continue;
            }
            hasInputs  = hasInputs || isInput;
            hasOutputs = hasOutputs || !isInput;
        }
    }
    return block::roleFromPorts(hasInputs, hasOutputs, isNotation);
}

/**
 * @brief The attributes map a definition declares under `definition_metadata.attributes`, with the role its ports read.
 *
 * The map is empty when the definition has no `attributes` key or when that key's value is not a map. A map value
 * reads as `block::attributesFromMap()` reads it, and the role is that of `definitionRole()`. A definition carries one
 * revision, so the map states `block::kDefaultVersion`. The loader adds one line to `rejected` for each entry the
 * reader rejects, for an `attributes` value that is not a map and for a `version` other than `kDefaultVersion`.
 */
[[nodiscard]] inline property_map definitionAttributes(const property_map& definition, const property_map& definitionMetadata, const BlockRegistry& registry, std::vector<std::string>& rejected) {
    const auto declaredIt = definitionMetadata.find("attributes");
    if (declaredIt == definitionMetadata.cend()) {
        return {};
    }
    const auto* declared = declaredIt->second.get_if<property_map>();
    if (declared == nullptr) {
        rejected.push_back(std::format("attributes: {} is not a map", declaredIt->second));
        return {};
    }
    std::vector<std::string> rejectedEntries;
    block::AttributesRead    read = block::attributesFromMap(*declared, registry.vocabulary(), rejectedEntries);
    for (const std::string& line : rejectedEntries) {
        rejected.push_back(std::format("attribute {}", line));
    }
    if (read.version != block::kDefaultVersion) {
        rejected.push_back(std::format("version: {} is not {}, the one revision a definition carries", read.version, block::kDefaultVersion));
        read.version = block::kDefaultVersion;
    }
    const std::optional<block::Label> role = definitionRole(definition, read.has(block::labels::plane::notation), registry);
    read.readRole                          = role.has_value() ? std::string(role->word) : std::string{};
    return block::attributesToMap(read);
}

/**
 * @brief The words a definition brings under `definition_metadata.vocabulary`.
 *
 * The value is a list of maps, each with `label` (a `class/word`), `meaning` (a string) and `physical` (a bool, false
 * when absent). The loader adds one line to `rejected` for a value that is not a list, an entry that is not a map and
 * a `label` that is not a well-formed `class/word`, and skips each.
 */
[[nodiscard]] inline block::Vocabulary definitionVocabulary(const property_map& definitionMetadata, std::vector<std::string>& rejected) {
    block::Vocabulary vocabulary;
    const auto        declaredIt = definitionMetadata.find("vocabulary");
    if (declaredIt == definitionMetadata.cend()) {
        return vocabulary;
    }
    const auto* entries = declaredIt->second.get_if<Tensor<pmt::Value>>();
    if (entries == nullptr) {
        rejected.push_back(std::format("vocabulary: {} is not a list", declaredIt->second));
        return vocabulary;
    }
    for (const pmt::Value& value : *entries) {
        const auto* entry = value.get_if<property_map>();
        if (entry == nullptr) {
            rejected.push_back(std::format("vocabulary: {} is not a map of label, meaning and physical", value));
            continue;
        }
        const auto             labelIt = entry->find("label");
        const std::string_view text    = labelIt == entry->cend() ? std::string_view{} : labelIt->second.value_or(std::string_view{});
        const auto             parsed  = block::parseLabel(text);
        if (!parsed.has_value()) {
            rejected.push_back(std::format("vocabulary: {}", parsed.error()));
            continue;
        }
        const auto meaningIt  = entry->find("meaning");
        const auto physicalIt = entry->find("physical");
        vocabulary.add(parsed->first, parsed->second, meaningIt == entry->cend() ? std::string_view{} : meaningIt->second.value_or(std::string_view{}), physicalIt != entry->cend() && physicalIt->second.value_or(false));
    }
    return vocabulary;
}

struct YamlDefinitionsLoader {
    struct Definition {
        gr::property_map   definition;
        gr_plugin_metadata metadata;
        gr::property_map   attributes{}; ///< see definitionAttributes()
        block::Vocabulary  vocabulary{}; ///< see definitionVocabulary()
    };

    static std::string assetsCacheDir() {
        if (const char* env = ::getenv("GR_DATA_CACHE_DIR"); env != nullptr) {
            return std::string(env);
        } else {
            return std::string(GR_DATA_CACHE_DIR);
        }
    }

    std::unordered_map<std::string, Definition> _definitionForBlockName;

    /// assets an index named that did not register; each was reported as it was skipped
    std::size_t _nSkippedAssets = 0UZ;

    [[nodiscard]] std::size_t nSkippedAssets() const noexcept { return _nSkippedAssets; }

    explicit YamlDefinitionsLoader(std::span<const std::string> uris, const BlockRegistry& registry) { loadBlockDefinitions(uris, registry); }

    void loadBlockDefinitions(std::span<const std::string> uris, const BlockRegistry& registry) {
        const auto cacheDir = std::filesystem::path(assetsCacheDir()) / "asset_cache";
        // the directory is made on first use, so a run that reaches no remote asset makes none and
        // says nothing about a cache it never needed
        std::optional<bool> cacheReady;
        auto                cacheAvailable = [&] {
            if (!cacheReady.has_value()) {
                std::error_code createEc;
                std::filesystem::create_directories(cacheDir, createEc);
                cacheReady = !createEc && std::filesystem::is_directory(cacheDir);
                if (!*cacheReady) {
                    std::println("warning: plugin cache directory {} is not available; caching disabled", cacheDir.string());
                }
            }
            return *cacheReady;
        };

        auto getMapField = []<typename R>(const auto& map, const auto& key, const R& defaultValue) {
            auto it = map.find(key);
            if (it == map.cend()) {
                return defaultValue;

            } else {
                return it->second.value_or(defaultValue);
            }
        };

        for (const auto& uriBase : uris) {
            // Note: If all this was expected-based, this could have been a chain of and_then calls
            const auto indexContent = readUriToString(joinUri(uriBase, "index.yaml"));
            if (!indexContent) {
                continue;
            }
            const auto indexMap = gr::pmt::yaml::deserialize(*indexContent);
            if (!indexMap) {
                continue;
            }
            const auto assetsList = getMapField(*indexMap, "assets", gr::Tensor<gr::pmt::Value>{});
            for (const gr::pmt::Value& assetEntry : assetsList) {
                const auto* assetMap = assetEntry.get_if<pmt::Value::Map>();
                if (!assetMap) {
                    continue;
                }
                const auto file = getMapField(*assetMap, "file", std::string());
                if (file.empty()) {
                    continue;
                }

                const auto blockUri = joinUri(uriBase, file);

                std::expected<std::string, ParseError> blockContent;
                // Only a remote asset is cached, and its copy is valid for exactly the `modified` stamp it was
                // taken at: that stamp is written to a sidecar and compared as text, so the decision is between
                // two versions of the same thing. A local asset is read straight from disk — copying a file that
                // is already there buys nothing, and the copy is the only thing that can fall behind it.
                if (isRemoteUri(blockUri) && cacheAvailable()) {
                    const auto modified      = getMapField(*assetMap, "modified", "undefined"s);
                    const auto cachePath     = cacheDir / uriToCacheFilename(blockUri);
                    const auto versionPath   = std::filesystem::path(cachePath).replace_extension(".version");
                    const auto cachedVersion = readUriToString(versionPath.string());
                    if (cachedVersion && *cachedVersion == modified) {
                        blockContent = readUriToString(cachePath.string());
                    } else {
                        blockContent = readUriToString(blockUri);
                        if (blockContent) {
                            // the stamp is written last, so an interrupted write leaves a miss rather than a
                            // fresh stamp over stale content
                            if (std::ofstream body(cachePath); body) {
                                body << *blockContent;
                                body.close();
                                if (std::ofstream version(versionPath); version) {
                                    version << modified;
                                }
                            }
                        }
                    }
                } else {
                    blockContent = readUriToString(blockUri);
                }
                // a skip is reported and counted where it happens: a definition that fails to
                // register is otherwise indistinguishable from one nobody listed, and the first
                // sign of it is a registry miss somewhere else entirely
                if (!blockContent) {
                    ++_nSkippedAssets;
                    std::println("warning: block definition {} skipped: could not be read ({})", blockUri, blockContent.error().message);
                    continue;
                }

                auto blockMap = gr::pmt::yaml::deserialize(*blockContent);
                if (!blockMap) {
                    ++_nSkippedAssets;
                    std::println("warning: block definition {} skipped: not valid YAML ({}, line {})", blockUri, blockMap.error().message, blockMap.error().line);
                    continue;
                }

                const auto meta  = getMapField(*blockMap, "definition_metadata", gr::property_map{});
                auto       field = [&](const auto& key) {
                    const auto it = meta.find(std::string(key));
                    return it != meta.end() ? it->second.value_or(std::string{}) : std::string{};
                };
                gr_plugin_metadata metadata{
                    .plugin_name    = field("plugin_name"),    //
                    .plugin_author  = field("plugin_author"),  //
                    .plugin_license = field("plugin_license"), //
                    .plugin_version = field("plugin_version"),
                    .block_type     = field("block_type"), //
                };

                if (metadata.block_type.empty()) {
                    ++_nSkippedAssets;
                    std::println("warning: block definition {} skipped: definition_metadata carries no block_type", blockUri);
                    continue;
                }

                // a rejected attribute is reported on stderr, and the definition registers regardless
                std::vector<std::string> rejected;
                gr::property_map         attributes = definitionAttributes(*blockMap, meta, registry, rejected);
                block::Vocabulary        vocabulary = definitionVocabulary(meta, rejected);
                for (const std::string& line : rejected) {
                    std::println(stderr, "warning: block definition {} ({}): {}", metadata.block_type, blockUri, line);
                }

                auto blockType = metadata.block_type;
                _definitionForBlockName.insert_or_assign(std::move(blockType), Definition{std::move(*blockMap), std::move(metadata), std::move(attributes), std::move(vocabulary)});
            }
        }
    }

    std::optional<Definition> definitionForBlockName(std::string_view name) const { //
        return detail::optionalMapAt<std::optional<Definition>>(_definitionForBlockName, name, std::nullopt);
    }

    /// the attributes map of the definition registered as `name`, empty when it declares none; nothing when no
    /// definition carries the name
    [[nodiscard]] std::optional<property_map> attributesForBlockName(std::string_view name) const {
        const auto it = _definitionForBlockName.find(std::string(name));
        return it == _definitionForBlockName.cend() ? std::nullopt : std::optional<property_map>{it->second.attributes};
    }

    /// the attributes map of `version` of the definition registered as `name`, which holds `block::kDefaultVersion`
    /// alone
    [[nodiscard]] std::optional<property_map> attributesForBlockName(std::string_view name, block::Version version) const { //
        return version == block::kDefaultVersion ? attributesForBlockName(name) : std::nullopt;
    }

    /// the words every definition brings
    [[nodiscard]] block::Vocabulary vocabulary() const {
        block::Vocabulary merged;
        for (const auto& entry : _definitionForBlockName) {
            merged.merge(entry.second.vocabulary);
        }
        return merged;
    }
};

std::expected<std::shared_ptr<gr::BlockModel>, gr::Error> instantiateBlockFromYamlDefinition(gr::PluginLoader& loader, const YamlDefinitionsLoader::Definition& def, const property_map& parameters = {}) noexcept;

/**
 * @brief Instantiates the one version a caller named, or says why it could not.
 *
 * A pin that cannot be honored is reported rather than rounded to a neighboring version, and the reason
 * names the versions that are registered wherever the loader can reach them, a plugin's own registry
 * included. A YAML definition carries one revision, `block::kDefaultVersion`.
 */
template<typename TLoader>
std::expected<std::shared_ptr<gr::BlockModel>, gr::Error> instantiatePinnedOrError(TLoader& loader, std::string_view name, block::Version version, const property_map& params) {
    if (auto result = loader.instantiatePinned(name, version, params)) {
        return result;
    }
    if (const std::vector<block::Version> known = loader.blockVersions(name); !known.empty()) {
        return std::unexpected(gr::Error(std::format("'{}' is registered, but not as version {}; registered versions: {}", name, version, gr::join(known, ", "))));
    }
    if (version != block::kDefaultVersion) {
        return std::unexpected(gr::Error(std::format("'{}' is not in the block registry, so it has no version {}", name, version)));
    }
    return loader.instantiate(name, params);
}

} // namespace detail

#ifdef INTERNAL_ENABLE_BLOCK_PLUGINS
// Plugins are not supported on WASM

using plugin_create_function_t  = void (*)(gr_plugin_base**);
using plugin_destroy_function_t = void (*)(gr_plugin_base*);

/// the file extensions a plugin directory scan opens; a name that only contains one, `libfoo.so.1`, is reported instead
#if defined(_WIN32)
inline constexpr std::array<std::string_view, 1> kLibraryExtensions{".dll"};
#elif defined(__APPLE__)
inline constexpr std::array<std::string_view, 2> kLibraryExtensions{".so", ".dylib"};
#else
inline constexpr std::array<std::string_view, 1> kLibraryExtensions{".so"};
#endif

class PluginHandler {
private:
    void*                     _dl_handle  = nullptr;
    plugin_create_function_t  _create_fn  = nullptr;
    plugin_destroy_function_t _destroy_fn = nullptr;
    gr_plugin_base*           _instance   = nullptr;

    std::string _status;

    /// destroys the plugin instance, leaving the library mapped
    void releaseInstance() {
        if (_instance && _destroy_fn) {
            _destroy_fn(_instance);
            _instance = nullptr;
        }
    }

    void release() {
        releaseInstance();

        if (_dl_handle) {
            dlclose(_dl_handle);
            _dl_handle = nullptr;
        }
    }

public:
    PluginHandler() = default;

    explicit PluginHandler(const std::string& plugin_file) {
        // RTLD_LOCAL keeps plugin symbols isolated but breaks RTTI/dynamic_cast across dylib boundaries.
        // On macOS (Mach-O two-level namespace), RTLD_LOCAL also risks duplicating singletons such as
        // globalBlockRegistry(); use RTLD_GLOBAL there to match Linux ELF flat-namespace behaviour.
#ifdef __APPLE__
        _dl_handle = dlopen(plugin_file.c_str(), RTLD_LAZY | RTLD_GLOBAL);
#else
        _dl_handle = dlopen(plugin_file.c_str(), RTLD_LAZY | RTLD_LOCAL);
#endif
        if (!_dl_handle) {
            // the linker's reason: a missing library, a versioned name the installation lacks, an undefined symbol
            const char* reason = dlerror();
            _status            = reason == nullptr ? "Failed to load the plugin file" : std::format("Failed to load the plugin file: {}", reason);
            return;
        }

        // FIXME: Casting a void* to function-pointer is UB in C++. Yes "… 'dlsym' is not C++ and therefore we can do
        // whateever …". But we don't need to. Simply have a single 'extern "C"' symbol in the plugin which is an object
        // storing two function pointers. Then we need a single cast from the 'dlsym' result to an aggregate type and
        // can then extract the two function pointers from it. That's simpler and more likely to be conforming C++.
        _create_fn = reinterpret_cast<plugin_create_function_t>(dlsym(_dl_handle, "gr_plugin_make"));
        if (!_create_fn) {
            _status = "Failed to load symbol gr_plugin_make";
            return;
        }

        _destroy_fn = reinterpret_cast<plugin_destroy_function_t>(dlsym(_dl_handle, "gr_plugin_free"));
        if (!_destroy_fn) {
            _status = "Failed to load symbol gr_plugin_free";
            return;
        }

        _create_fn(&_instance);
        if (!_instance) {
            _status = "Failed to create an instance of the plugin";
            return;
        }

        if (const std::uint8_t pluginAbiVersion = _instance->abiVersion(); pluginAbiVersion != GR_PLUGIN_CURRENT_ABI_VERSION) {
            // A refused plugin is unmapped here rather than at the end of the load, so that it cannot be taken
            // afterwards for one of the shared objects that register blocks without carrying a plugin interface,
            // which the load keeps mapped.
            _status = std::format("plugin ABI version {} does not match the host's plugin ABI version {}", pluginAbiVersion, GR_PLUGIN_CURRENT_ABI_VERSION);
            std::println("warning: plugin {} not loaded: {}", plugin_file, _status);
            release();
            return;
        }
    }

    PluginHandler(const PluginHandler& other)            = delete;
    PluginHandler& operator=(const PluginHandler& other) = delete;

    PluginHandler(PluginHandler&& other) noexcept : _dl_handle(std::exchange(other._dl_handle, nullptr)), _create_fn(std::exchange(other._create_fn, nullptr)), _destroy_fn(std::exchange(other._destroy_fn, nullptr)), _instance(std::exchange(other._instance, nullptr)) {}

    PluginHandler& operator=(PluginHandler&& other) noexcept {
        auto tmp = std::move(other);
        std::swap(_dl_handle, tmp._dl_handle);
        std::swap(_create_fn, tmp._create_fn);
        std::swap(_destroy_fn, tmp._destroy_fn);
        std::swap(_instance, tmp._instance);
        return *this;
    }

    ~PluginHandler() { release(); }

    explicit operator bool() const { return _instance; }

    /// whether the library is mapped, which it is even when it is not a plugin
    [[nodiscard]] bool isLoaded() const noexcept { return _dl_handle != nullptr; }

    /**
     * @brief Gives up the unload: the library stays mapped for the lifetime of the process.
     *
     * A library whose static initializers registered blocks or schedulers leaves factory pointers into its own
     * code in the registries, and those outlive every handle to it.
     */
    void keepMapped() noexcept { _dl_handle = nullptr; }

    [[nodiscard]] const std::string& status() const { return _status; }

    auto* operator->() const { return _instance; }
};

class PluginLoader {
public:
    /**
     * @brief A shared object that is not a plugin but registered blocks or schedulers when it loaded.
     *
     * It carries no `gr_plugin_make`; its entries reach the registries from static initializers, and it is kept
     * mapped for the lifetime of the process because those entries point into its code.
     */
    struct BlockLibrary {
        std::string file;
        std::size_t nBlockRegistrations     = 0UZ;
        std::size_t nSchedulerRegistrations = 0UZ;
    };

private:
    detail::YamlDefinitionsLoader                _yamlRegistry;
    std::vector<PluginHandler>                   _pluginHandlers;
    std::vector<BlockLibrary>                    _blockLibraries;
    std::vector<std::string>                     _skippedFiles;
    std::unordered_map<std::string, std::string> _failedPlugins;
    std::unordered_set<std::string>              _loadedPluginFiles;

    std::unordered_map<std::string, gr_plugin_base*> _pluginForBlockName;
    std::unordered_map<std::string, gr_plugin_base*> _pluginForSchedulerName;

    BlockRegistry*     _registry;
    SchedulerRegistry* _schedulerRegistry;

    gr_plugin_base* pluginForBlockName(std::string_view name) const { //
        return detail::optionalMapAt<gr_plugin_base*>(_pluginForBlockName, name, nullptr);
    }

    gr_plugin_base* pluginForSchedulerName(std::string_view name) const { //
        return detail::optionalMapAt<gr_plugin_base*>(_pluginForSchedulerName, name, nullptr);
    }

    /// How many registrations the registries a load can reach have taken. A library registers into the process-wide
    /// registries rather than the pair this loader was handed, so both are counted when they differ. A registration
    /// that replaces a key an earlier library registered leaves the entry count where it was.
    [[nodiscard]] std::pair<std::size_t, std::size_t> registryGenerations() const {
        std::size_t blockGeneration     = _registry->generation();
        std::size_t schedulerGeneration = _schedulerRegistry->generation();
        if (BlockRegistry& global = gr::globalBlockRegistry(); &global != _registry) {
            blockGeneration += global.generation();
        }
        if (SchedulerRegistry& global = gr::globalSchedulerRegistry(); &global != _schedulerRegistry) {
            schedulerGeneration += global.generation();
        }
        return {blockGeneration, schedulerGeneration};
    }

public:
    PluginLoader(BlockRegistry& registry, SchedulerRegistry& scheduler_registry, std::span<const std::string> paths) : _yamlRegistry(paths, registry), _registry(&registry), _schedulerRegistry(&scheduler_registry) {
        for (const auto& pathStr : paths) {
            const std::filesystem::path directory(pathStr);
            if (!std::filesystem::is_directory(directory)) {
                continue;
            }

            for (const auto& file : std::filesystem::directory_iterator{directory}) {
                const std::filesystem::path& path     = file.path();
                const std::string            fileName = path.filename().string();

                if (!file.is_regular_file() || !std::ranges::contains(kLibraryExtensions, path.extension().string())) {
                    if (std::ranges::any_of(kLibraryExtensions, [&fileName](std::string_view extension) { return fileName.contains(extension); })) {
                        _skippedFiles.push_back(path.string());
                    }
                    continue;
                }

                auto fileString = path.string();
                if (_loadedPluginFiles.contains(fileString)) {
                    continue;
                }
                _loadedPluginFiles.insert(fileString);

                const auto [blockGenerationBefore, schedulerGenerationBefore] = registryGenerations();

                if (PluginHandler handler(fileString); handler) {
                    for (std::string_view blockName : handler->availableBlocks()) {
                        _pluginForBlockName.emplace(std::string(blockName), handler.operator->());
                    }

                    for (std::string_view schedulerName : handler->availableSchedulers()) {
                        _pluginForSchedulerName.emplace(std::string(schedulerName), handler.operator->());
                    }

                    _pluginHandlers.push_back(std::move(handler));

                } else {
                    const auto [blockGenerationAfter, schedulerGenerationAfter] = registryGenerations();
                    const std::size_t blockRegistrations                        = blockGenerationAfter - blockGenerationBefore;
                    const std::size_t schedulerRegistrations                    = schedulerGenerationAfter - schedulerGenerationBefore;

                    if (handler.isLoaded() && (blockRegistrations != 0UZ || schedulerRegistrations != 0UZ)) {
                        handler.keepMapped();
                        _blockLibraries.push_back({.file = fileString, .nBlockRegistrations = blockRegistrations, .nSchedulerRegistrations = schedulerRegistrations});
                    } else {
                        _failedPlugins[fileString] = handler.status();
                    }
                }
            }
        }
    }

    BlockRegistry&     registry() { return *_registry; }
    SchedulerRegistry& schedulerRegistry() { return *_schedulerRegistry; }

    const auto& plugins() const { return _pluginHandlers; }

    /// the shared objects that registered blocks or schedulers without being plugins
    const std::vector<BlockLibrary>& blockLibraries() const { return _blockLibraries; }

    /// directory entries whose name reads as a shared object but which the scan did not open, so that a directory of them is not indistinguishable from an empty one
    const std::vector<std::string>& skippedFiles() const { return _skippedFiles; }

    const auto& failedPlugins() const { return _failedPlugins; }

    std::vector<std::string> availableBlocks() const {
        auto                     keysView = _pluginForBlockName | std::views::keys;
        std::vector<std::string> result(keysView.begin(), keysView.end());

        const auto& builtin = _registry->keys();
        result.insert(result.end(), builtin.begin(), builtin.end());

        // remove duplicates
        std::ranges::sort(result);
        auto newEnd = std::ranges::unique(result).begin();
        result.erase(newEnd, result.end());
        return result;
    }

    /// Instantiates and keeps the reason a definition refused: a null value is an ordinary miss --
    /// no block, plugin or YAML definition carries that name -- while an unexpected carries the
    /// definition's own reason, which for a recipe names the parameter the recipe requires.
    /// instantiate() calls this, prints the reason and drops it.
    std::expected<std::shared_ptr<gr::BlockModel>, gr::Error> instantiateOrError(std::string_view name, const property_map& params = property_map{}) {
        // Try to create a node from the global registry
        if (auto result = _registry->create(name, params)) {
            return std::shared_ptr<gr::BlockModel>(std::move(result));
        }

        if (auto* plugin = pluginForBlockName(name); plugin != nullptr) {
            return std::shared_ptr<gr::BlockModel>(plugin->createBlock(name, params));
        }

        if (const auto def = _yamlRegistry.definitionForBlockName(name)) {
            return detail::instantiateBlockFromYamlDefinition(*this, *def, params);
        }

        // a miss is an ordinary probe result (block, scheduler and YAML lookups are tried in
        // sequence): the null value is the signal, and the caller that treats it as terminal
        // reports it together with what was requested
        return std::shared_ptr<gr::BlockModel>{};
    }

    std::shared_ptr<gr::BlockModel> instantiate(std::string_view name, const property_map& params = property_map{}) {
        auto result = instantiateOrError(name, params);
        if (!result) {
            std::print("Error: YAML block instantiation failed for '{}': {} ({})\n", name, result.error().message, result.error().srcLoc());
            return {};
        }
        return *result;
    }

    /// every version of `name` a create can reach: the registry's, else those the plugin owning the name holds
    [[nodiscard]] std::vector<block::Version> blockVersions(std::string_view name) const {
        if (std::vector<block::Version> known = _registry->versions(name); !known.empty()) {
            return known;
        }
        if (const gr_plugin_base* plugin = pluginForBlockName(name); plugin != nullptr) {
            return plugin->blockVersions(name);
        }
        return {};
    }

    /**
     * @brief The attributes map of the newest version of `name`.
     *
     * The registry answers first, then the plugin owning the name, then a YAML definition. The map is empty for a
     * block that declares no attributes; nothing answers for a name none of them holds.
     */
    [[nodiscard]] std::optional<property_map> blockAttributes(std::string_view name) const {
        if (std::optional<property_map> known = _registry->attributes(name); known.has_value()) {
            return known;
        }
        if (const gr_plugin_base* plugin = pluginForBlockName(name); plugin != nullptr) {
            const std::vector<block::Version> versions = plugin->blockVersions(name);
            return versions.empty() ? std::nullopt : plugin->blockAttributes(name, std::ranges::max(versions));
        }
        return _yamlRegistry.attributesForBlockName(name);
    }

    /// the attributes map of that one version of `name`; the first source holding the name answers alone, as
    /// instantiatePinnedOrError() refuses a version the registry or the plugin lacks
    [[nodiscard]] std::optional<property_map> blockAttributes(std::string_view name, block::Version version) const {
        if (_registry->contains(name)) {
            return _registry->attributes(name, version);
        }
        if (const gr_plugin_base* plugin = pluginForBlockName(name); plugin != nullptr) {
            return plugin->blockAttributes(name, version);
        }
        return _yamlRegistry.attributesForBlockName(name, version);
    }

    /**
     * @brief The words of every block the loader can reach, with their meanings.
     *
     * The registry's vocabulary, each loaded plugin's `blockVocabulary()` and each YAML definition's words, merged:
     * a word with several meanings keeps each, and a word physical in any source is physical.
     */
    [[nodiscard]] block::Vocabulary vocabulary() const {
        block::Vocabulary merged = _registry->vocabulary();
        for (const PluginHandler& handler : _pluginHandlers) {
            std::vector<std::string> discarded;
            merged.merge(block::vocabularyFromMap(handler->blockVocabulary(), discarded));
        }
        merged.merge(_yamlRegistry.vocabulary());
        return merged;
    }

    /// the one version named, from the registry or from the plugin owning the name; the instance records the pin
    std::shared_ptr<gr::BlockModel> instantiatePinned(std::string_view name, block::Version version, const property_map& params = property_map{}) {
        if (auto result = _registry->create(name, version, params)) {
            return result;
        }

        if (auto* plugin = pluginForBlockName(name); plugin != nullptr) {
            return plugin->createPinnedBlock(name, version, params);
        }

        return {};
    }

    /// see gr::detail::instantiatePinnedOrError
    std::expected<std::shared_ptr<gr::BlockModel>, gr::Error> instantiatePinnedOrError(std::string_view name, block::Version version, const property_map& params = property_map{}) { return detail::instantiatePinnedOrError(*this, name, version, params); }

    std::shared_ptr<gr::SchedulerModel> instantiateScheduler(std::string_view name, const property_map& params = property_map{}) {
        if (auto result = _schedulerRegistry->create(name, params)) {
            return std::shared_ptr<gr::SchedulerModel>(result.release());
        }

        auto* plugin = pluginForSchedulerName(name);

        if (plugin == nullptr) {
            // a miss is an ordinary probe result, as for instantiate() above
            return {};
        }

        auto result = plugin->createScheduler(name, params);
        return std::shared_ptr<gr::SchedulerModel>(result.release());
    }

    std::vector<std::string> availableSchedulers() const {
        auto                     keysView = _pluginForSchedulerName | std::views::keys;
        std::vector<std::string> result(keysView.begin(), keysView.end());

        const auto& builtin = _schedulerRegistry->keys();
        result.insert(result.end(), builtin.begin(), builtin.end());

        // remove duplicates
        std::ranges::sort(result);
        auto newEnd = std::ranges::unique(result).begin();
        result.erase(newEnd, result.end());
        return result;
    }

    bool isBlockAvailable(std::string_view block) const { return _registry->contains(block) || pluginForBlockName(block) != nullptr; }

    bool isSchedulerAvailable(std::string_view scheduler) const { return _schedulerRegistry->contains(scheduler) || pluginForSchedulerName(scheduler) != nullptr; }

    const auto& definitionForBlockName() const { return _yamlRegistry._definitionForBlockName; }

    /// how many assets the definition roots named that did not register: unreadable, not
    /// deserializable, or carrying no block_type. Each was reported as it was skipped.
    [[nodiscard]] std::size_t nSkippedAssets() const noexcept { return _yamlRegistry.nSkippedAssets(); }
};
#else
// PluginLoader on WASM is just a wrapper on BlockRegistry to provide the
// same API as proper PluginLoader
class PluginLoader {
private:
    detail::YamlDefinitionsLoader _yamlRegistry;
    BlockRegistry*                _registry;
    SchedulerRegistry*            _schedulerRegistry;

public:
    PluginLoader(BlockRegistry& registry, SchedulerRegistry& scheduler_registry, std::span<const std::string> paths) : _yamlRegistry(paths, registry), _registry(&registry), _schedulerRegistry(&scheduler_registry) {}

    BlockRegistry&     registry() { return *_registry; }
    SchedulerRegistry& schedulerRegistry() { return *_schedulerRegistry; }

    auto availableBlocks() const { return _registry->keys(); }
    auto availableSchedulers() const { return _schedulerRegistry->keys(); }

    /// see the non-WASM PluginLoader::instantiateOrError
    std::expected<std::shared_ptr<gr::BlockModel>, gr::Error> instantiateOrError(std::string_view name, const property_map& params = {}) {
        if (auto result = _registry->create(name, params)) {
            return std::shared_ptr<gr::BlockModel>(std::move(result));
        }

        if (const auto def = _yamlRegistry.definitionForBlockName(name)) {
            return detail::instantiateBlockFromYamlDefinition(*this, *def, params);
        }

        return std::shared_ptr<gr::BlockModel>{};
    }

    std::shared_ptr<gr::BlockModel> instantiate(std::string_view name, const property_map& params = {}) {
        auto result = instantiateOrError(name, params);
        if (!result) {
            std::print("Error: YAML block instantiation failed for '{}': {} ({})\n", name, result.error().message, result.error().srcLoc());
            return nullptr;
        }
        return *result;
    }

    /// see the non-WASM PluginLoader::blockVersions
    [[nodiscard]] std::vector<block::Version> blockVersions(std::string_view name) const { return _registry->versions(name); }

    /// see the non-WASM PluginLoader::blockAttributes; the registry answers, then a YAML definition
    [[nodiscard]] std::optional<property_map> blockAttributes(std::string_view name) const {
        std::optional<property_map> known = _registry->attributes(name);
        return known.has_value() ? known : _yamlRegistry.attributesForBlockName(name);
    }

    /// see the non-WASM PluginLoader::blockAttributes
    [[nodiscard]] std::optional<property_map> blockAttributes(std::string_view name, block::Version version) const { //
        return _registry->contains(name) ? _registry->attributes(name, version) : _yamlRegistry.attributesForBlockName(name, version);
    }

    /// see the non-WASM PluginLoader::vocabulary; the registry's words and each YAML definition's
    [[nodiscard]] block::Vocabulary vocabulary() const {
        block::Vocabulary merged = _registry->vocabulary();
        merged.merge(_yamlRegistry.vocabulary());
        return merged;
    }

    /// see the non-WASM PluginLoader::instantiatePinned
    std::shared_ptr<gr::BlockModel> instantiatePinned(std::string_view name, block::Version version, const property_map& params = {}) { return _registry->create(name, version, params); }

    /// see the non-WASM PluginLoader::instantiatePinnedOrError
    std::expected<std::shared_ptr<gr::BlockModel>, gr::Error> instantiatePinnedOrError(std::string_view name, block::Version version, const property_map& params = {}) { return detail::instantiatePinnedOrError(*this, name, version, params); }

    std::shared_ptr<gr::SchedulerModel> instantiateScheduler(std::string_view name, const property_map& params = {}) {
        auto result = _schedulerRegistry->create(name, params);
        return result ? std::shared_ptr<gr::SchedulerModel>((result.release())) : nullptr;
    }

    bool isBlockAvailable(std::string_view block) const { return _registry->contains(block); }
    bool isSchedulerAvailable(std::string_view scheduler) const { return _schedulerRegistry->contains(scheduler); }

    const auto& definitionForBlockName() const { return _yamlRegistry._definitionForBlockName; }

    /// see the non-WASM PluginLoader::nSkippedAssets
    [[nodiscard]] std::size_t nSkippedAssets() const noexcept { return _yamlRegistry.nSkippedAssets(); }
};
#endif

PluginLoader& globalPluginLoader();

} // namespace gr

#endif // GNURADIO_PLUGIN_LOADER_HPP
