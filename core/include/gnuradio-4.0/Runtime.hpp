#ifndef GNURADIO_RUNTIME_HPP
#define GNURADIO_RUNTIME_HPP

/**
 * @brief Build and run a GNU Radio 4 flow graph without naming a framework template.
 *
 * Blocks and schedulers are named by string, parameters and ports by name, so this header
 * instantiates no `Block<T>`, `CtxSettings<T>`, `Port<T, ...>` or `SchedulerBase<...>`. Everything it
 * declares is compiled once into `libgnuradio-core`. The typed API is unchanged and remains the
 * choice where compile-time port checking matters; this is the choice for application wiring.
 *
 * Two constraints follow from linking the compiled library:
 *
 * - a consumer must be built with the same compiler family as the installed `libgnuradio-core`,
 *   because constrained-template explicit instantiations mangle differently between GCC and clang;
 * - a graph is built entirely through this entry or entirely typed. Fanning one output port out
 *   through both a typed `connect<"out", "in">` and a by-name `connect` starves the typed consumer,
 *   because the two record index-based and string-based port definitions that edge resolution does
 *   not reconcile.
 */

#include <gnuradio-4.0/Export.hpp>
#include <gnuradio-4.0/SettingsCtx.hpp>
#include <gnuradio-4.0/Tag.hpp>
#include <gnuradio-4.0/Value.hpp>

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace gr {

/// Declared, never defined here: RuntimeGraph::add takes a shared_ptr to one and Runtime::create takes
/// an rvalue reference to the other, neither of which needs a definition, so a consumer that builds no
/// block of its own and loads no graph of its own pays nothing for them.
class BlockModel;
struct Graph;

struct GNURADIO_EXPORT RuntimeError {
    std::string   message;
    std::string   where;    // the rendered source location of a framework error, or the failing method
    std::uint64_t time = 0; // ns since epoch when the framework reported it, 0 otherwise
};

struct GNURADIO_EXPORT EdgeSpec {
    std::size_t  minBufferSize = 0; // 0 selects the framework's own default for the port's value type
    std::int32_t weight        = 0;
    std::string  name;
};

/**
 * @brief One block inside a RuntimeGraph.
 *
 * Copyable and cheap. It keeps the graph's block alive but exposes no framework type, and it carries
 * no back-reference to its graph: passing a handle to a different RuntimeGraph is not diagnosed.
 */
class GNURADIO_EXPORT BlockHandle {
public:
    BlockHandle() noexcept = default;

    [[nodiscard]] bool             valid() const noexcept;
    [[nodiscard]] std::string_view name() const;
    [[nodiscard]] std::string_view uniqueName() const;
    [[nodiscard]] std::string_view typeName() const;

    /// Stores parameters for `ctx` and returns the key-value pairs that could not be set.
    [[nodiscard]] property_map set(const property_map& parameters, SettingsCtx ctx = {});

    /// Stages parameters without touching stored settings.
    [[nodiscard]] property_map setStaged(const property_map& parameters);

    [[nodiscard]] property_map              get(std::span<const std::string> keys = {}) const;
    [[nodiscard]] std::optional<pmt::Value> get(const std::string& key) const;
    [[nodiscard]] property_map              activeParameters() const;
    [[nodiscard]] property_map              stagedParameters() const;
    [[nodiscard]] property_map              defaultParameters() const;

    [[nodiscard]] std::optional<property_map> getStored(std::span<const std::string> keys = {}, SettingsCtx ctx = {}) const;
    [[nodiscard]] std::optional<pmt::Value>   getStored(const std::string& key, SettingsCtx ctx = {}) const;

    [[nodiscard]] std::optional<SettingsCtx> activateContext(SettingsCtx ctx = {});
    [[nodiscard]] SettingsCtx                activeContext() const;
    [[nodiscard]] bool                       removeContext(SettingsCtx ctx);
    [[nodiscard]] std::set<std::string>      autoUpdateParameters(SettingsCtx ctx = {});

    void storeDefaults();
    void resetDefaults();
    void loadParametersFromPropertyMap(const property_map& parameters, SettingsCtx ctx = {});

    [[nodiscard]] property_map metaInformation() const;
    void                       setMetaInformation(property_map information);

private:
    friend class RuntimeGraph;
    friend class Runtime;
    explicit BlockHandle(std::shared_ptr<void> model) noexcept;

    std::shared_ptr<void> _model; // aliases the graph's shared_ptr<BlockModel>
};

/**
 * @brief A flow graph built by name.
 *
 * A RuntimeGraph either owns its graph or views one owned by a subgraph block or a scheduler.
 * `interior()` and `Runtime::graph()` return views; a view does not destroy what it names, and
 * `clear()` on a view clears the viewed graph.
 */
class GNURADIO_EXPORT RuntimeGraph {
public:
    enum class Recursive : bool { No = false, Yes = true };

    RuntimeGraph();
    explicit RuntimeGraph(property_map initialSettings);
    ~RuntimeGraph();
    RuntimeGraph(RuntimeGraph&&) noexcept;
    RuntimeGraph& operator=(RuntimeGraph&&) noexcept;
    RuntimeGraph(const RuntimeGraph&)            = delete;
    RuntimeGraph& operator=(const RuntimeGraph&) = delete;

    /// Instantiates `type` from the registry, the loaded plugins or the YAML assets, names it and adds it.
    [[nodiscard]] std::expected<BlockHandle, RuntimeError> emplace(std::string_view type, std::string_view name, property_map parameters = {});

    /// Adds a block the caller built, so that a factory can keep its own typed pointer and hand the
    /// graph the erased one. `name` renames the block when it is not empty. Requires BlockModel.hpp.
    [[nodiscard]] std::expected<BlockHandle, RuntimeError> add(std::shared_ptr<BlockModel> block, std::string_view name = {});

    /// Adds a nested graph. The handle names it as a block here; `interior()` opens it for wiring.
    [[nodiscard]] std::expected<BlockHandle, RuntimeError> emplaceSubgraph(std::string_view name, property_map parameters = {});

    /// A non-owning view of a subgraph's interior, or of a nested scheduler's graph.
    [[nodiscard]] std::expected<RuntimeGraph, RuntimeError> interior(const BlockHandle& block) const;

    [[nodiscard]] std::vector<BlockHandle>                 blocks(Recursive recursive = Recursive::No) const;
    [[nodiscard]] std::expected<BlockHandle, RuntimeError> find(std::string_view uniqueName, Recursive recursive = Recursive::Yes) const;
    [[nodiscard]] std::expected<void, RuntimeError>        remove(const BlockHandle& block);
    void                                                   clear();

    [[nodiscard]] std::expected<void, RuntimeError> connect(const BlockHandle& sourceBlock, std::string_view sourcePort, //
        const BlockHandle& destinationBlock, std::string_view destinationPort, EdgeSpec edge = {});

    [[nodiscard]] std::expected<void, RuntimeError> disconnect(const BlockHandle& sourceBlock, std::string_view sourcePort, //
        const BlockHandle& destinationBlock, std::string_view destinationPort);

    [[nodiscard]] std::vector<std::string> inputPortNames(const BlockHandle& block) const;
    [[nodiscard]] std::vector<std::string> outputPortNames(const BlockHandle& block) const;
    [[nodiscard]] std::string              portTypeName(const BlockHandle& block, bool isInput, std::string_view portName) const;

    /// Exports `innerBlock`'s port under `exportedName`. Call on the view of a subgraph's interior;
    /// the exported port then appears on the subgraph's own handle in the parent.
    [[nodiscard]] std::expected<void, RuntimeError> exportPort(const BlockHandle& innerBlock, bool isInput, std::string_view portName, std::string_view exportedName);

    [[nodiscard]] std::expected<void, RuntimeError> unexportPort(const BlockHandle& innerBlock, bool isInput, std::string_view portName);

    /// Nested map: block unique name -> internal port name -> {"exportedName": name}.
    [[nodiscard]] property_map exportedInputPorts() const;
    [[nodiscard]] property_map exportedOutputPorts() const;

    [[nodiscard]] static std::vector<std::string> availableBlockTypes();
    [[nodiscard]] static std::vector<std::string> availableSchedulerTypes();

private:
    friend class Runtime;
    struct Impl;
    explicit RuntimeGraph(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> _impl;
};

/// One report the running graph made on the message plane.
struct GNURADIO_EXPORT RuntimeEvent {
    std::string   source;   // the reporting block's unique name
    std::string   endpoint; // the property endpoint it reported on
    bool          isError = false;
    std::string   text;     // the rendered error text when isError
    property_map  data;     // the message payload when not an error
    std::uint64_t time = 0; // ns since epoch when the error was raised, 0 otherwise
};

/**
 * @brief A scheduler holding a graph, and the subscriber on its message plane.
 *
 * The subscriber exists from `create()` until destruction whether or not the caller polls: a
 * scheduler whose msgOut has no reader turns a child block's error into an exception thrown on its
 * own worker thread, and the reason the block gave is lost with it.
 */
class GNURADIO_EXPORT Runtime {
public:
    enum class State : std::uint8_t { Idle, Initialized, Running, RequestedPause, Paused, RequestedStop, Stopped, Error };

    enum class Command : std::uint8_t { Invalid, Get, Set, Partial, Final, Ready, Disconnect, Subscribe, Unsubscribe, Notify, Heartbeat };

    static constexpr std::string_view kDefaultScheduler = "gr::scheduler::Simple<singleThreaded>";

    /// Instantiates scheduler `type`, hands it `graph` and takes ownership of both. `graph` is consumed.
    [[nodiscard]] static std::expected<Runtime, RuntimeError> create(RuntimeGraph&& graph, std::string_view type = kDefaultScheduler, property_map schedulerParameters = {});

    /// The same, for a graph that was not built through a RuntimeGraph -- one loaded by `gr::loadGrc`, or
    /// one built with the typed API. A caller that has a `gr::Graph` to pass has `Graph.hpp` already.
    [[nodiscard]] static std::expected<Runtime, RuntimeError> create(gr::Graph&& graph, std::string_view type = kDefaultScheduler, property_map schedulerParameters = {});

    ~Runtime();
    Runtime(Runtime&&) noexcept;
    Runtime& operator=(Runtime&&) noexcept;
    Runtime(const Runtime&)            = delete;
    Runtime& operator=(const Runtime&) = delete;

    /// Runs to completion on the calling thread.
    [[nodiscard]] std::expected<void, RuntimeError> runAndWait();

    /// Starts on a scheduler-owned thread and returns.
    void start();
    /// Requests a stop and joins.
    void stop();

    [[nodiscard]] State                             state() const noexcept;
    [[nodiscard]] std::expected<void, RuntimeError> requestState(State newState);

    /// A non-owning view of the running graph; `create()` moved it into the scheduler.
    [[nodiscard]] RuntimeGraph graph() const;

    /// Everything the graph has reported since the last call, oldest first.
    [[nodiscard]] std::vector<RuntimeEvent> pollEvents(std::size_t maxEvents = 64);

    /// Sends to the scheduler's message input, which reaches every string-keyed endpoint.
    [[nodiscard]] std::expected<void, RuntimeError> send(Command command, std::string_view serviceName, std::string_view endpoint, //
        property_map payload = {}, std::string_view clientRequestID = {});

    /// Quiesces the work loop for the duration of a live graph edit.
    class GNURADIO_EXPORT QuiescenceGuard {
    public:
        ~QuiescenceGuard();
        QuiescenceGuard(QuiescenceGuard&& other) noexcept;
        QuiescenceGuard& operator=(QuiescenceGuard&&)      = delete;
        QuiescenceGuard(const QuiescenceGuard&)            = delete;
        QuiescenceGuard& operator=(const QuiescenceGuard&) = delete;

    private:
        friend class Runtime;
        explicit QuiescenceGuard(Runtime* runtime) noexcept;
        Runtime* _runtime = nullptr;
    };

    [[nodiscard]] QuiescenceGuard quiesce();

private:
    struct Impl;
    explicit Runtime(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> _impl;
};

} // namespace gr

#endif // GNURADIO_RUNTIME_HPP
