#include <cstddef>
#include <cstdio>
#include <print>
#include <string>

#include <gnuradio-4.0/Plugin.hpp>

GR_PLUGIN("rungraph Fixture Plugin", "Unknown", "MIT", "v1")

/// Blocks for the cases that run rungraph. The build compiles this plugin twice, into two directories, with a different
/// GR_TEST_PLUGIN_ORIGIN each, so that a case can read which of the two supplied a block.
namespace fixture {

/// a block whose `origin` names the build of the plugin it came from, and which ends its run at once
struct Origin : gr::Block<Origin> {
    using Description = gr::Doc<"a block that names the plugin build it came from">;

    gr::Annotated<std::string, "origin", gr::Doc<"the plugin build the block came from">> origin = std::string(GR_TEST_PLUGIN_ORIGIN);

    GR_MAKE_REFLECTABLE(Origin, origin);

    [[nodiscard]] constexpr gr::work::Status processBulk() const noexcept { return gr::work::Status::DONE; }
};

/// a block that reports one error on the message plane and then fails its run or goes on without end
struct Fault : gr::Block<Fault> {
    using Description = gr::Doc<"a block that reports one error">;

    gr::Annotated<bool, "fails", gr::Doc<"return an error status after the report">> fails = false;

    GR_MAKE_REFLECTABLE(Fault, fails);

    bool _reported = false;

    [[nodiscard]] gr::work::Status processBulk() {
        if (!_reported) {
            _reported = true;
            this->emitErrorMessage("processBulk", "the fixture reports a fault");
            if (fails) {
                return gr::work::Status::ERROR;
            }
        }
        return gr::work::Status::OK;
    }
};

/// a sink that prints each `level` it applies and the samples it had taken by then, one line on standard output each
struct LevelSink : gr::Block<LevelSink> {
    using Description = gr::Doc<"a sink that prints each level it applies">;

    gr::PortIn<float>                                                 in;
    gr::Annotated<float, "level", gr::Doc<"a value the sink prints">> level = 0.f;

    GR_MAKE_REFLECTABLE(LevelSink, in, level);

    std::size_t _received = 0UZ;

    void settingsChanged(const gr::property_map& /*oldSettings*/, const gr::property_map& newSettings) {
        if (newSettings.contains("level")) {
            std::println("{}: level {} applied after {} samples", this->name.value, level.value, _received);
            std::fflush(stdout);
        }
    }

    void processOne(float /*sample*/) { ++_received; }
};

} // namespace fixture

const bool registeredOrigin [[maybe_unused]]    = grPluginBlockRegistry().insert<fixture::Origin>("=test::origin");
const bool registeredFault [[maybe_unused]]     = grPluginBlockRegistry().insert<fixture::Fault>("=test::fault");
const bool registeredLevelSink [[maybe_unused]] = grPluginBlockRegistry().insert<fixture::LevelSink>("=test::level_sink");
