#include <boost/ut.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <numbers>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <gnuradio-4.0/Block.hpp>
#include <gnuradio-4.0/BlockRegistry.hpp>
#include <gnuradio-4.0/Graph_yaml_importer.hpp>
#include <gnuradio-4.0/RecipeParameters.hpp>

using namespace boost::ut;
using gr::recipe::evaluate;
using gr::recipe::ParameterDeclaration;
using gr::recipe::parseExpression;
using gr::recipe::resolveParameters;
using gr::recipe::validateDeclarations;

namespace {

// boost.ut runs a suite from its runner's destructor, after the objects of this translation unit have been
// destroyed, so anything a test body reads is held by a function-local static and outlives the run.
[[nodiscard]] const std::vector<ParameterDeclaration>& nbfmDeclarations() {
    static const std::vector<ParameterDeclaration> declarations{
        {.name = "sample_rate", .type = "float32", .defaultValue = std::nullopt, .doc = ""},
        {.name = "deviation", .type = "float32", .defaultValue = std::nullopt, .doc = ""},
        {.name = "tau", .type = "float64", .defaultValue = gr::pmt::Value(7.5e-05), .doc = ""},
    };
    return declarations;
}

[[nodiscard]] double evaluated(std::string_view source, const std::vector<gr::pmt::Value>& values) {
    const auto expression = parseExpression(source, nbfmDeclarations());
    expect(expression.has_value()) << source;
    // expect() does not abort, so a refusal must be answered with a value that fails every
    // comparison after it, never with a dereference of the error
    if (!expression.has_value()) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    const auto result = evaluate(*expression, values);
    expect(result.has_value()) << source;
    if (!result.has_value()) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    // integer-mode expressions return an int64 value; read whichever numeric arrived
    return gr::recipe::detail::doubleOf(*result).value_or(0.0);
}

} // namespace

