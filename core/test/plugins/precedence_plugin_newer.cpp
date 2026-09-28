#include <gnuradio-4.0/Plugin.hpp>

GR_PLUGIN("Precedence Plugin Newer", "Unknown", "MIT", "v1")

/// a plugin that offers the key of the two precedence plugins at version 2, which coexists with their version 1
namespace precedence {

struct Newer : gr::Block<Newer> {
    using Description = gr::Doc<"version 2 of the block the precedence plugins offer">;

    static constexpr auto attributes = gr::block::describe(2U);

    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(Newer, in, out);

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value; }
};

} // namespace precedence

const bool registered [[maybe_unused]] = grPluginBlockRegistry().insert<precedence::Newer>("=test::precedence");
