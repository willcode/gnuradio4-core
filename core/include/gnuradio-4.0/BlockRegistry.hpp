#ifndef GNURADIO_BLOCK_REGISTRY_HPP
#define GNURADIO_BLOCK_REGISTRY_HPP

#include <gnuradio-4.0/meta/simd.hpp>

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gnuradio-4.0/config.hpp>
#include <gnuradio-4.0/meta/utils.hpp>

#include <gnuradio-4.0/BlockModel.hpp>
#include <gnuradio-4.0/SchedulerModel.hpp>

#include <gnuradio-4.0/BlockRegistration.hpp>
#include <gnuradio-4.0/Export.hpp>

/**
 *  namespace gr {
 *  template<typename T> struct AlgoImpl1 {};
 *  template<typename T> struct AlgoImpl2 {};
 *
 *  // register block with arbitrary NTTPs (here: 3UZ) and expand T in [float,double], U in [short, int, long, long long]
 *  GR_REGISTER_BLOCK(gr::basic::BlockN, ([T], [U], 3UZ), [ float, double ], [ short, int, long, long long ])
 *  // register block with arbitrary NTTPs (here: 4UZ) and expand T for [short], U for [short] only
 *  GR_REGISTER_BLOCK("CustomBlockNameN", gr::basic::BlockN, ([T], [U], 4UZ, gr::basic::AlgoImpl2<[T]>), [ short ], [ short ])
 *
 *  template<typename T, typename U, std::size_t N, typename Alog = AlgoImpl1<T>>
 *  struct BlockN : public gr::IBlock { ... };
 *
 *  } // namespace gr::basic
 *
 * other macro variants options:
 * GR_REGISTER_BLOCK("MyBlockName", gr::basic::Block1, ([T], [U]), [ float, double ], [int])
 * GR_REGISTER_BLOCK(gr::basic::Block0)
 * GR_REGISTER_BLOCK("blockN.hpp", gr::basic::BlockN, ([T],[U],3UZ,SomeAlgo<[T]>), [ short, int], [double])
 */
#define GR_REGISTER_BLOCK(...) /* Marker macro for parse_registrations */

// The version of the plugin interface, raised by every change to the layout of a type that crosses the plugin
// boundary: `gr_plugin_base` itself, the `BlockModel` and `SchedulerModel` interfaces whose objects a plugin or a
// registry factory hands back, and a block's settings: the `SettingsBase` interface and the `settings::BlockDescriptor`
// table behind it. A plugin records the version it was compiled against. A registry entry records the
// version of the code that calls `insert()`, so a block entry added through `insertBlockFactory()` records the
// version of core. A host loads only a plugin whose version equals its own. The host also reads the version of each
// scheduler a shared object registers, and keeps a scheduler only at its own version. A virtual call through another
// version's interface reaches the wrong function. A plugin's port also shares its stream ring and its tag store with a
// port of the host, and the layout of `gr::Tag`, the tag store's element, crosses the boundary with them. The host's
// scheduler reads the `msgIn` and `msgOut` ports of a plugin's block through typed pointers, and the layout of a port
// crosses the boundary too. `SchedulerModel::start()` returns the refusal of a start as a `std::expected`, and a host
// reads it from a plugin's scheduler. `gr_plugin_base` carries `blockVersions()` and `createPinnedBlock()`, through
// which a host reaches a plugin's block at a pinned version. `BlockModel` carries no `version()` or `status()`
// virtual: both read the block's attributes from its meta information. A block registration carries the labels of
// its type, and the attributes cross the boundary in their map form. `gr_plugin_base` ends with `deviceListings()`,
// and a `gr::DeviceListing` crosses the boundary through it.
#define GR_PLUGIN_CURRENT_ABI_VERSION 14

