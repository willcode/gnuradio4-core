#ifndef GNURADIO_RUNTIME_TEST_HPP
#define GNURADIO_RUNTIME_TEST_HPP

#include <gnuradio-4.0/BlockModel.hpp>
#include <gnuradio-4.0/Runtime.hpp>

#include <chrono>
#include <condition_variable>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace gr::test {

/**
 * @brief A test graph wired through gr::Runtime, with its blocks still reachable as their own types.
 *
 * Wiring a test graph with gr::Graph and gr::scheduler::Simple instantiates the graph, settings and
 * scheduler cascade in the test's own translation unit, once for every block type it names. Wiring it
 * here reaches that cascade through the copy already compiled into libgnuradio-core, so a test parses
 * its own block definitions and little else.
 *
 * What a test verifies does not change with it: emplace() builds the block and hands back TBlock&, so
 * assertions read the block's own members exactly as they did before. The reference is stable for the
 * lifetime of the RuntimeTest, because the block lives in a heap-allocated BlockWrapper that cannot
 * be moved.
 *
 * A test that names no block type of its own should use gr::Runtime directly instead: building a block
 * needs BlockModel.hpp, which the by-name runtime path does not.
 */
class RuntimeTest {
public:
    RuntimeTest()                              = default;
    ~RuntimeTest()                             = default;
    RuntimeTest(const RuntimeTest&)            = delete;
    RuntimeTest& operator=(const RuntimeTest&) = delete;
    RuntimeTest(RuntimeTest&&)                 = delete;
    RuntimeTest& operator=(RuntimeTest&&)      = delete;

    /// Builds `TBlock` from `parameters` -- "name" among them, as for Graph::emplaceBlock -- and adds it.
    template<typename TBlock>
    TBlock& emplace(property_map parameters = {}) {
        auto    model = std::make_shared<BlockWrapper<TBlock>>(std::move(parameters));
        TBlock* block = static_cast<TBlock*>(model->raw());

        if (auto handle = _graph.add(model); handle.has_value()) {
            _handles.emplace_back(static_cast<const void*>(block), *handle);
        } else {
            recordError(std::move(handle.error()));
        }
        _models.push_back(std::move(model));
        return *block;
    }

    /// Connects the blocks emplace() returned, addressing their ports by name.
    template<typename TSource, typename TDestination>
    [[nodiscard]] std::expected<void, RuntimeError> connect(const TSource& source, std::string_view sourcePort, //
        const TDestination& destination, std::string_view destinationPort, EdgeSpec edge = {}) {
        const BlockHandle* from = handleOf(std::addressof(source));
        const BlockHandle* to   = handleOf(std::addressof(destination));
        if (from == nullptr || to == nullptr) {
            return std::unexpected(RuntimeError{"a block being connected was not emplaced through this RuntimeTest", "RuntimeTest::connect", 0U});
        }
        return _graph.connect(*from, sourcePort, *to, destinationPort, std::move(edge));
    }

    /// Runs to completion on the calling thread.
    [[nodiscard]] std::expected<void, RuntimeError> run(std::string_view scheduler = Runtime::kDefaultScheduler, property_map schedulerParameters = {}) {
        if (auto created = create(scheduler, std::move(schedulerParameters)); !created.has_value()) {
            return created;
        }
        return _runtime->runAndWait();
    }

    /**
     * @brief Runs on its own thread with a deadline, requesting a stop when it expires.
     *
     * A graph that fails to end then fails an assertion instead of hanging the test binary.
     * Returns whether the run ended on its own.
     */
    [[nodiscard]] bool runWithin(std::chrono::milliseconds bound, std::string_view scheduler = Runtime::kDefaultScheduler, property_map schedulerParameters = {}) {
        if (auto created = create(scheduler, std::move(schedulerParameters)); !created.has_value()) {
            return false;
        }

        std::mutex              mutex;
        std::condition_variable finished;
        bool                    returned = false;

        std::thread runner([this, &mutex, &finished, &returned] {
            std::ignore = _runtime->runAndWait();
            {
                std::lock_guard lock(mutex);
                returned = true;
            }
            finished.notify_one();
        });

        bool inTime = false;
        {
            std::unique_lock lock(mutex);
            inTime = finished.wait_for(lock, bound, [&returned] { return returned; });
        }
        if (!inTime) {
            _runtime->stop(); // release the run loop so the process can still exit
        }
        runner.join();
        return inTime;
    }

    /// Starts on a scheduler-owned thread and returns; stop() or the destructor ends the run.
    [[nodiscard]] std::expected<void, RuntimeError> start(std::string_view scheduler = Runtime::kDefaultScheduler, property_map schedulerParameters = {}) {
        if (auto created = create(scheduler, std::move(schedulerParameters)); !created.has_value()) {
            return created;
        }
        _runtime->start();
        return {};
    }

    void stop() {
        if (_runtime.has_value()) {
            _runtime->stop();
        }
    }

    [[nodiscard]] Runtime::State state() const { return _runtime.has_value() ? _runtime->state() : Runtime::State::Idle; }

    [[nodiscard]] std::vector<RuntimeEvent> pollEvents(std::size_t maxEvents = 64) { return _runtime.has_value() ? _runtime->pollEvents(maxEvents) : std::vector<RuntimeEvent>{}; }

    /// The first failure the graph reported while it was being built, if there was one.
    [[nodiscard]] const std::optional<RuntimeError>& error() const noexcept { return _error; }

private:
    [[nodiscard]] const BlockHandle* handleOf(const void* block) const {
        for (const auto& [address, handle] : _handles) {
            if (address == block) {
                return std::addressof(handle);
            }
        }
        return nullptr;
    }

    void recordError(RuntimeError error) {
        if (!_error.has_value()) {
            _error = std::move(error);
        }
    }

    [[nodiscard]] std::expected<void, RuntimeError> create(std::string_view scheduler, property_map schedulerParameters) {
        if (_error.has_value()) {
            return std::unexpected(*_error);
        }
        if (_runtime.has_value()) {
            return {}; // already created: start() then state(), or a second run of the same graph
        }
        auto runtime = Runtime::create(std::move(_graph), scheduler, std::move(schedulerParameters));
        if (!runtime.has_value()) {
            return std::unexpected(std::move(runtime.error()));
        }
        _runtime.emplace(std::move(*runtime));
        return {};
    }

    RuntimeGraph                                     _graph;
    std::vector<std::shared_ptr<BlockModel>>         _models;  // keeps every block alive for the assertions
    std::vector<std::pair<const void*, BlockHandle>> _handles; // the typed block address a test holds -> its handle
    std::optional<Runtime>                           _runtime;
    std::optional<RuntimeError>                      _error;
};

} // namespace gr::test

#endif // GNURADIO_RUNTIME_TEST_HPP
