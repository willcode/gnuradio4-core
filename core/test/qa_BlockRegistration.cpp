#include <boost/ut.hpp>

#include <gnuradio-4.0/Block.hpp>
#include <gnuradio-4.0/BlockAttributes.hpp>
#include <gnuradio-4.0/BlockRegistry.hpp>
#include <gnuradio-4.0/PluginLoader.hpp>

#include <cstdint>
#include <memory>
#include <optional>
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

struct FilterV1 : gr::Block<FilterV1> {
    static constexpr gr::block::Attributes attributes{.status = {.deprecated = true, .experimental = true}, .version = 1U};

    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(FilterV1, in, out);

    explicit FilterV1(gr::property_map init = {}) : gr::Block<FilterV1>(std::move(init)) {}

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value; }
};

struct FilterV2 : gr::Block<FilterV2> {
    static constexpr gr::block::Attributes attributes{.version = 2U};

    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(FilterV2, in, out);

    explicit FilterV2(gr::property_map init = {}) : gr::Block<FilterV2>(std::move(init)) {}

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value; }
};

/// blocks that hold a device, one per port shape, and a processing block that declares it holds nothing
struct RadioSource : gr::Block<RadioSource> {
    static constexpr gr::block::Attributes attributes{.resource = gr::block::Resource::Device, .family = "qa"};

    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(RadioSource, out);

    explicit RadioSource(gr::property_map init = {}) : gr::Block<RadioSource>(std::move(init)) {}

    [[nodiscard]] constexpr float processOne() const noexcept { return 0.0f; }
};

struct RadioSink : gr::Block<RadioSink> {
    static constexpr gr::block::Attributes attributes{.resource = gr::block::Resource::Device, .family = "qa", .emits = gr::block::Emits::Rf};

    gr::PortIn<float> in;

    GR_MAKE_REFLECTABLE(RadioSink, in);

    explicit RadioSink(gr::property_map init = {}) : gr::Block<RadioSink>(std::move(init)) {}

    void processOne(float) const noexcept {}
};

struct RadioTransceiver : gr::Block<RadioTransceiver> {
    static constexpr gr::block::Attributes attributes{.resource = gr::block::Resource::Device, .family = "qa", .compute = gr::block::Compute::Fpga, .status = {.experimental = true}, .version = 2U};

    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(RadioTransceiver, in, out);

    explicit RadioTransceiver(gr::property_map init = {}) : gr::Block<RadioTransceiver>(std::move(init)) {}

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value; }
};

/// a device source with a message input of its own; only its type is read
struct RadioControlledSource : gr::Block<RadioControlledSource> {
    static constexpr gr::block::Attributes attributes{.resource = gr::block::Resource::Device, .family = "qa"};

    gr::MsgPortIn      control;
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(RadioControlledSource, control, out);

    [[nodiscard]] constexpr float processOne() const noexcept { return 0.0f; }
};

/// a device sink whose inputs are one dynamic collection; only its type is read
struct RadioCombiningSink : gr::Block<RadioCombiningSink> {
    static constexpr gr::block::Attributes attributes{.resource = gr::block::Resource::Device, .family = "qa"};

    std::vector<gr::PortIn<float>> inputs;

    GR_MAKE_REFLECTABLE(RadioCombiningSink, inputs);

    template<gr::InputSpanLike TInSpan>
    gr::work::Status processBulk(std::span<TInSpan>&) const noexcept {
        return gr::work::Status::OK;
    }
};

struct DeclaredProcessor : gr::Block<DeclaredProcessor> {
    static constexpr gr::block::Attributes attributes{.resource = gr::block::Resource::None};

    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(DeclaredProcessor, in, out);

    explicit DeclaredProcessor(gr::property_map init = {}) : gr::Block<DeclaredProcessor>(std::move(init)) {}

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value; }
};

template<typename TBlock>
std::unique_ptr<gr::BlockModel> makeBlock(gr::property_map params) {
    return std::make_unique<gr::BlockWrapper<TBlock>>(std::move(params));
}

