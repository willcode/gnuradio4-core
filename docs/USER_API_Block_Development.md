# GR4 Block Development — User-API Reference

Note: This document describes the core block API and blocklib SDK surface.
Standard block implementations live in `gnuradio4-blocks`, and reusable DSP
library code lives in `gnuradio4-library`. Use those downstream repositories for
full production block examples.

GR4 blocks are `struct`s that inherit from `gr::Block<Derived>` via CRTP.
A block declares its settings in a nested `struct Settings`, whose members are often
`Annotated<T, ...>` fields, and exposes them through compile-time reflection via
`GR_MAKE_REFLECTABLE`. Blocks implement exactly one
processing function — `processOne` (per-sample), `processBulk` (span-based), or `work(..)` (advanced, from scratch).

Focus on **your algorithm** — the framework handles scheduling, buffer management, tag
propagation, SIMD vectorisation, and settings synchronisation. Lean on `std::ranges`,
`std::algorithms`, and value semantics; avoid manual loops and raw pointers.

## TL;DR — minimal block

```cpp
#include <gnuradio-4.0/Block.hpp>
#include <gnuradio-4.0/BlockRegistry.hpp>

// marker macro — parsed at build time by the block-registration generator
// syntax: GR_REGISTER_BLOCK(fully::qualified::Name, [T], [type1, type2, ...])
//   [T]              — template parameter placeholder expanded over the type list
//   [float, double]  — types to instantiate the block for
GR_REGISTER_BLOCK(example::Gain, [T], [float, double])

namespace example {

template<typename T>
struct Gain : gr::Block<Gain<T>> {
    using Description = Doc<R""(Multiplies input samples by a constant gain factor.)"">;

    gr::PortIn<T>  in;
    gr::PortOut<T> out;

    struct Settings {
        gr::Annotated<float, gr::Doc<"linear gain">> gain = 1.0f;

        GR_MAKE_REFLECTABLE(Settings, gain);
    } settings;

    GR_MAKE_REFLECTABLE(Gain, in, out, settings);

    // t_or_simd<T> accepts both scalar T and SIMD vector types — the framework
    // automatically vectorises processOne calls when the method is const & noexcept
    template<gr::meta::t_or_simd<T> V>
    [[nodiscard]] constexpr V processOne(const V& a) const noexcept {
        return a * static_cast<T>(settings.gain);
    }
};

} // namespace example
```

## Block anatomy — canonical member order

Members **must** appear in this order (blank lines separate groups):

```cpp
template<typename T>
struct MyBlock : gr::Block<MyBlock<T>> {
    // 1. type aliases & nested types
    using Description = Doc<R""(one-line purpose)"">;
    using value_type  = T;

    // 2. ports
    gr::PortIn<T>  in;
    gr::PortOut<T> out;

    // 3. settings, the values the block reports, & public fields
    struct Settings {
        gr::Annotated<float, gr::Doc<"signal sample rate">, gr::Unit<"Hz">> sample_rate = 1.f;

        GR_MAKE_REFLECTABLE(Settings, sample_rate);
    } settings;

    struct Status {
        gr::Size_t overflows = 0U;

        GR_MAKE_REFLECTABLE(Status, overflows);
    } status;

    // 4. GR_MAKE_REFLECTABLE
    GR_MAKE_REFLECTABLE(MyBlock, in, out, settings, status);

    // 5. private state (prefixed _)
    float _cachedCoeff = 0.f;

    // 6. lifecycle methods: start(), stop(), reset(), pause(), resume()
    // 7. processing: processOne() XOR processBulk()
    // 8. checkSettings(const Settings& s) const, applySettings(Settings& s) or applySettings(Settings& s, const Settings& old)
    // 9. helper methods
};
```

> **No custom constructors or destructors.** `Block<T>` provides a carefully sequenced
> constructor that initialises the CRTP chain, reflection metadata, settings pipeline,
> and port buffers. Custom constructors bypass this and break settings deserialisation,
> `property_map` initialisation (via `emplaceBlock<T>({...})`), and lifecycle contracts.
> Use default member values for state, and `applySettings()` or `start()` for
> derived initialisation logic.

## Ports

### Static ports

```cpp
gr::PortIn<float>  in;       // single input
gr::PortOut<float> out;      // single output
```

### Dynamic ports

Declare the count setting `PortCountFor` the collection, and resize the collection in
`applySettings` when the count changed. Such a setting changes only while the graph is stopped,
and never removes a connected port:

```cpp
std::vector<gr::PortIn<T, gr::Async>>  inputs{};
std::vector<gr::PortOut<T, gr::Async>> outputs{};

struct Settings {
    gr::Annotated<gr::Size_t, gr::Visible, gr::Limits<gr::Min<1U>, gr::Max<32U>>, gr::PortCountFor<"inputs">> n_inputs = 2U;

    GR_MAKE_REFLECTABLE(Settings, n_inputs);
} settings;

void applySettings(const Settings& s) {
    if (s.n_inputs.changed()) {
        inputs.resize(s.n_inputs);
    }
}
```

### Port attributes

| Attribute                       | Effect                                               |
| ------------------------------- | ---------------------------------------------------- |
| _(none)_                        | synchronous, required (default)                      |
| `gr::Async`                     | rate-decoupled; excluded from synchronous scheduling |
| `gr::Optional`                  | may be left unconnected                              |
| `gr::RequiredSamples<min, max>` | constrain min/max samples per work call              |

### Port span concepts (for `processBulk`)

| Concept              | Used for                                               |
| -------------------- | ------------------------------------------------------ |
| `gr::InputSpanLike`  | input spans — `consume(n)`, `tags()`, `rawTags`        |
| `gr::OutputSpanLike` | output spans — `publish(n)`, `publishTag(map, offset)` |

Downstream examples: dynamic-port and hardware-backed blocks in `gnuradio4-blocks`.

## Settings & `Annotated<T, ...>`

### The two structs

A block declares each setting of its own as a member of its nested `struct Settings`, and each
value it reports as a member of its nested `struct Status`. Both list their members in their
own `GR_MAKE_REFLECTABLE`, and the block lists one member of each type in its. Core finds the two
members by their types; the examples here name them `settings` and `status`:

```cpp
struct Settings {
    float                                                                    gain_db     = 0.f;
    gr::Annotated<float, gr::Unit<"Hz">, gr::Limits<gr::Over<0.f>>> sample_rate = 48000.f;

    GR_MAKE_REFLECTABLE(Settings, gain_db, sample_rate);
} settings;

struct Status {
    float      current_gain_db = 0.f;
    gr::Size_t overflows       = 0U;

    GR_MAKE_REFLECTABLE(Status, current_gain_db, overflows);
} status;

GR_MAKE_REFLECTABLE(MyBlock, in, out, settings, status);
```

