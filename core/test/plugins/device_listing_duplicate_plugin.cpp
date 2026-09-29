#include <vector>

#include <gnuradio-4.0/DeviceListing.hpp>
#include <gnuradio-4.0/Plugin.hpp>

GR_PLUGIN("Device Listing Duplicate Plugin", "Unknown", "MIT", "v1")

/// A plugin that registers a listing under a family word the device listing plugin registers too.
namespace {

std::vector<gr::DeviceUnit> duplicateUnits() { return {{"Duplicate unit", "fakeplugin:serial=DUPLICATE"}}; }

const bool registered [[maybe_unused]] = static_cast<gr::DeviceListingRegistry&>(grPluginInstance()).add({.family = "fakeplugin", .opens = false, .receive = true, .transmit = true, .listUnits = duplicateUnits});

} // namespace