namespace gr {

using namespace std::string_literals;
using namespace std::string_view_literals;

using BlockFactory = std::unique_ptr<BlockModel> (*)(property_map);

namespace detail {

/// How one registration of a key at one version fares against the registrations filed under that key.
enum class Claim : std::uint8_t {
    Added,    ///< no registration held the version, and this one holds it
    Repeated, ///< the registration that holds the version was offered again
    Refused,  ///< another registration holds the version and keeps it
};

/**
 * @brief Files `offered` under `version` unless a registration holds that version already.
 *
 * Every registration of a block goes through this rule: a registry files its keys with it, and the plugin loader
 * files the keys its plugins offer with it. The first registration of a key at a version holds it. `isSame` tells a
 * repeat of the holder from another registration, which is refused and leaves the holder in place. The other versions
 * of the key are untouched either way.
 */
template<typename TEntry, typename TSame>
[[nodiscard]] Claim claimVersion(std::map<block::Version, TEntry>& versions, block::Version version, const TEntry& offered, TSame&& isSame) {
    const auto [held, added] = versions.try_emplace(version, offered);
    if (added) {
        return Claim::Added;
    }
    return isSame(held->second, offered) ? Claim::Repeated : Claim::Refused;
}

} // namespace detail

/**
 * @brief What a generated definition unit exports beside its factory.
 *
 * `name` and `alias` are the strings the typed `insert<TBlock>()` path would have derived for the
 * same block, so a declaration-only registration unit can hand them to `insertBlockFactory()`
 * without naming the type. `attributes` and `labels` are read in `makeBlockRegistration<TBlock>()`,
 * where the type is still named, so the unit that carries them names none either. `attributes` is the
 * map form of what the type declares, version included, and empty for a type that declares nothing.
 * `labels` views the type's declared labels, with their meanings, in the definition unit's static
 * storage; the registry copies them into its vocabulary.
 */
struct BlockRegistration {
    std::string                   name;
    std::string                   alias;
    BlockFactory                  factory = nullptr;
    property_map                  attributes{};
    std::span<const block::Label> labels{};
};

/// The registry alias for a block registered as `alias` with template parameters `aliasParameters`.
[[nodiscard]] inline std::string makeRegistryAlias(std::string_view alias, std::string_view aliasParameters) {
    if (alias.empty()) {
        return std::string{};
    }
    if (alias[0] == '=') {
        return std::string(alias.substr(1));
    }
    if (aliasParameters.empty()) {
        return meta::detail::makePortableTypeName(alias);
    }
    return meta::detail::makePortableTypeName(std::string{alias} + "<" + std::string{aliasParameters} + ">");
}

template<typename TModel, template<typename...> typename TWrapper>
class GeneralRegistry {
    using this_t = GeneralRegistry<TModel, TWrapper>;
    static std::unique_ptr<TModel> factoryProto(property_map params);

    template<typename TBlock>
    static std::unique_ptr<TModel> defaultFactory(property_map params) { //
        return std::make_unique<TWrapper<TBlock>>(std::move(params));
    }

    /// one registered revision of one key: its factory and the map form of the attributes the type declares
    struct TVersionHandler {
        decltype(this_t::factoryProto)* createFunction = nullptr;
        property_map                    attributes{};
    };

    /// A key holds every version registered under it, ordered, so the newest is the last entry. The first
    /// registration of a key at a version holds that version (`detail::claimVersion()`).
    struct TTypeHandler {
        std::string                               alias;
        std::map<block::Version, TVersionHandler> versions;
    };

    // a version belongs to the factory it was recorded with, so an entry a later insertion replaced without recording
    // one reads as unversioned
    struct RecordedAbiVersion {
        decltype(this_t::factoryProto)* createFunction = nullptr;
        std::uint8_t                    abiVersion     = 0;
    };

public:
    using Factory = decltype(this_t::factoryProto)*;

    /// A registration refused because an earlier registration holds its key at its version.
    struct Refusal {
        std::string    key;
        block::Version version = block::kDefaultVersion;
        Factory        refused = nullptr; ///< the factory the refused registration offered
        Factory        holder  = nullptr; ///< the factory of the registration that holds the key at that version
    };

private:
    // `_blockTypeHandlers` and `_generation` keep the layout of a registry that records no version. A shared object
    // built against that older layout inserts into them through its own inline copy of `insert()`, which does not
    // write `_abiVersions`. A host built against the older layout cannot load a shared object built against this one.
    // The shared object's static initializers write `_abiVersions` past the end of the older registry object.
    std::map<std::string, TTypeHandler, std::less<>>       _blockTypeHandlers;
    std::size_t                                            _generation = 0UZ;
    std::map<std::string, RecordedAbiVersion, std::less<>> _abiVersions;
    block::Vocabulary                                      _vocabulary = block::coreVocabulary();
    std::vector<Refusal>                                   _refused;

