#include <gnuradio-4.0/Plugin.hpp>

GR_PLUGIN("Precedence Plugin Second", "Unknown", "MIT", "v1")

/// one of two plugins that offer one key at one version; the loader holds the key for the one it loads first
namespace precedence {

struct Second : gr::Block<Second> {
    using Description = gr::Doc<"a block the second precedence plugin offers under the shared key">;

    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(Second, in, out);

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value; }
};

} // namespace precedence

const bool registered [[maybe_unused]] = grPluginBlockRegistry().insert<precedence::Second>("=test::precedence");