const boost::ut::suite<"RecipeParameters"> recipeParameterTests = [] {
    "the canonical derivation evaluates in float64 exactly"_test = [] {
        const std::vector<gr::pmt::Value> values{gr::pmt::Value(96000.0f), gr::pmt::Value(5000.0f), gr::pmt::Value(7.5e-05)};
        const double                      gain = evaluated("sample_rate / (2 * pi * deviation)", values);
        expect(eq(gain, 96000.0 / (2.0 * std::numbers::pi * 5000.0)));
    };

    "precedence, unary sign, parentheses and constants"_test = [] {
        const std::vector<gr::pmt::Value> values{gr::pmt::Value(4.0f), gr::pmt::Value(2.0f), gr::pmt::Value(1.0)};
        expect(eq(evaluated("1 + 2 * 3", values), 7.0));
        expect(eq(evaluated("(1 + 2) * 3", values), 9.0));
        expect(eq(evaluated("-sample_rate + 1", values), -3.0));
        expect(eq(evaluated("tau_circle / 2", values), std::numbers::pi));
        expect(eq(evaluated("sample_rate / deviation / 2", values), 1.0)) << "division is left-associative";
    };

    "clamp holds a derived value between its bounds"_test = [] {
        const std::vector<gr::pmt::Value> values{gr::pmt::Value(2000000.0f), gr::pmt::Value(2500.0f), gr::pmt::Value(7.5e-05)};
        expect(eq(evaluated("clamp(sample_rate * 0.05, 32768, 4194304)", values), 100000.0)) << "between the bounds the value stands";
        expect(eq(evaluated("clamp(sample_rate * 0.05, 200000, 4194304)", values), 200000.0)) << "below the low bound the low bound stands";
        expect(eq(evaluated("clamp(sample_rate * 0.05, 32768, 50000)", values), 50000.0)) << "above the high bound the high bound stands";
        expect(eq(evaluated("clamp(-sample_rate, -1, 1) + clamp(1, 0, 2)", values), 0.0)) << "a call is a factor like any other";
        expect(eq(evaluated("clamp(clamp(sample_rate, 0, 100), 0, 10)", values), 10.0)) << "an argument is a whole expression, calls included";

        const std::vector<ParameterDeclaration> integral{{.name = "n", .type = "int64", .defaultValue = std::nullopt, .doc = ""}};
        const auto                              expression = parseExpression("clamp(n, 32768, 4194304)", integral);
        expect(expression.has_value());
        if (!expression.has_value()) {
            return;
        }
        expect(expression->integerMode) << "an all-integral clamp stays on the int64 path";
        const std::vector<gr::pmt::Value> big{gr::pmt::Value(std::int64_t{9007199254740993})};
        const auto                        bounded = evaluate(*expression, big);
        expect(bounded.has_value());
        if (bounded.has_value()) {
            expect(eq(bounded->value_or(std::int64_t{}), std::int64_t{4194304})) << "the high bound holds without a trip through a double";
        }
    };

    "clamp refuses a wrong arity, a non-numeric argument and unordered bounds"_test = [] {
        const auto arity = parseExpression("clamp(sample_rate, 1)", nbfmDeclarations());
        expect(!arity.has_value());
        expect(arity.error().message.contains("recipe_expression_parse")) << arity.error().message;
        expect(arity.error().message.contains("clamp takes 3 arguments")) << arity.error().message;
        expect(!parseExpression("clamp(sample_rate, 1, 2, 3)", nbfmDeclarations()).has_value()) << "a fourth argument is refused too";
        expect(!parseExpression("clamp()", nbfmDeclarations()).has_value());
        expect(!parseExpression("clamp(sample_rate, 1, 2", nbfmDeclarations()).has_value()) << "an unclosed call is refused";

        const auto unknownFunction = parseExpression("ceil(sample_rate)", nbfmDeclarations());
        expect(!unknownFunction.has_value());
        expect(unknownFunction.error().message.contains("unknown function")) << unknownFunction.error().message;
        expect(unknownFunction.error().message.contains("clamp")) << "the refusal names the vocabulary it has";

        const std::vector<ParameterDeclaration> withText{
            {.name = "label", .type = "string", .defaultValue = std::nullopt, .doc = ""},
            {.name = "rate", .type = "float64", .defaultValue = std::nullopt, .doc = ""},
        };
        const auto text = parseExpression("clamp(label, 0, 1)", withText);
        expect(!text.has_value()) << "a call argument is an expression, so a non-numeric parameter is refused there too";
        if (!text.has_value()) {
            expect(text.error().message.contains("cannot appear in an expression")) << text.error().message;
        }

        const std::vector<gr::pmt::Value> values{gr::pmt::Value(2000000.0f), gr::pmt::Value(2500.0f), gr::pmt::Value(7.5e-05)};
        const auto                        unordered = parseExpression("clamp(sample_rate, 4194304, 32768)", nbfmDeclarations());
        expect(unordered.has_value()) << "the bounds' order is a value question, not a grammar one";
        if (unordered.has_value()) {
            const auto refused = evaluate(*unordered, values);
            expect(!refused.has_value());
            if (!refused.has_value()) {
                expect(refused.error().message.contains("recipe_expression_domain")) << refused.error().message;
            }
        }

        const std::vector<ParameterDeclaration> integral{{.name = "n", .type = "int64", .defaultValue = std::nullopt, .doc = ""}};
        const auto                              unorderedIntegers = parseExpression("clamp(n, 10, 1)", integral);
        expect(unorderedIntegers.has_value() && unorderedIntegers->integerMode);
        if (unorderedIntegers.has_value()) {
            const std::vector<gr::pmt::Value> one{gr::pmt::Value(std::int64_t{5})};
            const auto                        refusedIntegers = evaluate(*unorderedIntegers, one);
            expect(!refusedIntegers.has_value()) << "the int64 path refuses the same bounds";
            if (!refusedIntegers.has_value()) {
                expect(refusedIntegers.error().message.contains("recipe_expression_domain")) << refusedIntegers.error().message;
            }
        }

        const std::vector<ParameterDeclaration> shadow{{.name = "clamp", .type = "float32", .defaultValue = std::nullopt, .doc = ""}};
        const auto                              shadowed = validateDeclarations(shadow);
        expect(!shadowed.has_value()) << "a parameter may not shadow the dialect's own function name";
        if (!shadowed.has_value()) {
            expect(shadowed.error().message.contains("recipe_reserved_parameter")) << shadowed.error().message;
        }
    };

    "a bare parameter reference is plain forwarding"_test = [] {
        const std::vector<gr::pmt::Value> values{gr::pmt::Value(48000.0f), gr::pmt::Value(2500.0f), gr::pmt::Value(5e-05)};
        expect(eq(evaluated("tau", values), 5e-05));
    };

    "the integer rule preserves 2^53 + 1"_test = [] {
        const std::vector<ParameterDeclaration> declarations{{.name = "start", .type = "uint64", .defaultValue = std::nullopt, .doc = ""}};
        const auto                              expression = parseExpression("start + 1", declarations);
        expect(expression.has_value());
        expect(expression->integerMode) << "all-integral with no division evaluates in int64";
        const std::vector<gr::pmt::Value> values{gr::pmt::Value(std::int64_t{9007199254740992})};
        const auto                        result = evaluate(*expression, values);
        expect(result.has_value());
        expect(eq(result->value_or(std::int64_t{}), std::int64_t{9007199254740993})) << "the value a double would silently lose";
    };

    "division and float operands leave integer mode"_test = [] {
        const std::vector<ParameterDeclaration> declarations{{.name = "n", .type = "int32", .defaultValue = std::nullopt, .doc = ""}};
        const auto                              divided = parseExpression("n / 2", declarations);
        expect(divided.has_value() && !divided->integerMode);
        const auto scaled = parseExpression("n * 1.5", declarations);
        expect(scaled.has_value() && !scaled->integerMode);
        const auto shifted = parseExpression("n * 2 - 1", declarations);
        expect(shifted.has_value() && shifted->integerMode);
    };

    "parse refusals carry position and name"_test = [] {
        expect(!parseExpression("1 +", nbfmDeclarations()).has_value());
        expect(!parseExpression("(1 + 2", nbfmDeclarations()).has_value());
        expect(!parseExpression("1 ; 2", nbfmDeclarations()).has_value());
        expect(!parseExpression("", nbfmDeclarations()).has_value());
        const auto unknown = parseExpression("sample_rate * bandwidth", nbfmDeclarations());
        expect(!unknown.has_value());
        expect(unknown.error().message.contains("recipe_unknown_identifier")) << unknown.error().message;
        expect(unknown.error().message.contains("bandwidth")) << unknown.error().message;
        expect(unknown.error().message.contains("sample_rate")) << "the refusal lists the declared parameters";
    };

    "a string parameter cannot appear in an expression"_test = [] {
        const std::vector<ParameterDeclaration> declarations{{.name = "label", .type = "string", .defaultValue = std::nullopt, .doc = ""}};
        expect(!parseExpression("label + 1", declarations).has_value());
    };

    "a string or vector parameter is substituted, not evaluated"_test = [] {
        // D3's whole surface: these types carry no arithmetic, so a recipe hands one through unchanged. The
        // engine's part is to name them as substituted and to produce the value; the grammar stays numeric.
        const std::vector<ParameterDeclaration> declarations{
            {.name = "detector", .type = "string", .defaultValue = gr::pmt::Value(std::pmr::string("zero_crossing")), .doc = ""},
            {.name = "carriers", .type = "int32[]", .defaultValue = std::nullopt, .doc = ""},
            {.name = "rate", .type = "float64", .defaultValue = gr::pmt::Value(48000.0), .doc = ""},
        };
        expect(gr::recipe::detail::substitutedTypeWord(declarations[0].type));
        expect(gr::recipe::detail::substitutedTypeWord(declarations[1].type));
        expect(!gr::recipe::detail::substitutedTypeWord(declarations[2].type)) << "a numeric parameter stays on the expression path";
        expect(gr::recipe::detail::vectorTypeWord("string[]"));
        expect(!gr::recipe::detail::vectorTypeWord("int32[")) << "a malformed type word is not a vector";

        const std::vector<gr::pmt::Value> values{gr::pmt::Value(std::pmr::string("gardner")), gr::pmt::Value(std::vector<std::int32_t>{-2, 0, 3}), gr::pmt::Value(48000.0)};

        const gr::recipe::Binding substituted{.namePath = {"inner"}, .settingKey = "detector", .expression = {}, .substituted = 0UZ, .sequence = {}};
        const auto                bound = gr::recipe::bindingValue(substituted, std::span<const gr::pmt::Value>(values));
        expect(bound.has_value());
        const auto* text = bound->get_if<std::pmr::string>();
        expect(text != nullptr) << "the string arrives as a string, not converted";
        if (text != nullptr) {
            expect(eq(std::string_view(*text), std::string_view("gardner")));
        }

        const gr::recipe::Binding vectorBound{.namePath = {"inner"}, .settingKey = "carriers", .expression = {}, .substituted = 1UZ, .sequence = {}};
        const auto                carriers = gr::recipe::bindingValue(vectorBound, std::span<const gr::pmt::Value>(values));
        expect(carriers.has_value());
        expect(carriers->get_if<gr::Tensor<std::int32_t>>() != nullptr) << "the sequence arrives whole";

        // an index past the values is a refusal rather than a read off the end
        const gr::recipe::Binding outOfRange{.namePath = {"inner"}, .settingKey = "x", .expression = {}, .substituted = 9UZ, .sequence = {}};
        expect(!gr::recipe::bindingValue(outOfRange, std::span<const gr::pmt::Value>(values)).has_value());
    };

    "a vector declaration accepts only its own element type"_test = [] {
        const auto declare = [](std::string_view type, gr::pmt::Value value) {
            const std::vector<ParameterDeclaration> declarations{{.name = "seq", .type = std::string(type), .defaultValue = std::move(value), .doc = ""}};
            return validateDeclarations(std::span<const ParameterDeclaration>(declarations)).has_value();
        };
        expect(declare("int32[]", gr::pmt::Value(std::vector<std::int32_t>{1, 2})));
        expect(declare("float64[]", gr::pmt::Value(std::vector<double>{1., 2.})));
        expect(!declare("int32[]", gr::pmt::Value(std::vector<double>{1., 2.}))) << "an element type that disagrees is refused";
        expect(!declare("int32[]", gr::pmt::Value(3))) << "a scalar default under a vector type is refused";
        expect(!declare("complex64[]", gr::pmt::Value(std::vector<double>{1.}))) << "an element word the dialect has no name for is refused";
    };

    "a boolean parameter is substituted, and refuses to be an operand"_test = [] {
        // A toggle is a value to hand through, not one to compute with: adding two of them or scaling one has no
        // meaning a recipe should be able to write, so it joins the strings and vectors on the substitution path.
        // The exported name cannot be `enabled`: that is one of the framework's own settings and is reserved. The
        // interior setting it binds to is another matter — `settingKey` below is exactly that `enabled`.
        const std::vector<ParameterDeclaration> declarations{
            {.name = "dc_block_enabled", .type = "bool", .defaultValue = gr::pmt::Value(true), .doc = ""},
            {.name = "rate", .type = "float64", .defaultValue = gr::pmt::Value(48000.0), .doc = ""},
        };
        expect(validateDeclarations(std::span<const ParameterDeclaration>(declarations)).has_value());
        expect(gr::recipe::detail::substitutedTypeWord("bool"));
        expect(gr::recipe::detail::substitutedTypeWord("bool[]")) << "a table of toggles is substituted like any other sequence";

        const std::vector<ParameterDeclaration> reserved{{.name = "enabled", .type = "bool", .defaultValue = gr::pmt::Value(true), .doc = ""}};
        const auto                              refused = validateDeclarations(std::span<const ParameterDeclaration>(reserved));
        expect(!refused.has_value()) << "a block setting's own name is not available to export";
        if (!refused.has_value()) {
            expect(refused.error().message.contains("recipe_reserved_parameter")) << refused.error().message;
        }

        const std::vector<gr::pmt::Value> values{gr::pmt::Value(false), gr::pmt::Value(48000.0)};
        const gr::recipe::Binding         toggle{.namePath = {"inner"}, .settingKey = "enabled", .expression = {}, .substituted = 0UZ, .sequence = {}};
        const auto                        bound = gr::recipe::bindingValue(toggle, std::span<const gr::pmt::Value>(values));
        expect(bound.has_value());
        const auto* flag = bound->get_if<bool>();
        expect(flag != nullptr) << "the boolean arrives as a boolean, not as a number";
        if (flag != nullptr) {
            expect(!*flag) << "a supplied false is handed through, not defaulted back to the declaration";
        }

        const auto asOperand = parseExpression("dc_block_enabled * rate", std::span<const ParameterDeclaration>(declarations));
        expect(!asOperand.has_value()) << "there is no grammar over a boolean";
        if (!asOperand.has_value()) {
            expect(asOperand.error().message.contains("cannot appear in an expression")) << asOperand.error().message;
            expect(asOperand.error().message.contains("bool")) << "the refusal names the type it refused";
        }
    };

    "a boolean declaration accepts only a boolean default"_test = [] {
        const auto declare = [](std::string_view type, gr::pmt::Value value) {
            const std::vector<ParameterDeclaration> declarations{{.name = "flag", .type = std::string(type), .defaultValue = std::move(value), .doc = ""}};
            return validateDeclarations(std::span<const ParameterDeclaration>(declarations)).has_value();
        };
        expect(declare("bool", gr::pmt::Value(true)));
        expect(declare("bool", gr::pmt::Value(false)));
        expect(!declare("bool", gr::pmt::Value(1))) << "an integer standing in for a toggle is refused";
        expect(!declare("bool", gr::pmt::Value(std::pmr::string("true")))) << "the word true is a string, not a boolean";
        expect(!declare("float64", gr::pmt::Value(true))) << "a boolean default under a numeric type is refused";
        expect(!declare("bool[]", gr::pmt::Value(true))) << "a scalar default under a vector type is refused";
        expect(declare("bool[]", gr::pmt::Value(std::vector<bool>{true, false})));
    };

    "a non-finite result is refused"_test = [] {
        const std::vector<gr::pmt::Value> values{gr::pmt::Value(1.0f), gr::pmt::Value(0.0f), gr::pmt::Value(0.0)};
        const auto                        expression = parseExpression("sample_rate / deviation", nbfmDeclarations());
        expect(expression.has_value());
        expect(!evaluate(*expression, values).has_value());
    };

    "required parameters are refused together, all named"_test = [] {
        const auto resolved = resolveParameters(nbfmDeclarations(), {});
        expect(!resolved.has_value());
        expect(resolved.error().message.contains("recipe_parameter_required")) << resolved.error().message;
        expect(resolved.error().message.contains("sample_rate") && resolved.error().message.contains("deviation")) << resolved.error().message;
        expect(!resolved.error().message.contains("tau")) << "a defaulted parameter is not demanded";
    };

    "supplied values overlay defaults; unknown names are refused"_test = [] {
        gr::property_map supplied;
        supplied["sample_rate"] = 48000.0f;
        supplied["deviation"]   = 2500.0f;
        const auto resolved     = resolveParameters(nbfmDeclarations(), supplied);
        expect(resolved.has_value());
        expect(eq((*resolved)[0].value_or(float{}), 48000.0f));
        expect(eq((*resolved)[2].value_or(double{}), 7.5e-05)) << "the default filled in";

        supplied["bandwidth"] = 1.0f;
        const auto unknown    = resolveParameters(nbfmDeclarations(), supplied);
        expect(!unknown.has_value());
        expect(unknown.error().message.contains("recipe_unknown_parameter")) << unknown.error().message;
    };

    "hostile input cannot escape: depth, overflow and echo are all bounded"_test = [] {
        const std::string deep   = std::string(300, '(') + "1" + std::string(300, ')');
        const auto        nested = parseExpression(deep, nbfmDeclarations());
        expect(!nested.has_value()) << "hostile nesting is a refusal, not a stack overflow";
        expect(nested.error().message.contains("too deeply")) << nested.error().message;

        const std::vector<ParameterDeclaration> declarations{{.name = "n", .type = "int64", .defaultValue = std::nullopt, .doc = ""}};
        const auto                              product = parseExpression("n * n", declarations);
        expect(product.has_value() && product->integerMode);
        const std::vector<gr::pmt::Value> big{gr::pmt::Value(std::int64_t{4611686018427387904})};
        const auto                        overflowed = evaluate(*product, big);
        expect(!overflowed.has_value()) << "integer overflow refuses, never wraps or invokes undefined behavior";
        expect(overflowed.error().message.contains("overflows")) << overflowed.error().message;

        expect(!parseExpression("1e999", nbfmDeclarations()).has_value()) << "an out-of-range literal is refused at parse";

        const auto echoed = parseExpression("\x1b[31mred\x1b[0m", nbfmDeclarations());
        expect(!echoed.has_value());
        expect(echoed.error().message.find('\x1b') == std::string::npos) << "control characters never reach an error message";
    };

    "declaration validation refuses duplicates, reserved names, unknown types and mistyped defaults"_test = [] {
        expect(validateDeclarations(nbfmDeclarations()).has_value());
        const std::vector<ParameterDeclaration> duplicate{{.name = "a", .type = "float32", .defaultValue = std::nullopt, .doc = ""}, {.name = "a", .type = "float32", .defaultValue = std::nullopt, .doc = ""}};
        expect(!validateDeclarations(duplicate).has_value());
        const std::vector<ParameterDeclaration> reserved{{.name = "pi", .type = "float32", .defaultValue = std::nullopt, .doc = ""}};
        expect(!validateDeclarations(reserved).has_value());
        const std::vector<ParameterDeclaration> unknownType{{.name = "a", .type = "float128", .defaultValue = std::nullopt, .doc = ""}};
        expect(!validateDeclarations(unknownType).has_value());
        const std::vector<ParameterDeclaration> mistyped{{.name = "a", .type = "float32", .defaultValue = gr::pmt::Value(std::pmr::string("x")), .doc = ""}};
        expect(!validateDeclarations(mistyped).has_value());
    };
};

