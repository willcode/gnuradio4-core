#ifndef GNURADIO_BLOCK_ATTRIBUTES_HPP
#define GNURADIO_BLOCK_ATTRIBUTES_HPP

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <gnuradio-4.0/BlockTraits.hpp>
#include <gnuradio-4.0/Tag.hpp>

/**
 * @brief What a block type represents, declared once on the type as labels and read without building an instance.
 *
 * A label is a class and a word, written `class/word`. The eight classes are fixed here. A word is a `Label` constant
 * that its declarer defines with its class, a one-sentence meaning and, for a medium, whether it is physical; core's
 * words are the constants in `gr::block::labels`. A block declares its labels and its version once:
 *
 *     static constexpr auto attributes = gr::block::describe(2U, kFamily, gr::block::labels::holds::device);
 *
 * A registry collects the labels of every block it registers into a `Vocabulary`, so a listing prints the meaning of
 * every word it meets. The framework keeps, carries and prints the labels and never acts on one.
 *
 * Outside C++ the attributes travel as a `property_map`: `version` as an integer, `labels` as a list of `class/word`
 * strings in class order, and `role` as the role word the stream ports read. `kLabelClassNames` and `isWord()` hold
 * the spelling of the classes and the word grammar.
 */
namespace gr::block {

enum class LabelClass : std::uint8_t { Family, Role, Plane, Holds, Emits, Ingests, Compute, Status };

/// the class names, indexed by the enumerator's value, in the order a map lists labels
inline constexpr std::array<std::string_view, 8UZ> kLabelClassNames{"family", "role", "plane", "holds", "emits", "ingests", "compute", "status"};

[[nodiscard]] constexpr std::string_view className(LabelClass cls) noexcept {
    const auto index = static_cast<std::size_t>(std::to_underlying(cls));
    return index < kLabelClassNames.size() ? kLabelClassNames[index] : std::string_view{};
}

[[nodiscard]] constexpr std::optional<LabelClass> classNamed(std::string_view name) noexcept {
    for (std::size_t index = 0UZ; index < kLabelClassNames.size(); ++index) {
        if (kLabelClassNames[index] == name) {
            return static_cast<LabelClass>(index);
        }
    }
    return std::nullopt;
}

/// whether a block carries at most one word of `cls`: `family`, `role` and `compute` take one, the others many
[[nodiscard]] constexpr bool takesOneWord(LabelClass cls) noexcept { return cls == LabelClass::Family || cls == LabelClass::Role || cls == LabelClass::Compute; }

/// whether `word` is a lower-case letter followed by lower-case letters and digits, the grammar of every word
[[nodiscard]] constexpr bool isWord(std::string_view word) noexcept {
    if (word.empty() || word.front() < 'a' || word.front() > 'z') {
        return false;
    }
    return std::ranges::all_of(word, [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); });
}

/// one word of one class, with the sentence a listing prints for it
struct Label {
    LabelClass       cls{};
    std::string_view word{};
    std::string_view meaning{};
    bool             physical = false; ///< a medium of matter or energy in the world outside the host; set on emits and ingests words

    /// two labels are the same label when their class and word agree, whatever their meanings
    [[nodiscard]] constexpr bool operator==(const Label& other) const noexcept { return cls == other.cls && word == other.word; }

    [[nodiscard]] std::string text() const { return std::format("{}/{}", className(cls), word); }
};

/// A block's revision number. A block that declares none is version 1, so a second revision is `2U` and is newer by
/// the ordinary comparison.
using Version = std::uint32_t;

inline constexpr Version kDefaultVersion = 1U;

/// what a block type declares: its version and its labels
struct Attributes {
    Version                version = kDefaultVersion;
    std::span<const Label> labels{};
};

/// the most labels one declaration holds
inline constexpr std::size_t kMaxLabels = 16UZ;

/// The storage `describe()` fills. A block keeps it as its `attributes` member, which gives the storage static
/// duration, so the `Attributes` it converts to stays valid for the life of the program.
struct Declaration {
    Version                       version = kDefaultVersion;
    std::array<Label, kMaxLabels> storage{};
    std::size_t                   count = 0UZ;

    [[nodiscard]] constexpr operator Attributes() const noexcept { return {version, std::span<const Label>(storage.data(), count)}; }
};

