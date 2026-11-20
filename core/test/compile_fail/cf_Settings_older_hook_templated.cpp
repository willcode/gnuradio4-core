#include <gnuradio-4.0/Settings.hpp>

// A block that defines a member template named settingsChanged, which fails to compile.
struct OlderHookTemplated {
    struct Settings {
        float gain = 1.f;
        GR_MAKE_REFLECTABLE(Settings, gain);
    } settings;
    GR_MAKE_REFLECTABLE(OlderHookTemplated, settings);
    template<typename TMap>
    void settingsChanged(const TMap& /*oldSettings*/, const TMap& /*newSettings*/) {}
};

int main() {
    OlderHookTemplated                                block;
    gr::settings::BlockSettingsOf<OlderHookTemplated> settings(block);
    return settings.get().has_value() ? 0 : 1;
}
