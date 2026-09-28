#include <gnuradio-4.0/Block.hpp>
#include <gnuradio-4.0/BlockRegistry.hpp>

/// one of two block libraries that register one alias at one version; the registry holds it for the one mapped first
namespace gr::testing {

struct PrecedenceSecond : gr::Block<PrecedenceSecond> {
    using Description = gr::Doc<"a block the second precedence library registers under the shared alias">;

    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(PrecedenceSecond, in, out);

    explicit PrecedenceSecond(gr::property_map init = {}) : gr::Block<PrecedenceSecond>(std::move(init)) {}

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value; }
};

const bool registered [[maybe_unused]] = gr::globalBlockRegistry().insert<PrecedenceSecond>("=test::precedence_library");

} // namespace gr::testing