namespace qa_recipe_definitions {

struct RecipeScale : gr::Block<RecipeScale> {
    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    gr::Annotated<float, "gain">              gain  = 1.0f;
    gr::Annotated<float, "rate">              rate  = 0.0f;
    gr::Annotated<std::string, "label">       label = "";
    gr::Annotated<std::vector<float>, "taps"> taps{};
    gr::Annotated<std::uint32_t, "count">     count = 0U;

    GR_MAKE_REFLECTABLE(RecipeScale, in, out, gain, rate, label, taps, count);

    explicit RecipeScale(gr::property_map init = {}) : gr::Block<RecipeScale>(std::move(init)) {}

    [[nodiscard]] float processOne(float sample) const noexcept { return gain * sample; }
};

/// what one interior block's settingsChanged saw, kept by block name because the block itself
/// lives inside the composite and is reachable only as a BlockModel
struct SettingsChangedRecord {
    std::size_t              calls = 0UZ;
    std::vector<std::string> keys;
};

[[nodiscard]] std::map<std::string, SettingsChangedRecord>& recipeWatches() {
    static std::map<std::string, SettingsChangedRecord> watches;
    return watches;
}

struct RecipeWatched : gr::Block<RecipeWatched> {
    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    gr::Annotated<float, "gain"> gain = 1.0f;
    gr::Annotated<float, "rate"> rate = 0.0f;

    GR_MAKE_REFLECTABLE(RecipeWatched, in, out, gain, rate);

    explicit RecipeWatched(gr::property_map init = {}) : gr::Block<RecipeWatched>(std::move(init)) {}

    [[nodiscard]] float processOne(float sample) const noexcept { return gain * sample; }

    void settingsChanged(const gr::property_map& /*oldSettings*/, const gr::property_map& newSettings) {
        SettingsChangedRecord& record = recipeWatches()[std::string(this->name)];
        ++record.calls;
        record.keys.clear();
        for (const auto& [key, value] : newSettings) {
            record.keys.emplace_back(key);
        }
    }
};

void registerRecipeTestBlock() {
    static const bool registered = [] { return gr::globalBlockRegistry().insert<RecipeScale>("=qa::RecipeScale") && gr::globalBlockRegistry().insert<RecipeWatched>("=qa::RecipeWatched"); }();
    expect(registered) << "the test block must reach the global registry";
}

[[nodiscard]] gr::detail::YamlDefinitionsLoader::Definition definitionFrom(std::string_view yamlText) {
    const auto parsed = gr::pmt::yaml::deserialize(yamlText);
    expect(parsed.has_value()) << "the fixture yaml must parse";
    return {.definition = *parsed, .metadata = {}};
}

[[nodiscard]] gr::PluginLoader recipeTestLoader() {
    static gr::SchedulerRegistry          schedulerRegistry;
    static const std::vector<std::string> noPaths;
    return gr::PluginLoader(gr::globalBlockRegistry(), schedulerRegistry, noPaths);
}

constexpr std::string_view kParameterizedRecipe = R"yaml(
blocks:
  - id: SUBGRAPH
    parameters:
      name: scaled
    exported_parameters:
      - name: sample_rate
        type: float32
      - name: deviation
        type: float32
      - name: tau
        type: float64
        default: !!float64 7.5e-05
    graph:
      blocks:
        - id: "qa::RecipeScale"
          parameters:
            name: inner
            gain: "=sample_rate / (2 * pi * deviation)"
            rate: "=sample_rate"
            label: "\\=verbatim"
      exported_ports:
        - [inner, INPUT, in, in]
        - [inner, OUTPUT, out, out]
)yaml";

constexpr std::string_view kLiteralRecipe = R"yaml(
blocks:
  - id: SUBGRAPH
    parameters:
      name: fixed
    graph:
      blocks:
        - id: "qa::RecipeScale"
          parameters:
            name: inner
            gain: !!float32 2.5
      exported_ports:
        - [inner, INPUT, in, in]
        - [inner, OUTPUT, out, out]
)yaml";

constexpr std::string_view kEdgeSizedRecipe = R"yaml(
blocks:
  - id: SUBGRAPH
    parameters:
      name: sized
    exported_parameters:
      - name: sample_rate
        type: float32
    graph:
      blocks:
        - id: "qa::RecipeScale"
          parameters:
            name: first
        - id: "qa::RecipeScale"
          parameters:
            name: second
      connections:
        -
          - "first"
          - "out"
          - "second"
          - "in"
          - "=sample_rate * 0.05"
      exported_ports:
        - [first, INPUT, in, in]
        - [second, OUTPUT, out, out]
)yaml";

constexpr std::string_view kUnsizedEdgeRecipe = R"yaml(
blocks:
  - id: SUBGRAPH
    parameters:
      name: unsized
    graph:
      blocks:
        - id: "qa::RecipeScale"
          parameters:
            name: first
        - id: "qa::RecipeScale"
          parameters:
            name: second
      connections:
        -
          - "first"
          - "out"
          - "second"
          - "in"
          - "one hundred thousand"
      exported_ports:
        - [first, INPUT, in, in]
        - [second, OUTPUT, out, out]
)yaml";

constexpr std::string_view kDerivedTapsRecipe = R"yaml(
blocks:
  - id: SUBGRAPH
    parameters:
      name: shaped
    exported_parameters:
      - name: peak
        type: float32
    graph:
      blocks:
        - id: "qa::RecipeScale"
          parameters:
            name: inner
            taps:
              - "=peak"
              - "=peak / 2"
              - !!float32 0.25
      exported_ports:
        - [inner, INPUT, in, in]
        - [inner, OUTPUT, out, out]
)yaml";

constexpr std::string_view kClampedRecipe = R"yaml(
blocks:
  - id: SUBGRAPH
    parameters:
      name: clamped
    exported_parameters:
      - name: sample_rate
        type: float32
    graph:
      blocks:
        - id: "qa::RecipeScale"
          parameters:
            name: first
            gain: "=clamp(sample_rate * 0.05, 32768, 4194304)"
        - id: "qa::RecipeScale"
          parameters:
            name: second
      connections:
        -
          - "first"
          - "out"
          - "second"
          - "in"
          - "=clamp(sample_rate * 0.05, 32768, 4194304)"
      exported_ports:
        - [first, INPUT, in, in]
        - [second, OUTPUT, out, out]
)yaml";

