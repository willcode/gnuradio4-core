#include <gnuradio-4.0/SchedulerRegistration.hpp>

#include <gnuradio-4.0/BlockRegistry.hpp>
#include <gnuradio-4.0/Scheduler.hpp>

#include <string_view>

namespace gr {

namespace {
// the '=' prefix makes the literal the registry key verbatim, so it does not depend on how a
// compiler spells a non-type template argument
template<typename TScheduler>
std::size_t insertScheduler(SchedulerRegistry& registry, std::string_view alias) {
    return registry.insert<TScheduler>(alias) ? 1UZ : 0UZ;
}
} // namespace

std::size_t registerBuiltinSchedulers(SchedulerRegistry& registry) {
    using enum gr::scheduler::ExecutionPolicy;

    std::size_t nRegistered = 0UZ;
    nRegistered += insertScheduler<scheduler::Simple<singleThreaded>>(registry, "=gr::scheduler::Simple<singleThreaded>");
    nRegistered += insertScheduler<scheduler::Simple<multiThreaded>>(registry, "=gr::scheduler::Simple<multiThreaded>");
    nRegistered += insertScheduler<scheduler::Simple<singleThreadedBlocking>>(registry, "=gr::scheduler::Simple<singleThreadedBlocking>");

    // BreadthFirst and DepthFirst static_assert against singleThreadedBlocking
    nRegistered += insertScheduler<scheduler::BreadthFirst<singleThreaded>>(registry, "=gr::scheduler::BreadthFirst<singleThreaded>");
    nRegistered += insertScheduler<scheduler::BreadthFirst<multiThreaded>>(registry, "=gr::scheduler::BreadthFirst<multiThreaded>");
    nRegistered += insertScheduler<scheduler::DepthFirst<singleThreaded>>(registry, "=gr::scheduler::DepthFirst<singleThreaded>");
    nRegistered += insertScheduler<scheduler::DepthFirst<multiThreaded>>(registry, "=gr::scheduler::DepthFirst<multiThreaded>");

    return nRegistered;
}

std::size_t registerBuiltinSchedulers() {
    static const std::size_t nRegistered = registerBuiltinSchedulers(globalSchedulerRegistry());
    return nRegistered;
}

} // namespace gr
