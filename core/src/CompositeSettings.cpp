#include <gnuradio-4.0/CompositeSettings.hpp>

#include <algorithm>
#include <format>
#include <iterator>
#include <utility>

#include "SettingsHelpers.hpp"

namespace gr::settings::detail {

namespace {

// a parameter's value in its declared type: a number by the rule of a key of that type, a vector element by element, a
// text or a bool exactly
[[nodiscard]] std::expected<pmt::Value, std::string> convertParameter(const recipe::ParameterDeclaration& declaration, const pmt::Value& sent) {
    const std::string_view type    = declaration.type;
    const bool             vector  = type.ends_with("[]");
    const std::string_view element = vector ? type.substr(0UZ, type.size() - 2UZ) : type;
    if (element == "string" || (!vector && element == "bool")) {
        if (std::expected<void, gr::Error> agrees = recipe::checkParameterValue(declaration, sent); !agrees) {
            return std::unexpected(agrees.error().message);
        }
        return sent;
    }
    const std::optional<SettingType> settingType = parseTypeName(vector ? std::format("{}_vector", element) : std::string(element));
    if (!settingType) {
        return std::unexpected(std::format("the parameter '{}' declares the type {}, which converts no value", declaration.name, type));
    }
    return convert(Declaration{.name = declaration.name, .type = *settingType}, sent);
}

// the indices of the parameters a binding reads
[[nodiscard]] std::vector<std::size_t> parametersRead(const recipe::Binding& binding) {
    std::vector<std::size_t> read;
    const auto               add = [&read](const recipe::Expression& expression) {
        for (const recipe::Expression::Op& op : expression.ops) {
            if (op.kind == recipe::Expression::OpKind::pushParameter && std::ranges::find(read, op.parameterIndex) == read.end()) {
                read.push_back(op.parameterIndex);
            }
        }
    };
    if (binding.substituted) {
        read.push_back(*binding.substituted);
    } else if (!binding.sequence.empty()) {
        for (const recipe::SequenceElement& element : binding.sequence) {
            if (element.expression) {
                add(*element.expression);
            }
        }
    } else {
        add(binding.expression);
    }
    return read;
}

[[nodiscard]] std::string innerReason(std::string_view block, std::string_view key, std::string_view reason) { return std::format("block '{}' key '{}': {}", block, key, reason); }

// the parameters of the call behind an inner key; every parameter that reaches the block for an entry that names no key
[[nodiscard]] std::vector<std::string> parametersBehind(const detail::InnerChanges& inner, std::string_view key) {
    if (const auto it = inner.parametersOf.find(key); it != inner.parametersOf.end()) {
        return it->second;
    }
    std::vector<std::string> every;
    for (const auto& [_, parameters] : inner.parametersOf) {
        for (const std::string& parameter : parameters) {
            if (std::ranges::find(every, parameter) == every.end()) {
                every.push_back(parameter);
            }
        }
    }
    return every;
}

// the result of a call on parameters: each parameter of the call by the answers of the inner blocks behind it, with the
// own keys' answer joined. Each inner entry holds its inner changes and its answer; an inner block that refused the
// call at its commit fails each parameter behind it. A parameter's value entry carries the parameter's stored value,
// and the inner blocks' values in its reason
template<typename TInner>
[[nodiscard]] Values compositeResult(std::span<const std::string> parameters, const Values& stored, std::span<const TInner> inners, const Values& ownAnswer) {
    Values values;
    for (const std::string& parameter : parameters) {
        std::optional<std::string_view> pendingCause; // of the inner blocks' causes, the one declared first
        std::string                     failure;
        std::string                     actual;
        const auto                      join = [](std::string& text, const std::string& part) { text += text.empty() ? part : "; " + part; };
        for (const TInner& inner : inners) {
            if (!inner.answer->has_value()) {
                for (const Rejection& entry : inner.answer->error()) {
                    if (std::ranges::contains(parametersBehind(inner.inner, entry.key), parameter)) {
                        join(failure, innerReason(inner.inner.blockName, entry.key, entry.reason));
                    }
                }
                continue;
            }
            for (const auto& [key, entry] : **inner.answer) {
                if (const auto behind = inner.inner.parametersOf.find(key); behind == inner.inner.parametersOf.end() || std::ranges::find(behind->second, parameter) == behind->second.end()) {
                    continue;
                }
                if (entry.isFailed()) {
                    join(failure, innerReason(inner.inner.blockName, key, entry.reason));
                } else if (entry.isPending()) {
                    const std::string_view cause = entry.reason == kPendingNotRunning ? kPendingNotRunning : (entry.reason == kPendingOnWorker ? kPendingOnWorker : kPendingTimeout);
                    if (!pendingCause || cause == kPendingNotRunning || (cause == kPendingOnWorker && *pendingCause == kPendingTimeout)) {
                        pendingCause = cause;
                    }
                } else if (entry.isValue()) {
                    join(actual, std::format("block '{}' key '{}' runs {}{}", inner.inner.blockName, key, entry.value, entry.reason.empty() ? std::string{} : std::format(" ({})", entry.reason)));
                }
            }
        }
        if (!failure.empty()) {
            values.insert_or_assign(parameter, Value{.state = Value::State::failed, .value = {}, .reason = std::move(failure)});
        } else if (pendingCause) {
            values.insert_or_assign(parameter, Value{.state = Value::State::pending, .value = {}, .reason = std::string(*pendingCause)});
        } else if (!actual.empty()) {
            const auto storedValue = stored.find(parameter);
            values.insert_or_assign(parameter, Value{.state = Value::State::value, .value = storedValue != stored.end() ? storedValue->second.value : pmt::Value{}, .reason = std::move(actual)});
        }
    }
    values.merge(Values(ownAnswer));
    return values;
}

} // namespace

CompositeSettings::CompositeSettings(std::vector<recipe::ParameterDeclaration> declarations, std::vector<recipe::Binding> bindings, std::vector<pmt::Value> values, const property_map& given, InnerResolver resolve) : _declarations(std::move(declarations)), _bindings(std::move(bindings)), _resolve(std::move(resolve)), _tagKeys(_declarations.size()), _parameters(std::move(values)) {
    for (const auto& [name, value] : given) {
        if (const std::optional<std::size_t> index = indexOf(name); index && *index < _parameters.size()) {
            _record.insert_or_assign(std::string(std::string_view(name)), _parameters[*index]);
        }
    }
}

std::optional<std::size_t> CompositeSettings::indexOf(std::string_view name) const noexcept {
    const auto found = std::ranges::find(_declarations, name, &recipe::ParameterDeclaration::name);
    return found != _declarations.end() ? std::optional(static_cast<std::size_t>(std::distance(_declarations.begin(), found))) : std::nullopt;
}

bool CompositeSettings::declares(std::string_view name) const noexcept { return indexOf(name).has_value(); }

bool CompositeSettings::namesParameter(const property_map& changes) const noexcept {
    return std::ranges::any_of(changes, [this](const auto& entry) { return declares(entry.first); });
}

void CompositeSettings::couple(std::string_view parameter, std::string tagKey) {
    if (const std::optional<std::size_t> index = indexOf(parameter); index) {
        std::lock_guard lock(_lock);
        _tagKeys[*index] = std::move(tagKey);
    }
}

property_map CompositeSettings::couplings() const {
    std::lock_guard lock(_lock);
    property_map    table;
    for (std::size_t index = 0UZ; index < _tagKeys.size(); ++index) {
        if (!_tagKeys[index].empty()) {
            table.insert_or_assign(std::pmr::string(_declarations[index].name), pmt::Value(_tagKeys[index]));
        }
    }
    return table;
}

std::vector<std::string> CompositeSettings::parameterNames() const {
    std::vector<std::string> names;
    std::ranges::transform(_declarations, std::back_inserter(names), &recipe::ParameterDeclaration::name);
    return names;
}

bool CompositeSettings::isGiven(std::string_view name) const {
    std::lock_guard lock(_lock);
    return _record.contains(name);
}

property_map CompositeSettings::record() const {
    std::lock_guard lock(_lock);
    property_map    given;
    for (const auto& [name, value] : _record) {
        given.insert_or_assign(std::pmr::string(name), value);
    }
    return given;
}

void CompositeSettings::split(const property_map& changes, property_map& parameters, property_map& ownKeys) const {
    for (const auto& [key, value] : changes) {
        (declares(key) ? parameters : ownKeys).insert_or_assign(key, value);
    }
}

std::expected<CompositeSettings::Prepared, Rejected> CompositeSettings::prepare(const property_map& parameters) const {
    Prepared prepared;
    {
        std::lock_guard lock(_lock);
        prepared.trial        = _parameters;
        prepared.versionNoted = _version;
    }
    Rejected          entries;
    std::vector<bool> inCall(_declarations.size(), false);
    for (const auto& [name, sent] : parameters) {
        const std::size_t                      index     = *indexOf(name);
        std::expected<pmt::Value, std::string> converted = convertParameter(_declarations[index], sent);
        if (!converted) {
            entries.push_back(invalidEntry(std::string(std::string_view(name)), converted.error()));
            continue;
        }
        prepared.trial[index] = std::move(*converted);
        inCall[index]         = true;
        prepared.parameters.emplace_back(std::string_view(name));
    }
    if (!entries.empty()) {
        return std::unexpected(std::move(entries));
    }

    for (const recipe::Binding& binding : _bindings) {
        std::vector<std::string> fromCall;
        for (const std::size_t index : parametersRead(binding)) {
            if (index < inCall.size() && inCall[index]) {
                fromCall.push_back(_declarations[index].name);
            }
        }
        if (fromCall.empty()) {
            continue;
        }
        std::expected<pmt::Value, gr::Error> derived = recipe::bindingValue(binding, prepared.trial);
        if (!derived) {
            for (const std::string& parameter : fromCall) {
                entries.push_back(invalidEntry(parameter, derived.error().message));
            }
            continue;
        }
        std::expected<InnerTarget, std::string> resolved = _resolve(binding.namePath);
        if (!resolved) {
            for (const std::string& parameter : fromCall) {
                entries.push_back(invalidEntry(parameter, resolved.error()));
            }
            continue;
        }
        auto target = std::ranges::find(prepared.inners, resolved->settings, &detail::InnerChanges::settings);
        if (target == prepared.inners.end()) {
            target = prepared.inners.insert(prepared.inners.end(), detail::InnerChanges{.settings = resolved->settings, .blockName = std::move(resolved->blockName), .changes = {}, .parametersOf = {}});
        }
        target->changes.insert_or_assign(std::pmr::string(binding.settingKey), std::move(*derived));
        target->parametersOf.insert_or_assign(binding.settingKey, std::move(fromCall));
    }
    if (!entries.empty()) {
        return std::unexpected(std::move(entries));
    }

    for (const detail::InnerChanges& inner : prepared.inners) {
        if (std::expected<void, Rejected> checked = inner.settings->check(inner.changes); !checked) {
            for (const Rejection& entry : checked.error()) {
                for (const std::string& parameter : parametersBehind(inner, entry.key)) {
                    entries.push_back(invalidEntry(parameter, innerReason(inner.blockName, entry.key, entry.reason)));
                }
            }
        }
    }
    if (!entries.empty()) {
        return std::unexpected(std::move(entries));
    }
    return prepared;
}

std::optional<CompositeSettings::Committed> CompositeSettings::commit(SettingsBase& own, const Prepared& prepared, const std::optional<detail::CheckedCall>& ownKeys) {
    std::lock_guard lock(_lock);
    if (_version != prepared.versionNoted) {
        return std::nullopt;
    }
    Committed committed;
    if (ownKeys) { // the own keys merge first, so own keys that moved since their check leave every inner block as it was
        std::optional<detail::Merged> merged = own.merge(*ownKeys, false, false);
        if (!merged) {
            return std::nullopt;
        }
        committed.own = std::move(*merged);
    }
    for (const detail::InnerChanges& inner : prepared.inners) {
        // an inner block a direct call moved since its check may refuse here; the blocks merged before it keep theirs
        committed.inners.push_back(Committed::Inner{.inner = inner, .merged = inner.settings->checkAndMerge(inner.changes, nullptr, false)});
    }
    _parameters = prepared.trial;
    for (const std::string& name : prepared.parameters) {
        _record.insert_or_assign(name, _parameters[*indexOf(name)]);
    }
    ++_version;
    return committed;
}

std::expected<CompositeSettings::Checked, Rejected> CompositeSettings::checkCall(SettingsBase& own, const property_map& parameters, const property_map& ownKeys) const {
    std::expected<Prepared, Rejected>                           prepared = prepare(parameters);
    Rejected                                                    entries  = prepared ? Rejected{} : prepared.error();
    std::optional<std::expected<detail::CheckedCall, Rejected>> checkedOwn;
    if (!ownKeys.empty()) {
        checkedOwn = own.checkChanges(ownKeys);
        if (!*checkedOwn) {
            std::ranges::copy(checkedOwn->error(), std::back_inserter(entries));
        }
    }
    if (!entries.empty()) {
        return std::unexpected(std::move(entries));
    }
    return Checked{.prepared = std::move(*prepared), .own = checkedOwn ? std::optional(std::move(**checkedOwn)) : std::nullopt};
}

std::expected<CompositeSettings::Committed, Rejected> CompositeSettings::prepareAndCommit(SettingsBase& own, const property_map& changes, std::vector<std::string>& parameters) {
    property_map parameterChanges;
    property_map ownKeys;
    split(changes, parameterChanges, ownKeys);
    while (true) { // a version that moved since the check prepares the call again
        std::expected<Checked, Rejected> checked = checkCall(own, parameterChanges, ownKeys);
        if (!checked) {
            return std::unexpected(std::move(checked.error()));
        }
        if (std::optional<Committed> committed = commit(own, checked->prepared, checked->own); committed) {
            parameters = std::move(checked->prepared.parameters);
            return std::move(*committed);
        }
    }
}

std::expected<void, Rejected> CompositeSettings::check(SettingsBase& own, const property_map& changes) {
    property_map parameterChanges;
    property_map ownKeys;
    split(changes, parameterChanges, ownKeys);
    return checkCall(own, parameterChanges, ownKeys).transform([](const Checked&) {});
}

std::expected<Values, Rejected> CompositeSettings::set(SettingsBase& own, const property_map& changes, Timeout timeout) {
    const Clock::time_point            calledAt = Clock::now();
    std::vector<std::string>           parameters;
    std::expected<Committed, Rejected> committed = prepareAndCommit(own, changes, parameters);
    if (!committed) {
        return std::unexpected(std::move(committed.error()));
    }
    // each finish runs with the composite lock released, so an inner hook that reads the composite does not wait on it
    for (Committed::Inner& inner : committed->inners) {
        inner.answer = inner.merged ? std::expected<Values, Rejected>(inner.inner.settings->finish(*inner.merged, calledAt, timeout)) : std::unexpected(inner.merged.error());
    }
    const Values             ownAnswer = committed->own ? own.finish(*committed->own, calledAt, timeout) : Values{};
    std::vector<std::string> notParameters;
    return compositeResult(parameters, readParameters(parameters, notParameters), std::span<const Committed::Inner>(committed->inners), ownAnswer);
}

Values CompositeSettings::readParameters(std::span<const std::string> keys, std::vector<std::string>& otherKeys) const {
    std::lock_guard lock(_lock);
    Values          values;
    const auto      entryOf = [this](std::size_t index) { return Value{.state = Value::State::value, .value = _parameters[index], .reason = {}}; };
    if (keys.empty()) {
        for (std::size_t index = 0UZ; index < _declarations.size() && index < _parameters.size(); ++index) {
            values.insert_or_assign(_declarations[index].name, entryOf(index));
        }
        return values;
    }
    for (const std::string& key : keys) {
        if (const std::optional<std::size_t> index = indexOf(key); index && *index < _parameters.size()) {
            values.insert_or_assign(key, entryOf(*index));
        } else {
            otherKeys.push_back(key);
        }
    }
    return values;
}

std::string innerTagKeyPrefix(std::string_view compositeName) { return std::format("{}{}/", kInnerTagKeyPrefix, compositeName); }

namespace {
// the tag key a composite adds to a tag that enters it for one inner setting
std::string innerTagKey(std::string_view compositePrefix, std::string_view blockName, std::string_view settingKey) { return std::format("{}{}.{}", compositePrefix, blockName, settingKey); }
} // namespace

void CompositeSettings::setStreamPaths(std::string_view compositeName, std::vector<StreamEntry> entries, std::span<SettingsBase* const> innerBlocks) {
    std::lock_guard lock(_lock);
    for (SettingsBase* inner : innerBlocks) {
        CompositeSettings* entered = this;
        inner->_compositeEntry.compare_exchange_strong(entered, nullptr);
        std::lock_guard innerLock(inner->_lock);
        std::erase_if(inner->_innerTagKeys, [this](const auto& entry) { return entry.first.starts_with(_tagKeyPrefix); });
    }
    _tagKeyPrefix = innerTagKeyPrefix(compositeName);
    _entries      = std::move(entries);
    for (const StreamEntry& entry : _entries) {
        entry.entry->_compositeEntry.store(this);
        for (const recipe::Binding& binding : _bindings) {
            std::expected<InnerTarget, std::string> target = _resolve(binding.namePath);
            if (target && std::ranges::find(entry.reached, target->settings) != entry.reached.end()) {
                std::lock_guard innerLock(target->settings->_lock);
                target->settings->_innerTagKeys.insert_or_assign(innerTagKey(_tagKeyPrefix, target->blockName, binding.settingKey), binding.settingKey);
            }
        }
    }
}

property_map CompositeSettings::translateTag(const SettingsBase& entry, const property_map& tagMap) {
    property_map changes; // a tag that carries no bound tag key copies and allocates nothing
    {
        std::lock_guard lock(_lock);
        for (std::size_t index = 0UZ; index < _tagKeys.size(); ++index) {
            if (_tagKeys[index].empty()) {
                continue;
            }
            const auto tagged = tagMap.find(std::string_view(_tagKeys[index]));
            if (tagged == tagMap.end()) {
                continue;
            }
            // a tag at the stored value translates nothing; a value that does not convert stays for the check to refuse
            if (std::expected<pmt::Value, std::string> converted = convertParameter(_declarations[index], tagged->second); converted && sameValue(*converted, _parameters[index])) {
                continue;
            }
            changes.insert_or_assign(std::pmr::string(_declarations[index].name), tagged->second);
        }
    }
    if (changes.empty()) {
        return {};
    }
    while (true) {
        std::expected<Prepared, Rejected> prepared = prepare(changes);
        if (!prepared) {
            for (const Rejection& refused : prepared.error()) {
                entry.notify(NotificationLevel::error, NotificationKind::failed, refused.key, std::format("the tag's value is refused: {}", refused.reason));
            }
            return {};
        }
        property_map                                  translated;
        std::vector<std::pair<SettingsBase*, Merged>> staged;
        {
            std::lock_guard lock(_lock);
            if (_version != prepared->versionNoted) {
                continue;
            }
            const auto entryAt = std::ranges::find(_entries, &entry, &StreamEntry::entry);
            for (const detail::InnerChanges& inner : prepared->inners) {
                if (entryAt != _entries.end() && std::ranges::find(entryAt->reached, inner.settings) != entryAt->reached.end()) {
                    for (const auto& [key, value] : inner.changes) {
                        translated.insert_or_assign(std::pmr::string(innerTagKey(_tagKeyPrefix, inner.blockName, key)), value);
                    }
                    continue;
                }
                // an inner block off the stream merges under the composite lock, so a tag's older value cannot land over a newer call's
                KeySet tagSets;
                for (const auto& [key, _] : inner.changes) {
                    tagSets.emplace(std::string_view(key));
                }
                std::expected<Merged, Rejected> merged = inner.settings->checkAndMerge(inner.changes, &tagSets, false);
                if (!merged) {
                    for (const Rejection& refused : merged.error()) {
                        inner.settings->notify(NotificationLevel::error, NotificationKind::failed, refused.key, std::format("the tag's value is refused: {}", refused.reason));
                    }
                    continue;
                }
                staged.emplace_back(inner.settings, std::move(*merged));
            }
            _parameters = prepared->trial; // a tag's value follows the stream and stores no parameter in the record
            ++_version;
        }
        for (auto& [settings, merged] : staged) {
            if (!settings->applyAtCall(merged) && merged.schedulerLink != nullptr) {
                merged.schedulerLink->wake();
            }
        }
        return translated;
    }
}

} // namespace gr::settings::detail
