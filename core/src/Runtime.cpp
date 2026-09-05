#include <gnuradio-4.0/Runtime.hpp>

#include <gnuradio-4.0/BlockModel.hpp>
#include <gnuradio-4.0/Graph.hpp>
#include <gnuradio-4.0/Message.hpp>
#include <gnuradio-4.0/PluginLoader.hpp>
#include <gnuradio-4.0/Port.hpp>
#include <gnuradio-4.0/SchedulerModel.hpp>
#include <gnuradio-4.0/SchedulerRegistration.hpp>
#include <gnuradio-4.0/Settings.hpp>

#include <algorithm>
#include <deque>
#include <format>
#include <mutex>

namespace gr {

static_assert(static_cast<std::uint8_t>(Runtime::State::Idle) == static_cast<std::uint8_t>(lifecycle::State::IDLE));
static_assert(static_cast<std::uint8_t>(Runtime::State::Initialized) == static_cast<std::uint8_t>(lifecycle::State::INITIALISED));
static_assert(static_cast<std::uint8_t>(Runtime::State::Running) == static_cast<std::uint8_t>(lifecycle::State::RUNNING));
static_assert(static_cast<std::uint8_t>(Runtime::State::RequestedPause) == static_cast<std::uint8_t>(lifecycle::State::REQUESTED_PAUSE));
static_assert(static_cast<std::uint8_t>(Runtime::State::Paused) == static_cast<std::uint8_t>(lifecycle::State::PAUSED));
static_assert(static_cast<std::uint8_t>(Runtime::State::RequestedStop) == static_cast<std::uint8_t>(lifecycle::State::REQUESTED_STOP));
static_assert(static_cast<std::uint8_t>(Runtime::State::Stopped) == static_cast<std::uint8_t>(lifecycle::State::STOPPED));
static_assert(static_cast<std::uint8_t>(Runtime::State::Error) == static_cast<std::uint8_t>(lifecycle::State::ERROR));

static_assert(static_cast<std::uint8_t>(Runtime::Command::Invalid) == static_cast<std::uint8_t>(message::Command::Invalid));
static_assert(static_cast<std::uint8_t>(Runtime::Command::Get) == static_cast<std::uint8_t>(message::Command::Get));
static_assert(static_cast<std::uint8_t>(Runtime::Command::Set) == static_cast<std::uint8_t>(message::Command::Set));
static_assert(static_cast<std::uint8_t>(Runtime::Command::Partial) == static_cast<std::uint8_t>(message::Command::Partial));
static_assert(static_cast<std::uint8_t>(Runtime::Command::Final) == static_cast<std::uint8_t>(message::Command::Final));
static_assert(static_cast<std::uint8_t>(Runtime::Command::Ready) == static_cast<std::uint8_t>(message::Command::Ready));
static_assert(static_cast<std::uint8_t>(Runtime::Command::Disconnect) == static_cast<std::uint8_t>(message::Command::Disconnect));
static_assert(static_cast<std::uint8_t>(Runtime::Command::Subscribe) == static_cast<std::uint8_t>(message::Command::Subscribe));
static_assert(static_cast<std::uint8_t>(Runtime::Command::Unsubscribe) == static_cast<std::uint8_t>(message::Command::Unsubscribe));
static_assert(static_cast<std::uint8_t>(Runtime::Command::Notify) == static_cast<std::uint8_t>(message::Command::Notify));
static_assert(static_cast<std::uint8_t>(Runtime::Command::Heartbeat) == static_cast<std::uint8_t>(message::Command::Heartbeat));

namespace {

BlockModel* modelOf(const std::shared_ptr<void>& handle) noexcept { return static_cast<BlockModel*>(handle.get()); }

std::shared_ptr<BlockModel> modelPtrOf(const std::shared_ptr<void>& handle) noexcept { return std::static_pointer_cast<BlockModel>(handle); }

std::uint64_t toNanoseconds(std::chrono::system_clock::time_point when) { //
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(when.time_since_epoch()).count());
}

RuntimeError toRuntimeError(const Error& error) { return RuntimeError{.message = error.message, .where = error.srcLoc(), .time = toNanoseconds(error.errorTime)}; }

RuntimeError localError(std::string message, std::string_view where) { return RuntimeError{.message = std::move(message), .where = std::string(where), .time = 0ULL}; }

PluginLoader& loaderFor(const Graph& graph) { return graph._pluginLoader != nullptr ? *graph._pluginLoader : gr::globalPluginLoader(); }

// a collection is listed element by element, as "base#index", because that is the spelling
// dynamicPortFromName resolves and the only one an edge to a collection can carry
std::vector<std::string> portNamesOf(BlockModel& model, bool isInput) {
    model.initDynamicPorts();
    const BlockModel::DynamicPorts& ports = isInput ? model.dynamicInputPorts() : model.dynamicOutputPorts();

    std::vector<std::string> names;
    names.reserve(ports.size());
    for (const BlockModel::DynamicPortOrCollection& entry : ports) {
        if (const auto* collection = std::get_if<BlockModel::NamedPortCollection>(&entry); collection != nullptr) {
            for (std::size_t index = 0UZ; index < collection->ports.size(); ++index) {
                names.push_back(std::format("{}#{}", collection->name, index));
            }
        } else {
            names.push_back(BlockModel::portName(entry));
        }
    }
    return names;
}

std::string commaSeparated(const std::vector<std::string>& names) {
    std::string joined;
    for (const std::string& name : names) {
        if (!joined.empty()) {
            joined += ", ";
        }
        joined += name;
    }
    return joined;
}

// resolves exactly what Graph::connect's edge resolution resolves, so the pre-check can neither
// refuse a name that would have worked nor admit one that fails once the graph starts
std::expected<void, RuntimeError> checkPortExists(BlockModel& model, bool isInput, std::string_view portName, std::string_view where) {
    model.initDynamicPorts();
    const PortDefinition definition{std::string(portName)};
    const auto           port = isInput ? model.dynamicInputPort(definition) : model.dynamicOutputPort(definition);
    if (port.has_value()) {
        return {};
    }
    return std::unexpected(localError(std::format("block '{}' has no {} port '{}' -- it has [{}]", model.uniqueName(), isInput ? "input" : "output", portName, //
                                          commaSeparated(portNamesOf(model, isInput))),
        where));
}

} // namespace