constexpr std::string_view kTwoBoundBlocksRecipe = R"yaml(
blocks:
  - id: SUBGRAPH
    parameters:
      name: watched
    exported_parameters:
      - name: sample_rate
        type: float32
      - name: level
        type: float32
    graph:
      blocks:
        - id: "qa::RecipeWatched"
          parameters:
            name: first
            gain: "=level"
            rate: "=sample_rate"
        - id: "qa::RecipeWatched"
          parameters:
            name: second
            gain: "=sample_rate / 2"
      exported_ports:
        - [first, INPUT, in, in]
        - [second, OUTPUT, out, out]
)yaml";

constexpr std::string_view kTwoTargetsRecipe = R"yaml(
blocks:
  - id: SUBGRAPH
    parameters:
      name: counting
    exported_parameters:
      - name: level
        type: int32
    graph:
      blocks:
        - id: "qa::RecipeScale"
          parameters:
            name: first
            gain: "=level"
        - id: "qa::RecipeScale"
          parameters:
            name: second
            count: "=level"
      exported_ports:
        - [first, INPUT, in, in]
        - [second, OUTPUT, out, out]
)yaml";

[[nodiscard]] std::size_t interiorEdgeBufferSize(const std::shared_ptr<gr::BlockModel>& composite) {
    if (composite == nullptr || composite->graph() == nullptr || composite->graph()->edges().empty()) {
        expect(false) << "the definition must produce a composite with an interior edge";
        return 0UZ;
    }
    return composite->graph()->edges().front().minBufferSize();
}

[[nodiscard]] std::shared_ptr<gr::BlockModel> interiorBlockNamed(const std::shared_ptr<gr::BlockModel>& composite, std::string_view name) {
    if (composite == nullptr || composite->graph() == nullptr) {
        expect(false) << "the definition must produce a composite";
        return nullptr;
    }
    for (const auto& candidate : composite->graph()->blocks()) {
        if (candidate->name() == name) {
            return candidate;
        }
    }
    expect(false) << name << " is not an interior block of the composite";
    return nullptr;
}

[[nodiscard]] std::shared_ptr<gr::BlockModel> interiorBlock(const std::shared_ptr<gr::BlockModel>& composite) {
    // expect() does not abort, so a broken composite must be answered with null, never a dereference
    if (composite == nullptr || composite->graph() == nullptr || composite->graph()->blocks().empty()) {
        expect(false) << "the definition must produce a composite with an interior block";
        return nullptr;
    }
    return composite->graph()->blocks().front();
}

/// the parameterized fixture at sample_rate 48000 and deviation 2500; the loader outlives the composite,
/// which keeps a pointer to it
[[nodiscard]] std::shared_ptr<gr::BlockModel> parameterizedComposite(gr::PluginLoader& loader) {
    registerRecipeTestBlock();
    gr::property_map parameters;
    parameters["sample_rate"] = 48000.0f;
    parameters["deviation"]   = 2500.0f;
    const auto composite      = gr::detail::instantiateBlockFromYamlDefinition(loader, definitionFrom(kParameterizedRecipe), parameters);
    expect(composite.has_value()) << (composite.has_value() ? "" : composite.error().message);
    return composite.has_value() ? *composite : nullptr;
}

/// the fixture's discriminator gain at one parameter point, in the interior member's own type
[[nodiscard]] float derivedGain(double sampleRate, double deviation) { return static_cast<float>(sampleRate / (2.0 * std::numbers::pi * deviation)); }

/// a numeric value staged on `block` under `key`, NaN when none is staged, so a missing key fails every comparison
[[nodiscard]] float stagedNumber(const std::shared_ptr<gr::BlockModel>& block, std::string_view key) {
    const gr::property_map staged = block->settings().stagedParameters();
    const auto             it     = staged.find(std::pmr::string(key));
    return it == staged.end() ? std::numeric_limits<float>::quiet_NaN() : static_cast<float>(gr::recipe::detail::doubleOf(it->second).value_or(std::numeric_limits<double>::quiet_NaN()));
}

/// a numeric value of `settings` under `key`, NaN when there is none
[[nodiscard]] float readNumber(const gr::property_map& settings, std::string_view key) {
    const auto it = settings.find(std::pmr::string(key));
    return it == settings.end() ? std::numeric_limits<float>::quiet_NaN() : static_cast<float>(gr::recipe::detail::doubleOf(it->second).value_or(std::numeric_limits<double>::quiet_NaN()));
}

/**
 * A definitions root holding one recipe, `qa::HalvingRecipe`, written to a temporary directory.
 *
 * Its one exported parameter, `level`, has no default and is therefore required, and its interior block `inner`
 * derives `gain = 1000 / level`. A recipe reaches another recipe's interior only by its registry name, so this one is
 * read through a loader's definition roots.
 */
struct HalvingRecipeRoot {
    std::filesystem::path path = std::filesystem::temp_directory_path() / "gr4_qa_recipe_parameters_nested";

    HalvingRecipeRoot() {
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
        std::ofstream index(path / "index.yaml");
        index << "assets:\n  - file: halving_recipe.yaml\n    created: \"2024-01-01-00:00:00\"\n    modified: \"2024-01-15-10:00:00\"\n    block_type: qa::HalvingRecipe\n";
        std::ofstream asset(path / "halving_recipe.yaml");
        asset << R"yaml(definition_metadata:
  block_type: qa::HalvingRecipe
blocks:
  - id: SUBGRAPH
    parameters:
      name: halving
    exported_parameters:
      - name: level
        type: float32
    graph:
      blocks:
        - id: "qa::RecipeScale"
          parameters:
            name: inner
            gain: "=1000 / level"
      exported_ports:
        - [inner, INPUT, in, in]
        - [inner, OUTPUT, out, out]
)yaml";
    }

    HalvingRecipeRoot(const HalvingRecipeRoot&)            = delete;
    HalvingRecipeRoot& operator=(const HalvingRecipeRoot&) = delete;

    ~HalvingRecipeRoot() { std::filesystem::remove_all(path); }
};

/// a recipe whose interior holds a plain block and the recipe `qa::HalvingRecipe`, each deriving a value from `rate`
constexpr std::string_view kNestingRecipe = R"yaml(
blocks:
  - id: SUBGRAPH
    parameters:
      name: nesting
    exported_parameters:
      - name: rate
        type: float32
    graph:
      blocks:
        - id: "qa::RecipeScale"
          parameters:
            name: first
            gain: "=rate"
        - id: "qa::HalvingRecipe"
          parameters:
            name: stage
            level: "=rate - 1000"
      exported_ports:
        - [first, INPUT, in, in]
        - [first, OUTPUT, out, out]
)yaml";

/// the nesting fixture at `rate`, built through a loader that reads `root`; the loader outlives the composite
[[nodiscard]] std::shared_ptr<gr::BlockModel> nestingComposite(gr::PluginLoader& loader, float rate) {
    registerRecipeTestBlock();
    const auto composite = gr::detail::instantiateBlockFromYamlDefinition(loader, definitionFrom(kNestingRecipe), {{"rate", rate}});
    expect(composite.has_value()) << (composite.has_value() ? "" : composite.error().message);
    return composite.has_value() ? *composite : nullptr;
}

[[nodiscard]] gr::PluginLoader nestingLoader(const HalvingRecipeRoot& root) {
    static gr::SchedulerRegistry schedulerRegistry;
    return gr::PluginLoader(gr::globalBlockRegistry(), schedulerRegistry, std::vector<std::string>{root.path.string()});
}

/// the message of the gr::exception `call` throws, empty when it throws none
template<typename TCall>
[[nodiscard]] std::string refusalOf(TCall&& call) {
    try {
        std::forward<TCall>(call)();
    } catch (const gr::exception& e) {
        return e.message;
    }
    return {};
}

} // namespace qa_recipe_definitions

using namespace qa_recipe_definitions;

