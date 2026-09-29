#include <vector>

#include <gnuradio-4.0/DeviceListing.hpp>

/**
 * @brief A shared object that registers a device listing and no block, and is not a plugin.
 *
 * Its listing reaches the process's registry from the static initializer below. A loader keeps the object mapped,
 * because the registry holds a pointer into its code.
 */
namespace {

std::vector<gr::DeviceUnit> fakeLibraryUnits() { return {{"Fake library unit", "fakelibrary:index=0"}}; }

const bool registered [[maybe_unused]] = gr::globalDeviceListingRegistry().add({.family = "fakelibrary", .opens = false, .receive = true, .transmit = false, .listUnits = fakeLibraryUnits});

} // namespace
