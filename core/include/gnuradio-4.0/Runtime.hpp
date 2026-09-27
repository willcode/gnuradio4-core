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
 * One constraint follows from linking the compiled library: a consumer must be built with the same
 * compiler family as the installed `libgnuradio-core`, because constrained-template explicit
 * instantiations mangle differently between GCC and clang.
 */

#include <gnuradio-4.0/Export.hpp>
#include <gnuradio-4.0/SettingsCtx.hpp>
#include <gnuradio-4.0/Tag.hpp>
#include <gnuradio-4.0/Value.hpp>

#include <chrono>
#include <cstdint>
#include <expected>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gr {

/// Declared, never defined here: RuntimeGraph::add takes a shared_ptr to one and Runtime::create takes
/// an rvalue reference to the other, neither of which needs a definition, so a consumer that builds no
/// block of its own and loads no graph of its own pays nothing for them.
class BlockModel;
struct Graph;

/// Declared, never defined here: a program that builds its own loader includes `PluginLoader.hpp` for it.
class PluginLoader;

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

    /// Stages parameters without touching stored settings and returns the pairs the block does not declare. A value
    /// that does not convert to its setting's type, or one a declared parameter refuses, is an error carrying the
    /// block's reason, and the call stages nothing.
    [[nodiscard]] std::expected<property_map, RuntimeError> setStaged(const property_map& parameters);

    [[nodiscard]] property_map              get(std::span<const std::string> keys = {}) const;
    [[nodiscard]] std::optional<pmt::Value> get(const std::string& key) const;
    [[nodiscard]] property_map              stagedParameters() const;
    [[nodiscard]] property_map              defaultParameters() const;

    /// The keys `setStaged()` accepts: the type's writable members and the parameters the block declared.
    [[nodiscard]] std::set<std::string> writableMembers() const;

    /// Reads the block's members into the settings `get()` returns. `get()` returns the values of the last settings
    /// change applied until this call. A member the block writes while it works reads as it was at that change. Call it
    /// while no scheduler runs the block.
    void updateActiveParameters();

    [[nodiscard]] property_map metaInformation() const;
    void                       setMetaInformation(property_map information);

private:
    friend class RuntimeGraph;
    friend class Runtime;
    explicit BlockHandle(std::shared_ptr<void> model) noexcept;

    std::shared_ptr<void> _model; // aliases the graph's shared_ptr<BlockModel>
};

/// One edge of a graph. Each port carries the name `connect()` accepts, also for an edge made by index.
/// `edge.minBufferSize` is the minimum the edge records, with the framework's default in place of 0.
/// `edge.name` is "unnamed edge" when none was given.
struct GNURADIO_EXPORT RuntimeEdge {
    BlockHandle sourceBlock;
    std::string sourcePort;
    BlockHandle destinationBlock;
    std::string destinationPort;
    EdgeSpec    edge;
};

/// One port of a block, as `RuntimeGraph::inputPorts()` and `outputPorts()` describe it. A block lists its
/// ports once, when they are first read, and a port collection must not change size after that.
/// `minSamples` and `maxSamples` are the requirement the port declared when its block's ports were first
/// listed.
struct GNURADIO_EXPORT RuntimePort {
    std::string name;     // the name connect() accepts: "out", or "in#1" for element 1 of a collection
    std::string typeName; // the value type, as the port spells it
    bool        isInput       = false;
    bool        isMessage     = false;
    bool        isOptional    = false;
    bool        isSynchronous = false;
    bool        isConnected   = false; // true while a running scheduler holds the port connected; false before a run and after its blocks stop
    std::string domain;
    std::string collection;     // the collection's name for one of its elements, empty otherwise
    std::size_t index      = 0; // the element's position in its collection, 0 otherwise
    std::size_t minSamples = 0;
    std::size_t maxSamples = 0;
};

