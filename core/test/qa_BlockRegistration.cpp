#include <boost/ut.hpp>

#include <gnuradio-4.0/Block.hpp>
#include <gnuradio-4.0/BlockAttributes.hpp>
#include <gnuradio-4.0/BlockRegistry.hpp>
#include <gnuradio-4.0/PluginLoader.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/**
 * The generated definition units register through gr::makeBlockRegistration() and
 * gr::insertBlockFactory() instead of gr::registerBlock(). Both paths must leave the registry in the
 * same state, key for key and alias for alias, or a block that a graph resolves by name today stops
 * resolving tomorrow.
 */

namespace qa_registration {

struct Sink : gr::Block<Sink> {
    gr::PortIn<float> in;

    GR_MAKE_REFLECTABLE(Sink, in);

    explicit Sink(gr::property_map init = {}) : gr::Block<Sink>(std::move(init)) {}

    void processOne(float) const noexcept {}
};

struct DefaultPolicy {};

template<typename T, typename TPolicy = DefaultPolicy>
struct Scale : gr::Block<Scale<T, TPolicy>> {
    gr::PortIn<T>  in;
    gr::PortOut<T> out;

    GR_MAKE_REFLECTABLE(Scale, in, out);

    explicit Scale(gr::property_map init = {}) : gr::Block<Scale<T, TPolicy>>(std::move(init)) {}

    [[nodiscard]] constexpr T processOne(T value) const noexcept { return value; }
};

/// two revisions of one block under one name, the older one marked deprecated
struct Filter : gr::Block<Filter> {
    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(Filter, in, out);

    explicit Filter(gr::property_map init = {}) : gr::Block<Filter>(std::move(init)) {}

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value; }
};

namespace labels = gr::block::labels;

/// the family word every radio of this suite declares, and a word of the suite's own in a class core defines
inline constexpr gr::block::Label kQaFamily = labels::family("qa", "Radios of the registration suite.");
inline constexpr gr::block::Label kQaGpib{gr::block::LabelClass::Holds, "gpib", "Opens an instrument bus of the suite."};

struct FilterV1 : gr::Block<FilterV1> {
    static constexpr auto attributes = gr::block::describe(1U, labels::status::deprecated, labels::status::experimental);

    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(FilterV1, in, out);

    explicit FilterV1(gr::property_map init = {}) : gr::Block<FilterV1>(std::move(init)) {}

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value; }
};

struct FilterV2 : gr::Block<FilterV2> {
    static constexpr auto attributes = gr::block::describe(2U);

    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(FilterV2, in, out);

    explicit FilterV2(gr::property_map init = {}) : gr::Block<FilterV2>(std::move(init)) {}

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value; }
};

/// a radio source and sink, each declaring its role, whose ports read generator and consumer
struct RadioSource : gr::Block<RadioSource> {
    static constexpr auto attributes = gr::block::describe(1U, kQaFamily, labels::role::source, labels::holds::device, labels::ingests::rf);

    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(RadioSource, out);

    explicit RadioSource(gr::property_map init = {}) : gr::Block<RadioSource>(std::move(init)) {}

    [[nodiscard]] constexpr float processOne() const noexcept { return 0.0f; }
};

struct RadioSink : gr::Block<RadioSink> {
    static constexpr auto attributes = gr::block::describe(1U, kQaFamily, labels::role::sink, labels::holds::device, labels::emits::rf);

    gr::PortIn<float> in;

    GR_MAKE_REFLECTABLE(RadioSink, in);

    explicit RadioSink(gr::property_map init = {}) : gr::Block<RadioSink>(std::move(init)) {}

    void processOne(float) const noexcept {}
};

/// a processing block on the radio's FPGA: it holds the device and has stream inputs and outputs, and declares no
/// role; the labels are given out of class order
struct FpgaTransform : gr::Block<FpgaTransform> {
    static constexpr auto attributes = gr::block::describe(2U, labels::status::experimental, labels::compute::fpga, labels::holds::device, kQaFamily);

    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(FpgaTransform, in, out);

    explicit FpgaTransform(gr::property_map init = {}) : gr::Block<FpgaTransform>(std::move(init)) {}

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value; }
};

/// a device source with a message input of its own; only its type is read
struct RadioControlledSource : gr::Block<RadioControlledSource> {
    static constexpr auto attributes = gr::block::describe(1U, kQaFamily, labels::holds::device);

    gr::MsgPortIn      control;
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(RadioControlledSource, control, out);

    [[nodiscard]] constexpr float processOne() const noexcept { return 0.0f; }
};

/// a device sink whose inputs are one dynamic collection; only its type is read
struct RadioCombiningSink : gr::Block<RadioCombiningSink> {
    static constexpr auto attributes = gr::block::describe(1U, kQaFamily, labels::holds::device);

    std::vector<gr::PortIn<float>> inputs;

    GR_MAKE_REFLECTABLE(RadioCombiningSink, inputs);

    template<gr::InputSpanLike TInSpan>
    gr::work::Status processBulk(std::span<TInSpan>&) const noexcept {
        return gr::work::Status::OK;
    }
};

/// a rotator controller: a declared transceiver with no stream port, acting through messages alone; only its type is
/// read
struct RotatorController : gr::Block<RotatorController> {
    static constexpr auto attributes = gr::block::describe(1U, labels::role::transceiver, labels::plane::control, labels::holds::device, labels::emits::motion, labels::ingests::motion);

    gr::MsgPortIn command;

    GR_MAKE_REFLECTABLE(RotatorController, command);

    [[nodiscard]] constexpr gr::work::Status processBulk() const noexcept { return gr::work::Status::OK; }
};