struct RuntimeGraph::Impl {
    std::unique_ptr<Graph> owned;          // set when this RuntimeGraph owns the graph
    Graph*                 view = nullptr; // the graph in use, owned or not
    std::shared_ptr<void>  ownerModel;     // the BlockModel that owns a viewed graph, for port export
};

struct Runtime::Impl {
    MsgPortIn                       events;   // declared first so that it outlives the scheduler writing to it
    MsgPortOut                      commands; // feeds the scheduler's msgIn
    std::mutex                      mutex;
    std::deque<RuntimeEvent>        pending;
    std::shared_ptr<SchedulerModel> scheduler;
    std::shared_ptr<BlockModel>     schedulerBlock;

    static constexpr std::size_t kMaxPendingEvents = 1024UZ;

    void drain() {
        std::lock_guard guard(mutex);
        drainLocked();
    }

    void drainLocked() {
        ReaderSpanLike auto messages = events.streamReader().get();
        for (const Message& message : messages) {
            RuntimeEvent event{.source = message.serviceName, .endpoint = message.endpoint, .isError = !message.data.has_value(), .text = {}, .data = {}, .time = 0ULL};
            if (message.data.has_value()) {
                event.data = message.data.value();
            } else {
                event.text = std::format("{} at {}", message.data.error().message, message.data.error().srcLoc());
                event.time = toNanoseconds(message.data.error().errorTime);
            }
            pending.push_back(std::move(event));
        }
        std::ignore = messages.consume(messages.size());

        while (pending.size() > kMaxPendingEvents) {
            pending.pop_front();
        }
    }
};

BlockHandle::BlockHandle(std::shared_ptr<void> model) noexcept : _model(std::move(model)) {}

bool BlockHandle::valid() const noexcept { return _model != nullptr; }

std::string_view BlockHandle::name() const { return _model ? modelOf(_model)->name() : std::string_view{}; }

std::string_view BlockHandle::uniqueName() const { return _model ? modelOf(_model)->uniqueName() : std::string_view{}; }