/// What a plugin loader holds from one directory, as `RuntimeGraph::loadPlugins()` reports it.
struct GNURADIO_EXPORT RuntimePluginDirectory {
    struct BlockLibrary {
        std::string file;
        std::size_t nBlockRegistrations = 0; // block registrations the file made when it loaded
    };

    std::string                                      directory;      // as the caller named it
    std::vector<std::string>                         plugins;        // the files that loaded as plugins
    std::vector<BlockLibrary>                        blockLibraries; // the files that registered blocks without being plugins
    std::vector<std::pair<std::string, std::string>> failed;         // each file that did not load, with the loader's reason
    std::vector<std::string>                         skipped;        // the entries named like a shared object that were not opened
    std::vector<std::string>                         blockTypes;     // the block types these files supplied before any other file, sorted
    std::string                                      error;          // why the directory could not be read, empty when it was read
};

/**
 * @brief A flow graph built by name.
 *
 * A RuntimeGraph either owns its graph or views one owned by a subgraph block or a scheduler.
 * `interior()` and `Runtime::graph()` return views; a view does not destroy what it names, and an
 * edit through a view edits the viewed graph.
 *
 * From another thread, call edges(), inputPorts() and outputPorts() on a running graph only while no
 * edit is in flight, whether made through a view or by the scheduler's edit messages (quiesce() does
 * not hold those back), and call the port lists only while the scheduler is neither starting nor
 * stopping.
 */
class GNURADIO_EXPORT RuntimeGraph {
public:
    enum class Recursive : bool { No = false, Yes = true };

    /// An empty graph whose blocks come from the process's plugin loader, `gr::globalPluginLoader()`.
    RuntimeGraph();

    /// An empty graph whose blocks and scheduler come from `loader`, the process's loader when it is null. The graph
    /// and a Runtime created from it share ownership of the loader. A BlockHandle does not, so a handle to a block from
    /// a plugin is used only while its graph or its Runtime exists.
    explicit RuntimeGraph(std::shared_ptr<PluginLoader> loader);

    ~RuntimeGraph();
    RuntimeGraph(RuntimeGraph&&) noexcept;
    RuntimeGraph& operator=(RuntimeGraph&&) noexcept;
    RuntimeGraph(const RuntimeGraph&)            = delete;
    RuntimeGraph& operator=(const RuntimeGraph&) = delete;

    /// Instantiates `type` from the registry, the loaded plugins or the YAML assets, names it and adds it.
    /// A known type that cannot be built, such as a recipe missing a required parameter, is an error with its reason.
    /// A parameter the block does not declare or refuses is an error carrying the block's refusal.
    [[nodiscard]] std::expected<BlockHandle, RuntimeError> emplace(std::string_view type, std::string_view name, property_map parameters = {});

    /// Adds a block the caller built, so that a factory can keep its own typed pointer and hand the
    /// graph the erased one. `name` renames the block when it is not empty. Requires BlockModel.hpp. A block
    /// whose settings refuse a key or a value's type when the graph initializes it is an error carrying the reason.
    [[nodiscard]] std::expected<BlockHandle, RuntimeError> add(std::shared_ptr<BlockModel> block, std::string_view name = {});

    /// Adds a nested graph. The handle names it as a block here; `interior()` opens it for wiring.
    [[nodiscard]] std::expected<BlockHandle, RuntimeError> emplaceSubgraph(std::string_view name, property_map parameters = {});

    /// A non-owning view of a subgraph's interior, or of a nested scheduler's graph.
    [[nodiscard]] std::expected<RuntimeGraph, RuntimeError> interior(const BlockHandle& block) const;

    [[nodiscard]] std::vector<BlockHandle> blocks(Recursive recursive = Recursive::No) const;

    /// The block whose unique name is `name`, else the one block whose name is `name`. A block `fromYaml()` read from a
    /// document entry that carries a `unique_name` also answers to that name. A name no block carries and a name two
    /// blocks share are errors.
    [[nodiscard]] std::expected<BlockHandle, RuntimeError> find(std::string_view name, Recursive recursive = Recursive::Yes) const;