    /// files `entry` under `key` at `version` by `detail::claimVersion()`, counting an addition and recording a refusal
    detail::Claim claim(std::string_view key, block::Version version, const TVersionHandler& entry) {
        auto [handler, isNewKey]    = _blockTypeHandlers.try_emplace(std::string(key));
        const detail::Claim outcome = detail::claimVersion(handler->second.versions, version, entry, [](const TVersionHandler& held, const TVersionHandler& offered) { return held.createFunction == offered.createFunction; });
        if (outcome == detail::Claim::Added) {
            ++_generation;
        } else if (outcome == detail::Claim::Refused) {
            _refused.push_back({.key = handler->first, .version = version, .refused = entry.createFunction, .holder = handler->second.versions.at(version).createFunction});
        }
        return outcome;
    }

    [[nodiscard]] const TTypeHandler* handlerFor(std::string_view blockName) const {
        const auto it = _blockTypeHandlers.find(blockName);
        return it == _blockTypeHandlers.cend() ? nullptr : std::addressof(it->second);
    }

    [[nodiscard]] const TVersionHandler* versionHandlerFor(std::string_view blockName, block::Version version) const {
        const TTypeHandler* handler = handlerFor(blockName);
        if (handler == nullptr) {
            return nullptr;
        }
        const auto versionIt = handler->versions.find(version);
        return versionIt == handler->versions.cend() ? nullptr : std::addressof(versionIt->second);
    }

public:
    /// every entry of a registry, as `takeEntries()` moves them out and `restoreEntries()` puts them back
    struct Entries {
        std::map<std::string, TTypeHandler, std::less<>>       handlers;
        std::map<std::string, RecordedAbiVersion, std::less<>> abiVersions;
    };

    GeneralRegistry()                               = default;
    GeneralRegistry(const this_t& other)            = delete;
    GeneralRegistry& operator=(const this_t& other) = delete;

    GeneralRegistry(this_t&& other) noexcept : _blockTypeHandlers(std::exchange(other._blockTypeHandlers, {})), _abiVersions(std::exchange(other._abiVersions, {})), _vocabulary(std::exchange(other._vocabulary, {})), _refused(std::exchange(other._refused, {})) {}
    GeneralRegistry& operator=(this_t&& other) noexcept {
        auto tmp = std::move(other);
        std::swap(_blockTypeHandlers, tmp._blockTypeHandlers);
        std::swap(_abiVersions, tmp._abiVersions);
        std::swap(_refused, tmp._refused);
        std::swap(_vocabulary, tmp._vocabulary);
        return *this;
    }
    ~GeneralRegistry() = default;

#ifdef GR_ENABLE_BLOCK_REGISTRY
    /**
     * @brief Adds an entry a generated definition unit already produced: nothing here names the block type.
     *
     * The entry is filed under its key and, when it has one, its alias, each at the version `attributes` states, and
     * `block::kDefaultVersion` when it states none. Each of the two keys follows `detail::claimVersion()`: the first
     * registration of a key at a version holds it, the same factory offered again adds nothing, and another factory is
     * refused and recorded in `refused()`. A key names the alias of the first registration that holds both the key and
     * the alias. The `labels` join the vocabulary when the entry adds a key or a version. A key the entry is not
     * refused under records `abiVersion`. Its default argument is evaluated at the call site. The entry therefore
     * records the `GR_PLUGIN_CURRENT_ABI_VERSION` of the translation unit that registers, whichever copy of this
     * function runs. Returns whether it added a key or a version.
     */
    bool insert(std::string_view name, std::string_view alias, Factory factory, property_map attributes = {}, std::span<const block::Label> labels = {}, std::uint8_t abiVersion = GR_PLUGIN_CURRENT_ABI_VERSION) {
        const block::Version  version = block::attributesFromMap(attributes).version;
        const TVersionHandler entry{.createFunction = factory, .attributes = std::move(attributes)};

        const detail::Claim nameClaim = claim(name, version, entry);
        bool                added     = nameClaim == detail::Claim::Added;
        if (nameClaim != detail::Claim::Refused) {
            _abiVersions.insert_or_assign(std::string(name), RecordedAbiVersion{.createFunction = factory, .abiVersion = abiVersion});
        }
        if (!alias.empty()) {
            const detail::Claim aliasClaim = claim(alias, version, entry);
            added                          = added || aliasClaim == detail::Claim::Added;
            if (aliasClaim != detail::Claim::Refused) {
                _abiVersions.insert_or_assign(std::string(alias), RecordedAbiVersion{.createFunction = factory, .abiVersion = abiVersion});
            }
            if (std::string& named = _blockTypeHandlers.find(name)->second.alias; named.empty() && nameClaim != detail::Claim::Refused && aliasClaim != detail::Claim::Refused) {
                named = std::string(alias);
            }
        }
        if (added) {
            for (const block::Label& label : labels) {
                _vocabulary.add(label);
            }
        }
        return added;
    }