std::string_view BlockHandle::typeName() const { return _model ? modelOf(_model)->typeName() : std::string_view{}; }

property_map BlockHandle::set(const property_map& parameters, SettingsCtx ctx) { return _model ? modelOf(_model)->settings().set(parameters, ctx) : parameters; }

property_map BlockHandle::setStaged(const property_map& parameters) { return _model ? modelOf(_model)->settings().setStaged(parameters) : parameters; }

property_map BlockHandle::get(std::span<const std::string> keys) const { return _model ? modelOf(_model)->settings().get(keys) : property_map{}; }

std::optional<pmt::Value> BlockHandle::get(const std::string& key) const { return _model ? modelOf(_model)->settings().get(key) : std::nullopt; }

property_map BlockHandle::activeParameters() const { return _model ? modelOf(_model)->settings().activeParameters() : property_map{}; }

property_map BlockHandle::stagedParameters() const { return _model ? modelOf(_model)->settings().stagedParameters() : property_map{}; }

property_map BlockHandle::defaultParameters() const { return _model ? modelOf(_model)->settings().defaultParameters() : property_map{}; }

std::optional<property_map> BlockHandle::getStored(std::span<const std::string> keys, SettingsCtx ctx) const { return _model ? modelOf(_model)->settings().getStored(keys, ctx) : std::nullopt; }

std::optional<pmt::Value> BlockHandle::getStored(const std::string& key, SettingsCtx ctx) const { return _model ? modelOf(_model)->settings().getStored(key, ctx) : std::nullopt; }

std::optional<SettingsCtx> BlockHandle::activateContext(SettingsCtx ctx) { return _model ? modelOf(_model)->settings().activateContext(ctx) : std::nullopt; }

SettingsCtx BlockHandle::activeContext() const { return _model ? modelOf(_model)->settings().activeContext() : SettingsCtx{}; }

bool BlockHandle::removeContext(SettingsCtx ctx) { return _model ? modelOf(_model)->settings().removeContext(ctx) : false; }

std::set<std::string> BlockHandle::autoUpdateParameters(SettingsCtx ctx) { return _model ? modelOf(_model)->settings().autoUpdateParameters(ctx) : std::set<std::string>{}; }

void BlockHandle::storeDefaults() {
    if (_model) {
        modelOf(_model)->settings().storeDefaults();
    }
}

void BlockHandle::resetDefaults() {
    if (_model) {
        modelOf(_model)->settings().resetDefaults();
    }
}

void BlockHandle::loadParametersFromPropertyMap(const property_map& parameters, SettingsCtx ctx) {
    if (_model) {
        modelOf(_model)->settings().loadParametersFromPropertyMap(parameters, ctx);
    }
}

property_map BlockHandle::metaInformation() const { return _model ? modelOf(_model)->metaInformation() : property_map{}; }

void BlockHandle::setMetaInformation(property_map information) {
    if (_model) {
        modelOf(_model)->metaInformation() = std::move(information);
    }
}

RuntimeGraph::RuntimeGraph() : RuntimeGraph(property_map{}) {}

RuntimeGraph::RuntimeGraph(property_map initialSettings) : _impl(std::make_unique<Impl>()) {
    _impl->owned = std::make_unique<Graph>(std::move(initialSettings));
    _impl->view  = _impl->owned.get();
}

RuntimeGraph::RuntimeGraph(std::unique_ptr<Impl> impl) noexcept : _impl(std::move(impl)) {}

RuntimeGraph::~RuntimeGraph() = default;

RuntimeGraph::RuntimeGraph(RuntimeGraph&&) noexcept = default;

RuntimeGraph& RuntimeGraph::operator=(RuntimeGraph&&) noexcept = default;