const boost::ut::suite<"RecipeDefinitions"> recipeDefinitionTests = [] {
    "a parameterized definition instantiates with required parameters and derives interior settings"_test = [] {
        registerRecipeTestBlock();
        auto             loader = recipeTestLoader();
        const auto       def    = definitionFrom(kParameterizedRecipe);
        gr::property_map parameters;
        parameters["sample_rate"] = 48000.0f;
        parameters["deviation"]   = 2500.0f;
        const auto composite      = gr::detail::instantiateBlockFromYamlDefinition(loader, def, parameters);
        expect(composite.has_value()) << (composite.has_value() ? "" : composite.error().message);
        if (!composite.has_value()) {
            return;
        }
        const auto inner = interiorBlock(*composite);
        if (inner == nullptr) {
            return;
        }

        const double expectedGain = 48000.0 / (2.0 * std::numbers::pi * 2500.0);
        const auto   gain         = inner->settings().get("gain");
        expect(gain.has_value());
        expect(eq(gain->value_or(float{}), static_cast<float>(expectedGain))) << "the derivation reached the interior setting";
        const auto rate = inner->settings().get("rate");
        expect(rate.has_value());
        expect(eq(rate->value_or(float{}), 48000.0f)) << "a bare reference forwards";
        const auto label = inner->settings().get("label");
        expect(label.has_value());
        expect(eq(std::string(label->value_or(std::string_view{})), std::string("=verbatim"))) << "the escape yields a literal '='";
    };

    "missing required parameters are refused together"_test = [] {
        registerRecipeTestBlock();
        auto       loader  = recipeTestLoader();
        const auto def     = definitionFrom(kParameterizedRecipe);
        const auto refusal = gr::detail::instantiateBlockFromYamlDefinition(loader, def, {});
        expect(!refusal.has_value());
        expect(refusal.error().message.contains("recipe_parameter_required")) << refusal.error().message;
        expect(refusal.error().message.contains("sample_rate") && refusal.error().message.contains("deviation")) << refusal.error().message;
    };

    "an unknown parameter is refused by name"_test = [] {
        registerRecipeTestBlock();
        auto             loader = recipeTestLoader();
        const auto       def    = definitionFrom(kParameterizedRecipe);
        gr::property_map parameters;
        parameters["sample_rate"] = 48000.0f;
        parameters["deviation"]   = 2500.0f;
        parameters["bandwidth"]   = 1.0f;
        const auto refusal        = gr::detail::instantiateBlockFromYamlDefinition(loader, def, parameters);
        expect(!refusal.has_value());
        expect(refusal.error().message.contains("recipe_unknown_parameter")) << refusal.error().message;
    };

    "a live parameter change re-evaluates and stages; a refusal leaves the values standing"_test = [] {
        registerRecipeTestBlock();
        auto             loader = recipeTestLoader();
        const auto       def    = definitionFrom(kParameterizedRecipe);
        gr::property_map parameters;
        parameters["sample_rate"] = 48000.0f;
        parameters["deviation"]   = 2500.0f;
        const auto composite      = gr::detail::instantiateBlockFromYamlDefinition(loader, def, parameters);
        expect(composite.has_value()) << (composite.has_value() ? "" : composite.error().message);
        if (!composite.has_value()) {
            return;
        }
        auto* wrapper = dynamic_cast<gr::GraphWrapper<gr::Graph>*>(composite->get());
        expect(wrapper != nullptr) << "a parameterized composite carries the binding machinery";
        const auto inner = interiorBlock(*composite);
        if (wrapper == nullptr || inner == nullptr) {
            return;
        }

        gr::property_map change;
        change["deviation"] = 5000.0f;
        const auto applied  = wrapper->applyRecipeParameters(change);
        expect(applied.has_value()) << (applied.has_value() ? "" : applied.error().message);
        const auto staged = inner->settings().stagedParameters();
        const auto gainIt = staged.find("gain");
        expect(gainIt != staged.end()) << "the derived setting is staged on the interior block";
        if (gainIt != staged.end()) {
            // setStaged converts to the member's own float; read whichever numeric arrived
            const double stagedGain = gr::recipe::detail::doubleOf(gainIt->second).value_or(0.0);
            expect(eq(static_cast<float>(stagedGain), static_cast<float>(48000.0 / (2.0 * std::numbers::pi * 5000.0)))) << "re-evaluated with the changed parameter";
        }

        gr::property_map unknown;
        unknown["bandwidth"] = 1.0f;
        expect(!wrapper->applyRecipeParameters(unknown).has_value()) << "an undeclared parameter refuses";

        gr::property_map zero;
        zero["deviation"] = 0.0f;
        expect(!wrapper->applyRecipeParameters(zero).has_value()) << "a non-finite derivation refuses the change whole";

        gr::property_map next;
        next["sample_rate"]    = 96000.0f;
        const auto nextApplied = wrapper->applyRecipeParameters(next);
        expect(nextApplied.has_value()) << (nextApplied.has_value() ? "" : nextApplied.error().message);
        const auto stagedAfter = inner->settings().stagedParameters();
        const auto gainAfter   = stagedAfter.find("gain");
        expect(gainAfter != stagedAfter.end());
        if (gainAfter != stagedAfter.end()) {
            const double stagedGain = gr::recipe::detail::doubleOf(gainAfter->second).value_or(0.0);
            expect(eq(static_cast<float>(stagedGain), static_cast<float>(96000.0 / (2.0 * std::numbers::pi * 5000.0)))) << "deviation stood at its last committed value through the refusal";
        }
    };

    "a connection's buffer size derives from an exported parameter"_test = [] {
        registerRecipeTestBlock();
        auto       loader  = recipeTestLoader();
        const auto def     = definitionFrom(kEdgeSizedRecipe);
        const auto sizedAt = [&](float rate) {
            gr::property_map parameters;
            parameters["sample_rate"] = rate;
            const auto composite      = gr::detail::instantiateBlockFromYamlDefinition(loader, def, parameters);
            expect(composite.has_value()) << (composite.has_value() ? "" : composite.error().message);
            return composite.has_value() ? interiorEdgeBufferSize(*composite) : 0UZ;
        };
        expect(eq(sizedAt(2000000.0f), 100000UZ)) << "5 % of the rate in samples, not the loader's default size";
        expect(eq(sizedAt(2400000.0f), 120000UZ)) << "the edge follows the rate the recipe was instantiated at";
        expect(eq(sizedAt(1000010.0f), 50001UZ)) << "a fractional minimum rises to the next whole sample";
    };

    "a buffer size that is not a positive sample count is refused by name"_test = [] {
        registerRecipeTestBlock();
        auto             loader = recipeTestLoader();
        gr::property_map zero;
        zero["sample_rate"] = 0.0f;
        const auto refused  = gr::detail::instantiateBlockFromYamlDefinition(loader, definitionFrom(kEdgeSizedRecipe), zero);
        expect(!refused.has_value()) << "an edge of no samples is not a size";
        if (!refused.has_value()) {
            expect(refused.error().message.contains("connection buffer size")) << refused.error().message;
            expect(refused.error().message.contains("positive")) << refused.error().message;
        }

        const auto text = gr::detail::instantiateBlockFromYamlDefinition(loader, definitionFrom(kUnsizedEdgeRecipe), {});
        expect(!text.has_value()) << "a fifth element that is neither number nor expression used to become the unset marker";
        if (!text.has_value()) {
            expect(text.error().message.contains("connection buffer size")) << text.error().message;
        }
    };

    "a vector setting's elements derive from expressions and re-derive live"_test = [] {
        registerRecipeTestBlock();
        auto             loader = recipeTestLoader();
        const auto       def    = definitionFrom(kDerivedTapsRecipe);
        gr::property_map parameters;
        parameters["peak"]   = 0.5f;
        const auto composite = gr::detail::instantiateBlockFromYamlDefinition(loader, def, parameters);
        expect(composite.has_value()) << (composite.has_value() ? "" : composite.error().message);
        if (!composite.has_value()) {
            return;
        }
        const auto inner = interiorBlock(*composite);
        if (inner == nullptr) {
            return;
        }

        const auto taps = inner->settings().get("taps");
        expect(taps.has_value());
        const auto* derived = taps.has_value() ? taps->get_if<gr::Tensor<float>>() : nullptr;
        expect(derived != nullptr) << "the derived sequence reaches the setting in the member's own element type";
        if (derived != nullptr) {
            expect(eq(derived->size(), 3UZ));
            expect(eq((*derived)[0], 0.5f));
            expect(eq((*derived)[1], 0.25f));
            expect(eq((*derived)[2], 0.25f)) << "a literal element beside the expressions is carried unchanged";
        }

        auto* wrapper = dynamic_cast<gr::GraphWrapper<gr::Graph>*>(composite->get());
        expect(wrapper != nullptr) << "a parameterized composite carries the binding machinery";
        if (wrapper == nullptr) {
            return;
        }
        gr::property_map change;
        change["peak"]     = 0.8f;
        const auto applied = wrapper->applyRecipeParameters(change);
        expect(applied.has_value()) << (applied.has_value() ? "" : applied.error().message);
        const auto staged = inner->settings().stagedParameters();
        const auto tapsIt = staged.find("taps");
        expect(tapsIt != staged.end()) << "the whole sequence is staged, because a setting is staged whole";
        if (tapsIt != staged.end()) {
            const auto* stagedTaps = tapsIt->second.get_if<gr::Tensor<float>>();
            expect(stagedTaps != nullptr);
            if (stagedTaps != nullptr) {
                expect(eq(stagedTaps->size(), 3UZ));
                expect(eq((*stagedTaps)[0], 0.8f));
                expect(eq((*stagedTaps)[1], 0.4f)) << "every derived element re-evaluates against the changed parameter";
                expect(eq((*stagedTaps)[2], 0.25f));
            }
        }
    };

    "a clamped edge holds its bounds at every rate, and the derived setting re-evaluates live"_test = [] {
        registerRecipeTestBlock();
        auto       loader  = recipeTestLoader();
        const auto def     = definitionFrom(kClampedRecipe);
        const auto builtAt = [&](float rate) { //
            gr::property_map parameters;
            parameters["sample_rate"] = rate;
            const auto composite      = gr::detail::instantiateBlockFromYamlDefinition(loader, def, parameters);
            expect(composite.has_value()) << (composite.has_value() ? "" : composite.error().message);
            return composite;
        };
        const auto sizedAt = [&](float rate) {
            const auto composite = builtAt(rate);
            return composite.has_value() ? interiorEdgeBufferSize(*composite) : 0UZ;
        };
        expect(eq(sizedAt(2000000.0f), 100000UZ)) << "between the bounds the rate rule stands";
        expect(eq(sizedAt(96000.0f), 32768UZ)) << "a low rate takes the floor, not 5 % of itself";
        expect(eq(sizedAt(200000000.0f), 4194304UZ)) << "a high rate takes the ceiling";

        const auto composite = builtAt(96000.0f);
        if (!composite.has_value()) {
            return;
        }
        const auto first   = interiorBlockNamed(*composite, "first");
        auto*      wrapper = dynamic_cast<gr::GraphWrapper<gr::Graph>*>(composite->get());
        expect(wrapper != nullptr) << "a parameterized composite carries the binding machinery";
        if (first == nullptr || wrapper == nullptr) {
            return;
        }
        const auto gain = first->settings().get("gain");
        expect(gain.has_value());
        expect(eq(gain->value_or(float{}), 32768.0f)) << "the floor reached the interior setting";

        gr::property_map change;
        change["sample_rate"] = 2000000.0f;
        const auto applied    = wrapper->applyRecipeParameters(change);
        expect(applied.has_value()) << (applied.has_value() ? "" : applied.error().message);
        const auto staged = first->settings().stagedParameters();
        const auto gainIt = staged.find("gain");
        expect(gainIt != staged.end()) << "the clamped derivation re-evaluates through the binding";
        if (gainIt != staged.end()) {
            expect(eq(static_cast<float>(gr::recipe::detail::doubleOf(gainIt->second).value_or(0.0)), 100000.0f)) << "the new rate now falls between the bounds";
        }
    };

    "only the derived values that moved are staged"_test = [] {
        registerRecipeTestBlock();
        auto             loader = recipeTestLoader();
        const auto       def    = definitionFrom(kTwoBoundBlocksRecipe);
        gr::property_map parameters;
        parameters["sample_rate"] = 48000.0f;
        parameters["level"]       = 1.0f;
        const auto composite      = gr::detail::instantiateBlockFromYamlDefinition(loader, def, parameters);
        expect(composite.has_value()) << (composite.has_value() ? "" : composite.error().message);
        if (!composite.has_value()) {
            return;
        }
        auto*      wrapper = dynamic_cast<gr::GraphWrapper<gr::Graph>*>(composite->get());
        const auto first   = interiorBlockNamed(*composite, "first");
        const auto second  = interiorBlockNamed(*composite, "second");
        expect(wrapper != nullptr);
        if (wrapper == nullptr || first == nullptr || second == nullptr) {
            return;
        }
        const auto settle = [&] {
            std::ignore = first->settings().applyStagedParameters();
            std::ignore = second->settings().applyStagedParameters();
            recipeWatches().clear();
        };

        gr::property_map unchanged;
        unchanged["level"] = 1.0f;
        expect(wrapper->applyRecipeParameters(unchanged).has_value());
        expect(!first->settings().stagedParameters().empty() && !second->settings().stagedParameters().empty()) << "the first application after a load stages every bound key";
        settle();

        gr::property_map change;
        change["level"]    = 2.0f;
        const auto applied = wrapper->applyRecipeParameters(change);
        expect(applied.has_value()) << (applied.has_value() ? "" : applied.error().message);
        const auto stagedFirst = first->settings().stagedParameters();
        expect(eq(stagedFirst.size(), 1UZ)) << "only the key whose derived value moved is staged";
        expect(stagedFirst.contains("gain")) << "and it is the moved key";
        expect(second->settings().stagedParameters().empty()) << "the block whose bindings did not move is not staged at all";

        std::ignore = first->settings().applyStagedParameters();
        std::ignore = second->settings().applyStagedParameters();
        expect(eq(recipeWatches()["second"].calls, 0UZ)) << "so its settingsChanged is never called";
        expect(eq(recipeWatches()["first"].calls, 1UZ));
        expect(eq(recipeWatches()["first"].keys.size(), 1UZ)) << "the block that moved sees the moved key alone";
        if (recipeWatches()["first"].keys.size() == 1UZ) {
            expect(eq(recipeWatches()["first"].keys.front(), std::string("gain")));
        }

        recipeWatches().clear();
        expect(wrapper->applyRecipeParameters(change).has_value()) << "the same value again";
        expect(first->settings().stagedParameters().empty() && second->settings().stagedParameters().empty()) << "a parameter restated at its own value stages nothing";
        std::ignore = first->settings().applyStagedParameters();
        std::ignore = second->settings().applyStagedParameters();
        expect(eq(recipeWatches()["first"].calls, 0UZ));
        expect(eq(recipeWatches()["second"].calls, 0UZ));
    };

    "a literal definition is untouched, and parameters against it are refused"_test = [] {
        registerRecipeTestBlock();
        auto       loader    = recipeTestLoader();
        const auto def       = definitionFrom(kLiteralRecipe);
        const auto composite = gr::detail::instantiateBlockFromYamlDefinition(loader, def, {});
        expect(composite.has_value()) << (composite.has_value() ? "" : composite.error().message);
        if (!composite.has_value()) {
            return;
        }
        const auto inner = interiorBlock(*composite);
        if (inner == nullptr) {
            return;
        }
        const auto gain = inner->settings().get("gain");
        expect(gain.has_value());
        expect(eq(gain->value_or(float{}), 2.5f));

        gr::property_map parameters;
        parameters["sample_rate"] = 48000.0f;
        const auto refusal        = gr::detail::instantiateBlockFromYamlDefinition(loader, def, parameters);
        expect(!refusal.has_value());
        expect(refusal.error().message.contains("recipe_unknown_parameter")) << refusal.error().message;
    };
};

