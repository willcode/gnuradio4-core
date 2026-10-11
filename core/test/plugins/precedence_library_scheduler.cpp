#include <memory>

#include <gnuradio-4.0/BlockRegistry.hpp>
#include <gnuradio-4.0/SchedulerModel.hpp>

/// A block library that registers a scheduler under a key the loading program registers first. The factory makes no
/// scheduler, so a scheduler created under the key comes from the program's factory.
namespace gr::testing {

std::unique_ptr<gr::SchedulerModel> makePrecedenceScheduler(gr::property_map /*parameters*/) { return nullptr; }

const bool registered [[maybe_unused]] = gr::globalSchedulerRegistry().insert("test::precedence_scheduler", "", makePrecedenceScheduler);

} // namespace gr::testing