std::expected<BlockHandle, RuntimeError> RuntimeGraph::emplace(std::string_view type, std::string_view name, property_map parameters) {
    if (!_impl || _impl->view == nullptr) {
        return std::unexpected(localError("graph handle is empty", "RuntimeGraph::emplace"));
    }
    PluginLoader& loader = loaderFor(*_impl->view);

    std::shared_ptr<BlockModel> model = loader.instantiate(type, parameters);
    if (!model) {
        // the scheduler registry is the second half of the same lookup Graph::emplaceBlock performs,
        // and it is what nests a scheduler as a block
        std::ignore = gr::registerBuiltinSchedulers();
        if (std::shared_ptr<SchedulerModel> scheduler = loader.instantiateScheduler(type, parameters); scheduler) {
            model = SchedulerModel::asBlockModelPtr(std::move(scheduler));
        }
    }
    if (!model) {
        return std::unexpected(localError(std::format("unknown block type '{}' -- {} types are available, see availableBlockTypes()", type, loader.availableBlocks().size()), "RuntimeGraph::emplace"));
    }

    model->setName(std::string(name)); // before addBlock, so that init() sees the final name
    _impl->view->addBlock(model);
    return BlockHandle(std::shared_ptr<void>(std::move(model)));
}

std::expected<BlockHandle, RuntimeError> RuntimeGraph::add(std::shared_ptr<BlockModel> block, std::string_view name) {
    if (!_impl || _impl->view == nullptr) {
        return std::unexpected(localError("graph handle is empty", "RuntimeGraph::add"));
    }
    if (!block) {
        return std::unexpected(localError("block is null", "RuntimeGraph::add"));
    }

    if (!name.empty()) {
        block->setName(std::string(name)); // before addBlock, so that init() sees the final name
    }
    _impl->view->addBlock(block);
    return BlockHandle(std::shared_ptr<void>(std::move(block)));
}

std::expected<BlockHandle, RuntimeError> RuntimeGraph::emplaceSubgraph(std::string_view name, property_map parameters) {
    if (!_impl || _impl->view == nullptr) {
        return std::unexpected(localError("graph handle is empty", "RuntimeGraph::emplaceSubgraph"));
    }

    auto wrapper = std::make_shared<GraphWrapper<Graph>>(std::move(parameters));
    wrapper->setName(std::string(name));

    // Graph(property_map) always takes the global loader, so a nested graph would not inherit
    // whichever loader its parent was given
    if (Graph* inner = wrapper->graph(); inner != nullptr) {
        inner->_pluginLoader = _impl->view->_pluginLoader;
    }

    std::shared_ptr<BlockModel> model = wrapper;
    _impl->view->addBlock(model);
    return BlockHandle(std::shared_ptr<void>(std::move(model)));
}

std::expected<RuntimeGraph, RuntimeError> RuntimeGraph::interior(const BlockHandle& block) const {
    if (!block.valid()) {
        return std::unexpected(localError("block handle is empty", "RuntimeGraph::interior"));
    }

    // graph() is non-null exactly on the graph-like wrappers, which is a cheaper and wider test than
    // comparing typeName(): it also answers for a nested scheduler
    Graph* inner = modelOf(block._model)->graph();
    if (inner == nullptr) {
        return std::unexpected(localError(std::format("block '{}' is not a graph", block.uniqueName()), "RuntimeGraph::interior"));
    }

    auto impl        = std::make_unique<Impl>();
    impl->view       = inner;
    impl->ownerModel = block._model;
    return RuntimeGraph(std::move(impl));
}

std::expected<void, RuntimeError> RuntimeGraph::exportPort(const BlockHandle& innerBlock, bool isInput, std::string_view portName, std::string_view exportedName) {
    if (!_impl || !_impl->ownerModel || !innerBlock.valid()) {
        return std::unexpected(localError("exportPort needs the view of a subgraph's interior", "RuntimeGraph::exportPort"));
    }
    const PortDirection direction = isInput ? PortDirection::INPUT : PortDirection::OUTPUT;

    auto exported = modelOf(_impl->ownerModel)->exportPort(true, innerBlock.uniqueName(), direction, portName, exportedName);
    if (!exported) {
        return std::unexpected(toRuntimeError(exported.error()));
    }
    return {};
}

std::expected<void, RuntimeError> RuntimeGraph::unexportPort(const BlockHandle& innerBlock, bool isInput, std::string_view portName) {
    if (!_impl || !_impl->ownerModel || !innerBlock.valid()) {
        return std::unexpected(localError("unexportPort needs the view of a subgraph's interior", "RuntimeGraph::unexportPort"));
    }
    const PortDirection direction = isInput ? PortDirection::INPUT : PortDirection::OUTPUT;

    auto unexported = modelOf(_impl->ownerModel)->exportPort(false, innerBlock.uniqueName(), direction, portName, {});
    if (!unexported) {
        return std::unexpected(toRuntimeError(unexported.error()));
    }
    return {};
}

