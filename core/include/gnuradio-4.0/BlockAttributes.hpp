#ifndef GNURADIO_BLOCK_ATTRIBUTES_HPP
#define GNURADIO_BLOCK_ATTRIBUTES_HPP

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <gnuradio-4.0/BlockTraits.hpp>
#include <gnuradio-4.0/Tag.hpp>

/**
 * @brief What a block type represents, declared once on the type and read without building an instance.
 *
 * A block declares `static constexpr gr::block::Attributes attributes{...}` with designated initializers and names
 * only the fields it has something to say about. Each enumeration left at its default reads as unknown, a value
 * distinct from `None`, and so does an empty `family`; `version` left at its default reads 1 and `status` no flag.
 * The framework keeps, carries and prints the attributes and never acts on one.
 *
 * Outside C++ the attributes travel as a `property_map`: the key is the attribute's name, the value a lower-case
 * word, an integer or a list of words, and an unknown value has no key. `attributesToMap()` and
 * `attributesFromMap()` hold the only spelling of each word.
 */
namespace gr::block {

enum class Resource : std::uint8_t { Unknown = 0, None, Device, File, Network };
enum class Emits : std::uint8_t { Unknown = 0, None, Rf, Audio };
enum class Compute : std::uint8_t { Unknown = 0, Host, Gpu, Tpu, Fpga, Remote };
enum class Role : std::uint8_t { Unknown = 0, Source, Sink, Transceiver }; ///< derived by roleOf(), never declared

/// independent flags, which a block may set together
struct Status {
    bool deprecated   = false; ///< kept for existing graphs, superseded by something else
    bool experimental = false; ///< the interface or the numerics may still change

    [[nodiscard]] constexpr bool any() const noexcept { return deprecated || experimental; }

    [[nodiscard]] constexpr bool operator==(const Status&) const noexcept = default;
};

/// A block's revision number. A block that declares none is version 1, so a second revision is `2U` and is newer by
/// the ordinary comparison.
using Version = std::uint32_t;

inline constexpr Version kDefaultVersion = 1U;

struct Attributes {
    Resource         resource = Resource::Unknown; ///< what the block holds outside the process
    std::string_view family{};                     ///< the word a device selector names the family by; empty is unknown
    Emits            emits   = Emits::Unknown;     ///< what the block puts into the world outside the process
    Compute          compute = Compute::Unknown;   ///< where the processing the block stands for runs
    Status           status{};
    Version          version = kDefaultVersion;