    [[nodiscard]] std::expected<void, RuntimeError> remove(const BlockHandle& block);

    [[nodiscard]] std::expected<void, RuntimeError> connect(const BlockHandle& sourceBlock, std::string_view sourcePort, //
        const BlockHandle& destinationBlock, std::string_view destinationPort, EdgeSpec edge = {});

    [[nodiscard]] std::expected<void, RuntimeError> disconnect(const BlockHandle& sourceBlock, std::string_view sourcePort, //
        const BlockHandle& destinationBlock, std::string_view destinationPort);

    /// The edges of this level of the graph; a subgraph's own edges are read through `interior()`.
    [[nodiscard]] std::vector<RuntimeEdge> edges() const;

    [[nodiscard]] std::vector<std::string> inputPortNames(const BlockHandle& block) const;
    [[nodiscard]] std::vector<std::string> outputPortNames(const BlockHandle& block) const;

    /// Describes each port `inputPortNames()` and `outputPortNames()` list, in the same order.
    [[nodiscard]] std::vector<RuntimePort> inputPorts(const BlockHandle& block) const;
    [[nodiscard]] std::vector<RuntimePort> outputPorts(const BlockHandle& block) const;

    /// Exports `innerBlock`'s port under `exportedName`. Call on the view of a subgraph's interior;
    /// the exported port then appears on the subgraph's own handle in the parent.
    [[nodiscard]] std::expected<void, RuntimeError> exportPort(const BlockHandle& innerBlock, bool isInput, std::string_view portName, std::string_view exportedName);

    [[nodiscard]] std::expected<void, RuntimeError> unexportPort(const BlockHandle& innerBlock, bool isInput, std::string_view portName);

    /// Nested map: block unique name -> internal port name -> {"exportedName": name}.
    [[nodiscard]] property_map exportedInputPorts() const;
    [[nodiscard]] property_map exportedOutputPorts() const;

    /// Loads a graph document as `gr::loadGrc` does, through `loader`, into a graph this RuntimeGraph owns. The graph
    /// holds the loader as the constructor that takes one does, and a null `loader` selects the process's loader. A
    /// document the reader refuses is an error carrying the reader's message, and a block type the loader does not
    /// know is one naming the type. For a document that does not parse, the message is the reader's sentence headed by
    /// the line and column.
    ///
    /// `overrides` puts the caller's settings in place of the document's: each key names a block at the document's top
    /// level by its `unique_name` or name, and its values replace those of the block's parameters before the block
    /// reads them, as `gr::loadGrc` with a `gr::BlockSettings` does. A name no block carries, a name two blocks share
    /// and a key a block does not declare are errors.
    [[nodiscard]] static std::expected<RuntimeGraph, RuntimeError> fromYaml(std::shared_ptr<PluginLoader> loader, std::string_view document, const std::map<std::string, property_map, std::less<>>& overrides = {});

    /// `fromYaml` through the process's loader.
    [[nodiscard]] static std::expected<RuntimeGraph, RuntimeError> fromYaml(std::string_view document);

    /// `fromYaml` through the process's loader, with the caller's settings in place of the document's.
    [[nodiscard]] static std::expected<RuntimeGraph, RuntimeError> fromYaml(std::string_view document, const std::map<std::string, property_map, std::less<>>& overrides);

    /// Writes the graph as a graph document through `gr::saveGrc`, on an owning graph or a view. Call it
    /// only on a graph no scheduler is running. For a running graph, the reply to a `Get` on the
    /// scheduler's "GraphGRC" endpoint holds the document under `value`.
    [[nodiscard]] std::expected<std::string, RuntimeError> toYaml() const;

    /// The block types `loader` can build: those of the block registry it reads and those of its plugins, sorted.
    [[nodiscard]] static std::vector<std::string> availableBlockTypes(const PluginLoader& loader);