property_map RuntimeGraph::exportedInputPorts() const { return _impl && _impl->ownerModel ? modelOf(_impl->ownerModel)->exportedInputPorts() : property_map{}; }

property_map RuntimeGraph::exportedOutputPorts() const { return _impl && _impl->ownerModel ? modelOf(_impl->ownerModel)->exportedOutputPorts() : property_map{}; }

std::vector<BlockHandle> RuntimeGraph::blocks(Recursive recursive) const {
    std::vector<BlockHandle> handles;
    if (!_impl || _impl->view == nullptr) {
        return handles;
    }

    if (recursive == Recursive::No) {
        handles.reserve(_impl->view->blocks().size());
        for (const std::shared_ptr<BlockModel>& block : _impl->view->blocks()) {
            handles.emplace_back(BlockHandle(std::shared_ptr<void>(block)));
        }
        return handles;
    }

    gr::graph::forEachBlock<block::Category::All>(*_impl->view, [&handles](const std::shared_ptr<BlockModel>& block) { handles.emplace_back(BlockHandle(std::shared_ptr<void>(block))); });
    return handles;
}

std::expected<BlockHandle, RuntimeError> RuntimeGraph::find(std::string_view uniqueName, Recursive recursive) const {
    for (const BlockHandle& handle : blocks(recursive)) {
        if (handle.uniqueName() == uniqueName) {
            return handle;
        }
    }
    return std::unexpected(localError(std::format("no block named '{}' in this graph", uniqueName), "RuntimeGraph::find"));
}

std::expected<void, RuntimeError> RuntimeGraph::remove(const BlockHandle& block) {
    if (!_impl || _impl->view == nullptr || !block.valid()) {
        return std::unexpected(localError("graph handle or block handle is empty", "RuntimeGraph::remove"));
    }
    if (auto removed = _impl->view->removeBlockByName(block.uniqueName()); !removed) {
        return std::unexpected(toRuntimeError(removed.error()));
    }
    return {};
}

void RuntimeGraph::clear() {
    if (_impl && _impl->view != nullptr) {
        _impl->view->clear();
    }
}

std::expected<void, RuntimeError> RuntimeGraph::connect(const BlockHandle& sourceBlock, std::string_view sourcePort, //
    const BlockHandle& destinationBlock, std::string_view destinationPort, EdgeSpec edge) {
    if (!_impl || _impl->view == nullptr || !sourceBlock.valid() || !destinationBlock.valid()) {
        return std::unexpected(localError("graph handle or block handle is empty", "RuntimeGraph::connect"));
    }

    // Graph::connect only records the edge; a name that resolves to nothing would surface at
    // edge-resolution time, long after the typo was made
    if (auto ok = checkPortExists(*modelOf(sourceBlock._model), false, sourcePort, "RuntimeGraph::connect"); !ok) {
        return ok;
    }
    if (auto ok = checkPortExists(*modelOf(destinationBlock._model), true, destinationPort, "RuntimeGraph::connect"); !ok) {
        return ok;
    }

    EdgeParameters parameters;
    parameters.minBufferSize = edge.minBufferSize == 0UZ ? undefined_size : edge.minBufferSize;
    parameters.weight        = edge.weight;
    if (!edge.name.empty()) {
        parameters.name = std::move(edge.name);
    }

    auto result = _impl->view->connect(modelPtrOf(sourceBlock._model), PortDefinition(std::string(sourcePort)), //
        modelPtrOf(destinationBlock._model), PortDefinition(std::string(destinationPort)), std::move(parameters));
    if (!result) {
        return std::unexpected(toRuntimeError(result.error()));
    }
    return {};
}

std::expected<void, RuntimeError> RuntimeGraph::disconnect(const BlockHandle& sourceBlock, std::string_view sourcePort, //
    const BlockHandle& destinationBlock, std::string_view destinationPort) {
    if (!_impl || _impl->view == nullptr || !sourceBlock.valid() || !destinationBlock.valid()) {
        return std::unexpected(localError("graph handle or block handle is empty", "RuntimeGraph::disconnect"));
    }

    auto removed = _impl->view->removeEdgeBySourcePort(sourceBlock.uniqueName(), sourcePort, destinationBlock.uniqueName(), destinationPort);
    if (!removed) {
        return std::unexpected(toRuntimeError(removed.error()));
    }
    return {};
}