    template<BlockLike TBlock>
    requires std::is_constructible_v<TBlock, property_map>
    bool insert(std::string_view alias = "", std::string_view aliasParameters = "", std::uint8_t abiVersion = GR_PLUGIN_CURRENT_ABI_VERSION) {
        return insert(gr::meta::type_name<TBlock>(), makeRegistryAlias(alias, aliasParameters), defaultFactory<TBlock>, block::detail::declaredAttributesMap<TBlock>(), block::attributesOf<TBlock>().labels, abiVersion);
    }
#else
    bool insert([[maybe_unused]] std::string_view name, [[maybe_unused]] std::string_view alias, [[maybe_unused]] Factory factory, [[maybe_unused]] property_map attributes = {}, [[maybe_unused]] std::span<const block::Label> labels = {}, [[maybe_unused]] std::uint8_t abiVersion = GR_PLUGIN_CURRENT_ABI_VERSION) { return false; }

    template<BlockLike TBlock>
    requires std::is_constructible_v<TBlock, property_map>
    bool insert([[maybe_unused]] std::string_view alias = "", [[maybe_unused]] std::string_view aliasParameters = "", [[maybe_unused]] std::uint8_t abiVersion = GR_PLUGIN_CURRENT_ABI_VERSION) {
        return false;
        // disables plugin system in favour of faster compile-times and when runtime or Python wrapping APIs are not requrired
        // e.g. for compile-time only flow-graphs or for CI runners
    }
#endif

    /// the newest version registered under `blockName`
    [[nodiscard]] std::unique_ptr<TModel> create(std::string_view blockName, property_map blockParams) const {
        const TTypeHandler* handler = handlerFor(blockName);
        if (handler == nullptr || handler->versions.empty()) {
            return nullptr;
        }
        return std::prev(handler->versions.cend())->second.createFunction(std::move(blockParams));
    }

    /// the one version named, or nothing; the instance records that it was pinned
    [[nodiscard]] std::unique_ptr<TModel> create(std::string_view blockName, block::Version version, property_map blockParams) const {
        const TVersionHandler* entry = versionHandlerFor(blockName, version);
        if (entry == nullptr) {
            return nullptr;
        }
        auto created = entry->createFunction(std::move(blockParams));
        // only a block model records the pin; a scheduler model has no such state
        if constexpr (requires { created->setPinnedVersion(version); }) {
            if (created) {
                created->setPinnedVersion(version);
            }
        }
        return created;
    }

    /// every version registered under `blockName`, oldest first; empty for a key that is not registered
    [[nodiscard]] std::vector<block::Version> versions(std::string_view blockName) const {
        const TTypeHandler* handler = handlerFor(blockName);
        if (handler == nullptr) {
            return {};
        }
        auto view = handler->versions | std::views::keys;
        return {view.begin(), view.end()};
    }

    [[nodiscard]] std::optional<block::Version> newestVersion(std::string_view blockName) const {
        const TTypeHandler* handler = handlerFor(blockName);
        if (handler == nullptr || handler->versions.empty()) {
            return std::nullopt;
        }
        return std::prev(handler->versions.cend())->first;
    }