    /// `availableBlockTypes` of the process's loader, which is built on the first call that uses it.
    [[nodiscard]] static std::vector<std::string> availableBlockTypes();

    /// The scheduler types `loader` can build, the ones `Runtime::create` takes for a graph over it, sorted. The call
    /// registers the shipped schedulers into `gr::globalSchedulerRegistry()` first, so the list holds them when `loader`
    /// reads that registry, and it holds the schedulers of the loader's plugins.
    [[nodiscard]] static std::vector<std::string> availableSchedulerTypes(const PluginLoader& loader);

    /// `availableSchedulerTypes` of the process's loader, which is built on the first call that uses it.
    [[nodiscard]] static std::vector<std::string> availableSchedulerTypes();

    /**
     * @brief Searches each directory for YAML block definitions, plugins and block libraries with `loader`, and reports
     * what the loader holds from each directory, in the order given.
     *
     * A directory the loader searched before, at its construction or in an earlier call, is not searched again, and its
     * report lists what the first search found. A directory that does not exist reports nothing. A directory the file
     * system does not let the search read reports why in `error`, and the search goes on with the next one.
     *
     * A graph built through the loader takes a block type from the block registry first, then from the plugins, then
     * from the YAML definitions. Of two plugins that supply one type, the first loaded supplies it. Of two block
     * libraries, the last loaded supplies it, because a registration replaces the one before it. The files of one
     * directory load in the order the file system lists them.
     *
     * The loader holds no lock. Call this while no other thread uses the loader, a running scheduler whose graph came
     * through it included.
     */
    [[nodiscard]] static std::vector<RuntimePluginDirectory> loadPlugins(PluginLoader& loader, std::span<const std::string> directories);

    /// `loadPlugins` with the process's loader, the one `fromYaml()` and `emplace()` use when given none. That loader
    /// searched the directories of `GNURADIO4_PLUGIN_DIRECTORIES` at its construction.
    [[nodiscard]] static std::vector<RuntimePluginDirectory> loadPlugins(std::span<const std::string> directories);

private:
    friend class Runtime;
    struct Impl;
    explicit RuntimeGraph(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> _impl;
};

/// A message-plane command: the enumerators and values of the framework's `message::Command`.
enum class RuntimeCommand : std::uint8_t { Invalid, Get, Set, Partial, Final, Ready, Disconnect, Subscribe, Unsubscribe, Notify, Heartbeat };

/// One report the running graph made on the message plane.
struct GNURADIO_EXPORT RuntimeEvent {
    std::string    source;   // the reporting block's unique name
    std::string    endpoint; // the property endpoint it reported on
    bool           isError = false;
    std::string    text;                              // the error's message when isError
    std::string    where;                             // the rendered source location of the error when isError
    property_map   data;                              // the message payload when not an error
    std::uint64_t  time    = 0;                       // ns since epoch when the error was raised, 0 otherwise
    RuntimeCommand command = RuntimeCommand::Invalid; // Final for the reply to a request, Notify for a notification
    std::string    clientRequestID;                   // the id of the request or subscription it answers, empty for none
};

/**
 * @brief A scheduler holding a graph, and the subscriber on its message plane.
 *
 * The subscriber exists from `create()` until destruction whether or not the caller polls: a
 * scheduler whose msgOut has no reader turns a child block's error into an exception thrown on its
 * own worker thread, and the reason the block gave is lost with it.
 *
 * A Runtime holds one run of its graph at a time, by `runAndWait()` on the calling thread or by
 * `start()` on a thread of its own, and keeps the last run's result. The destructor stops the run in
 * progress and waits for it to end.
 *
 * A stop is requested on another thread the Runtime owns. A run ends once the scheduler has returned
 * from it and the stop requested of it has returned. A block whose stop() is slow delays the end of
 * the run, and `stopFor()` still returns at its timeout.
 */
class GNURADIO_EXPORT Runtime {
public:
    enum class State : std::uint8_t { Idle, Initialized, Running, RequestedPause, Paused, RequestedStop, Stopped, Error };

