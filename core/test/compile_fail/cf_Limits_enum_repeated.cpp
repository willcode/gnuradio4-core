#include <gnuradio-4.0/annotated.hpp>

namespace cf_limits {
enum class Taper { rectangular, hann, hamming };
}

// OneOf naming one enumerator twice.
gr::Annotated<cf_limits::Taper, gr::Limits<gr::OneOf<cf_limits::Taper::hann, cf_limits::Taper::hann>>> member = cf_limits::Taper::hann;

int main() { return member.value == cf_limits::Taper::rectangular ? 1 : 0; }
