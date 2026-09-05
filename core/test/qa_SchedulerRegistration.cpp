#include <boost/ut.hpp>

#include <gnuradio-4.0/BlockRegistry.hpp>
#include <gnuradio-4.0/Graph.hpp>
#include <gnuradio-4.0/PluginLoader.hpp>
#include <gnuradio-4.0/SchedulerRegistration.hpp>

#include <algorithm>
#include <array>
#include <string>
#include <vector>

namespace {

// BreadthFirst and DepthFirst static_assert against singleThreadedBlocking, so it is Simple only there
constexpr std::array kExpectedKeys = {
    "gr::scheduler::Simple<singleThreaded>",         //
    "gr::scheduler::Simple<multiThreaded>",          //
    "gr::scheduler::Simple<singleThreadedBlocking>", //
    "gr::scheduler::BreadthFirst<singleThreaded>",   //
    "gr::scheduler::BreadthFirst<multiThreaded>",    //
    "gr::scheduler::DepthFirst<singleThreaded>",     //
    "gr::scheduler::DepthFirst<multiThreaded>",      //
};

} // namespace

const boost::ut::suite<"scheduler registration"> schedulerRegistrationTests = [] {
    using namespace boost::ut;

    "the shipped schedulers are registered under stable keys"_test = [] {
        expect(eq(gr::registerBuiltinSchedulers(), kExpectedKeys.size()));
        expect(eq(gr::registerBuiltinSchedulers(), kExpectedKeys.size())) << "registration is idempotent";

        const std::vector<std::string> available = gr::globalPluginLoader().availableSchedulers();
        for (const char* key : kExpectedKeys) {
            expect(std::ranges::contains(available, std::string(key))) << key << " is missing from availableSchedulers()";
            expect(gr::globalPluginLoader().isSchedulerAvailable(key)) << key;
        }
    };

    "every registered key instantiates a scheduler"_test = [] {
        std::ignore = gr::registerBuiltinSchedulers();
        for (const char* key : kExpectedKeys) {
            expect(gr::globalPluginLoader().instantiateScheduler(key) != nullptr) << key;
        }
    };

    "initial settings reach a scheduler created by name"_test = [] {
        std::ignore = gr::registerBuiltinSchedulers();
        for (const char* key : kExpectedKeys) {
            auto scheduler = gr::globalPluginLoader().instantiateScheduler(key, {{"timeout_ms", gr::Size_t{42U}}});
            expect(scheduler != nullptr) << key;
            if (scheduler == nullptr) {
                continue;
            }
            const auto timeout = scheduler->asBlockModel()->settings().get("timeout_ms");
            expect(timeout.has_value()) << key;
            expect(eq(timeout.value().value_or(gr::Size_t{0U}), gr::Size_t{42U})) << key;
        }
    };

    "Graph::emplaceBlock creates a nested scheduler once the registry knows one"_test = [] {
        std::ignore = gr::registerBuiltinSchedulers();

        gr::Graph  graph;
        const auto nested = graph.emplaceBlock("gr::scheduler::Simple<singleThreaded>", {{"name", std::string("nested")}});
        expect(nested != nullptr);
        expect(eq(nested->name(), std::string_view("nested")));
        expect(nested->blockCategory() == gr::block::Category::ScheduledBlockGroup);
    };
};

int main() { /* tests are statically registered */ }