/**
 * @brief The declaration of `version` and `labels`, the labels in class order and each class in the order given.
 *
 * The call does not compile for more than `kMaxLabels` labels, for a label given twice, for a second word in a class
 * that takes one and for a word outside the grammar of `isWord()`.
 */
template<std::same_as<Label>... TLabels>
requires(sizeof...(TLabels) <= kMaxLabels)
[[nodiscard]] consteval Declaration describe(Version version, TLabels... declared) {
    Declaration                                 declaration{.version = version};
    const std::array<Label, sizeof...(TLabels)> given{declared...};
    for (std::size_t cls = 0UZ; cls < kLabelClassNames.size(); ++cls) {
        for (const Label& label : given) {
            if (static_cast<std::size_t>(std::to_underlying(label.cls)) != cls) {
                continue;
            }
            if (!isWord(label.word)) {
                throw "a word is a lower-case letter, then lower-case letters and digits";
            }
            for (std::size_t index = 0UZ; index < declaration.count; ++index) {
                if (declaration.storage[index] == label) {
                    throw "a label is given twice";
                }
                if (declaration.storage[index].cls == label.cls && takesOneWord(label.cls)) {
                    throw "a second word is given in a class that takes one";
                }
            }
            declaration.storage[declaration.count++] = label;
        }
    }
    return declaration;
}