/// registers `TBlock` under `key`, keeping the version and the attributes the type declares
template<typename TBlock>
bool insertAs(gr::BlockRegistry& registry, std::string_view key) {
    const gr::BlockRegistration declared = gr::makeBlockRegistration<TBlock>(&makeBlock<TBlock>);
    return registry.insert(key, "", declared.factory, declared.attributes);
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
    using gr::block::Attributes;
    using gr::block::Compute;
    using gr::block::Emits;
    using gr::block::Resource;
    using gr::block::Role;
    using gr::block::Status;
    using gr::block::Version;
    using qa_registration::carriesEntry;
    using qa_registration::registeredMap;
    using qa_registration::wordAt;
    using qa_registration::wordsAt;

    "a block that declares nothing reads unknown everywhere and carries no Attributes key"_test = [] {
        static_assert(!gr::block::HasDeclaredAttributes<qa_registration::Filter>);
        expect(gr::block::attributesOf<qa_registration::Filter>() == Attributes{});
        expect(gr::block::roleOf<qa_registration::Filter>() == Role::Unknown);

        const auto undeclared = std::make_shared<gr::BlockWrapper<qa_registration::Filter>>();
        expect(eq(undeclared->version(), gr::block::kDefaultVersion));
        expect(undeclared->status() == Status{});
        expect(gr::block::attributesOf(*undeclared) == Attributes{});
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
        expect(gr::insertBlockFactory(registry, registration));
        expect(registry.attributes(registration.name) == std::optional<gr::property_map>{gr::property_map{}}) << "registered, and declaring nothing";
    };

    "a declared block reads its own words on the type, the instance and the registry"_test = [] {
        static_assert(gr::block::HasDeclaredAttributes<qa_registration::RadioTransceiver>);
        constexpr Attributes declared = gr::block::attributesOf<qa_registration::RadioTransceiver>();
        expect(declared == Attributes{.resource = Resource::Device, .family = "qa", .compute = Compute::Fpga, .status = {.experimental = true}, .version = 2U});

        const gr::BlockRegistration registration = gr::makeBlockRegistration<qa_registration::RadioTransceiver>(&qa_registration::makeBlock<qa_registration::RadioTransceiver>);
        const gr::property_map&     map          = registration.attributes;
        expect(eq(wordAt(map, "resource"), "device"sv));
        expect(eq(wordAt(map, "family"), "qa"sv));
        expect(eq(wordAt(map, "compute"), "fpga"sv));
        expect(eq(wordAt(map, "role"), "transceiver"sv));
        expect(!map.contains("emits"sv)) << "an unknown value writes no key";
        expect(wordsAt(map, "status") == std::vector<std::string>{"experimental"});
        expect(eq(map.at("version").value_or(Version{}), Version{2U}));

        const gr::BlockWrapper<qa_registration::RadioTransceiver> block;
        expect(carriesEntry(block, map)) << "the instance carries the map the registration carries";
        expect(gr::block::attributesOf(block) == declared);
        expect(eq(block.version(), Version{2U}));
        expect(block.status() == Status{.experimental = true});

        gr::BlockRegistry registry;
        expect(gr::insertBlockFactory(registry, registration));
        expect(registry.attributes(registration.name) == std::optional<gr::property_map>{map}) << "the registry keeps the same map";
        expect(registry.versions(registration.name) == std::vector<Version>{2U}) << "and files it under the version the map states";
    };

    "a direct set() leaves the Attributes entry to the type and returns the key as not applied"_test = [] {
        const std::pmr::string key(gr::block::kAttributesMetaKey);
        constexpr Attributes   declared = gr::block::attributesOf<qa_registration::RadioTransceiver>();
        const gr::property_map own      = registeredMap<qa_registration::RadioTransceiver>();
        const gr::property_map another  = gr::block::attributesToMap(Attributes{.status = {.deprecated = true}, .version = Version{declared.version + 1U}}, Role::Unknown);
        expect(another != own);

        gr::BlockWrapper<qa_registration::RadioTransceiver> block;
        expect(carriesEntry(block, own));

        const gr::property_map notSet = block.settings().set({{"qa_note", gr::pmt::Value(std::string("kept"))}, {key, gr::pmt::Value(another)}});
        expect(notSet.contains("qa_note"sv) && notSet.contains(key)) << "set() returns both keys it did not apply";
        expect(block.metaInformation().contains("qa_note"sv)) << "an undeclared key is filed in meta_information";
        expect(carriesEntry(block, own)) << "the Attributes entry is not";
        expect(gr::block::attributesOf(block) == declared);
        expect(eq(block.version(), declared.version));
        expect(block.status() == declared.status);

        expect(block.settings().setStaged({{key, gr::pmt::Value(another)}}).contains(key));
        expect(block.settings().applyStagedParameters().appliedParameters.empty());
        expect(carriesEntry(block, own)) << "the staged path leaves the entry as well";

        block.settings().loadParametersFromPropertyMap({{key, gr::pmt::Value(another)}});
        expect(carriesEntry(block, own)) << "and so does the graph-file path";

        gr::BlockWrapper<qa_registration::Filter> undeclared;
        expect(undeclared.settings().set({{key, gr::pmt::Value(another)}}).contains(key));
        expect(!undeclared.metaInformation().contains(key)) << "a type that declares nothing carries no Attributes entry";
        expect(gr::block::attributesOf(undeclared) == Attributes{});
        expect(eq(undeclared.version(), gr::block::kDefaultVersion));
        expect(undeclared.status() == Status{});
    };

    "a device block's role follows its stream ports"_test = [] {
        expect(gr::block::roleOf<qa_registration::RadioSource>() == Role::Source);
        expect(gr::block::roleOf<qa_registration::RadioSink>() == Role::Sink);
        expect(gr::block::roleOf<qa_registration::RadioTransceiver>() == Role::Transceiver);
        expect(gr::block::roleOf<qa_registration::RadioCombiningSink>() == Role::Sink) << "a dynamic port collection counts as one port";
        expect(gr::block::roleOf<qa_registration::RadioControlledSource>() == Role::Source) << "a message port counts for nothing";

        const gr::property_map sourceMap = registeredMap<qa_registration::RadioSource>();
        const gr::property_map sinkMap   = registeredMap<qa_registration::RadioSink>();
        expect(eq(wordAt(sourceMap, "role"), "source"sv));
        expect(eq(wordAt(sinkMap, "role"), "sink"sv));
        expect(eq(wordAt(sinkMap, "emits"), "rf"sv));
        expect(!sourceMap.contains("emits"sv));

        const gr::BlockWrapper<qa_registration::RadioSource> source;
        const gr::BlockWrapper<qa_registration::RadioSink>   sink;
        expect(carriesEntry(source, sourceMap));
        expect(carriesEntry(sink, sinkMap));
    };

    "the role rule depends on the resource and the two port kinds alone"_test = [] {
        using gr::block::roleFrom;
        for (const Resource resource : {Resource::Device, Resource::File, Resource::Network}) {
            expect(roleFrom(resource, false, true) == Role::Source);
            expect(roleFrom(resource, true, false) == Role::Sink);
            expect(roleFrom(resource, true, true) == Role::Transceiver);
            expect(roleFrom(resource, false, false) == Role::Unknown);
        }
        for (const Resource resource : {Resource::Unknown, Resource::None}) {
            expect(roleFrom(resource, false, true) == Role::Unknown);
            expect(roleFrom(resource, true, false) == Role::Unknown);
            expect(roleFrom(resource, true, true) == Role::Unknown);
        }
    };

    "a processing block that declares it holds nothing derives no role"_test = [] {
        expect(gr::block::roleOf<qa_registration::DeclaredProcessor>() == Role::Unknown);
        expect(gr::block::roleOf<qa_registration::FilterV2>() == Role::Unknown) << "a declaration without a resource derives none either";

        const gr::property_map map = registeredMap<qa_registration::DeclaredProcessor>();
        expect(eq(wordAt(map, "resource"), "none"sv));
        expect(!map.contains("role"sv));

        const gr::BlockWrapper<qa_registration::DeclaredProcessor> block;
        expect(carriesEntry(block, map));
    };

    "every word on the wire names one enumerator"_test = [] {
        auto readResource = [](std::string_view word) { return gr::block::attributesFromMap(gr::property_map{{"resource", std::string(word)}}).resource; };
        expect(readResource("none") == Resource::None);
        expect(readResource("device") == Resource::Device);
        expect(readResource("file") == Resource::File);
        expect(readResource("network") == Resource::Network);

        auto readEmits = [](std::string_view word) { return gr::block::attributesFromMap(gr::property_map{{"emits", std::string(word)}}).emits; };
        expect(readEmits("none") == Emits::None);
        expect(readEmits("rf") == Emits::Rf);
        expect(readEmits("audio") == Emits::Audio);

        auto readCompute = [](std::string_view word) { return gr::block::attributesFromMap(gr::property_map{{"compute", std::string(word)}}).compute; };
        expect(readCompute("host") == Compute::Host);
        expect(readCompute("gpu") == Compute::Gpu);
        expect(readCompute("tpu") == Compute::Tpu);
        expect(readCompute("fpga") == Compute::Fpga);
        expect(readCompute("remote") == Compute::Remote);

        auto writtenRole = [](Role role) { return std::string(wordAt(gr::block::attributesToMap(Attributes{}, role), "role")); };
        expect(eq(writtenRole(Role::Source), "source"s));
        expect(eq(writtenRole(Role::Sink), "sink"s));
        expect(eq(writtenRole(Role::Transceiver), "transceiver"s));

        const gr::property_map   stated{{"resource", std::string("device")}, {"family", std::string("uhd")}, {"compute", std::string("fpga")}, {"role", std::string("transceiver")}, {"status", std::vector<std::string>{"experimental"}}, {"version", std::int64_t{2}}};
        std::vector<std::string> rejected;
        expect(gr::block::attributesFromMap(stated, rejected) == Attributes{.resource = Resource::Device, .family = "uhd", .compute = Compute::Fpga, .status = {.experimental = true}, .version = 2U});
        expect(rejected.empty()) << "a map of known words rejects nothing";

        const gr::property_map nothingKnown = gr::block::attributesToMap(Attributes{}, Role::Unknown);
        expect(eq(nothingKnown.size(), 1UZ)) << "only the version, which always has a value";
        expect(eq(nothingKnown.at("version").value_or(Version{}), gr::block::kDefaultVersion));
    };

    "the map round-trips through both conversions"_test = [] {
        const Attributes       full{.resource = Resource::Network, .family = "net", .emits = Emits::Audio, .compute = Compute::Remote, .status = {.deprecated = true, .experimental = true}, .version = 7U};
        const gr::property_map map = gr::block::attributesToMap(full, Role::Transceiver);
        expect(gr::block::attributesFromMap(map) == full);
        expect(gr::block::attributesToMap(gr::block::attributesFromMap(map), Role::Transceiver) == map);
        expect(wordsAt(map, "status") == std::vector<std::string>{"deprecated", "experimental"});

        for (const Resource resource : {Resource::Unknown, Resource::None, Resource::Device, Resource::File, Resource::Network}) {
            expect(gr::block::attributesFromMap(gr::block::attributesToMap(Attributes{.resource = resource}, Role::Unknown)).resource == resource);
        }
        for (const Emits emits : {Emits::Unknown, Emits::None, Emits::Rf, Emits::Audio}) {
            expect(gr::block::attributesFromMap(gr::block::attributesToMap(Attributes{.emits = emits}, Role::Unknown)).emits == emits);
        }
        for (const Compute compute : {Compute::Unknown, Compute::Host, Compute::Gpu, Compute::Tpu, Compute::Fpga, Compute::Remote}) {
            expect(gr::block::attributesFromMap(gr::block::attributesToMap(Attributes{.compute = compute}, Role::Unknown)).compute == compute);
        }
    };

    "a word outside an attribute's set reads unknown"_test = [] {
        const gr::property_map stated{{"resource", std::string("satellite")}, {"family", std::int64_t{3}}, {"emits", std::string("RF")}, {"compute", std::string("quantum")}, {"role", std::string("sideways")}, {"status", std::vector<std::string>{"retired", "deprecated"}}, {"version", std::string("two")}};
        expect(gr::block::attributesFromMap(stated) == Attributes{.status = {.deprecated = true}}) << "the known status word survives the unknown one beside it";

        std::vector<std::string> rejected;
        std::ignore = gr::block::attributesFromMap(stated, rejected);
        expect(fatal(eq(rejected.size(), 6UZ))) << "one line per rejected key, and none for role, which is not read";
        expect(eq(rejected[0], "resource: 'satellite' is not one of none, device, file, network"s));
        expect(eq(rejected[1], "emits: 'RF' is not one of none, rf, audio"s));
        expect(eq(rejected[2], "compute: 'quantum' is not one of host, gpu, tpu, fpga, remote"s));
        expect(rejected[3].starts_with("family: ") && rejected[3].ends_with(" is not a word")) << rejected[3];
        expect(rejected[4].starts_with("status: ") && rejected[4].contains("retired") && rejected[4].ends_with(" is not a list of deprecated and experimental")) << rejected[4];
        expect(eq(rejected[5], "version: 'two' is not a non-negative integer"s));

        expect(eq(gr::block::attributesFromMap(gr::property_map{{"version", std::int64_t{-1}}}).version, gr::block::kDefaultVersion));
        expect(eq(gr::block::attributesFromMap(gr::property_map{{"version", std::int64_t{1} << 40}}).version, gr::block::kDefaultVersion));
        expect(gr::block::attributesFromMap(gr::property_map{{"status", std::string("deprecated")}}).status == Status{}) << "status is a list, not a word";
    };

    "the declared flags are independent and may be set together"_test = [] {
        constexpr Status both = gr::block::attributesOf<qa_registration::FilterV1>().status;
        expect(both.deprecated);
        expect(both.experimental);
        expect(both.any());

        const gr::property_map map = registeredMap<qa_registration::FilterV1>();
        expect(wordsAt(map, "status") == std::vector<std::string>{"deprecated", "experimental"});
        expect(eq(map.at("version").value_or(Version{}), Version{1U}));

        const gr::BlockWrapper<qa_registration::FilterV1> wrapper;
        expect(wrapper.status() == both);
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
            expect(pinned->status().deprecated);
        }

        expect(registry.create("qa::Filter", Version{7U}, {}) == nullptr) << "a version that was never registered is a miss, not a substitution";
    };

    "the registry answers each registration's map, the newest when no version is named"_test = [] {
        gr::BlockRegistry registry;
        expect(qa_registration::insertAs<qa_registration::FilterV1>(registry, "qa::Filter"));
        expect(qa_registration::insertAs<qa_registration::FilterV2>(registry, "qa::Filter"));

        const std::optional<gr::property_map> newest = registry.attributes("qa::Filter");
        expect(fatal(newest.has_value()));
        expect(gr::block::attributesFromMap(*newest) == gr::block::attributesOf<qa_registration::FilterV2>());

        const std::optional<gr::property_map> older = registry.attributes("qa::Filter", 1U);
        expect(fatal(older.has_value()));
        expect(gr::block::attributesFromMap(*older) == gr::block::attributesOf<qa_registration::FilterV1>());

        expect(!registry.attributes("qa::Filter", 3U).has_value());
        expect(!registry.attributes("qa::NoSuchBlock").has_value());
        expect(!registry.attributes("qa::NoSuchBlock", 1U).has_value());
    };

    "the registry reports each version's status without acting on it"_test = [] {
        gr::BlockRegistry registry;
        expect(qa_registration::insertAs<qa_registration::FilterV1>(registry, "qa::Filter"));
        expect(qa_registration::insertAs<qa_registration::FilterV2>(registry, "qa::Filter"));

        expect(registry.status("qa::Filter", 1U) == std::optional<Status>{Status{.deprecated = true, .experimental = true}});
        expect(registry.status("qa::Filter", 2U) == std::optional<Status>{Status{}});
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
        expect(registration.attributes == gr::block::attributesToMap(gr::block::attributesOf<qa_registration::FilterV1>(), Role::Unknown));

        gr::BlockRegistry registry;
        expect(gr::insertBlockFactory(registry, registration));
        expect(registry.status(gr::meta::type_name<qa_registration::FilterV1>(), 1U) == std::optional<Status>{Status{.deprecated = true, .experimental = true}});
    };

    "the registry files an entry under the version its map states, and 1 for an empty map"_test = [] {
        gr::BlockRegistry registry;
        expect(registry.insert("qa::Stated", "", &qa_registration::makeBlock<qa_registration::Filter>, gr::block::attributesToMap(Attributes{.version = 3U}, Role::Unknown)));
        expect(registry.insert("qa::Stated", "", &qa_registration::makeBlock<qa_registration::Filter>, gr::property_map{}));
        expect(registry.versions("qa::Stated") == std::vector<Version>{1U, 3U});
    };
};

int main() { /* tests are statically registered */ }