A host names each member by its own name, `gain_db` or `overflows`; the structs add no prefix. The
compiler refuses a name that both structs hold, and a block that declares a setting as a direct
member of its own. A block without settings declares no `Settings`.

A plain member works; `Annotated<>` adds a description, a unit, visibility and limits. Use it
when a setting appears in generated documentation or a settings panel, or when its value has limits.

### Full syntax

```cpp
gr::Annotated<T, Doc<"...">, Unit<"[V]">, Visible, Limits<Min<lo>, Under<hi>>>
```

The variable's name is the setting's name: `Annotated` takes no name string, and every argument after `T` is a type.

| Annotation    | Purpose                                                           |
| ------------- | ----------------------------------------------------------------- |
| `Doc<"...">`  | human-readable description                                        |
| `Unit<"[V]">` | physical SI unit (ISO 80000-1:2022)                               |
| `Visible`     | show in UI settings panels                                        |
| `Limits<...>` | the predicates a number must pass (see "Limits and enumerations") |

A block declares no tag that sets a setting, and the framework infers nothing from a setting's name.
The application or the graph couples a setting to a tag key per block instance, with
`block.setTagCouplings()` or a graph file's `tag_couplings`. A block
that hands a value downstream names it with `this->forwardTag(key, value)` in `applySettings()`. The tag mechanics
document (`USER_API_Tag_mechanics.md`) gives the full rules.

### Shorthand alias

Blocks with many settings often define a local alias:

```cpp
template<typename U, typename... Arguments>
using A = gr::Annotated<U, Arguments...>;

struct Settings {
    A<gr::Size_t, gr::Visible, gr::Limits<gr::Min<1U>, gr::Max<32U>>> n_inputs = 1U;

    GR_MAKE_REFLECTABLE(Settings, n_inputs);
} settings;
```

### Supported types

Scalars (`float`, `double`, `int`, ...), `std::string`, enums, `std::vector<T>`,
`std::array<T, N>`, `gr::Tensor<T>`, and `std::complex<T>`. The
`gnuradio4-library` package additionally provides `gr::UncertainValue<T>`.

### When to use `std::vector<T>` vs. `std::array<T, N>` vs. `gr::Tensor<T>`

| Type               | Use case                                                                                                 |
| ------------------ | -------------------------------------------------------------------------------------------------------- |
| `std::vector<T>`   | variable-length 1D data — filter coefficients, lookup tables, any list whose size changes at runtime     |
| `std::array<T, N>` | fixed-length 1D data — compile-time-known small buffers (e.g. 3-element RGB, fixed-order filter)         |
| `gr::Tensor<T>`    | multi-dimensional data — matrices, spectral maps, anything with rank > 1 or mixed static/dynamic extents |

For simple 1D settings, prefer `std::vector<T>` (runtime-sized) or `std::array<T, N>`
(fixed-size). Reserve `Tensor<T>` for genuinely multi-dimensional data.

### `GR_MAKE_REFLECTABLE`

Lists the block type followed by **all** reflected members: the ports and the members of types
`Settings` and `Status`.
The macro enables compile-time reflection, serialisation, and the settings pipeline.
`gr::Block` adds its own settings `input_chunk_size`, `output_chunk_size`, `stride`, `disconnect_on_done`,
`compute_domain`, `name`, `ui_constraints` and the read-only `unique_name`. These names are reserved: a block
that declares a member of one of them fails to compile, whether or not the base's setting can be written.
`notification_level` is a scheduler's: a block that declares it fails its initialization, so a graph
refuses to add it.

```cpp
GR_MAKE_REFLECTABLE(MyBlock, in, out, settings, status);
```

### `property_map` wire format

```cpp
using gr::property_map = gr::pmt::Value::Map;  // std::pmr::map<std::pmr::string, pmt::Value>
```

Settings are read and written as `property_map` key–value pairs where keys match
the `snake_case` setting name and values are `pmt::Value`.

Downstream examples: richly annotated hardware and oscillator blocks in `gnuradio4-blocks`.

### Per-setting annotations

Further `Annotated<>` arguments declare how the settings treat a member:

| Annotation                          | Effect                                                                                                                                                                       |
| ----------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `StoppedOnly`                       | a change is refused while the graph runs or is paused                                                                                                                        |
| `PortCountFor<"inputs", "outputs">` | the count of the named dynamic port collections; the block resizes them in `applySettings()`; refused while the graph runs or is paused, or when it removes a connected port |

```cpp
gr::Annotated<gr::Size_t, gr::PortCountFor<"inputs">> n_inputs = 2U;
gr::Annotated<std::string, gr::StoppedOnly>           serial;
```

The compiler refuses `PortCountFor` on a non-integer member, a name that is no dynamic port collection
of the block, and a collection two members size. A `const` or `gr::meta::immutable<>` member is
read-only. The block's metadata carries `<key>::type` for every key, and `<key>::update_policy`,
`::port_count_for`, `::access`, `::minimum` and `::maximum` where they apply. Beside each end,
`::minimum_excluded` and `::maximum_excluded` say whether it is excluded; `::one_of` holds the
values of a `OneOf` in the setting's type, and `::limits` the limits as a refusal states them after
"must be", `"> 0 and < 0.5"`, which `grinfo` prints.

### Limits and enumerations

A limit on one value is written beside its declaration, as predicates in `Limits`, all of which must
hold:

```cpp
gr::Annotated<gr::Size_t, gr::Limits<gr::Min<1U>, gr::Max<64U>>>       taps       = 31U;
gr::Annotated<float, gr::Limits<gr::Over<0.f>, gr::Under<0.5f>>>     cutoff     = 0.25f;
gr::Annotated<float, gr::Limits<gr::Over<0.f>, gr::Max<1.f>>>        weight     = 1.f;
gr::Annotated<gr::Size_t, gr::Limits<gr::Min<1U>>>                   length     = 8U;
gr::Annotated<gr::Size_t, gr::Limits<gr::OneOf<1U, 2U, 4U>>>     decimation = 2U;
```

`Min` and `Max` include their value, `Over` and `Under` exclude it, and `OneOf` holds for a value
equal to one it lists. A NaN fails every predicate, and a floating-point setting with any predicate
refuses positive and negative infinity with `"must be finite"`; a setting without limits takes both
as sent.
`Limits` takes these predicates and nothing else. A value the predicates refuse is
refused with one text that names each of them, `"must be > 0 and < 0.5"`, `"must be >= 1"`,
`"must be one of: 1, 2, 4"`; an end no value of the setting's type can break is left out. A
declared default outside them holds the block not ready with `"the default must be > 0 and < 0.5"`
until a value inside them is set.