/// Core's words, one namespace per class. A medium word exists once in `emits` and once in `ingests`, with one meaning.
namespace labels {

/// A family word names a library; a device selector finds the library's blocks by it. `family` is the one class whose
/// words core does not define.
[[nodiscard]] constexpr Label family(std::string_view word, std::string_view meaning) noexcept { return {LabelClass::Family, word, meaning, false}; }

namespace role {
inline constexpr Label source{LabelClass::Role, "source", "Brings signals from the world into the graph across a boundary the block owns."};
inline constexpr Label sink{LabelClass::Role, "sink", "Carries signals from the graph into the world across a boundary the block owns."};
inline constexpr Label transceiver{LabelClass::Role, "transceiver", "Carries signals both ways across a boundary the block owns."};
inline constexpr Label generator{LabelClass::Role, "generator", "Produces stream data inside the graph from its settings alone."};
inline constexpr Label processor{LabelClass::Role, "processor", "Turns stream input into stream output inside the graph."};
inline constexpr Label consumer{LabelClass::Role, "consumer", "Takes stream input and ends it inside the graph."};
inline constexpr Label notation{LabelClass::Role, "notation", "Appears in a drawing of the graph and never runs."};
} // namespace role

namespace plane {
inline constexpr Label control{LabelClass::Plane, "control", "Acts or reports through messages and settings."};
inline constexpr Label notation{LabelClass::Plane, "notation", "Is drawn and documented, and never runs."};
} // namespace plane

namespace holds {
inline constexpr Label device{LabelClass::Holds, "device", "Opens a hardware unit attached to the host."};
inline constexpr Label storage{LabelClass::Holds, "storage", "Opens a file, a disk or a database."};
inline constexpr Label network{LabelClass::Holds, "network", "Opens a socket or a connection to another host."};
inline constexpr Label ipc{LabelClass::Holds, "ipc", "Opens a channel to another process on the same host: shared memory, a pipe, a local socket."};
} // namespace holds

namespace detail {
inline constexpr std::string_view kRf         = "Radio-frequency energy, radiated or conducted.";
inline constexpr std::string_view kSound      = "Pressure waves in air, water or a solid, audible or not.";
inline constexpr std::string_view kLight      = "Optical energy, infrared to ultraviolet.";
inline constexpr std::string_view kMotion     = "The movement or position of a physical body: a motor, a mount, a position fix.";
inline constexpr std::string_view kElectrical = "A voltage or a current on a line outside radio frequencies: a keying line, a relay, a GPIO pin.";
inline constexpr std::string_view kAmbient    = "A condition of the surroundings: temperature, humidity, pressure, a radiation dose.";
inline constexpr std::string_view kTime       = "A time or frequency reference: a pulse per second, a 10 MHz reference, a clock fix.";
inline constexpr std::string_view kStorage    = "Data at rest in a file, a disk or a database.";
inline constexpr std::string_view kNetwork    = "Data carried to or from another host.";
inline constexpr std::string_view kIpc        = "Data carried to or from another process on the same host.";
inline constexpr std::string_view kGraphical  = "A picture on a display.";
inline constexpr std::string_view kText       = "Characters for a person or a log.";
} // namespace detail

namespace emits {
inline constexpr Label rf{LabelClass::Emits, "rf", detail::kRf, true};
inline constexpr Label sound{LabelClass::Emits, "sound", detail::kSound, true};
inline constexpr Label light{LabelClass::Emits, "light", detail::kLight, true};
inline constexpr Label motion{LabelClass::Emits, "motion", detail::kMotion, true};
inline constexpr Label electrical{LabelClass::Emits, "electrical", detail::kElectrical, true};
inline constexpr Label ambient{LabelClass::Emits, "ambient", detail::kAmbient, true};
inline constexpr Label time{LabelClass::Emits, "time", detail::kTime, true};
inline constexpr Label storage{LabelClass::Emits, "storage", detail::kStorage};
inline constexpr Label network{LabelClass::Emits, "network", detail::kNetwork};
inline constexpr Label ipc{LabelClass::Emits, "ipc", detail::kIpc};
inline constexpr Label graphical{LabelClass::Emits, "graphical", detail::kGraphical};
inline constexpr Label text{LabelClass::Emits, "text", detail::kText};
} // namespace emits

namespace ingests {
inline constexpr Label rf{LabelClass::Ingests, "rf", detail::kRf, true};
inline constexpr Label sound{LabelClass::Ingests, "sound", detail::kSound, true};
inline constexpr Label light{LabelClass::Ingests, "light", detail::kLight, true};
inline constexpr Label motion{LabelClass::Ingests, "motion", detail::kMotion, true};
inline constexpr Label electrical{LabelClass::Ingests, "electrical", detail::kElectrical, true};
inline constexpr Label ambient{LabelClass::Ingests, "ambient", detail::kAmbient, true};
inline constexpr Label time{LabelClass::Ingests, "time", detail::kTime, true};
inline constexpr Label storage{LabelClass::Ingests, "storage", detail::kStorage};
inline constexpr Label network{LabelClass::Ingests, "network", detail::kNetwork};
inline constexpr Label ipc{LabelClass::Ingests, "ipc", detail::kIpc};
inline constexpr Label graphical{LabelClass::Ingests, "graphical", detail::kGraphical};
inline constexpr Label text{LabelClass::Ingests, "text", detail::kText};
} // namespace ingests

namespace compute {
inline constexpr Label cpu{LabelClass::Compute, "cpu", "Runs its processing on the host's CPU."};
inline constexpr Label gpu{LabelClass::Compute, "gpu", "Stands for processing on a graphics processor."};
inline constexpr Label tpu{LabelClass::Compute, "tpu", "Stands for processing on a tensor processor."};
inline constexpr Label fpga{LabelClass::Compute, "fpga", "Stands for processing in programmable logic, such as a radio's FPGA image."};
inline constexpr Label remote{LabelClass::Compute, "remote", "Stands for processing on another host's CPU."};
} // namespace compute

namespace status {
inline constexpr Label deprecated{LabelClass::Status, "deprecated", "Serves existing graphs; another block serves new ones."};
inline constexpr Label experimental{LabelClass::Status, "experimental", "Its interface or its numerics may still change."};
} // namespace status

/// every word core defines, the seed of every vocabulary
inline constexpr std::array kCore{role::source, role::sink, role::transceiver, role::generator, role::processor, role::consumer, role::notation,                                                             //
    plane::control, plane::notation,                                                                                                                                                                         //
    holds::device, holds::storage, holds::network, holds::ipc,                                                                                                                                               //
    emits::rf, emits::sound, emits::light, emits::motion, emits::electrical, emits::ambient, emits::time, emits::storage, emits::network, emits::ipc, emits::graphical, emits::text,                         //
    ingests::rf, ingests::sound, ingests::light, ingests::motion, ingests::electrical, ingests::ambient, ingests::time, ingests::storage, ingests::network, ingests::ipc, ingests::graphical, ingests::text, //
    compute::cpu, compute::gpu, compute::tpu, compute::fpga, compute::remote,                                                                                                                                //
    status::deprecated, status::experimental};

} // namespace labels

template<typename TBlock>
concept HasDeclaredAttributes = requires {
    { TBlock::attributes } -> std::convertible_to<Attributes>;
};