/// a control-plane block that declares no role, and a notation block, neither with a stream port
struct ControlOnly : gr::Block<ControlOnly> {
    static constexpr auto attributes = gr::block::describe(1U, labels::plane::control, kQaGpib);

    gr::MsgPortIn command;

    GR_MAKE_REFLECTABLE(ControlOnly, command);

    [[nodiscard]] constexpr gr::work::Status processBulk() const noexcept { return gr::work::Status::OK; }
};

struct Antenna : gr::Block<Antenna> {
    static constexpr auto attributes = gr::block::describe(1U, labels::plane::notation);

    std::string feeds;

    GR_MAKE_REFLECTABLE(Antenna, feeds);

    [[nodiscard]] constexpr gr::work::Status processBulk() const noexcept { return gr::work::Status::DONE; }
};

/// a declared source whose stream ports read consumer; the framework carries both and refuses neither
struct ContradictedSource : gr::Block<ContradictedSource> {
    static constexpr auto attributes = gr::block::describe(1U, labels::role::source);

    gr::PortIn<float> in;

    GR_MAKE_REFLECTABLE(ContradictedSource, in);

    void processOne(float) const noexcept {}
};

/// whether `describe()` accepts the labels, as a constraint that is false where the call does not compile
template<const gr::block::Label&... kLabels>
concept Describable = requires { typename std::integral_constant<std::size_t, gr::block::describe(1U, kLabels...).count>; };

inline constexpr gr::block::Label kOtherFamily = labels::family("other", "Radios of another library.");
inline constexpr gr::block::Label kUpperCaseWord{gr::block::LabelClass::Holds, "GPIB", "An instrument bus."};

template<typename TBlock>
std::unique_ptr<gr::BlockModel> makeBlock(gr::property_map params) {
    return std::make_unique<gr::BlockWrapper<TBlock>>(std::move(params));
}

/// registers `TBlock` under `key`, keeping the version and the attributes the type declares
template<typename TBlock>
bool insertAs(gr::BlockRegistry& registry, std::string_view key) {
    const gr::BlockRegistration declared = gr::makeBlockRegistration<TBlock>(&makeBlock<TBlock>);
    return registry.insert(key, "", declared.factory, declared.attributes, declared.labels);
}

/// the map `makeBlockRegistration()` fills for `TBlock`
template<typename TBlock>
gr::property_map registeredMap() {
    return gr::makeBlockRegistration<TBlock>(&makeBlock<TBlock>).attributes;
}

/// whether an instance's meta_information carries exactly `expected` under the Attributes key
bool carriesEntry(const gr::BlockModel& block, const gr::property_map& expected) {
    const gr::property_map& meta  = block.metaInformation();
    const auto              entry = meta.find(gr::block::kAttributesMetaKey);
    return entry != meta.cend() && entry->second == gr::pmt::Value(expected);
}

/// the meta_information a block's serialized form carries, empty when it carries none
gr::property_map savedMeta(const std::shared_ptr<gr::BlockModel>& block) {
    gr::BlockRegistry      registry;
    gr::SchedulerRegistry  schedulerRegistry;
    gr::PluginLoader       loader(registry, schedulerRegistry, std::span<const std::string>{});
    const gr::property_map serialized = gr::serializeBlock(loader, block, gr::BlockSerializationFlags::All & ~gr::BlockSerializationFlags::Ports);
    const auto             entry      = serialized.find(gr::serialization_fields::BLOCK_META_INFORMATION);
    const auto*            meta       = entry == serialized.cend() ? nullptr : entry->second.get_if<gr::property_map>();
    return meta == nullptr ? gr::property_map{} : *meta;
}

std::string_view wordAt(const gr::property_map& map, std::string_view key) {
    const auto entry = map.find(key);
    return entry == map.cend() ? std::string_view{} : entry->second.value_or(std::string_view{});
}

std::vector<std::string> wordsAt(const gr::property_map& map, std::string_view key) {
    std::vector<std::string> words;
    const auto               entry = map.find(key);
    if (entry == map.cend()) {
        return words;
    }
    if (const auto* list = entry->second.get_if<gr::Tensor<gr::pmt::Value>>(); list != nullptr) {
        for (const gr::pmt::Value& word : *list) {
            words.emplace_back(word.value_or(std::string_view{}));
        }
    }
    return words;
}

std::string joined(const std::vector<std::string>& keys) {
    std::string out;
    for (const std::string& key : keys) {
        out += key;
        out += '|';
    }
    return out;
}

template<typename TBlock, gr::meta::fixed_string OverrideName = "">
void expectRegistrationParity(std::string_view what) {
    using namespace boost::ut;

    gr::BlockRegistry typedRegistry;
    gr::registerBlock<TBlock, OverrideName>(typedRegistry);

    gr::BlockRegistry factoryRegistry;
    expect(gr::insertBlockFactory(factoryRegistry, gr::makeBlockRegistration<TBlock, OverrideName>(&makeBlock<TBlock>))) << what;

    const std::vector<std::string> typedKeys = typedRegistry.keys();
    expect(!typedKeys.empty()) << what;
    expect(eq(joined(typedKeys), joined(factoryRegistry.keys()))) << what;

    for (const std::string& key : typedKeys) {
        const std::shared_ptr<gr::BlockModel> fromTyped   = typedRegistry.create(key, {});
        const std::shared_ptr<gr::BlockModel> fromFactory = factoryRegistry.create(key, {});
        expect(fromTyped != nullptr) << key;
        expect(fromFactory != nullptr) << key;
        if (fromTyped == nullptr || fromFactory == nullptr) {
            continue;
        }
        expect(eq(std::string(fromTyped->typeName()), std::string(fromFactory->typeName()))) << key;
        expect(eq(typedRegistry.typeName(fromTyped), factoryRegistry.typeName(fromFactory))) << key << "alias";
        expect(typedRegistry.attributes(key) == factoryRegistry.attributes(key)) << key << "attributes";
    }
}

} // namespace qa_registration