// A composite's exported parameters are its settings: every consumer that lists, reads or writes a block's settings
// reaches them through the one settings object, and none reaches the interior.
const boost::ut::suite<"RecipeSettings"> recipeSettingsTests = [] {
    "a composite lists its exported parameters beside the framework's settings, and nothing of its interior"_test = [] {
        auto       loader    = recipeTestLoader();
        const auto composite = parameterizedComposite(loader);
        if (composite == nullptr) {
            return;
        }
        const std::set<std::string>& writable = composite->settings().writableMembers();
        for (const std::string_view parameter : {"sample_rate", "deviation", "tau"}) {
            expect(writable.contains(std::string(parameter))) << parameter << " is exported and must be a writable setting";
        }
        expect(writable.contains("name")) << "the framework's own settings stay beside them";
        for (const std::string_view interior : {"gain", "rate", "label"}) {
            expect(!writable.contains(std::string(interior))) << interior << " belongs to the interior and stays private";
        }
    };

    "reading an exported parameter returns the value in force"_test = [] {
        auto       loader    = recipeTestLoader();
        const auto composite = parameterizedComposite(loader);
        if (composite == nullptr) {
            return;
        }
        expect(eq(readNumber(composite->settings().get(), "sample_rate"), 48000.0f)) << "a supplied value reads back";
        expect(eq(readNumber(composite->settings().get(), "deviation"), 2500.0f));
        const auto tau = composite->settings().get("tau");
        expect(tau.has_value()) << "a defaulted parameter reads back its default";
        if (tau.has_value()) {
            expect(eq(tau->value_or(0.0), 7.5e-05));
        }
    };

    "an exported parameter set and activated re-derives the interior"_test = [] {
        auto       loader    = recipeTestLoader();
        const auto composite = parameterizedComposite(loader);
        const auto inner     = composite == nullptr ? nullptr : interiorBlock(composite);
        if (inner == nullptr) {
            return;
        }
        gr::SettingsBase& settings = composite->settings();
        expect(settings.set({{"deviation", 5000.0f}}).empty()) << "the composite takes its exported parameter";
        expect(!composite->metaInformation().contains("deviation")) << "an accepted parameter is not filed as meta information";
        expect(settings.activateContext().has_value());
        expect(eq(stagedNumber(inner, "gain"), derivedGain(48000.0, 5000.0))) << "activating the stored value re-derived the interior gain";
        expect(eq(readNumber(settings.get(), "deviation"), 5000.0f)) << "the new value is in force";
        expect(!settings.stagedParameters().contains("deviation")) << "the composite holds nothing back for a later apply";
    };

    "an exported parameter staged on the composite applies at once and reads back"_test = [] {
        auto       loader    = recipeTestLoader();
        const auto composite = parameterizedComposite(loader);
        const auto inner     = composite == nullptr ? nullptr : interiorBlock(composite);
        if (inner == nullptr) {
            return;
        }
        gr::SettingsBase& settings = composite->settings();
        expect(settings.setStaged({{"deviation", 5000.0f}}).empty()) << "the composite stages its exported parameter";
        expect(eq(stagedNumber(inner, "gain"), derivedGain(48000.0, 5000.0))) << "the derived gain waits on the interior block for its next work call";
        expect(eq(readNumber(settings.get(), "deviation"), 5000.0f));
        expect(!settings.stagedParameters().contains("deviation")) << "nothing waits on the composite, which has no work call of its own";

        expect(settings.setStaged({{"sample_rate", 96000.0f}, {"name", std::string("renamed")}}).empty()) << "an exported parameter and a framework setting in one call";
        expect(eq(stagedNumber(inner, "gain"), derivedGain(96000.0, 5000.0))) << "the second change re-derived against the first";
        expect(settings.stagedParameters().contains("name")) << "the framework's setting takes its ordinary staged path";
    };

    "one change through the settings stages each moved derived value once"_test = [] {
        registerRecipeTestBlock();
        auto             loader = recipeTestLoader();
        gr::property_map parameters;
        parameters["sample_rate"] = 48000.0f;
        parameters["level"]       = 1.0f;
        const auto composite      = gr::detail::instantiateBlockFromYamlDefinition(loader, definitionFrom(kTwoBoundBlocksRecipe), parameters);
        expect(composite.has_value()) << (composite.has_value() ? "" : composite.error().message);
        if (!composite.has_value()) {
            return;
        }
        const auto first  = interiorBlockNamed(*composite, "first");
        const auto second = interiorBlockNamed(*composite, "second");
        if (first == nullptr || second == nullptr) {
            return;
        }
        const auto applyInterior = [&] {
            std::ignore = first->settings().applyStagedParameters();
            std::ignore = second->settings().applyStagedParameters();
        };
        gr::SettingsBase& settings = (*composite)->settings();

        // the first application after a load stages every bound key, so it is settled before anything is counted
        std::ignore = settings.setStaged({{"level", 1.0f}});
        applyInterior();
        recipeWatches().clear();

        expect(settings.set({{"level", 2.0f}}).empty());
        std::ignore = settings.activateContext();
        std::ignore = settings.applyStagedParameters(); // the composite's own apply, as its init runs it
        applyInterior();
        expect(eq(recipeWatches()["first"].calls, 1UZ)) << "the block whose derived value moved sees one change";
        expect(recipeWatches()["first"].keys == std::vector<std::string>{"gain"}) << "and it names the moved key alone";
        expect(eq(recipeWatches()["second"].calls, 0UZ)) << "the block whose derived values held sees none";
    };

    "a settings message reaches an exported parameter, and a settings Get lists it"_test = [] {
        auto       loader    = recipeTestLoader();
        const auto composite = parameterizedComposite(loader);
        const auto inner     = composite == nullptr ? nullptr : interiorBlock(composite);
        if (inner == nullptr) {
            return;
        }
        gr::Graph&     graph = *composite->graph();
        gr::MsgPortOut toComposite;
        gr::MsgPortIn  fromComposite;
        expect(graph.msgOut.connect(fromComposite).has_value());
        expect(toComposite.connect(graph.msgIn).has_value());

        gr::sendMessage<gr::message::Command::Set>(toComposite, "", gr::block::property::kSetting, {{"deviation", 5000.0f}});
        composite->processScheduledMessages();
        expect(eq(stagedNumber(inner, "gain"), derivedGain(48000.0, 5000.0))) << "a Set on the settings property re-derives the interior";

        gr::sendMessage<gr::message::Command::Set>(toComposite, "", gr::block::property::kStagedSetting, {{"sample_rate", 96000.0f}});
        composite->processScheduledMessages();
        expect(eq(stagedNumber(inner, "gain"), derivedGain(96000.0, 5000.0))) << "a Set on the staged-settings property re-derives the interior";
        expect(eq(fromComposite.streamReader().available(), 0UZ)) << "neither Set was answered with an error";

        gr::sendMessage<gr::message::Command::Get>(toComposite, "", gr::block::property::kSetting, gr::property_map{});
        composite->processScheduledMessages();
        expect(eq(fromComposite.streamReader().available(), 1UZ)) << "a Get is answered";
        if (fromComposite.streamReader().available() != 1UZ) {
            return;
        }
        gr::ReaderSpanLike auto replies = fromComposite.streamReader().get<gr::SpanReleasePolicy::ProcessAll>(1UZ);
        const gr::Message       reply   = replies[0];
        expect(replies.consume(replies.size()));
        expect(reply.data.has_value()) << "the Get is answered with the settings";
        if (reply.data.has_value()) {
            expect(eq(readNumber(*reply.data, "deviation"), 5000.0f)) << "the reply lists each exported parameter at its value in force";
            expect(eq(readNumber(*reply.data, "sample_rate"), 96000.0f));
        }
    };

    "a name the recipe does not export is refused, and a refused derivation leaves the value standing"_test = [] {
        auto       loader    = recipeTestLoader();
        const auto composite = parameterizedComposite(loader);
        const auto inner     = composite == nullptr ? nullptr : interiorBlock(composite);
        if (inner == nullptr) {
            return;
        }
        gr::SettingsBase& settings = composite->settings();
        expect(settings.setStaged({{"bandwidth", 1.0f}}).contains("bandwidth")) << "a name nothing declares comes back unset";
        expect(settings.setStaged({{"gain", 3.0f}}).contains("gain")) << "an interior block's setting is not reachable through the composite";
        expect(settings.set({{"gain", 3.0f}}).contains("gain")) << "nor through the stored path";

        std::string reported;
        try {
            std::ignore = settings.setStaged({{"deviation", 0.0f}});
        } catch (const gr::exception& e) {
            reported = e.message;
        }
        expect(reported.contains("recipe_expression_conversion")) << "a non-finite derivation refuses the change by name: " << reported;
        expect(eq(readNumber(settings.get(), "deviation"), 2500.0f)) << "the value in force stands";
        expect(!inner->settings().stagedParameters().contains("gain")) << "nothing reached the interior";
    };

    "an exported parameter is held and read back in its declared type"_test = [] {
        registerRecipeTestBlock();
        auto       loader    = recipeTestLoader();
        const auto composite = gr::detail::instantiateBlockFromYamlDefinition(loader, definitionFrom(kParameterizedRecipe), {{"sample_rate", 48000.0}, {"deviation", std::int64_t{2500}}});
        expect(composite.has_value()) << (composite.has_value() ? "" : composite.error().message);
        if (!composite.has_value()) {
            return;
        }
        gr::SettingsBase& settings = (*composite)->settings();
        const auto        rate     = settings.get("sample_rate");
        expect(rate.has_value() && rate->get_if<float>() != nullptr) << "a float32 parameter supplied as a float64 is held as a float32";
        const auto deviation = settings.get("deviation");
        expect(deviation.has_value() && deviation->get_if<float>() != nullptr) << "and one supplied as an integer";

        expect(settings.setStaged({{"tau", 5.0e-05f}}).empty());
        const auto tau = settings.get("tau");
        expect(tau.has_value() && tau->get_if<double>() != nullptr) << "a float64 parameter staged as a float32 is held as a float64";

        const std::string refused = refusalOf([&] { std::ignore = settings.setStaged({{"sample_rate", std::string("fast")}}); });
        expect(refused.contains("sample_rate")) << "a value the settings conversion refuses is refused by name: " << refused;
        expect(eq(readNumber(settings.get(), "sample_rate"), 48000.0f)) << "and the value in force stands";

        const auto counting = gr::detail::instantiateBlockFromYamlDefinition(loader, definitionFrom(kTwoTargetsRecipe), {{"level", 4.0}});
        expect(counting.has_value()) << "a whole float64 converts to an int32 parameter: " << (counting.has_value() ? "" : counting.error().message);
        if (counting.has_value()) {
            const auto level = (*counting)->settings().get("level");
            expect(level.has_value() && level->get_if<std::int32_t>() != nullptr) << "and is held as an int32";
        }
        const auto fractional = gr::detail::instantiateBlockFromYamlDefinition(loader, definitionFrom(kTwoTargetsRecipe), {{"level", 4.5}});
        expect(!fractional.has_value()) << "a fraction for an int32 parameter is refused, as the literal path refuses it";
    };

    "a recipe inside a recipe re-derives its own interior when the outer parameter changes"_test = [] {
        const HalvingRecipeRoot root;
        auto                    loader    = nestingLoader(root);
        const auto              composite = nestingComposite(loader, 2000.0f);
        const auto              first     = composite == nullptr ? nullptr : interiorBlockNamed(composite, "first");
        const auto              stage     = composite == nullptr ? nullptr : interiorBlockNamed(composite, "stage");
        const auto              innermost = stage == nullptr ? nullptr : interiorBlockNamed(stage, "inner");
        if (first == nullptr || innermost == nullptr) {
            return;
        }
        expect(eq(readNumber(stage->settings().get(), "level"), 1000.0f)) << "the inner recipe was built from the outer's derived value";
        expect(eq(readNumber(innermost->settings().get(), "gain"), 1.0f)) << "and derived its own interior from it";

        expect(composite->settings().setStaged({{"rate", 3000.0f}}).empty());
        expect(eq(readNumber(stage->settings().get(), "level"), 2000.0f)) << "the outer binding reached the inner recipe's exported parameter";
        expect(eq(stagedNumber(innermost, "gain"), 0.5f)) << "and the inner recipe re-derived its interior from it";
        expect(eq(stagedNumber(first, "gain"), 3000.0f)) << "beside the outer's own interior block";
    };

    "a value the recipe refuses is refused whole by set() and by setStaged(), and a later change is taken"_test = [] {
        auto       loader    = recipeTestLoader();
        const auto composite = parameterizedComposite(loader);
        const auto inner     = composite == nullptr ? nullptr : interiorBlock(composite);
        if (inner == nullptr) {
            return;
        }
        gr::SettingsBase&                   settings        = composite->settings();
        const std::optional<gr::pmt::Value> storedDeviation = settings.getStored("deviation");
        const std::optional<gr::pmt::Value> storedName      = settings.getStored("name");

        const std::string bySet = refusalOf([&] { std::ignore = settings.set({{"deviation", 0.0f}, {"name", std::string("renamed")}}); });
        expect(bySet.contains("recipe_expression_conversion")) << "set() refuses a value the recipe cannot derive from, as it refuses a member's bad value: " << bySet;
        expect(settings.getStored("deviation") == storedDeviation) << "the refused value is not stored";
        expect(settings.getStored("name") == storedName) << "nor is the framework setting named beside it";

        const std::string later = refusalOf([&] {
            std::ignore = settings.set({{"sample_rate", 96000.0f}});
            std::ignore = settings.activateContext();
        });
        expect(later.empty()) << "a later change of another parameter is taken: " << later;
        expect(eq(readNumber(settings.get(), "sample_rate"), 96000.0f));
        expect(eq(readNumber(settings.get(), "deviation"), 2500.0f)) << "the refused value never came into force";

        const std::string byStaged = refusalOf([&] { std::ignore = settings.setStaged({{"deviation", 0.0f}, {"name", std::string("staged")}}); });
        expect(byStaged.contains("recipe_expression_conversion")) << byStaged;
        expect(!settings.stagedParameters().contains("name")) << "the framework setting of the refused call is not staged";
        expect(eq(stagedNumber(inner, "gain"), derivedGain(96000.0, 2500.0))) << "the interior holds the last derivation the recipe accepted";
    };

    "a derived value an interior block refuses is refused before any interior block takes one"_test = [] {
        registerRecipeTestBlock();
        auto       loader    = recipeTestLoader();
        const auto composite = gr::detail::instantiateBlockFromYamlDefinition(loader, definitionFrom(kTwoTargetsRecipe), {{"level", std::int32_t{4}}});
        expect(composite.has_value()) << (composite.has_value() ? "" : composite.error().message);
        if (!composite.has_value()) {
            return;
        }
        const auto first  = interiorBlockNamed(*composite, "first");
        const auto second = interiorBlockNamed(*composite, "second");
        if (first == nullptr || second == nullptr) {
            return;
        }
        gr::SettingsBase& settings = (*composite)->settings();

        const std::string staged = refusalOf([&] { std::ignore = settings.setStaged({{"level", std::int32_t{-1}}}); });
        expect(staged.contains("recipe_expression_conversion")) << "the refusal names the recipe's reason: " << staged;
        expect(staged.contains("second")) << "and the interior block that refused the derived value: " << staged;
        expect(!first->settings().stagedParameters().contains("gain")) << "the interior block that would take its value took nothing";
        expect(eq(readNumber(settings.get(), "level"), 4.0f)) << "the value in force stands";

        const std::string stored = refusalOf([&] { std::ignore = settings.set({{"level", std::int32_t{-1}}}); });
        expect(stored.contains("recipe_expression_conversion")) << "set() refuses the same value: " << stored;

        expect(settings.setStaged({{"level", std::int32_t{6}}}).empty()) << "a value both interior blocks take";
        expect(eq(stagedNumber(first, "gain"), 6.0f));
        expect(eq(stagedNumber(second, "count"), 6.0f));
    };

    "a refusal inside a nested recipe refuses the outer change whole"_test = [] {
        const HalvingRecipeRoot root;
        auto                    loader    = nestingLoader(root);
        const auto              composite = nestingComposite(loader, 2000.0f);
        const auto              first     = composite == nullptr ? nullptr : interiorBlockNamed(composite, "first");
        const auto              stage     = composite == nullptr ? nullptr : interiorBlockNamed(composite, "stage");
        if (first == nullptr || stage == nullptr) {
            return;
        }
        const std::string refused = refusalOf([&] { std::ignore = composite->settings().setStaged({{"rate", 1000.0f}}); });
        expect(refused.contains("recipe_expression_conversion")) << "a rate of 1000 derives a level of zero, where 1000 / level has no finite value: " << refused;
        expect(!first->settings().stagedParameters().contains("gain")) << "the outer's plain interior block took nothing";
        expect(eq(readNumber(stage->settings().get(), "level"), 1000.0f)) << "the inner recipe's value in force stands";
        expect(eq(readNumber(composite->settings().get(), "rate"), 2000.0f)) << "and the outer's";
    };

    "returning to a context restores its exported parameters, and an activation in the same context re-applies only what set() named"_test = [] {
        auto       loader    = recipeTestLoader();
        const auto composite = parameterizedComposite(loader);
        const auto inner     = composite == nullptr ? nullptr : interiorBlock(composite);
        if (inner == nullptr) {
            return;
        }
        gr::SettingsBase& settings = composite->settings();
        expect(eq(readNumber(settings.getStored().value_or(gr::property_map{}), "deviation"), 2500.0f)) << "the default context stores the exported parameters beside the members";

        const gr::SettingsCtx wide{.time = 0ULL, .context = std::string("wide")};
        expect(settings.set({{"deviation", 5000.0f}}, wide).empty());
        expect(settings.activateContext(wide).has_value());
        expect(eq(readNumber(settings.get(), "deviation"), 5000.0f)) << "the context 'wide' is in force";
        std::ignore = inner->settings().applyStagedParameters();

        expect(settings.activateContext().has_value());
        expect(eq(readNumber(settings.get(), "deviation"), 2500.0f)) << "the default context's deviation is in force again";
        expect(eq(stagedNumber(inner, "gain"), derivedGain(48000.0, 2500.0))) << "and the interior re-derived from it";

        expect(settings.setStaged({{"deviation", 3000.0f}}).empty());
        expect(settings.set({{"sample_rate", 96000.0f}}).empty());
        expect(settings.activateContext().has_value());
        expect(eq(readNumber(settings.get(), "sample_rate"), 96000.0f)) << "an activation in the same context applies what set() named";
        expect(eq(readNumber(settings.get(), "deviation"), 3000.0f)) << "and leaves a parameter set() never named at its value in force, as it leaves a member";
    };

    "a subscriber hears of a change to an exported parameter, whichever path made it"_test = [] {
        auto       loader    = recipeTestLoader();
        const auto composite = parameterizedComposite(loader);
        if (composite == nullptr) {
            return;
        }
        gr::Graph&     graph = *composite->graph();
        gr::MsgPortOut toComposite;
        gr::MsgPortIn  fromComposite;
        expect(graph.msgOut.connect(fromComposite).has_value());
        expect(toComposite.connect(graph.msgIn).has_value());
        const auto takeMessages = [&fromComposite] {
            std::vector<gr::Message> taken;
            const std::size_t        available = fromComposite.streamReader().available();
            if (available == 0UZ) {
                return taken;
            }
            gr::ReaderSpanLike auto messages = fromComposite.streamReader().get<gr::SpanReleasePolicy::ProcessAll>(available);
            taken.assign(messages.begin(), messages.end());
            expect(messages.consume(messages.size()));
            return taken;
        };
        const auto notified = [](const std::vector<gr::Message>& messages, std::string_view endpoint) {
            for (const gr::Message& message : messages) {
                if (message.endpoint == endpoint && message.clientRequestID == "watcher" && message.data.has_value()) {
                    return *message.data;
                }
            }
            return gr::property_map{};
        };

        gr::sendMessage<gr::message::Command::Subscribe>(toComposite, "", gr::block::property::kSetting, gr::property_map{}, "watcher");
        gr::sendMessage<gr::message::Command::Subscribe>(toComposite, "", gr::block::property::kStagedSetting, gr::property_map{}, "watcher");
        composite->processScheduledMessages();
        expect(takeMessages().empty()) << "a subscription is not answered";

        gr::sendMessage<gr::message::Command::Set>(toComposite, "", gr::block::property::kSetting, {{"deviation", 5000.0f}});
        composite->processScheduledMessages();
        const std::vector<gr::Message> afterMessage = takeMessages();
        expect(eq(afterMessage.size(), 2UZ)) << "one notification for each subscribed property";
        expect(eq(readNumber(notified(afterMessage, gr::block::property::kStagedSetting), "deviation"), 5000.0f)) << "the staged-settings subscriber hears the applied parameter";
        expect(eq(readNumber(notified(afterMessage, gr::block::property::kSetting), "deviation"), 5000.0f)) << "the settings subscriber hears the settings in force";
        expect(eq(readNumber(notified(afterMessage, gr::block::property::kSetting), "sample_rate"), 48000.0f));

        expect(composite->settings().setStaged({{"sample_rate", 96000.0f}}).empty());
        composite->processScheduledMessages();
        expect(eq(readNumber(notified(takeMessages(), gr::block::property::kSetting), "sample_rate"), 96000.0f)) << "a change staged by a direct call is announced at the composite's next message pass";

        composite->processScheduledMessages();
        expect(takeMessages().empty()) << "each change is announced once";
    };

    "the reply to a staged-settings Set names each key it set, an exported parameter included"_test = [] {
        auto       loader    = recipeTestLoader();
        const auto composite = parameterizedComposite(loader);
        if (composite == nullptr) {
            return;
        }
        gr::Graph&     graph = *composite->graph();
        gr::MsgPortOut toComposite;
        gr::MsgPortIn  fromComposite;
        expect(graph.msgOut.connect(fromComposite).has_value());
        expect(toComposite.connect(graph.msgIn).has_value());

        gr::sendMessage<gr::message::Command::Set>(toComposite, "", gr::block::property::kStagedSetting, {{"deviation", 5000.0f}, {"name", std::string("renamed")}}, "request-1");
        composite->processScheduledMessages();
        expect(eq(fromComposite.streamReader().available(), 1UZ)) << "a Set with a request id is answered";
        if (fromComposite.streamReader().available() != 1UZ) {
            return;
        }
        gr::ReaderSpanLike auto replies = fromComposite.streamReader().get<gr::SpanReleasePolicy::ProcessAll>(1UZ);
        const gr::Message       reply   = replies[0];
        expect(replies.consume(replies.size()));
        expect(reply.cmd == gr::message::Command::Final) << "the reply is final";
        expect(reply.data.has_value()) << "the Set was taken";
        if (reply.data.has_value()) {
            expect(eq(readNumber(*reply.data, "deviation"), 5000.0f)) << "the reply names the exported parameter at its value in force";
            expect(reply.data->contains("name")) << "and the framework setting, as staged";
        }
    };
};

int main() { /* not needed for ut */ }