/// the attributes `TBlock` declares, or version 1 and no label when it declares none
template<typename TBlock>
[[nodiscard]] constexpr Attributes attributesOf() noexcept {
    if constexpr (HasDeclaredAttributes<TBlock>) {
        return TBlock::attributes;
    } else {
        return {};
    }
}

/**
 * @brief The role a block's stream ports read.
 *
 * Stream outputs alone read `generator`, stream inputs alone `consumer` and both `processor`. A block with no stream
 * port reads `notation` when it is `plane/notation` and no role otherwise. No port shape tells a source, a sink or a
 * transceiver from a processing block, so the ports never read one of the three.
 */
[[nodiscard]] constexpr std::optional<Label> roleFromPorts(bool hasStreamInputs, bool hasStreamOutputs, bool isNotation) noexcept {
    if (hasStreamInputs && hasStreamOutputs) {
        return labels::role::processor;
    }
    if (hasStreamOutputs) {
        return labels::role::generator;
    }
    if (hasStreamInputs) {
        return labels::role::consumer;
    }
    return isNotation ? std::optional<Label>{labels::role::notation} : std::nullopt;
}

/// `roleFromPorts()` for the stream ports `TBlock` declares: a dynamic port collection counts as one port, and message
/// ports count for nothing
template<typename TBlock>
[[nodiscard]] constexpr std::optional<Label> portRoleOf() noexcept {
    if constexpr (!PortReflectable<TBlock>) {
        return std::nullopt;
    } else {
        const bool isNotation = std::ranges::contains(attributesOf<TBlock>().labels, labels::plane::notation);
        return roleFromPorts(traits::block::stream_input_ports<TBlock>::size() > 0UZ, traits::block::stream_output_ports<TBlock>::size() > 0UZ, isNotation);
    }
}

/// the role `TBlock` declares, else the role its stream ports read
template<typename TBlock>
[[nodiscard]] constexpr std::optional<Label> roleOf() noexcept {
    for (const Label& label : attributesOf<TBlock>().labels) {
        if (label.cls == LabelClass::Role) {
            return label;
        }
    }
    return portRoleOf<TBlock>();
}

/// one word of a vocabulary, with every meaning its declarers gave it
struct VocabularyEntry {
    LabelClass               cls{};
    std::string              word;
    std::vector<std::string> meanings; ///< one per distinct meaning, in the order they were added
    bool                     physical = false;

    [[nodiscard]] std::string text() const { return std::format("{}/{}", className(cls), word); }

    [[nodiscard]] bool operator==(const VocabularyEntry&) const = default;
};

/**
 * @brief The words a set of declarations brings, one entry per class and word.
 *
 * A word added again with another meaning keeps both meanings, and a word physical in any declaration is physical.
 * Nothing is refused.
 */
class Vocabulary {
    std::vector<VocabularyEntry> _entries; // ordered by class, then by word

    [[nodiscard]] auto position(LabelClass cls, std::string_view word) const noexcept {
        return std::ranges::lower_bound(_entries, std::pair(cls, word), std::less{}, [](const VocabularyEntry& entry) { return std::pair(entry.cls, std::string_view(entry.word)); });
    }

public:
    void add(LabelClass cls, std::string_view word, std::string_view meaning, bool physical) {
        auto it = position(cls, word);
        if (it == _entries.cend() || it->cls != cls || it->word != word) {
            it = _entries.insert(it, VocabularyEntry{.cls = cls, .word = std::string(word), .meanings = {}, .physical = false});
        }
        VocabularyEntry& entry = _entries[static_cast<std::size_t>(std::distance(_entries.cbegin(), it))];
        if (!meaning.empty() && !std::ranges::contains(entry.meanings, meaning)) {
            entry.meanings.emplace_back(meaning);
        }
        entry.physical = entry.physical || physical;
    }

    void add(const Label& label) { add(label.cls, label.word, label.meaning, label.physical); }

    void merge(const Vocabulary& other) {
        for (const VocabularyEntry& entry : other._entries) {
            for (const std::string& meaning : entry.meanings) {
                add(entry.cls, entry.word, meaning, entry.physical);
            }
            if (entry.meanings.empty()) {
                add(entry.cls, entry.word, {}, entry.physical);
            }
        }
    }

    [[nodiscard]] const VocabularyEntry* find(LabelClass cls, std::string_view word) const noexcept {
        const auto it = position(cls, word);
        return it != _entries.cend() && it->cls == cls && it->word == word ? std::addressof(*it) : nullptr;
    }

