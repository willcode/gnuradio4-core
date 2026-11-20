#include <gnuradio-4.0/SettingDeclaration.hpp>

// A block that declares a key applied at the call: only a scheduler applies a key on the caller's thread.
struct AtCallBlock {
    struct Settings {
        double frequency = 100e6;
        GR_MAKE_REFLECTABLE(Settings, frequency);
    } settings;
    GR_MAKE_REFLECTABLE(AtCallBlock, settings);
    static constexpr std::array<std::string_view, 1UZ> kSettingsAtCall{"frequency"};
};

int main() { return gr::settings::detail::declarationTable<AtCallBlock>().keys.empty() ? 1 : 0; }