The compiler refuses a list of predicates that is faulty, with one sentence each: a value the
setting's type cannot hold, two lower or two upper ends, a lower end at or above the upper end (equal ends pass
when `Min` and `Max` both include it), a `OneOf` with no value or one that repeats a value, and a
`OneOf` value another predicate excludes. Two numbers or a test function in place of the predicates are
refused with `"Limits takes predicates alone: Min, Max, Over, Under and OneOf"`. Any other rule on a value, and
every rule that relates settings, goes into `checkSettings`.

A choice among named options is an `enum class` member. Its enumerators are spelled in lower case
with underscores, as the standard library spells its own (`std::memory_order::relaxed`):
`msb_first`. A host, a tag and a graph file send that spelling exactly; core refuses any other name
with the list, `"must be one of: msb_first, lsb_first"`, and returns the enumerator as spelled. The
block carries no check for it.

A block that takes part of a shared enumeration lists the enumerators it takes in `OneOf`:

```cpp
gr::Annotated<Taper, gr::Limits<gr::OneOf<Taper::hann, Taper::hamming>>> taper = Taper::hann;
```

Any other enumerator is refused with `"must be one of: hann, hamming"`, and the metadata key
`taper::one_of` lists those names beside `taper::enum_values`, which lists the whole enumeration. On an
enumeration `OneOf` is the one predicate: `Min`, `Max`, `Over` and `Under` bound a number, and the
compiler refuses them there, as it refuses a `OneOf` that repeats an enumerator or names one of another
enumeration.

### Changing a setting while the graph runs

`block.set(params)` checks `params` and returns `std::expected<Values, Rejected>`. On a block no
graph holds and that does not run, the change applies at the call. A block of a graph that does not
run holds it until its next start, and `set()` returns the key pending, "not running". A
`PortCountFor` setting applies at the call. On a running block the change waits for the top of the
block's next `work()` call, and `set()` waits up to its timeout for that apply;
`block.set(params, settings::noWait)` is the same call that does not wait. A block never calls
`set` or `resetDefaults` on its own settings: from its work function or its hooks the call is
refused with `"a block changes its own settings in applySettings()"`.

A `Rejected` result means no key of the call took effect; each `Rejection` names a refused key and
its reason. The `Values` of an accepted call hold a `gr::settings::Value` for each key not in force
as sent, and none for a key that is: an empty `Values` means every key took. An entry's `state` is
`value` for a substitute the block published, with its reason; `pending` for a key that waits, with
the cause "not running", "timeout", or "applies at the block's next work call" for a `noWait` call or
a call from a scheduler's worker thread, neither of which waits; or `failed` for a key whose apply failed, with the
reason. `block.check(params)` refuses as `set()` would and stores nothing; on a block not yet set up
it first runs the setup, apply hook included. `block.get(key)` returns
the key's `Value`: the value in force, a substitute the block published with its reason, or, on a
block that does not run, a value set that waits for the next start. `get(keys)` and `get()` return
`Values` for the keys asked or for every key, and `get<T>(key)` returns the value as `T`, refused
when `T` cannot hold it without loss.

A host reads a status value as it reads a setting, `block.get("overflows")`, and `set()` refuses it
with `"status is set by the block"`. The block writes `status` from its own functions: its work
function, its hooks and `applySettings()`. Core copies `Status` for a host after each hook and,
while a host reads or a watcher follows it, after each work call; a value from another thread is
handed to the work function, which writes it.

Inside `applySettings()` the block assigns a setting it replaces, `s.gain = v`.
`this->publish("key", value, reason)` reports a value that changes when no `set()` waits, such as a
device that later runs another value than the one set, from any of the block's threads. The
settings keep the published value and its reason beside the value last set until the next write of
the key, and `get()` returns the published value with its reason. A key the block does not declare,
or a value its field cannot hold, raises an error notification. A `publish` during the apply, from
the hook or the reset hook, changes nothing and raises one `failed` notification whose reason says
to assign the setting in `applySettings()`.

A setting is given at construction, `Block({{"gain", 2.f}})`, or by `set()`; neither the block's
constructor nor a host assigns `block.settings.gain` itself. At the setup a setting that no caller
gave and that differs from the value a default-constructed `Settings` holds holds the block not
ready with `"gain: assigned directly; give it at construction or call set()"`, until a call gives
it. After the setup a setting written outside `applySettings()`, by the block or from outside,
raises an error notification naming it at the next apply or when the block stops, and takes back
its value in force.
`BlockModel::setName()` on a block set up is a `set()` of `name` with `noWait`: a save returns the
new name at once, `get("name")` too unless the block runs, and `name()` returns it once the block
applies it, at the start on a stopped graph; a scheduler-managed subgraph that runs applies it at
its scheduler's next start. It returns the result of the `set()`, a refusal included. A host
writes the UI map with `set("ui_constraints", ...)`; `uiConstraints()` reads it.

A notification names the block, the key and a reason: `failed` at the `error` level for a key that
failed, `changed` at `info` for a key that moved. The nearest scheduler's `notification_level`,
`warning` by default, sets the lowest level that passes. A scheduler at `off` prints nothing; its
installed readers still receive warnings and errors. A block's own `off` sends nothing. Until a host
reads them, the top scheduler prints the first notification of each block, key and level to standard
error, then a count at each tenfold. A host reads them by `installReader(reader)` on the scheduler, which hands each `gr::settings::Notification` to
`reader`, or by a subscription to the scheduler's `SettingsNotification` endpoint
(`gr::Runtime::kSettingsNotificationEndpoint`), whose events carry `level`, `what`, `block`, `key`,
`reason` and `replaced`. A block no scheduler holds prints its notifications to standard error at
once.

A merged block's members run inside its work function. The scheduler runs a blocking block's stop
and pause hooks while a worker may be inside its work call, and there they count as another thread:
the block's next work call copies the `Status` they wrote. A hook that the block's own work call runs, at the end of its stream or after
`requestStop()`, runs on the block's own thread. A block that overrides `work()` and does not call
the base's runs its code as another thread.

## Processing functions

