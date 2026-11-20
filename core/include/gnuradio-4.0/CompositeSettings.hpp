#ifndef GNURADIO_COMPOSITE_SETTINGS_HPP
#define GNURADIO_COMPOSITE_SETTINGS_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gnuradio-4.0/RecipeParameters.hpp>
#include <gnuradio-4.0/Settings.hpp>

namespace gr::settings::detail {

// an inner block where an input of the composite enters, with the inner blocks a stream from that input reaches
struct StreamEntry {
    SettingsBase*              entry = nullptr;
    std::vector<SettingsBase*> reached;
};

// the prefix of every inner tag key of the composite of that unique name
[[nodiscard]] std::string innerTagKeyPrefix(std::string_view compositeName);

// the inner block a binding's name path names: its settings and its name
struct InnerTarget {
    SettingsBase* settings = nullptr;
    std::string   blockName;
};

// finds the inner block of a binding's name path in the composite's graph as it stands, or says why it cannot
using InnerResolver = std::function<std::expected<InnerTarget, std::string>(std::span<const std::string> namePath)>;

// the settings one inner block receives from a call, and the parameters of the call behind each of its keys
struct InnerChanges {
    SettingsBase*                                                settings = nullptr;
    std::string                                                  blockName;
    property_map                                                 changes;
    std::map<std::string, std::vector<std::string>, std::less<>> parametersOf;
};

/**
 * @brief The parameters of a recipe composite: their declarations, the inner settings each derives, the values as
 * stored, and the record of the values a call gave.
 *
 * A call on the composite's settings that names a parameter passes five steps. Each parameter converts to its declared
 * type; each binding that reads a parameter of the call evaluates over the stored parameters with the call's in place;
 * each inner block checks the settings it receives. Under the composite's lock the own keys merge, each inner block
 * merges its settings, and the parameters are stored. A refusal before the merges changes nothing. The composite's
 * own keys apply at the call, since no work call reaches a composite.
 */
struct CompositeSettings {
    using Clock = std::chrono::steady_clock;

    CompositeSettings(std::vector<recipe::ParameterDeclaration> declarations, std::vector<recipe::Binding> bindings, std::vector<pmt::Value> values, const property_map& given, InnerResolver resolve);
    CompositeSettings(const CompositeSettings&)            = delete;
    CompositeSettings& operator=(const CompositeSettings&) = delete;

    [[nodiscard]] bool                     declares(std::string_view name) const noexcept;
    [[nodiscard]] bool                     namesParameter(const property_map& changes) const noexcept;
    [[nodiscard]] std::vector<std::string> parameterNames() const;
    [[nodiscard]] bool                     isGiven(std::string_view name) const;
    [[nodiscard]] property_map             record() const;                                         // each parameter a call gave, at the value it gave
    void                                   couple(std::string_view parameter, std::string tagKey); // an empty key removes the coupling
    [[nodiscard]] property_map             couplings() const;                                      // each coupled parameter's tag key

    // a call that names a parameter: a set waits for each inner block as `set` does; a check stores nothing
    [[nodiscard]] std::expected<Values, Rejected> set(SettingsBase& own, const property_map& changes, Timeout timeout);
    [[nodiscard]] std::expected<void, Rejected>   check(SettingsBase& own, const property_map& changes);
    // each parameter asked, or every parameter when none is asked, at its value as stored; the rest of the keys asked
    [[nodiscard]] Values readParameters(std::span<const std::string> keys, std::vector<std::string>& otherKeys) const;

    // the stream paths through the composite named `compositeName`, set again when a port is exported or withdrawn:
    // where each input enters and what its stream reaches. The marks of an earlier call leave `innerBlocks`, every
    // inner block's settings
    void setStreamPaths(std::string_view compositeName, std::vector<StreamEntry> entries, std::span<SettingsBase* const> innerBlocks);
    // a tag that enters at `entry`: each parameter whose tag key it carries at a new value, through the five steps. An
    // inner block the stream reaches takes the inner tag keys returned; every other inner block merges at the call
    [[nodiscard]] property_map translateTag(const SettingsBase& entry, const property_map& tagMap);

private:
    struct Prepared {
        std::vector<pmt::Value>           trial;
        std::vector<std::string>          parameters; // the parameters of the call
        std::vector<detail::InnerChanges> inners;
        std::uint64_t                     versionNoted = 0U;
    };
    struct Committed {
        std::optional<detail::Merged> own;
        struct Inner {
            detail::InnerChanges                           inner;
            std::expected<detail::Merged, Rejected>        merged;
            std::optional<std::expected<Values, Rejected>> answer{}; // the finish's answer, or the commit's refusal
        };
        std::vector<Inner> inners;
    };
    struct Checked {
        Prepared                           prepared;
        std::optional<detail::CheckedCall> own;
    };

    std::vector<recipe::ParameterDeclaration> _declarations;
    std::vector<recipe::Binding>              _bindings;
    InnerResolver                             _resolve; // each prepare finds the blocks it sets in the graph as it stands

    std::string              _tagKeyPrefix; // of every inner tag key of this composite
    std::vector<StreamEntry> _entries;      // under the composite lock

    mutable std::mutex       _lock;    // the composite lock: the parameters, their record and tag keys, and the version
    std::vector<std::string> _tagKeys; // the tag key that sets each parameter, empty for none
    std::vector<pmt::Value>  _parameters;
    ValueMap                 _record{};
    std::uint64_t            _version = 0U;

    [[nodiscard]] std::optional<std::size_t>        indexOf(std::string_view name) const noexcept;
    void                                            split(const property_map& changes, property_map& parameters, property_map& ownKeys) const;
    [[nodiscard]] std::expected<Prepared, Rejected> prepare(const property_map& parameters) const;
    [[nodiscard]] std::optional<Committed>          commit(SettingsBase& own, const Prepared& prepared, const std::optional<detail::CheckedCall>& ownKeys); // nothing when a version moved
    // the call's steps up to the commit: the prepared parameters and the checked own keys, or every refusal of both
    [[nodiscard]] std::expected<Checked, Rejected>   checkCall(SettingsBase& own, const property_map& parameters, const property_map& ownKeys) const;
    [[nodiscard]] std::expected<Committed, Rejected> prepareAndCommit(SettingsBase& own, const property_map& changes, std::vector<std::string>& parameters);
};

} // namespace gr::settings::detail

#endif // GNURADIO_COMPOSITE_SETTINGS_HPP