std::vector<std::string> RuntimeGraph::inputPortNames(const BlockHandle& block) const { return block.valid() ? portNamesOf(*modelOf(block._model), true) : std::vector<std::string>{}; }

std::vector<std::string> RuntimeGraph::outputPortNames(const BlockHandle& block) const { return block.valid() ? portNamesOf(*modelOf(block._model), false) : std::vector<std::string>{}; }

std::string RuntimeGraph::portTypeName(const BlockHandle& block, bool isInput, std::string_view portName) const {
    if (!block.valid()) {
        return {};
    }
    BlockModel& model = *modelOf(block._model);
    model.initDynamicPorts();

    const PortDefinition definition{std::string(portName)};
    const auto           port = isInput ? model.dynamicInputPort(definition) : model.dynamicOutputPort(definition);
    return port.has_value() ? port.value()->typeName() : std::string{};
}

std::vector<std::string> RuntimeGraph::availableBlockTypes() { return gr::globalPluginLoader().availableBlocks(); }

std::vector<std::string> RuntimeGraph::availableSchedulerTypes() {
    std::ignore = gr::registerBuiltinSchedulers();
    return gr::globalPluginLoader().availableSchedulers();
}

Runtime::Runtime(std::unique_ptr<Impl> impl) noexcept : _impl(std::move(impl)) {}

Runtime::Runtime(Runtime&&) noexcept = default;

Runtime& Runtime::operator=(Runtime&&) noexcept = default;

Runtime::~Runtime() {
    if (_impl && _impl->scheduler) {
        _impl->scheduler->stop();
        _impl->drain();
    }
}

std::expected<Runtime, RuntimeError> Runtime::create(RuntimeGraph&& graph, std::string_view type, property_map schedulerParameters) {
    if (!graph._impl || !graph._impl->owned) {
        return std::unexpected(localError("Runtime::create needs a graph this RuntimeGraph owns, not a view", "Runtime::create"));
    }
    std::unique_ptr<Graph> owned = std::move(graph._impl->owned);
    graph._impl.reset();
    return create(std::move(*owned), type, std::move(schedulerParameters));
}

std::expected<Runtime, RuntimeError> Runtime::create(gr::Graph&& graph, std::string_view type, property_map schedulerParameters) {
    std::ignore = gr::registerBuiltinSchedulers();

    PluginLoader& loader = loaderFor(graph);

    std::shared_ptr<SchedulerModel> scheduler = loader.instantiateScheduler(type, schedulerParameters);
    if (!scheduler) {
        return std::unexpected(localError(std::format("unknown scheduler type '{}' -- {} types are available, see availableSchedulerTypes()", type, loader.availableSchedulers().size()), "Runtime::create"));
    }

    auto impl            = std::make_unique<Runtime::Impl>();
    impl->scheduler      = std::move(scheduler);
    impl->schedulerBlock = SchedulerModel::asBlockModelPtr(impl->scheduler);

    // subscribe before the graph starts: an unread msgOut turns a child's error into an exception
    // thrown on a worker thread, taking the reason with it
    if (auto connected = impl->schedulerBlock->msgOut->connect(impl->events); !connected) {
        return std::unexpected(toRuntimeError(connected.error()));
    }
    if (auto connected = impl->commands.connect(*impl->schedulerBlock->msgIn); !connected) {
        return std::unexpected(toRuntimeError(connected.error()));
    }

    impl->scheduler->setGraph(std::move(graph));

    return Runtime(std::move(impl));
}

std::expected<void, RuntimeError> Runtime::runAndWait() {
    if (!_impl) {
        return std::unexpected(localError("runtime handle is empty", "Runtime::runAndWait"));
    }
    const std::expected<void, Error> result = _impl->scheduler->runAndWait();
    _impl->drain();
    if (!result) {
        return std::unexpected(toRuntimeError(result.error()));
    }
    return {};
}