Implement exactly one: `processOne`, `processBulk`, or — for full control —
override `work()` directly (see [Advanced: custom `work()` function](#advanced-custom-work-function)).

### `processOne` — per-sample (preferred for 1:1 transforms)

```cpp
// 1:1 transform
template<gr::meta::t_or_simd<T> V>
[[nodiscard]] constexpr V processOne(const V& a) const noexcept {
    return a * static_cast<T>(gain);
}

// source (no input args)
[[nodiscard]] constexpr T processOne() noexcept {
    return _nextSample++;
}

// sink (void return)
constexpr void processOne(const T& input) noexcept {
    _buffer.push_back(input);
}
```

### `processBulk` — span-based (resampling, variable-rate, tag access)

```cpp
[[nodiscard]] constexpr gr::work::Status processBulk(
        std::span<const T> input, std::span<T> output) noexcept {
    std::ranges::copy(input, output.begin());
    return gr::work::Status::OK;
}

// with InputSpanLike / OutputSpanLike for tag access and manual publish/consume
gr::work::Status processBulk(gr::InputSpanLike auto& inSpan,
                             gr::OutputSpanLike auto& outSpan) {
    const auto nSamples = std::min(inSpan.size(), outSpan.size());
    std::ranges::copy_n(inSpan.begin(), nSamples, outSpan.begin());
    outSpan.publish(nSamples); // optional, if ommitted then all samples are consumed/published
    return gr::work::Status::OK;
}

// source (output only)
gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
    std::ranges::fill(outSpan, T{});
    outSpan.publish(outSpan.size());
    return gr::work::Status::OK;
}

// multi-input (dynamic ports)
template<gr::InputSpanLike TInSpan>
gr::work::Status processBulk(std::span<TInSpan>& ins, gr::OutputSpanLike auto& out) {
    // ins[0], ins[1], ...
    return gr::work::Status::OK;
}
```

`publish(n)` and `consume(n)` may be smaller than the span the framework handed over — ending a
chunk early is how a block stops at a retune point or a frame boundary, and the unused remainder
of the output reservation is released back to the buffer. `n` larger than the span is a contract
violation: it would hand the downstream block ring slots that were never written, so it is clamped
to the span and reported on `stderr`. Output ports on a multi-producer buffer are the one exception
and must publish the whole span, because the gap would stall every later publication.

### SIMD-aware `processOne`

When `processOne` is `const` and `noexcept`, the framework automatically vectorises it
using `vir::simd` / `std::simd`. The concept `gr::meta::t_or_simd<T>` accepts both
scalar `T` and SIMD vector types — no special code is needed:

```cpp
template<gr::meta::t_or_simd<T> V>
[[nodiscard]] constexpr V processOne(const V& a) const noexcept {
    if constexpr (gr::meta::any_simd<V>) {
        // optional: SIMD-specific path (rarely needed)
    } else {
        // scalar path
    }
    return a * static_cast<T>(gain);  // works for both scalar and SIMD
}
```

The framework determines the optimal SIMD width at compile time, packs input samples
into SIMD registers, calls `processOne` with the widened type, and scatters results
back to the output buffer — all transparently. A non-`const` or non-`noexcept`
`processOne` falls back to scalar-only evaluation.

### SIMD-aware source blocks

Processing blocks receive SIMD-widened inputs from the framework, so SIMD support
comes naturally via `t_or_simd<T>`. Source blocks have no inputs — they produce samples
from internal state. To enable SIMD output, provide a second `processOne` overload that
takes a compile-time width parameter:

```cpp
template<typename T>
struct MySource : gr::Block<MySource<T>> {
    gr::PortOut<T> out;
    GR_MAKE_REFLECTABLE(MySource, out);

    // scalar: produce one sample
    [[nodiscard]] constexpr T processOne() const noexcept { return T{}; }

    // SIMD: produce N samples at once (N is a compile-time constant)
    [[nodiscard]] constexpr auto processOne(gr::meta::constexpr_value auto N) const noexcept {
        return gr::meta::simdize<T, N>{};  // zero-filled SIMD vector
    }
};
```

The framework detects the width-accepting overload and calls it with the optimal SIMD
width instead of invoking the scalar `processOne()` in a loop. The width parameter `N`
is a `gr::meta::constexpr_value` — use it directly with `gr::meta::simdize<T, N>` to construct
SIMD return types.

The scalar `processOne()` (no arguments) is always required as the fallback — it is
used for non-SIMD types and for the epilogue when the sample count is not a multiple
of the SIMD width.

A stateful source (non-`const` `processOne`) can also provide the SIMD overload:

```cpp
[[nodiscard]] constexpr T processOne() noexcept {
    return static_cast<T>(count++);  // scalar fallback
}

[[nodiscard]] constexpr auto processOne(gr::meta::constexpr_value auto N) noexcept
requires std::is_arithmetic_v<T>
{
    gr::meta::simdize<T, N> result;
    for (std::size_t i = 0; i < result.size(); ++i) {
        result[i] = static_cast<T>(count++);
    }
    return result;
}
```

### `work::Status`

| Value                            | Meaning                               |
| -------------------------------- | ------------------------------------- |
| `OK` (0)                         | processed successfully                |
| `DONE` (-1)                      | block finished; flowgraph should stop |
| `INSUFFICIENT_INPUT_ITEMS` (-2)  | need more input samples               |
| `INSUFFICIENT_OUTPUT_ITEMS` (-3) | need a larger output buffer           |
| `ERROR` (-100)                   | error occurred                        |

Downstream examples: source, sink, 1:1 transform, and history-based blocks in
`gnuradio4-blocks`.

## Resampling & decimation/interpolation

Resampling is a **first-class citizen** in GR4. Declare the ratio as a `Block` template
argument and the framework handles chunked scheduling, buffer sizing, and automatic
`sample_rate` tag adjustment.

### Declaring resampling

```cpp
// default: 1:1 fixed during compile-time (isConst = true)

// decimation 8:1 — for every 8 input samples, produce 1 output
template<typename T>
struct Decimator : gr::Block<Decimator<T>, gr::Resampling<8U, 1U, true>> { ... };

// interpolation 1:2 — for every 1 input sample, produce 2 outputs
template<typename T>
struct Upsampler : gr::Block<Upsampler<T>, gr::Resampling<1U, 2U, true>> { ... };

// runtime-configurable ratio (isConst = false)
template<typename T>
struct FlexResampler : gr::Block<FlexResampler<T>, gr::Resampling<10U, 1U>> { ... };
```

| Parameter                                | Default           | Purpose                                       |
| ---------------------------------------- | ----------------- | --------------------------------------------- |
| `Resampling<inChunk, outChunk, isConst>` | `<1U, 1U, false>` | decimation / interpolation ratio              |
| `Stride<N, isConst>`                     | `<0, false>`      | sample spacing between calls (0 = no overlap) |

### How it works

For `Resampling<N, M>`:

- the scheduler provides exactly `k * N` input samples and `k * M` output samples to `processBulk`
- settings `input_chunk_size` and `output_chunk_size` are auto-created on the block
- when `isConst = false` (default), these settings can be changed at runtime
- **automatic `sample_rate` tag adjustment**: when the framework forwards a `sample_rate`
  tag through a resampling block, it multiplies by `output_chunk_size / input_chunk_size`

### Stride (overlapping windows)

`Stride<N>` advances the input pointer by `N` samples between `processBulk` calls
instead of consuming the full input span. This enables overlapping-window processing
(e.g. FFT with 50 % overlap):

```cpp
template<typename T>
struct WindowedFFT : gr::Block<WindowedFFT<T>, gr::Stride<512U>> { ... };
```

A stride longer than the chunk skips the samples between two chunks. The tags on them set the
block's coupled settings. The default forwarder publishes them at the next chunk's first output
sample, or at the end-of-stream index. A `forwardTags()` override receives them with the next chunk
(see `USER_API_Tag_mechanics.md`). A stride shorter than the chunk reads a sample in two chunks. The
chunk that consumes a tag's sample forwards it, once.

Downstream examples: converter and filter blocks in `gnuradio4-blocks`.

## Lifecycle methods

### State machine

The full state diagram including transition functions
(see also [`LifeCycle.hpp`](../core/include/gnuradio-4.0/LifeCycle.hpp)):

```
                Block<T>()              can be reached from
                   │                   anywhere and anytime.
              ┌─────┴────┐                   ┌────┴────┐
 ┌────────────┤   IDLE   │                   │  ERROR  │
 │            └────┬─────┘                   └────┬────┘
 │                 │ init()                       │ reset()
 │                 v                              │
 │         ┌───────┴───────┐                      │
 ├<────────┤  INITIALISED  ├<─────────────────────┤
 │         └───────┬───────┘                      │
 │                 │ start()                      │
 │                 v                              │
 │   stop() ┌──────┴──────┐                      │  ╓
 │ ┌────────┤   RUNNING   ├<──────────┐          │  ║
 │ │        └─────┬───────┘           │          │  ║  isActive(state) → true
 │ │              │ pause()           │          │  ║
 │ │              v                   │ resume() │  ║
 │ │    ┌─────────┴─────────┐   ┌─────┴─────┐   │  ║
 │ │    │  REQUESTED_PAUSE  ├──>┤  PAUSED   │   │  ║
 │ │    └──────────┬────────┘   └─────┬─────┘   │  ╙
 │ │               │ stop()           │ stop()  │
 │ │               v                  │         │  ╓
 │ │     ┌─────────┴────────┐         │         │  ║
 │ └────>┤  REQUESTED_STOP  ├<────────┘         │  ║
 │       └────────┬─────────┘                   │  ║  isShuttingDown(state) → true
 │                │                             │  ║
 │                v                             │  ║
 │          ┌─────┴─────┐ reset()               │  ║
 └─────────>│  STOPPED  ├──────────────────────>┘  ║
            └─────┬─────┘                          ╙
                  │
                  v
              ~Block<T>()
```

### Optional lifecycle overrides

All lifecycle methods are **optional** — implement only what your block needs:

```cpp
void start()  { /* acquire resources */ }
void stop()   { /* release resources */ }
void reset()  { /* clear internal state, return to INITIALISED */ }
void pause()  { /* suspend activity */ }
void resume() { /* resume after pause */ }
```

### Runtime control

```cpp
this->requestStop();                          // signal end from within processOne/processBulk
gr::lifecycle::isActive(this->state());       // true for RUNNING, REQUESTED_PAUSE, PAUSED
gr::lifecycle::isShuttingDown(this->state()); // true for REQUESTED_STOP, STOPPED
```

Downstream examples: hardware-backed lifecycle and finite-source blocks in
`gnuradio4-blocks`.

## Settings change callback

A rule that relates settings belongs in the optional check, which refuses a call before any key of
it merges. The check takes each value as within its own `Limits`:

```cpp
std::optional<gr::Refused> checkSettings(const Settings& s) const;
```

Core may call the check often and from a host's thread, for a `check()` that stores nothing as for a
`set()`. So the check reads its argument `s` alone, never the block's own `Settings` member, which the
apply may be writing at the time; changes nothing in the block; and returns quickly. The compiler refuses a check
that is not `const`.

`s` holds every setting at the value the call would put in force. `gr::Refused(s.map_out, "map_in
and map_out must have the same length")` refuses the setting by its member, `gr::Refused("...")`
names none, and `add()` collects several. A refusal that names no setting is reported against each
setting the call changes; at the block's setup and at a start it stays against the block. A throw
from the check is its refusal with the thrown message. A refusal states what the value must be,
with "must", and a rule that relates two settings names both, with the setting a host most likely
changes attached.

The apply hook runs on the block's applying thread, **before** the next processing call, once per
batch of changes that moves a value, after core writes the new values into the block's `Settings`
member. It takes that member as `s`:

```cpp
void applySettings(const Settings& s); // or void applySettings(const Settings& s, const Settings& old);
```

The hook takes `s` as `Settings&` where it assigns a setting, and `const` otherwise. A block that
names its member other than `settings` receives that member. A form without `s` is refused by the
compiler.

A block learns what changed from the setting, `if (s.cutoff.changed())`. `changed()` is a member of
`Annotated`, so a setting the hook asks about is declared as one. It reads true for every setting at
the block's setup, so a block builds its state there the same way it rebuilds it later, and
afterwards for a setting whose value differs from the one before the batch. It reads false outside
the hook, also after a hook that throws. The hook's own assignment to a setting leaves its flag, also
an assignment from another setting such as `s.gain = old.gain`, and a copy of `Settings` constructed
from `s` carries the flags `s` held. `old` holds the values before the batch, the
declared defaults at the setup; it serves a block that needs the earlier value itself, such as the
size of a step. Inside the hook the block may:

- assign one of its own settings, `s.trigger = false;` or `s.gain_db = nearest;`: a
  host reads and saves the assigned value; one that differs from the value before the batch is
  announced at `info` as the caller's change, and one outside the setting's `Limits` fails the key.
  It serves a command the block takes back and a value rounded to what a device has; a value others depend on, such as
  `sample_rate`, keeps the setting and reports the value in force as `current_sample_rate` in
  `Status`;
- fail a key where doing the thing fails, `this->fail(s.level, "runs 10 at most")`. At the setup
  a failure refuses the block;
- hand a value downstream, `this->forwardTag("gr:sample_rate", rate)`; what the hook forwards at the
  setup is dropped.

A throw from the hook, a `fail()` of any setting or status value, and an assigned value outside its
limits each take back every key the batch changed and every setting the hook assigned, none
announced as changed. A block that runs another value than the one set assigns it in the hook. The
host sees the failure under each key: the text of a throw, or the reason under the key the block
failed or assigned and that key before the reason under any other, as in
`attenuation_db: not reachable`. Core restores the settings and does not run the hook again, so a
hook builds its new state first and commits it last, after the last place it can fail.

Each way of reporting what the block runs serves one case:

- a member of `Status`: a value only the block sets, such as a level it measures; a host reads it
  and never sets or saves it;
- an assignment in the hook: the block replaces the value set, a command it takes back or a value
  rounded to what the device has; a host reads and saves the assigned value;
- `this->publish(key, value, reason)`: the device runs another value than the one set, reported
  after the hook from any of the block's threads; a read returns it with its reason, and a host
  saves the value set;
- `this->fail(key, reason)`: the device could not take the value set; the key fails with `reason`.
  In the hook the batch goes back; in the start hook the key is one the device lacks, and a read
  lists it no more.

A block that both sets a value and reports the value in force splits it into a pair: the setting
`gain_db` and the status value `current_gain_db`.

```cpp
struct Leveler : gr::Block<Leveler> {
    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    struct Settings {
        gr::Annotated<float, gr::Limits<gr::Min<0.f>, gr::Max<1.f>>>     target  = 0.5f;
        gr::Annotated<float, gr::Doc<"start gain">, gr::Limits<gr::Over<0.f>>> gain    = 1.f;
        bool                                                                       restart = false;

        GR_MAKE_REFLECTABLE(Settings, target, gain, restart);
    } settings;

    struct Status {
        float      current_gain = 1.f;
        gr::Size_t count        = 0U;

        GR_MAKE_REFLECTABLE(Status, current_gain, count);
    } status;

    GR_MAKE_REFLECTABLE(Leveler, in, out, settings, status);

    float _step = 0.f;

    // takes back `restart`, a command
    void applySettings(Settings& s) {
        _step = 0.01f * s.target;
        if (s.gain.changed() || s.restart) {
            status.current_gain = s.gain;
            s.restart           = false;
        }
    }

    [[nodiscard]] gr::work::Status processBulk(std::span<const float> input, std::span<float> output) {
        float level = 0.f;
        for (std::size_t i = 0UZ; i < input.size(); ++i) {
            output[i] = status.current_gain * input[i];
            level     = std::max(level, std::abs(output[i]));
        }
        status.current_gain = level > settings.target ? std::max(status.current_gain - _step, 0.01f) : status.current_gain + _step;
        ++status.count;
        return gr::work::Status::OK;
    }
};
```

The framework publishes the forwarded settings on the block's outputs. It writes no tag for a
setting itself.

### Common patterns

**Dynamic port resize:**

```cpp
if (s.n_inputs.changed())  { inputs.resize(s.n_inputs); }
if (s.n_outputs.changed()) { outputs.resize(s.n_outputs); }
```

**A device value rounded to what the device has:**

```cpp
if (s.gain_db.changed()) {
    s.gain_db = device.nearestGain(s.gain_db); // a host reads the rounded gain
    device.setGain(s.gain_db);
}
```

Downstream examples: mutually exclusive settings and dynamic-port validation in
`gnuradio4-blocks`.

## Tags & streaming metadata

A tag is a `{index, property_map}` pair attached to a specific sample position.

### Tag access in `processOne`

```cpp
[[nodiscard]] constexpr T processOne(const T& input) noexcept {
    if (this->inputTagsPresent()) {
        const gr::Tag& tag = this->mergedInputTag();
        // read tag.map entries
    }
    this->publishTag({{"my:event", "detected"}}, 0UZ);  // publish at current sample
    return input;
}
```

### Tag access in `processBulk`

```cpp
gr::work::Status processBulk(gr::InputSpanLike auto& inSpan,
                             gr::OutputSpanLike auto& outSpan) {
    // iterate over tags within current chunk
    for (const auto& [normalisedIndex, tagMap] : inSpan.tags()) {
        outSpan.publishTag(tagMap.get(), static_cast<std::size_t>(normalisedIndex));
    }

    // or access raw tags directly
    for (const auto& tag : inSpan.rawTags) { /* tag.index, tag.map */ }

    std::ranges::copy(inSpan, outSpan.begin());
    outSpan.publish(inSpan.size());
    return gr::work::Status::OK;
}
```

### Tag forwarding policy

By default, every tag from every synchronous input port is forwarded to every synchronous output
port, with every key. On a block with only synchronous stream ports and without `Resampling<>` or
`Stride<>`, a tag leaves at the offset it arrived at. On a block with `Stride<>` or an asynchronous
stream port the framework ends a chunk at the next tag, and the tag leaves on the call's first
output sample. A block declaring `Resampling<>` maps input index `i` of the samples a call consumed
to `floor(i * output_chunk_size / input_chunk_size)`, after the call. `BackwardTagPropagation` puts
every tag of a chunk on its first output sample. `DroppedTagKeys<"key", ...>` drops the listed keys.
A block whose output samples do not stand where the framework puts a tag, a delay for instance,
handles tags itself, with `forwardTags()` or with default forwarding disabled:

```cpp
template<typename T>
struct MyBlock : gr::Block<MyBlock<T>, gr::NoTagPropagation> { ... };
```

### End-of-stream

```cpp
this->publishEoS();  // convenience: publishes {gr::tag::END_OF_STREAM, true}
```

### End-of-stream epilogue — `processEpilogue()`

When `end_of_stream` arrives and the remaining input samples cannot form a complete chunk
(fewer than `input_chunk_size`), the default behaviour is to **drop the trailing samples** and
stop. For blocks that accumulate internal state (FFT windows, protocol framers, file writers),
provide `processEpilogue()` with the same signature as `processBulk()`:

```cpp
struct MyAccumulator : gr::Block<MyAccumulator, gr::Resampling<1024, 1024, true>> {
    gr::PortIn<float>  in;
    gr::PortOut<float> out;
    GR_MAKE_REFLECTABLE(MyAccumulator, in, out);

    gr::work::Status processBulk(InputSpanLike auto& input, OutputSpanLike auto& output) noexcept {
        // normal processing — input.size() == 1024
    }

    gr::work::Status processEpilogue(InputSpanLike auto& input, OutputSpanLike auto& output) noexcept {
        // trailing samples — input.size() < 1024
        const auto n = std::min(input.size(), output.size()); // the span can be shorter than the tail
        std::copy_n(input.begin(), n, output.begin());
        std::ranges::fill(output.subspan(n), 0.f); // zero-pad remainder
        output.publish(output.size());              // explicit publish required
        return gr::work::Status::OK;
    }
};
```

- **Opt-in**: detected via `if constexpr` — zero cost for blocks without it.
- **Output publishing**: call `output.publish(n)` explicitly; default is zero samples published.
- **Output span size**: `max(ceil(trailing * output_chunk_size / input_chunk_size), output_chunk_size)`, and at most
  the size of the output ring, which can be shorter than the tail needs. The epilogue runs once the output has that
  much room. When the blocks downstream take nothing for 1024 calls in a row, the span is the room the output has.
- **Stop request**: when the block requests its own stop during a call, the span is the smaller of the request and
  the room the output has. A stop the scheduler requests ends the block without the epilogue.
- **Short span**: the epilogue writes at most `output.size()` samples. A block whose output cannot be split writes
  nothing into a short span.
- **Call sequence**: called once, immediately before transitioning to `REQUESTED_STOP`.
- **Without `processEpilogue()`**: trailing samples are dropped. At the end of a stream, the block sends one plain
  message on its message output for each connected input, synchronous or asynchronous, that holds samples no call
  was handed: endpoint `dropped tail`, with `port` and `samples_dropped`. The count is the samples in front of the
  input's end-of-stream tag, or all the input holds when it has none, less those a `processBulk()` call was handed
  past the input's read position and nothing has consumed since. An input without an end-of-stream tag reports what
  it holds when the block stops. An input of a port collection is named `<name>#<index>`, the form
  `connect()` takes. The block sends no message when it is stopping. A block that processes the tail defines
  `processEpilogue()`.

### Standard tag keys

A tag key is `domain:name`. Only GNU Radio defines a new `gr:` key. Any block may write or read a
`gr:` key with the meaning GNU Radio gives it. A block's new keys go in its author's own domain.
The constants in `gr::tag::` hold the `gr:` keys.

| Key                                   | Type          | Description                        |
| ------------------------------------- | ------------- | ---------------------------------- |
| `"gr:sample_rate"`                    | `float`       | signal sample rate [Hz]            |
| `"gr:signal_name"`                    | `std::string` | signal name                        |
| `"gr:signal_quantity"`                | `std::string` | physical quantity (e.g. "voltage") |
| `"gr:signal_unit"`                    | `std::string` | SI unit (e.g. "[V]")               |
| `"gr:signal_min"` / `"gr:signal_max"` | `float`       | expected value range               |
| `"gr:trigger_name"`                   | `std::string` | trigger identifier                 |
| `"gr:trigger_time"`                   | `uint64_t`    | UTC timestamp [ns]                 |
| `"gr:trigger_offset"`                 | `float`       | sample delay w.r.t. trigger [s]    |
| `"gr:context"`                        | `std::string` | multiplexing context key           |
| `"gr:end_of_stream"`                  | `bool`        | end-of-stream marker               |
| `"gr:reset_default"`                  | `bool`        | reset block to stored defaults     |

Downstream examples: tag source, tag monitor, and clock source blocks in
`gnuradio4-blocks`.

## Graph & connections

```cpp
#include <gnuradio-4.0/Graph.hpp>
#include <gnuradio-4.0/Scheduler.hpp>

gr::Graph graph;

// emplace blocks with optional initial settings
auto& src  = graph.emplaceBlock<example::Source<float>>({{"n_samples_max", gr::Size_t(1024)}});
auto& gain = graph.emplaceBlock<example::Gain<float>>({{"gain", 2.0f}});
auto& sink = graph.emplaceBlock<example::Sink<float>>();

// connect by port name
graph.connect<"out">(src).to<"in">(gain);
graph.connect<"out">(gain).to<"in">(sink);

// run
gr::scheduler::Simple sched;
sched.exchange(std::move(graph));
sched.runAndWait();
```

## Block registration

`GR_REGISTER_BLOCK` is a marker macro parsed at build time by the block-registration generator.
It **must** appear inside the namespace, immediately **before** the block's `template`
declaration. It does not generate code itself — the generator script scans for it
while the library configures and writes the required template instantiations and
registry entries.

### Syntax

```
GR_REGISTER_BLOCK(BlockType)
GR_REGISTER_BLOCK(BlockType, [T], [type1, type2, ...])
GR_REGISTER_BLOCK(BlockType, ([T], [U]), [types for T], [types for U])
GR_REGISTER_BLOCK(BlockType, ([T], 3UZ), [types for T])
GR_REGISTER_BLOCK("custom::name", BlockType, ([T], policy<[T]>), [types for T])
```

### Variants

```cpp
// non-template block
GR_REGISTER_BLOCK(my::BlockNoTemplates)

// single type parameter — [T] is expanded over [float, double]
GR_REGISTER_BLOCK(my::Gain, [T], [float, double])

// multiple type parameters — each [T], [U] has its own type list
GR_REGISTER_BLOCK(my::Converter, ([T], [U]), [float, double], [int, long])

// extra non-type template arguments (here: 3UZ)
GR_REGISTER_BLOCK("my::ThreePhasePower", my::PowerMetrics,
                   ([T], 3UZ), [float, double])

// custom name + policy template argument — [T] inside policy is also expanded
GR_REGISTER_BLOCK("my::AddConst", my::MathOpImpl,
                   ([T], std::plus<[T]>), [float, double, std::complex<float>])
```

| Syntax element          | Meaning                                                                           |
| ----------------------- | --------------------------------------------------------------------------------- |
| `[T]`, `[U]`            | type parameters to expand                                                         |
| `3UZ`, `std::plus<[T]>` | fixed non-type/type template arguments                                            |
| `[float, double]`       | type list for expansion of corresponding `[T]`                                    |
| `"custom::name"`        | optional custom registered name (default: the block type as the marker writes it) |

Fixing a template argument in the registration is supported but is not how
`gnuradio4-blocks` spells these blocks. Where a block is an alias for a more
general template — `AddConst<T>` for `MathOpImpl<T, std::plus<T>>`,
`ThreePhasePowerMetrics<T>` for `PowerMetrics<T, 3UZ>` — register the alias
with a single `[T]`. A marker without a quoted name registers the block type it
names, and the registered name is then the element type alone,
`gr::blocks::math::AddConst<float32>`, rather than one that repeats the fixed
argument.

Downstream examples: common blocks, math blocks, power estimators, and
hardware-backed blocks in `gnuradio4-blocks`.

### Device listings

A library whose blocks drive hardware registers a `gr::DeviceListing` for each
family word it serves: the word, whether listing the family opens a unit, the
directions its blocks carry, and a function that returns each attached unit as
a label and a selector. A block library and a plugin both add it to
`gr::globalDeviceListingRegistry()` from a static initializer. The first
registration of a family word holds it, and `add()` returns false for a later
one.

```cpp
std::vector<gr::DeviceUnit> listMyUnits(); // the label and selector of each unit

const bool registered = gr::globalDeviceListingRegistry().add(
    {.family = "mydev", .opens = false, .receive = true, .transmit = false, .listUnits = listMyUnits});
```

`PluginLoader::listDevices()` asks every family whose listing opens no unit, and
`listDevices(gr::Opening::Allowed)` asks every family. The result holds each
unit with its family's word and directions, and a report of every family: a
family left out, a listing that threw and a listing that found nothing each
carry their reason. `grinfo devices` prints the result. Core compares and
orders family words and passes the selector on unread; it gives neither a
meaning.

## Dos and don'ts

| Do                                                 | Don't                                                                                                  |
| -------------------------------------------------- | ------------------------------------------------------------------------------------------------------ |
| use `struct` (default)                             | use `class` without a genuine invariant                                                                |
| `snake_case` for reflected settings                | `camelCase` for settings                                                                               |
| `lowerCamelCase` for methods                       | `snake_case` for methods                                                                               |
| settings in `struct Settings`, reports in `Status` | settings as direct members of the block                                                                |
| `Annotated<T, ...>` for user-facing settings       | bare fields for user-facing settings (no UI metadata)                                                  |
| `[[nodiscard]]`, `constexpr`, `noexcept`           | omit attributes on processing functions                                                                |
| `std::expected` / `std::optional` for errors       | throw exceptions in library code                                                                       |
| `vir::simd` / `std::simd` for SIMD                 | raw compiler intrinsics                                                                                |
| `gr::pmt::Value` for type-erased values            | `std::variant` for wire-format data                                                                    |
| default member values + `applySettings()`          | custom constructors/destructors (break CRTP init, settings pipeline, `emplaceBlock` property_map init) |
| `std::ranges` / `std::algorithms` for transforms   | manual `for` loops when an algorithm exists                                                            |
| lean on the framework — focus on your algorithm    | re-implement scheduling, buffering, or tag forwarding                                                  |
| verify APIs against actual headers                 | assume/hallucinate API methods                                                                         |

## Advanced: custom `work()` function

For blocks that need full control over buffer acquisition, tag handling, and
publish/consume semantics, override `work()` directly. This bypasses the framework's
`processOne`/`processBulk` dispatch entirely — you are responsible for everything.

### Signature

```cpp
gr::work::Result work(std::size_t requestedWork = std::numeric_limits<std::size_t>::max()) noexcept {
    // ... custom implementation ...
    return { requestedWork, performedWork, gr::work::Status::OK };
}
```

### `work::Result`

```cpp
struct work::Result {
    std::size_t requested_work = std::numeric_limits<std::size_t>::max();
    std::size_t performed_work = 0;
    Status      status         = Status::OK;
};
```

### Direct port buffer access

Ports expose low-level reader/writer handles for direct buffer and tag manipulation:

```cpp
// input port — reading samples and tags
auto& reader    = in.streamReader();     // CircularBuffer reader
auto& tagReader = in.tagReader();        // tag ring-buffer reader

// output port — writing samples and tags
auto& writer    = out.streamWriter();    // CircularBuffer writer
auto& tagWriter = out.tagWriter();       // tag ring-buffer writer
```

_N.B. the reader/writer handler should be initialised and kept before the first call to `work()`.
The single producer buffer are not thread-safe and you should need to access only one at a time._

### Example: custom `work()` with manual tag handling

```cpp
gr::work::Result work(std::size_t requestedWork) noexcept {
    auto& reader = in.streamReader();
    auto& writer = out.streamWriter();

    const auto nAvailable = std::min(reader.available(), requestedWork);
    if (nAvailable == 0UZ) {
        return { requestedWork, 0UZ, gr::work::Status::INSUFFICIENT_INPUT_ITEMS };
    }

    // acquire input and output spans
    const auto inData  = reader.get(nAvailable);
    auto       outData = writer.tryReserve(nAvailable);
    if (outData.size() < nAvailable) {
        return { requestedWork, 0UZ, gr::work::Status::INSUFFICIENT_OUTPUT_ITEMS };
    }

    // process samples
    std::ranges::transform(inData, outData.begin(), [this](const auto& s) {
        return s * static_cast<T>(gain);
    });

    // manual tag forwarding
    const auto tags = in.tagReader().get();
    for (const auto& tag : tags) {
        if (tag.index >= reader.position() && tag.index < reader.position() + nAvailable) {
            out.publishTag(tag.map, tag.index - reader.position());
        }
    }
    in.tagReader().consume(tags.size());

    // publish output and consume input
    outData.publish(nAvailable);
    std::ignore = inData.consume(nAvailable);

    return { requestedWork, nAvailable, gr::work::Status::OK };
}
```

> **Prefer `processOne` or `processBulk`** unless you have a genuine need for
> manual buffer control. The framework's default `work()` implementation handles
> settings application, tag merging, SIMD vectorisation, resampling alignment, and
> lifecycle state checks — all of which you must replicate or forgo when overriding.

Reference: [`Block.hpp`](../core/include/gnuradio-4.0/Block.hpp) (`workInternal` implementation), [`Port.hpp`](../core/include/gnuradio-4.0/Port.hpp) (`streamReader`/`streamWriter`/`tagReader`/`tagWriter`)

## See also

- [`USER_API_Tensor_Value.md`](USER_API_Tensor_Value.md) — `pmt::Value` and `Tensor<T>`
- [`USER_API_handling_blocking_blocks.md`](USER_API_handling_blocking_blocks.md) — `BlockingSync<T>` pattern
- [`USER_API_Drawable_UI.md`](USER_API_Drawable_UI.md) — `Drawable<>` UI blocks
- [`Block.hpp`](../core/include/gnuradio-4.0/Block.hpp) — `Block<Derived>` base class
- [`Port.hpp`](../core/include/gnuradio-4.0/Port.hpp) — port types and concepts
- [`annotated.hpp`](../core/include/gnuradio-4.0/annotated.hpp) — `Annotated<T>`, `Limits`, `Doc`, `Unit`, `Visible`
- [`Tag.hpp`](../core/include/gnuradio-4.0/Tag.hpp) — `Tag` struct and standard keys
- [`LifeCycle.hpp`](../core/include/gnuradio-4.0/LifeCycle.hpp) — state machine (canonical diagram)
- `gnuradio4-blocks` — standard block implementations and block-level examples
- `gnuradio4-library` — reusable DSP libraries consumed by downstream blocks