    static constexpr std::string_view kDefaultScheduler = "gr::scheduler::Simple<singleThreaded>";

    /// Instantiates scheduler `type` through the plugin loader `graph` holds, hands it `graph` and takes ownership of
    /// both and of the graph's share of the loader; `graph` is consumed when a runtime is returned and left as it was
    /// on an error. A parameter the scheduler does not declare, or a value its
    /// constructor refuses, is an error; a value it accepts at construction and rejects later, such as an unknown
    /// `poolName`, is reported as an error event instead.
    [[nodiscard]] static std::expected<Runtime, RuntimeError> create(RuntimeGraph&& graph, std::string_view type = kDefaultScheduler, property_map schedulerParameters = {});

    /// The same, for a graph that was not built through a RuntimeGraph -- one loaded by `gr::loadGrc`, or
    /// one built with the typed API. A caller that has a `gr::Graph` to pass has `Graph.hpp` already. The scheduler
    /// comes from the loader the graph names, which the caller keeps alive until the Runtime is destroyed.
    [[nodiscard]] static std::expected<Runtime, RuntimeError> create(gr::Graph&& graph, std::string_view type = kDefaultScheduler, property_map schedulerParameters = {});

    ~Runtime();
    Runtime(Runtime&&) noexcept;
    Runtime& operator=(Runtime&&) noexcept;
    Runtime(const Runtime&)            = delete;
    Runtime& operator=(const Runtime&) = delete;

    /// Runs to completion on the calling thread and returns the run's result. Refused while a run is in progress.
    [[nodiscard]] std::expected<void, RuntimeError> runAndWait();

    /// Runs the graph as runAndWait() does, on a thread the Runtime owns, and returns at once. Returns why
    /// no run started, a run in progress among the reasons, and nothing once the run started.
    std::optional<RuntimeError> start();

    /// Requests the stop of the run in progress, by start() or by runAndWait() on another thread, and
    /// returns once the run has ended. Without a run in progress it does nothing.
    void stop();

    /// Requests the stop as stop() does and returns once the run has ended or `timeout` has passed, and whether the
    /// run has ended, true without a run in progress. The request stands after a return at the timeout, and a later
    /// call waits for the same stop.
    [[nodiscard]] bool stopFor(std::chrono::nanoseconds timeout);

    /// Returns once no run is in progress.
    void wait() const;

    /// Returns once no run is in progress or `timeout` has passed, and whether no run is in progress.
    [[nodiscard]] bool waitFor(std::chrono::nanoseconds timeout) const;

    /// The last run's result, as runAndWait() returns it. Success before the first run, and an error while a
    /// run is in progress.
    [[nodiscard]] std::expected<void, RuntimeError> result() const;

    [[nodiscard]] State                             state() const noexcept;
    [[nodiscard]] std::expected<void, RuntimeError> requestState(State newState);

    /// A non-owning view of the running graph; `create()` moved it into the scheduler.
    [[nodiscard]] RuntimeGraph graph() const;

    /// Returns the scheduler as a block handle. Its own settings (`timeout_ms`, `poolName` and the rest)
    /// are read, staged and described like any block's. The handle keeps the scheduler alive after the
    /// Runtime is destroyed, and it is invalid on an empty Runtime.
    [[nodiscard]] BlockHandle scheduler() const;

    /// Everything the graph has reported since the last call, oldest first.
    [[nodiscard]] std::vector<RuntimeEvent> pollEvents(std::size_t maxEvents = 64);

    /// Sends to the scheduler's message input, which reaches every string-keyed endpoint.
    [[nodiscard]] std::expected<void, RuntimeError> send(RuntimeCommand command, std::string_view serviceName, std::string_view endpoint, //
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