    [[nodiscard]] constexpr bool operator==(const Attributes&) const noexcept = default;
};

template<typename TBlock>
concept HasDeclaredAttributes = requires {
    { TBlock::attributes } -> std::convertible_to<Attributes>;
};

/// the attributes `TBlock` declares, or every field at its default when it declares none
template<typename TBlock>
[[nodiscard]] constexpr Attributes attributesOf() noexcept {
    if constexpr (HasDeclaredAttributes<TBlock>) {
        return TBlock::attributes;
    } else {
        return {};
    }
}

/**
 * @brief Which way a block that holds a resource faces, from whether it has stream inputs and stream outputs.
 *
 * Only a block whose `resource` is neither `Unknown` nor `None` has a role: stream outputs alone make a `Source`,
 * stream inputs alone a `Sink`, and both a `Transceiver`. The gate on the resource keeps a processing block, which has
 * stream inputs and outputs too, from reading as a transceiver.
 */
[[nodiscard]] constexpr Role roleFrom(Resource resource, bool hasStreamInputs, bool hasStreamOutputs) noexcept {
    if (resource == Resource::Unknown || resource == Resource::None) {
        return Role::Unknown;
    }
    if (hasStreamInputs && hasStreamOutputs) {
        return Role::Transceiver;
    }
    if (hasStreamOutputs) {
        return Role::Source;
    }
    return hasStreamInputs ? Role::Sink : Role::Unknown;
}

/// `roleFrom()` for the stream ports `TBlock` declares: a dynamic port collection counts as one port, and message
/// ports count for nothing
template<typename TBlock>
[[nodiscard]] constexpr Role roleOf() noexcept {
    if constexpr (!PortReflectable<TBlock>) {
        return Role::Unknown;
    } else {
        return roleFrom(attributesOf<TBlock>().resource, traits::block::stream_input_ports<TBlock>::size() > 0UZ, traits::block::stream_output_ports<TBlock>::size() > 0UZ);
    }
}

/// the meta_information key an instance of a type that declares attributes carries them under
inline constexpr std::string_view kAttributesMetaKey = "Attributes";

namespace detail {

inline constexpr std::string_view kResourceKey = "resource";
inline constexpr std::string_view kFamilyKey   = "family";
inline constexpr std::string_view kEmitsKey    = "emits";
inline constexpr std::string_view kComputeKey  = "compute";
inline constexpr std::string_view kRoleKey     = "role";
inline constexpr std::string_view kStatusKey   = "status";
inline constexpr std::string_view kVersionKey  = "version";

// indexed by the enumerator's value; the empty word at index 0 stands for `Unknown`, which writes no key
inline constexpr std::array<std::string_view, 5UZ> kResourceWords{"", "none", "device", "file", "network"};
inline constexpr std::array<std::string_view, 4UZ> kEmitsWords{"", "none", "rf", "audio"};
inline constexpr std::array<std::string_view, 6UZ> kComputeWords{"", "host", "gpu", "tpu", "fpga", "remote"};
inline constexpr std::array<std::string_view, 4UZ> kRoleWords{"", "source", "sink", "transceiver"};
inline constexpr std::string_view                  kDeprecatedWord   = "deprecated";
inline constexpr std::string_view                  kExperimentalWord = "experimental";

static_assert(kResourceWords.size() == std::to_underlying(Resource::Network) + 1UZ, "one word per Resource enumerator");
static_assert(kEmitsWords.size() == std::to_underlying(Emits::Audio) + 1UZ, "one word per Emits enumerator");
static_assert(kComputeWords.size() == std::to_underlying(Compute::Remote) + 1UZ, "one word per Compute enumerator");
static_assert(kRoleWords.size() == std::to_underlying(Role::Transceiver) + 1UZ, "one word per Role enumerator");

template<typename TEnum, std::size_t kSize>
[[nodiscard]] constexpr std::string_view wordOf(TEnum value, const std::array<std::string_view, kSize>& words) noexcept {
    const auto index = static_cast<std::size_t>(std::to_underlying(value));
    return index < kSize ? words[index] : std::string_view{};
}

template<typename TEnum, std::size_t kSize>
[[nodiscard]] constexpr TEnum enumeratorOf(std::string_view word, const std::array<std::string_view, kSize>& words) noexcept {
    for (std::size_t index = 1UZ; index < kSize; ++index) {
        if (words[index] == word) {
            return static_cast<TEnum>(index);
        }
    }
    return TEnum::Unknown;
}

/// a non-negative integer of any width that fits a `Version`, else nothing
[[nodiscard]] inline std::optional<Version> versionFrom(const pmt::Value& value) noexcept {
    std::optional<Version> version;
    const auto             readAs = [&value, &version]<typename TInteger>(std::type_identity<TInteger>) {
        if (const TInteger* number = value.get_if<TInteger>(); number != nullptr && std::in_range<Version>(*number)) {
            version = static_cast<Version>(*number);
        }
    };
    readAs(std::type_identity<std::int8_t>{});
    readAs(std::type_identity<std::int16_t>{});
    readAs(std::type_identity<std::int32_t>{});
    readAs(std::type_identity<std::int64_t>{});
    readAs(std::type_identity<std::uint8_t>{});
    readAs(std::type_identity<std::uint16_t>{});
    readAs(std::type_identity<std::uint32_t>{});
    readAs(std::type_identity<std::uint64_t>{});
    return version;
}

template<std::size_t kSize>
[[nodiscard]] std::string oneOf(const std::array<std::string_view, kSize>& words) {
    std::string text = "one of";
    for (std::size_t index = 1UZ; index < kSize; ++index) {
        text += std::format("{} {}", index == 1UZ ? "" : ",", words[index]);
    }
    return text;
}

[[nodiscard]] inline std::string rejection(std::string_view key, const pmt::Value& given, std::string_view allowed) {
    if (given.is_string()) {
        return std::format("{}: '{}' is not {}", key, given.value_or(std::string_view{}), allowed);
    }
    return std::format("{}: {} is not {}", key, given, allowed);
}

} // namespace detail

/**
 * @brief The map form of `attributes`, with the derived `role` beside them.
 *
 * Each of `resource`, `family`, `emits`, `compute` and `role` has a key only when its value is known. `status` is the
 * list of the flags that are set and has no key when none is. `version` is always written, as an integer.
 */
[[nodiscard]] inline property_map attributesToMap(const Attributes& attributes, Role role) {
    property_map map;
    const auto   writeWord = [&map](std::string_view key, std::string_view word) {
        if (!word.empty()) {
            map.insert_or_assign(std::pmr::string(key), pmt::Value(word));
        }
    };
    writeWord(detail::kResourceKey, detail::wordOf(attributes.resource, detail::kResourceWords));
    writeWord(detail::kFamilyKey, attributes.family);
    writeWord(detail::kEmitsKey, detail::wordOf(attributes.emits, detail::kEmitsWords));
    writeWord(detail::kComputeKey, detail::wordOf(attributes.compute, detail::kComputeWords));
    writeWord(detail::kRoleKey, detail::wordOf(role, detail::kRoleWords));

    std::vector<std::string> flags;
    if (attributes.status.deprecated) {
        flags.emplace_back(detail::kDeprecatedWord);
    }
    if (attributes.status.experimental) {
        flags.emplace_back(detail::kExperimentalWord);
    }
    if (!flags.empty()) {
        map.insert_or_assign(std::pmr::string(detail::kStatusKey), pmt::Value(std::move(flags)));
    }

    map.insert_or_assign(std::pmr::string(detail::kVersionKey), pmt::Value(attributes.version));
    return map;
}

/**
 * @brief The attributes a map written by `attributesToMap()` or declared in a file states.
 *
 * An absent key reads as its default: unknown for each enumeration and for `family`, 1 for `version`, no flag for
 * `status`. A key whose value is of the wrong type or is a word outside the attribute's set reads the same, and adds
 * one line to `rejected` naming the key, the value given and the values allowed. A status list keeps the flags it
 * names and is rejected once for any word outside the set. The reader skips `role`, which a block never declares.
 * The returned `family` views the string held in `map` and is valid while `map` is alive and unchanged.
 */
[[nodiscard]] inline Attributes attributesFromMap(const property_map& map, std::vector<std::string>& rejected) {
    const auto readWord = [&map, &rejected]<typename TEnum, std::size_t kSize>(std::string_view key, const std::array<std::string_view, kSize>& words, std::type_identity<TEnum>) {
        const auto it = map.find(key);
        if (it == map.cend()) {
            return TEnum::Unknown;
        }
        const TEnum value = detail::enumeratorOf<TEnum>(it->second.value_or(std::string_view{}), words);
        if (value == TEnum::Unknown) {
            rejected.push_back(detail::rejection(key, it->second, detail::oneOf(words)));
        }
        return value;
    };

    Attributes attributes{
        .resource = readWord(detail::kResourceKey, detail::kResourceWords, std::type_identity<Resource>{}),
        .emits    = readWord(detail::kEmitsKey, detail::kEmitsWords, std::type_identity<Emits>{}),
        .compute  = readWord(detail::kComputeKey, detail::kComputeWords, std::type_identity<Compute>{}),
    };

    if (const auto it = map.find(detail::kFamilyKey); it != map.cend()) {
        attributes.family = it->second.value_or(std::string_view{});
        if (!it->second.is_string()) {
            rejected.push_back(detail::rejection(detail::kFamilyKey, it->second, "a word"));
        }
    }

    if (const auto it = map.find(detail::kStatusKey); it != map.cend()) {
        const auto* words    = it->second.get_if<Tensor<pmt::Value>>();
        bool        allKnown = words != nullptr;
        if (words != nullptr) {
            for (const pmt::Value& word : *words) {
                const std::string_view flag    = word.value_or(std::string_view{});
                attributes.status.deprecated   = attributes.status.deprecated || flag == detail::kDeprecatedWord;
                attributes.status.experimental = attributes.status.experimental || flag == detail::kExperimentalWord;
                allKnown                       = allKnown && (flag == detail::kDeprecatedWord || flag == detail::kExperimentalWord);
            }
        }
        if (!allKnown) {
            rejected.push_back(detail::rejection(detail::kStatusKey, it->second, std::format("a list of {} and {}", detail::kDeprecatedWord, detail::kExperimentalWord)));
        }
    }

    if (const auto it = map.find(detail::kVersionKey); it != map.cend()) {
        const std::optional<Version> version = detail::versionFrom(it->second);
        attributes.version                   = version.value_or(kDefaultVersion);
        if (!version.has_value()) {
            rejected.push_back(detail::rejection(detail::kVersionKey, it->second, "a non-negative integer"));
        }
    }
    return attributes;
}

/// `attributesFromMap(map, rejected)` with the rejected lines discarded
[[nodiscard]] inline Attributes attributesFromMap(const property_map& map) {
    std::vector<std::string> discarded;
    return attributesFromMap(map, discarded);
}

namespace detail {

/// the map a registration and an instance of `TBlock` carry: empty when the type declares no attributes
template<typename TBlock>
[[nodiscard]] property_map declaredAttributesMap() {
    if constexpr (HasDeclaredAttributes<TBlock>) {
        return attributesToMap(attributesOf<TBlock>(), roleOf<TBlock>());
    } else {
        return {};
    }
}

} // namespace detail

} // namespace gr::block

#endif // GNURADIO_BLOCK_ATTRIBUTES_HPP