void Runtime::start() {
    if (_impl) {
        _impl->scheduler->start();
    }
}

void Runtime::stop() {
    if (_impl) {
        _impl->scheduler->stop();
        _impl->drain();
    }
}

Runtime::State Runtime::state() const noexcept { return _impl ? static_cast<State>(_impl->schedulerBlock->state()) : State::Idle; }

std::expected<void, RuntimeError> Runtime::requestState(State newState) {
    if (!_impl) {
        return std::unexpected(localError("runtime handle is empty", "Runtime::requestState"));
    }
    if (auto changed = _impl->schedulerBlock->changeStateTo(static_cast<lifecycle::State>(newState)); !changed) {
        return std::unexpected(toRuntimeError(changed.error()));
    }
    return {};
}

std::vector<RuntimeEvent> Runtime::pollEvents(std::size_t maxEvents) {
    std::vector<RuntimeEvent> events;
    if (!_impl) {
        return events;
    }

    std::lock_guard guard(_impl->mutex);
    _impl->drainLocked();

    const std::size_t nEvents = std::min(maxEvents, _impl->pending.size());
    events.reserve(nEvents);
    for (std::size_t i = 0UZ; i < nEvents; ++i) {
        events.push_back(std::move(_impl->pending.front()));
        _impl->pending.pop_front();
    }
    return events;
}

std::expected<void, RuntimeError> Runtime::send(Command command, std::string_view serviceName, std::string_view endpoint, property_map payload, std::string_view clientRequestID) {
    if (!_impl) {
        return std::unexpected(localError("runtime handle is empty", "Runtime::send"));
    }
    MsgPortOut& port = _impl->commands;

    using enum message::Command;
    switch (command) {
    case Command::Get: sendMessage<Get>(port, serviceName, endpoint, std::move(payload), clientRequestID); break;
    case Command::Set: sendMessage<Set>(port, serviceName, endpoint, std::move(payload), clientRequestID); break;
    case Command::Partial: sendMessage<Partial>(port, serviceName, endpoint, std::move(payload), clientRequestID); break;
    case Command::Final: sendMessage<Final>(port, serviceName, endpoint, std::move(payload), clientRequestID); break;
    case Command::Ready: sendMessage<Ready>(port, serviceName, endpoint, std::move(payload), clientRequestID); break;
    case Command::Disconnect: sendMessage<Disconnect>(port, serviceName, endpoint, std::move(payload), clientRequestID); break;
    case Command::Subscribe: sendMessage<Subscribe>(port, serviceName, endpoint, std::move(payload), clientRequestID); break;
    case Command::Unsubscribe: sendMessage<Unsubscribe>(port, serviceName, endpoint, std::move(payload), clientRequestID); break;
    case Command::Notify: sendMessage<Notify>(port, serviceName, endpoint, std::move(payload), clientRequestID); break;
    case Command::Heartbeat: sendMessage<Heartbeat>(port, serviceName, endpoint, std::move(payload), clientRequestID); break;
    case Command::Invalid: return std::unexpected(localError("Command::Invalid is not sendable", "Runtime::send"));
    }
    return {};
}

Runtime::QuiescenceGuard::QuiescenceGuard(Runtime* runtime) noexcept : _runtime(runtime) {}

Runtime::QuiescenceGuard::QuiescenceGuard(QuiescenceGuard&& other) noexcept : _runtime(std::exchange(other._runtime, nullptr)) {}

Runtime::QuiescenceGuard::~QuiescenceGuard() {
    if (_runtime != nullptr && _runtime->_impl) {
        _runtime->_impl->scheduler->releaseWorkQuiescence();
    }
}

Runtime::QuiescenceGuard Runtime::quiesce() {
    if (_impl) {
        _impl->scheduler->requestWorkQuiescence();
        return QuiescenceGuard(this);
    }
    return QuiescenceGuard(nullptr);
}

RuntimeGraph Runtime::graph() const {
    auto impl = std::make_unique<RuntimeGraph::Impl>();
    if (_impl) {
        impl->view       = _impl->schedulerBlock->graph();
        impl->ownerModel = std::shared_ptr<void>(_impl->schedulerBlock);
    }
    return RuntimeGraph(std::move(impl));
}

} // namespace gr
