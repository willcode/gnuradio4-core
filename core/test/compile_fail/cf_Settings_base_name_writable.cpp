#include <gnuradio-4.0/Block.hpp>

// A block that declares its own `compute_domain`, a setting of the base block a caller can write.
struct OwnDomain : gr::Block<OwnDomain> {
    struct Settings {
        std::string compute_domain = "local";
        GR_MAKE_REFLECTABLE(Settings, compute_domain);
    } settings;
    GR_MAKE_REFLECTABLE(OwnDomain, settings);
};

int main() { return gr::settings::detail::declarationTable<OwnDomain>().keys.empty() ? 1 : 0; }
