#ifndef GNURADIO_SCHEDULER_REGISTRATION_HPP
#define GNURADIO_SCHEDULER_REGISTRATION_HPP

#include <gnuradio-4.0/Export.hpp>

#include <cstddef>

namespace gr {

class SchedulerRegistry;

/**
 * @brief Inserts the shipped scheduler templates into a registry, so that they can be created by name.
 *
 * The keys are `gr::scheduler::<Simple|BreadthFirst|DepthFirst><<policy>>`, plus each type's own
 * `meta::type_name`. BreadthFirst and DepthFirst support `singleThreaded` and `multiThreaded` only.
 *
 * Callers reach the same schedulers through `PluginLoader::instantiateScheduler`,
 * `Graph::emplaceBlock(type, settings)` and `loadGrc`'s nested-scheduler blocks.
 */
GNURADIO_EXPORT std::size_t registerBuiltinSchedulers(SchedulerRegistry& registry);

/// Registers into `globalSchedulerRegistry()` on the first call and returns the same count afterwards.
GNURADIO_EXPORT std::size_t registerBuiltinSchedulers();

} // namespace gr

#endif // GNURADIO_SCHEDULER_REGISTRATION_HPP