    /// whether the word is physical; true for a word the vocabulary lacks, since nothing tells what it stands for
    [[nodiscard]] bool physical(LabelClass cls, std::string_view word) const noexcept {
        const VocabularyEntry* entry = find(cls, word);
        return entry == nullptr || entry->physical;
    }

    [[nodiscard]] std::vector<const VocabularyEntry*> words(LabelClass cls) const {
        std::vector<const VocabularyEntry*> found;
        for (const VocabularyEntry& entry : _entries) {
            if (entry.cls == cls) {
                found.push_back(std::addressof(entry));
            }
        }
        return found;
    }

    [[nodiscard]] std::span<const VocabularyEntry> entries() const noexcept { return _entries; }

    [[nodiscard]] bool operator==(const Vocabulary&) const = default;
};

/// a vocabulary of core's words alone
[[nodiscard]] inline const Vocabulary& coreVocabulary() {
    static const Vocabulary vocabulary = [] {
        Vocabulary core;
        for (const Label& label : labels::kCore) {
            core.add(label);
        }
        return core;
    }();
    return vocabulary;
}

/// the meta_information key an instance of a type that declares attributes carries them under
inline constexpr std::string_view kAttributesMetaKey = "Attributes";

/// the keys of the map form
inline constexpr std::string_view kVersionKey = "version";
inline constexpr std::string_view kLabelsKey  = "labels";
inline constexpr std::string_view kRoleKey    = "role";

/// a label read from a map: its class and word, and whether the vocabulary it was read against holds the word
struct LabelRead {
    LabelClass  cls{};
    std::string word;
    bool        known = false;

    [[nodiscard]] std::string text() const { return std::format("{}/{}", className(cls), word); }

    [[nodiscard]] bool operator==(const LabelRead&) const = default;
};

/// the attributes a map states, with the words it holds and the role its ports read
struct AttributesRead {
    Version                version = kDefaultVersion;
    std::vector<LabelRead> labels;   ///< in class order, each class in the order the map lists it
    std::string            readRole; ///< the role word the stream ports read; empty when the map carries none

    [[nodiscard]] bool has(LabelClass cls, std::string_view word) const noexcept {
        return std::ranges::any_of(labels, [cls, word](const LabelRead& label) { return label.cls == cls && label.word == word; });
    }

    [[nodiscard]] bool has(const Label& label) const noexcept { return has(label.cls, label.word); }

    [[nodiscard]] std::vector<std::string_view> words(LabelClass cls) const {
        std::vector<std::string_view> found;
        for (const LabelRead& label : labels) {
            if (label.cls == cls) {
                found.emplace_back(label.word);
            }
        }
        return found;
    }

    /// the role word declared, else the role word the ports read; empty when neither is known
    [[nodiscard]] std::string_view role() const noexcept {
        const auto declared = std::ranges::find(labels, LabelClass::Role, &LabelRead::cls);
        return declared != labels.cend() ? std::string_view(declared->word) : std::string_view(readRole);
    }

    [[nodiscard]] bool operator==(const AttributesRead&) const = default;
};

namespace detail {

/// an integer of any width from 0 to the largest `Version`, else nothing
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

[[nodiscard]] inline std::string quoted(const pmt::Value& value) {
    if (value.is_string()) {
        return std::format("'{}'", value.value_or(std::string_view{}));
    }
    return std::format("{}", value);
}

[[nodiscard]] inline std::string classList() {
    std::string text;
    for (std::size_t index = 0UZ; index < kLabelClassNames.size(); ++index) {
        text += std::format("{}{}", index == 0UZ ? "" : ", ", kLabelClassNames[index]);
    }
    return text;
}

} // namespace detail

/**
 * @brief The class and word of `text` when it is a well-formed `class/word`, else the reason it is not.
 *
 * The reason names `text` and the form allowed, as a line a reader prints.
 */
[[nodiscard]] inline std::expected<std::pair<LabelClass, std::string_view>, std::string> parseLabel(std::string_view text) {
    const std::size_t               slash = text.find('/');
    const std::optional<LabelClass> cls   = slash == std::string_view::npos ? std::nullopt : classNamed(text.substr(0UZ, slash));
    if (!cls.has_value()) {
        return std::unexpected(std::format("'{}' is not class/word with a class of {}", text, detail::classList()));
    }
    const std::string_view word = text.substr(slash + 1UZ);
    if (!isWord(word)) {
        return std::unexpected(std::format("'{}' is not class/word with a word of a lower-case letter, then lower-case letters and digits", text));
    }
    return std::pair{*cls, word};
}