    /// the attributes map of the newest version registered under `blockName`, empty for a type that declares none;
    /// nothing when the key is not registered
    [[nodiscard]] std::optional<property_map> attributes(std::string_view blockName) const {
        const std::optional<block::Version> newest = newestVersion(blockName);
        return newest.has_value() ? attributes(blockName, *newest) : std::nullopt;
    }

    /// the attributes map of that one registration; nothing when the key or the version is not registered
    [[nodiscard]] std::optional<property_map> attributes(std::string_view blockName, block::Version version) const {
        const TVersionHandler* entry = versionHandlerFor(blockName, version);
        return entry == nullptr ? std::nullopt : std::optional<property_map>{entry->attributes};
    }

    /// the words of the class `status` that one registration declares; nothing when the key or the version is not
    /// registered
    [[nodiscard]] std::optional<std::vector<std::string>> status(std::string_view blockName, block::Version version) const {
        const TVersionHandler* entry = versionHandlerFor(blockName, version);
        if (entry == nullptr) {
            return std::nullopt;
        }
        const block::AttributesRead read = block::attributesFromMap(entry->attributes, _vocabulary);
        std::vector<std::string>    words;
        for (const std::string_view word : read.words(block::LabelClass::Status)) {
            words.emplace_back(word);
        }
        return words;
    }

    /// core's words and the words of every registration, with their meanings
    [[nodiscard]] const block::Vocabulary& vocabulary() const noexcept { return _vocabulary; }

    [[nodiscard]] std::vector<std::string> keys() const {
        auto view = _blockTypeHandlers | std::views::keys;
        return {view.begin(), view.end()};
    }

    /// increments once for each key, and each version under a key, that a registration adds
    [[nodiscard]] std::size_t generation() const noexcept { return _generation; }

    /// every registration this registry refused, in the order they were offered
    [[nodiscard]] const std::vector<Refusal>& refused() const noexcept { return _refused; }

    /// the factory of the registration that holds `blockName` at `version`, null when none holds it
    [[nodiscard]] Factory factoryFor(std::string_view blockName, block::Version version) const {
        const TVersionHandler* entry = versionHandlerFor(blockName, version);
        return entry == nullptr ? nullptr : entry->createFunction;
    }

    [[nodiscard]] bool contains(std::string_view blockName) const { return _blockTypeHandlers.contains(blockName); }

    /// the plugin ABI version the entry under `key` was registered at. Empty for a key the registry does not hold.
    /// Empty as well for an entry that code built against a registry without versions registered.
    [[nodiscard]] std::optional<std::uint8_t> abiVersion(std::string_view key) const {
        const auto handler  = _blockTypeHandlers.find(key);
        const auto recorded = _abiVersions.find(key);
        if (handler == _blockTypeHandlers.end() || recorded == _abiVersions.end()) {
            return std::nullopt;
        }
        if (std::ranges::none_of(handler->second.versions | std::views::values, [&recorded](const TVersionHandler& entry) { return entry.createFunction == recorded->second.createFunction; })) {
            return std::nullopt;
        }
        return recorded->second.abiVersion;
    }

    /// Moves every entry out and leaves the generation as it is. Until `restoreEntries()` puts the taken entries back,
    /// the registry holds only what is registered after this call.
    [[nodiscard]] Entries takeEntries() { return {.handlers = std::exchange(_blockTypeHandlers, {}), .abiVersions = std::exchange(_abiVersions, {})}; }

    /// Puts back the entries `takeEntries()` returned. With `keepRegistered`, each entry registered since then is a
    /// later registration of its key and version than the taken ones and follows `detail::claimVersion()`: it stays
    /// where no taken entry holds its key at its version, and is refused and recorded in `refused()` where one does.
    /// A key every version of which stays keeps its alias and its recorded ABI version. Without it, every such entry is
    /// dropped.
    void restoreEntries(Entries taken, bool keepRegistered) {
        std::map<std::string, TTypeHandler, std::less<>>       registered         = std::exchange(_blockTypeHandlers, std::move(taken.handlers));
        std::map<std::string, RecordedAbiVersion, std::less<>> registeredVersions = std::exchange(_abiVersions, std::move(taken.abiVersions));
        if (!keepRegistered) {
            return;
        }
        for (const auto& [key, handler] : registered) {
            bool heldAll = true;
            for (const auto& [version, entry] : handler.versions) {
                heldAll = claim(key, version, entry) != detail::Claim::Refused && heldAll;
            }
            if (!heldAll || handler.versions.empty()) {
                continue;
            }
            if (std::string& named = _blockTypeHandlers.find(key)->second.alias; named.empty()) {
                named = handler.alias;
            }
            if (const auto recorded = registeredVersions.find(key); recorded != registeredVersions.end()) {
                _abiVersions.insert_or_assign(key, recorded->second);
            }
        }
    }