const boost::ut::suite<"block registration"> blockRegistrationTests = [] {
    using namespace boost::ut;

    "a block with no template parameters"_test = [] { qa_registration::expectRegistrationParity<qa_registration::Sink>("Sink"); };

    "a template instantiation, whose alias carries the parameter spelling"_test = [] { qa_registration::expectRegistrationParity<qa_registration::Scale<float>>("Scale<float>"); };

    "a defaulted template parameter, which the key spells out and the alias need not"_test = [] {
        qa_registration::expectRegistrationParity<qa_registration::Scale<std::complex<float>>>("Scale<complex<float32>>");

        gr::BlockRegistry registry;
        expect(gr::insertBlockFactory(registry, gr::makeBlockRegistration<qa_registration::Scale<std::complex<float>>>(&qa_registration::makeBlock<qa_registration::Scale<std::complex<float>>>)));
        expect(registry.contains("qa_registration::Scale<complex<float32>, qa_registration::DefaultPolicy>")) << "the portable key";
        expect(registry.contains("qa_registration::Scale<complex<float32>>")) << "the portable alias";
        expect(!registry.contains("qa_registration::Scale<std::complex<float32>>")) << "no standard-library spelling";
    };

    "an overridden name, which registers a second key"_test = [] {
        qa_registration::expectRegistrationParity<qa_registration::Sink, "qa::CustomSink">("Sink as qa::CustomSink");

        gr::BlockRegistry registry;
        expect(gr::insertBlockFactory(registry, gr::makeBlockRegistration<qa_registration::Sink, "qa::CustomSink">(&qa_registration::makeBlock<qa_registration::Sink>)));
        expect(eq(registry.keys().size(), 2UZ));
        expect(registry.contains("qa::CustomSink"));
        expect(registry.contains(gr::meta::type_name<qa_registration::Sink>()));
    };

    "insert reports a newly added alias"_test = [] {
        gr::BlockRegistry registry;
        expect(gr::insertBlockFactory(registry, gr::makeBlockRegistration<qa_registration::Sink, "">(&qa_registration::makeBlock<qa_registration::Sink>)));
        expect(gr::insertBlockFactory(registry, gr::makeBlockRegistration<qa_registration::Sink, "qa::CustomSink">(&qa_registration::makeBlock<qa_registration::Sink>))) //
            << "the alias key is new even though the name key repeats";
        expect(!gr::insertBlockFactory(registry, gr::makeBlockRegistration<qa_registration::Sink, "qa::CustomSink">(&qa_registration::makeBlock<qa_registration::Sink>))) //
            << "repeating both keys adds nothing";
    };

    "the factory a definition unit exports builds the block the key names"_test = [] {
        gr::BlockRegistry registry;
        expect(gr::insertBlockFactory(registry, gr::makeBlockRegistration<qa_registration::Scale<float>>(&qa_registration::makeBlock<qa_registration::Scale<float>>)));

        const std::string                     key   = gr::meta::type_name<qa_registration::Scale<float>>();
        const std::shared_ptr<gr::BlockModel> block = registry.create(key, {{"name", std::string("scaled")}});
        expect(block != nullptr) << key;
        if (block == nullptr) {
            return;
        }
        expect(eq(std::string(block->typeName()), key));

        // the key comes from the type, the alias from the reflected spelling, so a defaulted template
        // parameter appears in one and not the other -- both must resolve
        expect(eq(key, std::string("qa_registration::Scale<float32, qa_registration::DefaultPolicy>")));
        expect(registry.contains("qa_registration::Scale<float32>")) << "the reflected alias";
        expect(registry.create("qa_registration::Scale<float32>", {}) != nullptr) << "the reflected alias";
    };

    "an integer parameter, whose alias is spelled the portable way"_test = [] {
        qa_registration::expectRegistrationParity<qa_registration::Scale<std::int16_t>>("Scale<int16>");

        gr::BlockRegistry registry;
        expect(gr::insertBlockFactory(registry, gr::makeBlockRegistration<qa_registration::Scale<std::int16_t>>(&qa_registration::makeBlock<qa_registration::Scale<std::int16_t>>)));

        // the compiler renders the reflected parameter as "short int"; the alias maps like the key
        expect(eq(registry.keys().size(), 2UZ)) << "the key with its defaulted parameter, and the alias without";
        expect(registry.contains("qa_registration::Scale<int16, qa_registration::DefaultPolicy>")) << "the key";
        expect(registry.contains("qa_registration::Scale<int16>")) << "the reflected alias";
        expect(!registry.contains("qa_registration::Scale<short int>")) << "the literal spelling";
    };

    "an overridden name, spelled the way a registration marker writes it"_test = [] {
        gr::BlockRegistry registry;
        expect(gr::insertBlockFactory(registry, gr::makeBlockRegistration<qa_registration::Scale<std::int16_t>, "qa::Scale<std::int16_t>">(&qa_registration::makeBlock<qa_registration::Scale<std::int16_t>>)));

        expect(registry.contains("qa::Scale<int16>")) << "the overridden name, mapped";
        expect(!registry.contains("qa::Scale<std::int16_t>")) << "the literal spelling";
    };
};