/// the map form of `attributes`: `version` always, `labels` when there is one, and `role` for the role the ports read
[[nodiscard]] inline property_map attributesToMap(const Attributes& attributes, std::optional<Label> readRole) {
    property_map map;
    map.insert_or_assign(std::pmr::string(kVersionKey), pmt::Value(attributes.version));
    std::vector<std::string> texts;
    for (std::size_t cls = 0UZ; cls < kLabelClassNames.size(); ++cls) {
        for (const Label& label : attributes.labels) {
            if (static_cast<std::size_t>(std::to_underlying(label.cls)) == cls) {
                texts.push_back(label.text());
            }
        }
    }
    if (!texts.empty()) {
        map.insert_or_assign(std::pmr::string(kLabelsKey), pmt::Value(std::move(texts)));
    }
    if (readRole.has_value()) {
        map.insert_or_assign(std::pmr::string(kRoleKey), pmt::Value(readRole->word));
    }
    return map;
}

/// the map form of attributes read from another map; `attributesFromMap()` reads it back unchanged
[[nodiscard]] inline property_map attributesToMap(const AttributesRead& attributes) {
    property_map map;
    map.insert_or_assign(std::pmr::string(kVersionKey), pmt::Value(attributes.version));
    std::vector<std::string> texts;
    for (const LabelRead& label : attributes.labels) {
        texts.push_back(label.text());
    }
    if (!texts.empty()) {
        map.insert_or_assign(std::pmr::string(kLabelsKey), pmt::Value(std::move(texts)));
    }
    if (!attributes.readRole.empty()) {
        map.insert_or_assign(std::pmr::string(kRoleKey), pmt::Value(attributes.readRole));
    }
    return map;
}

/**
 * @brief The attributes a map written by `attributesToMap()` or declared in a file states.
 *
 * An absent `version` reads 1 and an absent `labels` no label. A well-formed label of a known class is kept whatever
 * word it names, and marked known when `vocabulary` holds the word. The reader skips, with one line in `rejected`
 * naming the entry and the form allowed: an entry that is not a string of the form `class/word`, a class outside the
 * eight, a word outside the grammar of `isWord()`, a label given twice, and a second word in a class that takes one,
 * where the first is kept. A `version` that is not an integer from 0 to 4294967295 reads 1 and adds a line, and so
 * does a `labels` value that is not a list and a `role` value that is not a word.
 */
[[nodiscard]] inline AttributesRead attributesFromMap(const property_map& map, const Vocabulary& vocabulary, std::vector<std::string>& rejected) {
    AttributesRead read;
    if (const auto it = map.find(kVersionKey); it != map.cend()) {
        const std::optional<Version> version = detail::versionFrom(it->second);
        read.version                         = version.value_or(kDefaultVersion);
        if (!version.has_value()) {
            rejected.push_back(std::format("{}: {} is not an integer from 0 to {}", kVersionKey, detail::quoted(it->second), std::numeric_limits<Version>::max()));
        }
    }

    std::vector<LabelRead> given;
    if (const auto it = map.find(kLabelsKey); it != map.cend()) {
        const auto* entries = it->second.get_if<Tensor<pmt::Value>>();
        if (entries == nullptr) {
            rejected.push_back(std::format("{}: {} is not a list of class/word strings", kLabelsKey, detail::quoted(it->second)));
        } else {
            for (const pmt::Value& entry : *entries) {
                if (!entry.is_string()) {
                    rejected.push_back(std::format("{}: {} is not a class/word string", kLabelsKey, detail::quoted(entry)));
                    continue;
                }
                const std::string_view text   = entry.value_or(std::string_view{});
                const auto             parsed = parseLabel(text);
                if (!parsed.has_value()) {
                    rejected.push_back(std::format("{}: {}", kLabelsKey, parsed.error()));
                    continue;
                }
                const LabelClass       cls  = parsed->first;
                const std::string_view word = parsed->second;
                const auto             same = std::ranges::find_if(given, [cls, word](const LabelRead& label) { return label.cls == cls && (label.word == word || takesOneWord(cls)); });
                if (same != given.cend()) {
                    rejected.push_back(same->word == word ? std::format("{}: '{}' is given twice", kLabelsKey, text) : std::format("{}: '{}' is a second word in {}, which takes one; '{}' is kept", kLabelsKey, text, className(cls), same->text()));
                    continue;
                }
                given.push_back(LabelRead{.cls = cls, .word = std::string(word), .known = vocabulary.find(cls, word) != nullptr});
            }
        }
    }
    for (std::size_t cls = 0UZ; cls < kLabelClassNames.size(); ++cls) {
        for (LabelRead& label : given) {
            if (static_cast<std::size_t>(std::to_underlying(label.cls)) == cls) {
                read.labels.push_back(std::move(label));
            }
        }
    }

    if (const auto it = map.find(kRoleKey); it != map.cend()) {
        const std::string_view word = it->second.value_or(std::string_view{});
        if (it->second.is_string() && isWord(word)) {
            read.readRole = word;
        } else {
            rejected.push_back(std::format("{}: {} is not a word", kRoleKey, detail::quoted(it->second)));
        }
    }
    return read;
}