    std::string typeName(const std::shared_ptr<BlockModel>& block) {
        auto name = block->typeName();
        auto it   = _blockTypeHandlers.find(name);
        if (it != _blockTypeHandlers.end() && !it->second.alias.empty()) {
            return it->second.alias;
        }
        return std::string(name);
    }

    void merge(this_t& anotherRegistry) {
        if (this == std::addressof(anotherRegistry)) {
            return;
        }

        // each entry of the other registry is a later registration of its key and version
        for (const auto& [key, handler] : anotherRegistry._blockTypeHandlers) {
            bool heldAll = true;
            for (const auto& [version, entry] : handler.versions) {
                heldAll = claim(key, version, entry) != detail::Claim::Refused && heldAll;
            }
            if (heldAll && !handler.versions.empty()) {
                if (std::string& named = _blockTypeHandlers.find(key)->second.alias; named.empty()) {
                    named = handler.alias;
                }
            }
        }
        _abiVersions.insert(anotherRegistry._abiVersions.cbegin(), anotherRegistry._abiVersions.cend());
        _vocabulary.merge(anotherRegistry._vocabulary);
    }
};

class BlockRegistry : public GeneralRegistry<BlockModel, BlockWrapper> {
    friend BlockRegistry& globalBlockRegistry(std::source_location location);
};

class SchedulerRegistry : public GeneralRegistry<SchedulerModel, SchedulerWrapper> {
    friend SchedulerRegistry& globalSchedulerRegistry(std::source_location location);
};

/**
 * @brief The entry `registerBlock<TBlock, OverrideName>()` would produce, for a factory the caller
 * owns.
 *
 * This is what a generated definition unit exports: it derives the key and the alias through the
 * same `meta::type_name<TBlock>()` and `makeRegistryAlias()` the `insert<TBlock>()` path uses, so a
 * registration through `insertBlockFactory()` is indistinguishable from the typed one.
 */
template<typename TBlock, meta::fixed_string OverrideName = "">
[[nodiscard]] BlockRegistration makeBlockRegistration(BlockFactory factory) {
    using namespace vir::literals;
    constexpr auto name     = refl::class_name<TBlock>;
    constexpr auto longname = refl::type_name<TBlock>;
    if constexpr (OverrideName != "") {
        return {gr::meta::type_name<TBlock>(), makeRegistryAlias(OverrideName, {}), factory, block::detail::declaredAttributesMap<TBlock>(), block::attributesOf<TBlock>().labels};
    } else if constexpr (name != longname) {
        constexpr auto tmpl = longname.substring(name.size + 1_cw, longname.size - 2_cw - name.size);
        return {gr::meta::type_name<TBlock>(), makeRegistryAlias(name, tmpl), factory, block::detail::declaredAttributesMap<TBlock>(), block::attributesOf<TBlock>().labels};
    } else {
        return {gr::meta::type_name<TBlock>(), makeRegistryAlias(name, {}), factory, block::detail::declaredAttributesMap<TBlock>(), block::attributesOf<TBlock>().labels};
    }
}

GNURADIO_EXPORT
SchedulerRegistry& globalSchedulerRegistry(std::source_location location = std::source_location::current());

} // namespace gr

extern "C" {
GNURADIO_EXPORT
gr::BlockRegistry* grGlobalBlockRegistry(std::source_location location = std::source_location::current());

GNURADIO_EXPORT
gr::SchedulerRegistry* grGlobalSchedulerRegistry(std::source_location location = std::source_location::current());
}

#endif // GNURADIO_BLOCK_REGISTRY_HPP
