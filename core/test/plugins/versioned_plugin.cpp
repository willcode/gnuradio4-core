#include <gnuradio-4.0/Plugin.hpp>

GR_PLUGIN("Versioned Plugin", "Unknown", "MIT", "v1")

/// a plugin carrying two revisions of one alias, which only the versioned plugin interface can tell apart, a notation
/// block and a block whose declared role its ports contradict
namespace good {

inline constexpr gr::block::Label kVersionedFamily = gr::block::labels::family("versioned", "Blocks of the versioned test plugin.");
inline constexpr gr::block::Label kTestBus{gr::block::LabelClass::Holds, "testbus", "Opens the test plugin's own bus."};

struct VersionedFirst : gr::Block<VersionedFirst> {
    using Description = gr::Doc<"the older revision of the block this plugin registers twice">;

    static constexpr auto attributes = gr::block::describe(1U);

    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(VersionedFirst, in, out);

    explicit VersionedFirst(gr::property_map init = {}) : gr::Block<VersionedFirst>(std::move(init)) {}

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value; }
};

struct VersionedSecond : gr::Block<VersionedSecond> {
    using Description = gr::Doc<"the newer revision, which an unpinned create takes">;

    static constexpr auto attributes = gr::block::describe(2U, kVersionedFamily, gr::block::labels::holds::device, kTestBus, gr::block::labels::status::experimental);

    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(VersionedSecond, in, out);

    explicit VersionedSecond(gr::property_map init = {}) : gr::Block<VersionedSecond>(std::move(init)) {}

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return 2.0f * value; }
};

struct Antenna : gr::Block<Antenna> {
    using Description = gr::Doc<"an antenna drawn beside the block it feeds">;

    static constexpr auto attributes = gr::block::describe(1U, gr::block::labels::plane::notation);

    gr::Annotated<std::string, "feeds", gr::Doc<"the block the antenna feeds">>       feeds;
    gr::Annotated<std::string, "fed by", gr::Doc<"the block that feeds the antenna">> fed_by;

    GR_MAKE_REFLECTABLE(Antenna, feeds, fed_by);

    [[nodiscard]] constexpr gr::work::Status processBulk() const noexcept { return gr::work::Status::DONE; }
};

/// a block that declares the source role while its one stream port is an input, which a listing notes
struct MislabeledSource : gr::Block<MislabeledSource> {
    using Description = gr::Doc<"a declared source whose stream port is an input">;

    static constexpr auto attributes = gr::block::describe(1U, gr::block::labels::role::source);

    gr::PortIn<float> in;

    GR_MAKE_REFLECTABLE(MislabeledSource, in);

    void processOne(float) const noexcept {}
};

} // namespace good

const bool registeredFirst [[maybe_unused]]      = grPluginBlockRegistry().insert<good::VersionedFirst>("=test::versioned");
const bool registeredSecond [[maybe_unused]]     = grPluginBlockRegistry().insert<good::VersionedSecond>("=test::versioned");
const bool registeredAntenna [[maybe_unused]]    = grPluginBlockRegistry().insert<good::Antenna>("=test::antenna");
const bool registeredMislabeled [[maybe_unused]] = grPluginBlockRegistry().insert<good::MislabeledSource>("=test::mislabeled");