/// `attributesFromMap(map, vocabulary, rejected)` with the rejected lines discarded
[[nodiscard]] inline AttributesRead attributesFromMap(const property_map& map, const Vocabulary& vocabulary = coreVocabulary()) {
    std::vector<std::string> discarded;
    return attributesFromMap(map, vocabulary, discarded);
}

/// The map form of a vocabulary, which crosses the plugin boundary: the key is `class/word`, the value a map of
/// `meaning` (a string, or a list of strings for a word with several) and `physical` (a bool).
[[nodiscard]] inline property_map vocabularyToMap(const Vocabulary& vocabulary) {
    property_map map;
    for (const VocabularyEntry& entry : vocabulary.entries()) {
        property_map value;
        if (entry.meanings.size() == 1UZ) {
            value.insert_or_assign(std::pmr::string("meaning"), pmt::Value(entry.meanings.front()));
        } else if (!entry.meanings.empty()) {
            value.insert_or_assign(std::pmr::string("meaning"), pmt::Value(entry.meanings));
        }
        value.insert_or_assign(std::pmr::string("physical"), pmt::Value(entry.physical));
        map.insert_or_assign(std::pmr::string(entry.text()), pmt::Value(std::move(value)));
    }
    return map;
}

/// the vocabulary `vocabularyToMap()` wrote; a key that is not a well-formed `class/word` or a value that is not a map
/// is skipped with one line in `rejected`
[[nodiscard]] inline Vocabulary vocabularyFromMap(const property_map& map, std::vector<std::string>& rejected) {
    Vocabulary vocabulary;
    for (const auto& [key, value] : map) {
        const auto  parsed = parseLabel(std::string_view(key));
        const auto* entry  = value.get_if<property_map>();
        if (!parsed.has_value()) {
            rejected.push_back(std::format("vocabulary: {}", parsed.error()));
            continue;
        }
        if (entry == nullptr) {
            rejected.push_back(std::format("vocabulary: '{}' carries {}, not a map of meaning and physical", std::string_view(key), detail::quoted(value)));
            continue;
        }
        const auto physicalIt = entry->find("physical");
        const bool physical   = physicalIt != entry->cend() && physicalIt->second.value_or(false);
        const auto meaningIt  = entry->find("meaning");
        if (meaningIt == entry->cend()) {
            vocabulary.add(parsed->first, parsed->second, {}, physical);
        } else if (const auto* meanings = meaningIt->second.get_if<Tensor<pmt::Value>>(); meanings != nullptr) {
            for (const pmt::Value& meaning : *meanings) {
                vocabulary.add(parsed->first, parsed->second, meaning.value_or(std::string_view{}), physical);
            }
        } else {
            vocabulary.add(parsed->first, parsed->second, meaningIt->second.value_or(std::string_view{}), physical);
        }
    }
    return vocabulary;
}

namespace detail {

/// the map a registration and an instance of `TBlock` carry: empty when the type declares no attributes
template<typename TBlock>
[[nodiscard]] property_map declaredAttributesMap() {
    if constexpr (HasDeclaredAttributes<TBlock>) {
        return attributesToMap(attributesOf<TBlock>(), portRoleOf<TBlock>());
    } else {
        return {};
    }
}

} // namespace detail

} // namespace gr::block

#endif // GNURADIO_BLOCK_ATTRIBUTES_HPP