const boost::ut::suite<"block attributes"> blockAttributesTests = [] {
    using namespace boost::ut;
    using namespace std::string_literals;
    using namespace std::string_view_literals;
    using gr::block::AttributesRead;
    using gr::block::Label;
    using gr::block::LabelClass;
    using gr::block::LabelRead;
    using gr::block::Version;
    using qa_registration::carriesEntry;
    using qa_registration::registeredMap;
    using qa_registration::wordAt;
    using qa_registration::wordsAt;
    namespace labels = gr::block::labels;

    "a block that declares nothing reads version 1 and no label, and carries no Attributes key"_test = [] {
        static_assert(!gr::block::HasDeclaredAttributes<qa_registration::Filter>);
        expect(gr::block::attributesOf<qa_registration::Filter>().labels.empty());
        expect(eq(gr::block::attributesOf<qa_registration::Filter>().version, gr::block::kDefaultVersion));
        expect(gr::block::roleOf<qa_registration::Filter>() == std::optional<Label>{labels::role::processor}) << "the role its ports read";

        const auto undeclared = std::make_shared<gr::BlockWrapper<qa_registration::Filter>>();
        expect(eq(undeclared->version(), gr::block::kDefaultVersion));
        expect(undeclared->status().empty());
        expect(gr::block::attributesOf(*undeclared) == AttributesRead{});
        expect(!undeclared->metaInformation().contains(gr::block::kAttributesMetaKey));

        // FilterV2 differs from Filter only in its declaration
        const auto       declared                  = std::make_shared<gr::BlockWrapper<qa_registration::FilterV2>>();
        gr::property_map declaredWithoutAttributes = declared->metaInformation();
        expect(eq(declaredWithoutAttributes.erase(std::pmr::string(gr::block::kAttributesMetaKey)), 1UZ));
        expect(declaredWithoutAttributes == undeclared->metaInformation()) << "a declaration adds one key and changes no other";

        gr::property_map savedDeclared = qa_registration::savedMeta(declared);
        expect(eq(savedDeclared.erase(std::pmr::string(gr::block::kAttributesMetaKey)), 1UZ)) << "the saved form carries the declaration";
        expect(savedDeclared == qa_registration::savedMeta(undeclared)) << "and without it the two saved forms agree";

        gr::BlockRegistry           registry;
        const gr::BlockRegistration registration = gr::makeBlockRegistration<qa_registration::Filter>(&qa_registration::makeBlock<qa_registration::Filter>);
        expect(registration.attributes.empty());
        expect(registration.labels.empty());
        expect(gr::insertBlockFactory(registry, registration));
        expect(registry.attributes(registration.name) == std::optional<gr::property_map>{gr::property_map{}}) << "registered, and declaring nothing";
    };

    "a declared block reads its labels in class order on the type, the instance and the registry"_test = [] {
        static_assert(gr::block::HasDeclaredAttributes<qa_registration::FpgaTransform>);
        constexpr gr::block::Attributes declared = gr::block::attributesOf<qa_registration::FpgaTransform>();
        expect(eq(declared.version, Version{2U}));
        expect(std::ranges::equal(declared.labels, std::array{qa_registration::kQaFamily, labels::holds::device, labels::compute::fpga, labels::status::experimental})) << "class order, whatever order describe() was given";

        const gr::BlockRegistration registration = gr::makeBlockRegistration<qa_registration::FpgaTransform>(&qa_registration::makeBlock<qa_registration::FpgaTransform>);
        const gr::property_map&     map          = registration.attributes;
        expect(wordsAt(map, "labels") == std::vector<std::string>{"family/qa", "holds/device", "compute/fpga", "status/experimental"});
        expect(eq(wordAt(map, "role"), "processor"sv)) << "a block that holds a device and has stream inputs and outputs reads processor";
        expect(eq(map.at("version").value_or(Version{}), Version{2U}));
        expect(eq(map.size(), 3UZ)) << "version, labels and the read role";
        expect(std::ranges::equal(registration.labels, declared.labels)) << "the registration views the declared labels";

        const gr::BlockWrapper<qa_registration::FpgaTransform> block;
        expect(carriesEntry(block, map)) << "the instance carries the map the registration carries";
        const AttributesRead read = gr::block::attributesOf(block);
        expect(eq(read.version, Version{2U}));
        expect(read.has(qa_registration::kQaFamily) && read.has(labels::holds::device) && read.has(labels::compute::fpga) && read.has(labels::status::experimental));
        expect(eq(read.role(), "processor"sv));
        expect(eq(block.version(), Version{2U}));
        expect(block.status() == std::vector<std::string>{"experimental"});

        gr::BlockRegistry registry;
        expect(gr::insertBlockFactory(registry, registration));
        expect(registry.attributes(registration.name) == std::optional<gr::property_map>{map}) << "the registry keeps the same map";
        expect(registry.versions(registration.name) == std::vector<Version>{2U}) << "and files it under the version the map states";
    };

    "a direct set() leaves the Attributes entry to the type and returns the key as not applied"_test = [] {
        const std::pmr::string                  key(gr::block::kAttributesMetaKey);
        constexpr gr::block::Attributes         declared = gr::block::attributesOf<qa_registration::FpgaTransform>();
        const gr::property_map                  own      = registeredMap<qa_registration::FpgaTransform>();
        static constexpr std::array<Label, 1UZ> kDeprecated{labels::status::deprecated};
        const gr::property_map                  another = gr::block::attributesToMap(gr::block::Attributes{.version = Version{declared.version + 1U}, .labels = kDeprecated}, std::nullopt);
        expect(another != own);

        gr::BlockWrapper<qa_registration::FpgaTransform> block;
        expect(carriesEntry(block, own));

        const gr::property_map notSet = block.settings().set({{"qa_note", gr::pmt::Value(std::string("kept"))}, {key, gr::pmt::Value(another)}});
        expect(notSet.contains("qa_note"sv) && notSet.contains(key)) << "set() returns both keys it did not apply";
        expect(block.metaInformation().contains("qa_note"sv)) << "an undeclared key is filed in meta_information";
        expect(carriesEntry(block, own)) << "the Attributes entry is not";
        expect(eq(block.version(), declared.version));
        expect(block.status() == std::vector<std::string>{"experimental"});

        expect(block.settings().setStaged({{key, gr::pmt::Value(another)}}).contains(key));
        expect(block.settings().applyStagedParameters().appliedParameters.empty());
        expect(carriesEntry(block, own)) << "the staged path leaves the entry as well";

        block.settings().loadParametersFromPropertyMap({{key, gr::pmt::Value(another)}});
        expect(carriesEntry(block, own)) << "and so does the graph-file path";

        gr::BlockWrapper<qa_registration::Filter> undeclared;
        expect(undeclared.settings().set({{key, gr::pmt::Value(another)}}).contains(key));
        expect(!undeclared.metaInformation().contains(key)) << "a type that declares nothing carries no Attributes entry";
        expect(gr::block::attributesOf(undeclared) == AttributesRead{});
        expect(eq(undeclared.version(), gr::block::kDefaultVersion));
        expect(undeclared.status().empty());
    };

    "a declared role stands, and a block that declares none reads one from its stream ports"_test = [] {
        using gr::block::portRoleOf;
        using gr::block::roleOf;
        using Role = std::optional<Label>;
        expect(roleOf<qa_registration::RadioSource>() == Role{labels::role::source});
        expect(portRoleOf<qa_registration::RadioSource>() == Role{labels::role::generator});
        expect(roleOf<qa_registration::RadioSink>() == Role{labels::role::sink});
        expect(portRoleOf<qa_registration::RadioSink>() == Role{labels::role::consumer});
        expect(roleOf<qa_registration::FpgaTransform>() == Role{labels::role::processor}) << "holding a device makes no transceiver";
        expect(roleOf<qa_registration::RadioCombiningSink>() == Role{labels::role::consumer}) << "a dynamic port collection counts as one port";
        expect(roleOf<qa_registration::RadioControlledSource>() == Role{labels::role::generator}) << "a message port counts for nothing";
        expect(roleOf<qa_registration::RotatorController>() == Role{labels::role::transceiver});
        expect(portRoleOf<qa_registration::RotatorController>() == Role{}) << "no stream port reads no role";
        expect(roleOf<qa_registration::ControlOnly>() == Role{}) << "a control-only block that declares no role has none";
        expect(roleOf<qa_registration::Antenna>() == Role{labels::role::notation}) << "no stream port and plane/notation read notation";
        expect(roleOf<qa_registration::ContradictedSource>() == Role{labels::role::source});
        expect(portRoleOf<qa_registration::ContradictedSource>() == Role{labels::role::consumer}) << "the declaration and the ports disagree, and both are kept";

        const gr::property_map sourceMap = registeredMap<qa_registration::RadioSource>();
        expect(wordsAt(sourceMap, "labels") == std::vector<std::string>{"family/qa", "role/source", "holds/device", "ingests/rf"});
        expect(eq(wordAt(sourceMap, "role"), "generator"sv)) << "the map carries the role the ports read beside the declared one";
        expect(eq(wordAt(registeredMap<qa_registration::ContradictedSource>(), "role"), "consumer"sv));
        expect(!registeredMap<qa_registration::ControlOnly>().contains("role"sv));
        expect(!registeredMap<qa_registration::RotatorController>().contains("role"sv));
        expect(eq(wordAt(registeredMap<qa_registration::Antenna>(), "role"), "notation"sv));

        const gr::BlockWrapper<qa_registration::RadioSource> source;
        const gr::BlockWrapper<qa_registration::RadioSink>   sink;
        expect(carriesEntry(source, sourceMap));
        expect(carriesEntry(sink, registeredMap<qa_registration::RadioSink>()));
        expect(eq(gr::block::attributesOf(sink).role(), "sink"sv)) << "the declared role answers first";
    };

    "the ports read a role from the two port kinds and plane/notation alone"_test = [] {
        using gr::block::roleFromPorts;
        using Role = std::optional<Label>;
        for (const bool isNotation : {false, true}) {
            expect(roleFromPorts(false, true, isNotation) == Role{labels::role::generator});
            expect(roleFromPorts(true, false, isNotation) == Role{labels::role::consumer});
            expect(roleFromPorts(true, true, isNotation) == Role{labels::role::processor});
        }
        expect(roleFromPorts(false, false, true) == Role{labels::role::notation});
        expect(roleFromPorts(false, false, false) == Role{});
    };

    "describe() refuses a label given twice, a second word in a class of one, a malformed word and a seventeenth label"_test = [] {
        using qa_registration::Describable;
        static_assert(Describable<labels::role::source, labels::holds::device, labels::holds::storage>);
        static_assert(!Describable<labels::holds::device, labels::holds::device>);
        static_assert(!Describable<labels::role::source, labels::role::sink>);
        static_assert(!Describable<labels::compute::gpu, labels::compute::fpga>);
        static_assert(!Describable<qa_registration::kQaFamily, qa_registration::kOtherFamily>);
        static_assert(!Describable<qa_registration::kUpperCaseWord>);
        static_assert(Describable<labels::emits::rf, labels::emits::sound, labels::emits::light, labels::emits::motion, labels::emits::electrical, labels::emits::ambient, labels::emits::time, labels::emits::storage, //
            labels::emits::network, labels::emits::ipc, labels::emits::graphical, labels::emits::text, labels::ingests::rf, labels::ingests::sound, labels::ingests::light, labels::ingests::motion>);
        static_assert(!Describable<labels::emits::rf, labels::emits::sound, labels::emits::light, labels::emits::motion, labels::emits::electrical, labels::emits::ambient, labels::emits::time, labels::emits::storage, //
                      labels::emits::network, labels::emits::ipc, labels::emits::graphical, labels::emits::text, labels::ingests::rf, labels::ingests::sound, labels::ingests::light, labels::ingests::motion, labels::ingests::time>);
        expect(eq(gr::block::kMaxLabels, 16UZ));
    };

    "the map round-trips, and a word outside the vocabulary is carried and marked"_test = [] {
        static constexpr auto  kDeclared = gr::block::describe(7U, labels::status::deprecated, qa_registration::kQaGpib, qa_registration::kQaFamily, labels::role::sink, labels::emits::storage, labels::status::experimental);
        const gr::property_map map       = gr::block::attributesToMap(kDeclared, labels::role::consumer);
        expect(wordsAt(map, "labels") == std::vector<std::string>{"family/qa", "role/sink", "holds/gpib", "emits/storage", "status/deprecated", "status/experimental"});

        std::vector<std::string> rejected;
        const AttributesRead     read = gr::block::attributesFromMap(map, gr::block::coreVocabulary(), rejected);
        expect(rejected.empty()) << "well-formed words of known classes reject nothing";
        expect(eq(read.version, Version{7U}));
        expect(eq(read.readRole, "consumer"s));
        expect(eq(read.role(), "sink"sv));
        expect(gr::block::attributesToMap(read) == map);
        expect(read.words(LabelClass::Status) == std::vector<std::string_view>{"deprecated", "experimental"});

        const auto gpib = std::ranges::find(read.labels, "gpib"s, &LabelRead::word);
        expect(fatal(gpib != read.labels.cend())) << "a word core does not define is kept";
        expect(!gpib->known) << "and marked outside the vocabulary it was read against";
        expect(std::ranges::find(read.labels, "sink"s, &LabelRead::word)->known) << "a core word is known";

        gr::BlockRegistry registry;
        expect(qa_registration::insertAs<qa_registration::ControlOnly>(registry, "qa::ControlOnly"));
        expect(qa_registration::insertAs<qa_registration::RadioSource>(registry, "qa::RadioSource"));
        const AttributesRead againstRegistry = gr::block::attributesFromMap(map, registry.vocabulary());
        expect(std::ranges::all_of(againstRegistry.labels, &LabelRead::known)) << "a registration brings its words into the registry's vocabulary";

        const gr::property_map nothingKnown = gr::block::attributesToMap(gr::block::Attributes{}, std::nullopt);
        expect(eq(nothingKnown.size(), 1UZ)) << "only the version, which always has a value";
        expect(eq(nothingKnown.at("version").value_or(Version{}), gr::block::kDefaultVersion));
    };

    "a label under no class, a malformed word and a second word in a class of one are rejected with a line each"_test = [] {
        const gr::property_map   stated{{"version", std::string("two")}, {"role", std::int64_t{5}},
              {"labels", gr::Tensor<gr::pmt::Value>(std::pmr::vector<gr::pmt::Value>{gr::pmt::Value(std::string("rf")), gr::pmt::Value(std::string("shade/red")), gr::pmt::Value(std::string("emits/RF")), gr::pmt::Value(std::int64_t{3}), //
                             gr::pmt::Value(std::string("role/source")), gr::pmt::Value(std::string("role/sink")), gr::pmt::Value(std::string("holds/device")), gr::pmt::Value(std::string("holds/device")), gr::pmt::Value(std::string("emits/tachyon"))})}};
        std::vector<std::string> rejected;
        const AttributesRead     read = gr::block::attributesFromMap(stated, gr::block::coreVocabulary(), rejected);
        expect(fatal(eq(rejected.size(), 8UZ))) << "one line per rejected entry";
        expect(eq(rejected[0], "version: 'two' is not an integer from 0 to 4294967295"s));
        expect(eq(rejected[1], "labels: 'rf' is not class/word with a class of family, role, plane, holds, emits, ingests, compute, status"s));
        expect(eq(rejected[2], "labels: 'shade/red' is not class/word with a class of family, role, plane, holds, emits, ingests, compute, status"s));
        expect(eq(rejected[3], "labels: 'emits/RF' is not class/word with a word of a lower-case letter, then lower-case letters and digits"s));
        expect(eq(rejected[4], "labels: 3 is not a class/word string"s));
        expect(eq(rejected[5], "labels: 'role/sink' is a second word in role, which takes one; 'role/source' is kept"s));
        expect(eq(rejected[6], "labels: 'holds/device' is given twice"s));
        expect(eq(rejected[7], "role: 5 is not a word"s));

        expect(eq(read.version, gr::block::kDefaultVersion));
        const std::vector<std::string> kept = read.labels | std::views::transform(&LabelRead::text) | std::ranges::to<std::vector>();
        expect(kept == std::vector<std::string>{"role/source", "holds/device", "emits/tachyon"}) << "the first word of a class of one is kept, and an unknown word under a known class";

        std::vector<std::string> notAList;
        std::ignore = gr::block::attributesFromMap(gr::property_map{{"labels", std::string("holds/device")}}, gr::block::coreVocabulary(), notAList);
        expect(notAList == std::vector<std::string>{"labels: 'holds/device' is not a list of class/word strings"});
        expect(eq(gr::block::attributesFromMap(gr::property_map{{"version", std::int64_t{-1}}}).version, gr::block::kDefaultVersion));
        std::vector<std::string> tooLarge;
        expect(eq(gr::block::attributesFromMap(gr::property_map{{"version", std::int64_t{1} << 40}}, gr::block::coreVocabulary(), tooLarge).version, gr::block::kDefaultVersion));
        expect(tooLarge == std::vector<std::string>{"version: 1099511627776 is not an integer from 0 to 4294967295"}) << "a version past the largest one reads 1";
    };

    "the registry's vocabulary holds core's words and each registration's, with every meaning and the physical reading"_test = [] {
        gr::BlockRegistry registry;
        expect(qa_registration::insertAs<qa_registration::RadioSource>(registry, "qa::RadioSource"));
        expect(qa_registration::insertAs<qa_registration::ControlOnly>(registry, "qa::ControlOnly"));
        static constexpr auto kOtherMeaning = gr::block::describe(1U, labels::family("qa", "Radios the suite registers a second time."));
        expect(registry.insert("qa::Second", "", &qa_registration::makeBlock<qa_registration::Filter>, {}, gr::block::Attributes(kOtherMeaning).labels));

        const gr::block::Vocabulary& vocabulary = registry.vocabulary();
        const auto*                  source     = vocabulary.find(LabelClass::Role, "source");
        expect(fatal(source != nullptr)) << "core's words seed every registry";
        expect(source->meanings == std::vector<std::string>{std::string(labels::role::source.meaning)});

        const auto* family = vocabulary.find(LabelClass::Family, "qa");
        expect(fatal(family != nullptr));
        expect(family->meanings == std::vector<std::string>{"Radios of the registration suite.", "Radios the suite registers a second time."}) << "two meanings of one word, in registration order";
        expect(vocabulary.find(LabelClass::Holds, "gpib") != nullptr) << "a word of a class core defines, declared by a block";
        expect(vocabulary.find(LabelClass::Holds, "absent") == nullptr);

        expect(vocabulary.physical(LabelClass::Emits, "rf"));
        expect(vocabulary.physical(LabelClass::Emits, "time"));
        expect(!vocabulary.physical(LabelClass::Emits, "storage"));
        expect(!vocabulary.physical(LabelClass::Ingests, "text"));
        expect(vocabulary.physical(LabelClass::Emits, "tachyon")) << "an emits word the vocabulary does not know reads as physical";
        expect(eq(vocabulary.words(LabelClass::Compute).size(), 5UZ));
        expect(eq(vocabulary.words(LabelClass::Status).size(), 2UZ));

        std::vector<std::string>    rejected;
        const gr::block::Vocabulary copied = gr::block::vocabularyFromMap(gr::block::vocabularyToMap(vocabulary), rejected);
        expect(rejected.empty());
        expect(copied == vocabulary) << "the map form that crosses the plugin boundary keeps every meaning and flag";
    };

    "a vocabulary map entry under a malformed key or with a value that is not a map is skipped with a line each"_test = [] {
        const gr::property_map      stated{{"rf", gr::property_map{{"meaning", std::string("A word under no class.")}}}, {"holds/bus", std::int64_t{3}}, {"holds/good", gr::property_map{{"meaning", std::string("A word the map carries well.")}}}};
        std::vector<std::string>    rejected;
        const gr::block::Vocabulary read = gr::block::vocabularyFromMap(stated, rejected);
        expect(fatal(eq(rejected.size(), 2UZ))) << "one line per skipped entry";
        expect(std::ranges::contains(rejected, "vocabulary: 'rf' is not class/word with a class of family, role, plane, holds, emits, ingests, compute, status"s));
        expect(std::ranges::contains(rejected, "vocabulary: 'holds/bus' carries 3, not a map of meaning and physical"s));
        expect(eq(read.entries().size(), 1UZ)) << "the well-formed entry stands";
        const gr::block::VocabularyEntry* good = read.find(LabelClass::Holds, "good");
        expect(fatal(good != nullptr));
        expect(good->meanings == std::vector<std::string>{"A word the map carries well."});
    };

    "the identity rule finds a family's source and sink by their declared labels alone"_test = [] {
        gr::BlockRegistry registry;
        expect(qa_registration::insertAs<qa_registration::RadioSource>(registry, "qa::RadioSource"));
        expect(qa_registration::insertAs<qa_registration::RadioSink>(registry, "qa::RadioSink"));
        expect(qa_registration::insertAs<qa_registration::FpgaTransform>(registry, "qa::FpgaTransform"));
        expect(qa_registration::insertAs<qa_registration::RadioControlledSource>(registry, "qa::RadioControlledSource"));

        const auto keysWith = [&registry](const Label& family, const Label& role) {
            std::vector<std::string> keys;
            for (const std::string& key : registry.keys()) {
                const AttributesRead read = gr::block::attributesFromMap(registry.attributes(key).value_or(gr::property_map{}));
                if (read.has(family) && read.has(role)) {
                    keys.push_back(key);
                }
            }
            return keys;
        };
        expect(keysWith(qa_registration::kQaFamily, labels::role::source) == std::vector<std::string>{"qa::RadioSource"}) << "a source whose ports read generator, by its declared role";
        expect(keysWith(qa_registration::kQaFamily, labels::role::sink) == std::vector<std::string>{"qa::RadioSink"});
        expect(keysWith(qa_registration::kQaFamily, labels::role::generator).empty()) << "a role read from the ports never matches";
        expect(keysWith(qa_registration::kOtherFamily, labels::role::source).empty());
    };

    "the declared status words are independent and may be set together"_test = [] {
        const gr::property_map map = registeredMap<qa_registration::FilterV1>();
        expect(wordsAt(map, "labels") == std::vector<std::string>{"status/deprecated", "status/experimental"});
        expect(eq(map.at("version").value_or(Version{}), Version{1U}));

        const gr::BlockWrapper<qa_registration::FilterV1> wrapper;
        expect(wrapper.status() == std::vector<std::string>{"deprecated", "experimental"});
        expect(carriesEntry(wrapper, map));
        expect(!wrapper.metaInformation().contains("Version"sv) && !wrapper.metaInformation().contains("Status"sv)) << "both live inside the Attributes entry";
    };

    "two versions of one key coexist, and the newest is what a caller that names none gets"_test = [] {
        gr::BlockRegistry registry;
        expect(qa_registration::insertAs<qa_registration::FilterV1>(registry, "qa::Filter"));
        expect(qa_registration::insertAs<qa_registration::FilterV2>(registry, "qa::Filter")) << "a new version under a known key contributes";

        expect(eq(registry.keys().size(), 1UZ)) << "one key, two versions";
        const std::vector<Version> registered = registry.versions("qa::Filter");
        expect(eq(registered.size(), 2UZ));
        if (registered.size() == 2UZ) {
            expect(eq(registered[0], Version{1U})) << "oldest first";
            expect(eq(registered[1], Version{2U}));
        }
        expect(registry.newestVersion("qa::Filter") == std::optional<Version>{2U});

        const std::unique_ptr<gr::BlockModel> newest = registry.create("qa::Filter", {});
        expect(newest != nullptr);
        if (newest != nullptr) {
            expect(eq(newest->version(), Version{2U}));
            expect(!newest->pinnedVersion().has_value()) << "taking the newest is not pinning";
        }
    };

    "a pinned version is the one that is built, and the instance remembers it was pinned"_test = [] {
        gr::BlockRegistry registry;
        expect(qa_registration::insertAs<qa_registration::FilterV1>(registry, "qa::Filter"));
        expect(qa_registration::insertAs<qa_registration::FilterV2>(registry, "qa::Filter"));

        const std::unique_ptr<gr::BlockModel> pinned = registry.create("qa::Filter", Version{1U}, {});
        expect(pinned != nullptr);
        if (pinned != nullptr) {
            expect(eq(pinned->version(), Version{1U}));
            expect(pinned->pinnedVersion() == std::optional<Version>{1U});
            expect(std::ranges::contains(pinned->status(), "deprecated"s));
        }

        expect(registry.create("qa::Filter", Version{7U}, {}) == nullptr) << "a version that was never registered is a miss, not a substitution";
    };

    "the registry answers each registration's map, the newest when no version is named"_test = [] {
        gr::BlockRegistry registry;
        expect(qa_registration::insertAs<qa_registration::FilterV1>(registry, "qa::Filter"));
        expect(qa_registration::insertAs<qa_registration::FilterV2>(registry, "qa::Filter"));

        const std::optional<gr::property_map> newest = registry.attributes("qa::Filter");
        expect(fatal(newest.has_value()));
        expect(*newest == registeredMap<qa_registration::FilterV2>());

        const std::optional<gr::property_map> older = registry.attributes("qa::Filter", 1U);
        expect(fatal(older.has_value()));
        expect(*older == registeredMap<qa_registration::FilterV1>());

        expect(!registry.attributes("qa::Filter", 3U).has_value());
        expect(!registry.attributes("qa::NoSuchBlock").has_value());
        expect(!registry.attributes("qa::NoSuchBlock", 1U).has_value());
    };

    "the registry reports each version's status words without acting on them"_test = [] {
        gr::BlockRegistry registry;
        expect(qa_registration::insertAs<qa_registration::FilterV1>(registry, "qa::Filter"));
        expect(qa_registration::insertAs<qa_registration::FilterV2>(registry, "qa::Filter"));

        expect(registry.status("qa::Filter", 1U) == std::optional<std::vector<std::string>>{{"deprecated", "experimental"}});
        expect(registry.status("qa::Filter", 2U) == std::optional<std::vector<std::string>>{std::vector<std::string>{}});
        expect(!registry.status("qa::Filter", 3U).has_value());
        expect(!registry.status("qa::NoSuchBlock", 1U).has_value());
        expect(registry.versions("qa::NoSuchBlock").empty());
        expect(!registry.newestVersion("qa::NoSuchBlock").has_value());
    };

    "one key and one version registered twice is the collision it always was: the last wins"_test = [] {
        gr::BlockRegistry registry;
        expect(qa_registration::insertAs<qa_registration::FilterV1>(registry, "qa::Filter"));
        expect(!qa_registration::insertAs<qa_registration::FilterV1>(registry, "qa::Filter")) << "nothing new";
        expect(eq(registry.versions("qa::Filter").size(), 1UZ));
    };

    "what the generated definition unit hands over carries the declarations"_test = [] {
        const gr::BlockRegistration registration = gr::makeBlockRegistration<qa_registration::FilterV1>(&qa_registration::makeBlock<qa_registration::FilterV1>);
        expect(registration.attributes == gr::block::attributesToMap(gr::block::attributesOf<qa_registration::FilterV1>(), labels::role::processor));
        expect(std::ranges::equal(registration.labels, std::array{labels::status::deprecated, labels::status::experimental}));

        gr::BlockRegistry registry;
        expect(gr::insertBlockFactory(registry, registration));
        expect(registry.status(gr::meta::type_name<qa_registration::FilterV1>(), 1U) == std::optional<std::vector<std::string>>{{"deprecated", "experimental"}});
    };

    "the registry files an entry under the version its map states, and 1 for an empty map"_test = [] {
        gr::BlockRegistry registry;
        expect(registry.insert("qa::Stated", "", &qa_registration::makeBlock<qa_registration::Filter>, gr::block::attributesToMap(gr::block::Attributes{.version = 3U}, std::nullopt)));
        expect(registry.insert("qa::Stated", "", &qa_registration::makeBlock<qa_registration::Filter>, gr::property_map{}));
        expect(registry.versions("qa::Stated") == std::vector<Version>{1U, 3U});
    };
};

int main() { /* tests are statically registered */ }
