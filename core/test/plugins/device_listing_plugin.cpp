#include <stdexcept>
#include <vector>

#include <gnuradio-4.0/DeviceListing.hpp>
#include <gnuradio-4.0/Plugin.hpp>

GR_PLUGIN("Device Listing Plugin", "Unknown", "MIT", "v1")

/**
 * @brief A plugin that registers device listings and no block.
 *
 * Its families list invented units and open nothing. One lists units, one of them without a selector; one states that
 * its listing opens a unit; one throws; one finds no unit.
 */
namespace {

std::vector<gr::DeviceUnit> fakePluginUnits() { return {{"Fake unit A", "fakeplugin:serial=FAKE0001"}, {"Fake unit B", "fakeplugin:serial=FAKE0002"}, {"Fake unit without a serial", ""}}; }

std::vector<gr::DeviceUnit> fakeOpeningUnits() { return {{"Fake opened unit", "fakeopening:bus=1"}}; }

std::vector<gr::DeviceUnit> fakeThrowingUnits() { throw std::runtime_error("the fake bus does not answer"); }

std::vector<gr::DeviceUnit> fakeEmptyUnits() { return {}; }

const bool registered [[maybe_unused]] = [](gr::DeviceListingRegistry& registry) {
    registry.add({.family = "fakeplugin", .opens = false, .receive = true, .transmit = true, .listUnits = fakePluginUnits});
    registry.add({.family = "fakeopening", .opens = true, .receive = true, .transmit = false, .listUnits = fakeOpeningUnits});
    registry.add({.family = "fakethrowing", .opens = false, .receive = true, .transmit = false, .listUnits = fakeThrowingUnits});
    registry.add({.family = "fakeempty", .opens = false, .receive = false, .transmit = true, .listUnits = fakeEmptyUnits});
    return true;
}(static_cast<gr::DeviceListingRegistry&>(grPluginInstance()));

} // namespace
