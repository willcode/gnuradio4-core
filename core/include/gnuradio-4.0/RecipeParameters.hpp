#ifndef GNURADIO_RECIPEPARAMETERS_HPP
#define GNURADIO_RECIPEPARAMETERS_HPP

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <expected>
#include <format>
#include <limits>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gnuradio-4.0/Message.hpp>
#include <gnuradio-4.0/Settings.hpp>
#include <gnuradio-4.0/Tag.hpp>

namespace gr::recipe {

/**
 * @brief Exported recipe parameters and the bounded arithmetic that derives interior settings from them.
 *
 * The dialect has one engine and two front ends. The YAML definitions loader compiles a recipe
 * document into these types. Generated typed headers compile the same bindings statically. A
 * composite behaves the same way however it was made.
 *
 * The grammar is small by design. It holds numbers, parameter references, the named constants
 * `pi` and `tau_circle` (2·pi), the four arithmetic operators, unary sign, parentheses, and one
 * function, `clamp(x, low, high)`. An expression reads exported parameters and constants only. It
 * reads no other derived value. Evaluation is therefore one pass.
 */

/// One exported parameter of a recipe. A declaration without a default is required at
/// instantiation. A default hides behavior from a user unaware of the parameter. A general
/// recipe therefore states no default. A well-defined recipe states values that are normal in use.
struct ParameterDeclaration {
    std::string               name;
    std::string               type; // dialect type word: bool, int8..int64, uint8..uint64, float32, float64, string
    std::optional<pmt::Value> defaultValue;
    std::string               doc;

    [[nodiscard]] bool required() const noexcept { return !defaultValue.has_value(); }
};

namespace detail {

[[nodiscard]] constexpr bool integralTypeWord(std::string_view type) noexcept {
    return type == "int8" || type == "int16" || type == "int32" || type == "int64" //
           || type == "uint8" || type == "uint16" || type == "uint32" || type == "uint64";
}

[[nodiscard]] constexpr bool numericTypeWord(std::string_view type) noexcept { return integralTypeWord(type) || type == "float32" || type == "float64"; }

/// A vector type word is a scalar type word followed by `[]`, as in `float64[]`, `int32[]` and `string[]`.
[[nodiscard]] constexpr bool vectorTypeWord(std::string_view type) noexcept { return type.ends_with("[]") && (numericTypeWord(type.substr(0UZ, type.size() - 2UZ)) || type.substr(0UZ, type.size() - 2UZ) == "bool" || type.substr(0UZ, type.size() - 2UZ) == "string"); }

/// True for a declared type whose value is placed unchanged and never computed. Strings, booleans and vectors are such
/// types. A recipe exports one for a caller to set and places the value unchanged. A boolean is not a number here.
/// Arithmetic on it, such as adding two toggles or scaling one, has no meaning a recipe should be able to write.
[[nodiscard]] constexpr bool substitutedTypeWord(std::string_view type) noexcept { return type == "string" || type == "bool" || vectorTypeWord(type); }

[[nodiscard]] constexpr bool knownTypeWord(std::string_view type) noexcept { return numericTypeWord(type) || type == "bool" || type == "string" || vectorTypeWord(type); }

/// the numeric content of a parameter value, whichever numeric alternative it holds
[[nodiscard]] inline std::optional<double> doubleOf(const pmt::Value& value) noexcept {
    if (const auto* v = value.get_if<double>()) {
        return *v;
    }
    if (const auto* v = value.get_if<float>()) {
        return static_cast<double>(*v);
    }
    if (const auto* v = value.get_if<std::int64_t>()) {
        return static_cast<double>(*v);
    }
    if (const auto* v = value.get_if<std::uint64_t>()) {
        return static_cast<double>(*v);
    }
    if (const auto* v = value.get_if<std::int32_t>()) {
        return static_cast<double>(*v);
    }
    if (const auto* v = value.get_if<std::uint32_t>()) {
        return static_cast<double>(*v);
    }
    if (const auto* v = value.get_if<std::int16_t>()) {
        return static_cast<double>(*v);
    }
    if (const auto* v = value.get_if<std::uint16_t>()) {
        return static_cast<double>(*v);
    }
    if (const auto* v = value.get_if<std::int8_t>()) {
        return static_cast<double>(*v);
    }
    if (const auto* v = value.get_if<std::uint8_t>()) {
        return static_cast<double>(*v);
    }
    return std::nullopt;
}

/// the integral content of a parameter value. The integer evaluation path uses it. An index-grade
/// value then never passes through a double, which holds integers exactly only up to 2^53.
[[nodiscard]] inline std::optional<std::int64_t> integerOf(const pmt::Value& value) noexcept {
    if (const auto* v = value.get_if<std::int64_t>()) {
        return *v;
    }
    if (const auto* v = value.get_if<std::uint64_t>()) {
        return *v <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ? std::optional<std::int64_t>(static_cast<std::int64_t>(*v)) : std::nullopt;
    }
    if (const auto* v = value.get_if<std::int32_t>()) {
        return static_cast<std::int64_t>(*v);
    }
    if (const auto* v = value.get_if<std::uint32_t>()) {
        return static_cast<std::int64_t>(*v);
    }
    if (const auto* v = value.get_if<std::int16_t>()) {
        return static_cast<std::int64_t>(*v);
    }
    if (const auto* v = value.get_if<std::uint16_t>()) {
        return static_cast<std::int64_t>(*v);
    }
    if (const auto* v = value.get_if<std::int8_t>()) {
        return static_cast<std::int64_t>(*v);
    }
    if (const auto* v = value.get_if<std::uint8_t>()) {
        return static_cast<std::int64_t>(*v);
    }
    return std::nullopt;
}

} // namespace detail

/// A parsed expression as a postfix program. Parameters are referenced by index into the
/// declaration list the expression was parsed against, so evaluation needs only the value
/// vector aligned with those declarations.
struct Expression {
    enum class OpKind : std::uint8_t { pushNumber, pushInteger, pushParameter, add, subtract, multiply, divide, negate, clamp };
    struct Op {
        OpKind       kind;
        double       number{};
        std::int64_t integer{};
        std::size_t  parameterIndex{};
    };

    std::vector<Op> ops;
    std::string     source;
    // true when every operand is integral and there is no division. The expression is then
    // evaluated in int64, and integer-grade values never round-trip through a double.
    bool integerMode = false;
};

namespace detail {

/// Returns user text fit to quote in an error message. Each control character becomes '?', and
/// text longer than kMaxEcho characters is cut and ends in "...".
[[nodiscard]] inline std::string printableEcho(std::string_view text) {
    constexpr std::size_t kMaxEcho = 120;
    std::string           echo;
    echo.reserve(std::min(text.size(), kMaxEcho));
    for (const char c : text.substr(0, kMaxEcho)) {
        echo.push_back(static_cast<unsigned char>(c) < 0x20U || c == 0x7F ? '?' : c);
    }
    if (text.size() > kMaxEcho) {
        echo += "...";
    }
    return echo;
}

inline constexpr std::size_t kMaxExpressionDepth = 64;

/// What the '=' sentinel makes of one string scalar. The dialect's escape removes exactly one
/// backslash from a leading backslash run before '='. Every literal spelling therefore stays
/// expressible wherever an expression may appear.
enum class SentinelKind : std::uint8_t { plainText, escapedLiteral, expression };

[[nodiscard]] inline SentinelKind sentinelOf(std::string_view text) noexcept {
    const std::size_t backslashes = text.find_first_not_of('\\');
    if (backslashes != std::string_view::npos && backslashes > 0UZ && text[backslashes] == '=') {
        return SentinelKind::escapedLiteral;
    }
    return text.starts_with("=") ? SentinelKind::expression : SentinelKind::plainText;
}

struct Parser {
    std::string_view                      text;
    std::size_t                           position = 0;
    std::span<const ParameterDeclaration> declarations;
    Expression*                           out;
    bool                                  sawDivide           = false;
    bool                                  allOperandsIntegral = true;
    std::size_t                           depth               = 0;

    void skipSpace() noexcept {
        while (position < text.size() && (text[position] == ' ' || text[position] == '\t')) {
            ++position;
        }
    }

    [[nodiscard]] bool atEnd() noexcept {
        skipSpace();
        return position >= text.size();
    }

    [[nodiscard]] std::optional<char> peek() noexcept {
        skipSpace();
        return position < text.size() ? std::optional<char>(text[position]) : std::nullopt;
    }

    [[nodiscard]] std::unexpected<gr::Error> fail(std::string_view what) const { return std::unexpected(gr::Error(std::format("recipe_expression_parse: {} at position {} in '{}'", what, position, printableEcho(text)))); }

    [[nodiscard]] std::expected<void, gr::Error> parseExpr() {
        if (auto term = parseTerm(); !term.has_value()) {
            return term;
        }
        while (auto c = peek()) {
            if (*c != '+' && *c != '-') {
                break;
            }
            ++position;
            if (auto term = parseTerm(); !term.has_value()) {
                return term;
            }
            out->ops.push_back({.kind = *c == '+' ? Expression::OpKind::add : Expression::OpKind::subtract});
        }
        return {};
    }

    [[nodiscard]] std::expected<void, gr::Error> parseTerm() {
        if (auto factor = parseFactor(); !factor.has_value()) {
            return factor;
        }
        while (auto c = peek()) {
            if (*c != '*' && *c != '/') {
                break;
            }
            sawDivide = sawDivide || *c == '/';
            ++position;
            if (auto factor = parseFactor(); !factor.has_value()) {
                return factor;
            }
            out->ops.push_back({.kind = *c == '*' ? Expression::OpKind::multiply : Expression::OpKind::divide});
        }
        return {};
    }

    [[nodiscard]] std::expected<void, gr::Error> parseFactor() {
        // The depth guard bounds recursion. Deep nesting from a remote catalog is refused and
        // cannot overflow the stack.
        if (depth >= kMaxExpressionDepth) {
            return fail("expression nested too deeply");
        }
        ++depth;
        auto parsed = parseFactorAtDepth();
        --depth;
        return parsed;
    }

    [[nodiscard]] std::expected<void, gr::Error> parseFactorAtDepth() {
        const auto c = peek();
        if (!c.has_value()) {
            return fail("expected a value");
        }
        if (*c == '-' || *c == '+') {
            ++position;
            if (auto factor = parseFactor(); !factor.has_value()) {
                return factor;
            }
            if (*c == '-') {
                out->ops.push_back({.kind = Expression::OpKind::negate});
            }
            return {};
        }
        return parsePrimary();
    }

    [[nodiscard]] std::expected<void, gr::Error> parsePrimary() {
        const auto c = peek();
        if (!c.has_value()) {
            return fail("expected a value");
        }
        if (*c == '(') {
            ++position;
            if (auto expr = parseExpr(); !expr.has_value()) {
                return expr;
            }
            if (peek() != ')') {
                return fail("expected ')'");
            }
            ++position;
            return {};
        }
        if ((*c >= '0' && *c <= '9') || *c == '.') {
            return parseNumber();
        }
        if ((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || *c == '_') {
            return parseIdentifier();
        }
        return fail(std::format("unexpected character '{}'", printableEcho(std::string_view(&*c, 1))));
    }

    [[nodiscard]] std::expected<void, gr::Error> parseNumber() {
        const std::size_t start = position;
        while (position < text.size() && ((text[position] >= '0' && text[position] <= '9') || text[position] == '.' || text[position] == 'e' || text[position] == 'E' //
                                             || ((text[position] == '+' || text[position] == '-') && (text[position - 1] == 'e' || text[position - 1] == 'E')))) {
            ++position;
        }
        const std::string_view token      = text.substr(start, position - start);
        const bool             isIntegral = token.find('.') == std::string_view::npos && token.find('e') == std::string_view::npos && token.find('E') == std::string_view::npos;
        if (isIntegral) {
            std::int64_t value{};
            const auto [ptr, ec] = std::from_chars(token.data(), token.data() + token.size(), value);
            if (ec != std::errc{} || ptr != token.data() + token.size()) {
                return fail(std::format("unparsable integer '{}'", token));
            }
            out->ops.push_back({.kind = Expression::OpKind::pushInteger, .integer = value});
            return {};
        }
        double value{};
        const auto [ptr, ec] = std::from_chars(token.data(), token.data() + token.size(), value);
        if (ec != std::errc{} || ptr != token.data() + token.size()) {
            return fail(std::format("unparsable number '{}'", token));
        }
        allOperandsIntegral = false;
        out->ops.push_back({.kind = Expression::OpKind::pushNumber, .number = value});
        return {};
    }

    [[nodiscard]] std::expected<void, gr::Error> parseIdentifier() {
        const std::size_t start = position;
        while (position < text.size() && ((text[position] >= 'a' && text[position] <= 'z') || (text[position] >= 'A' && text[position] <= 'Z') || (text[position] >= '0' && text[position] <= '9') || text[position] == '_')) {
            ++position;
        }
        const std::string_view name = text.substr(start, position - start);
        if (peek() == '(') {
            return parseCall(name);
        }
        if (name == "pi") {
            allOperandsIntegral = false;
            out->ops.push_back({.kind = Expression::OpKind::pushNumber, .number = std::numbers::pi});
            return {};
        }
        if (name == "tau_circle") {
            allOperandsIntegral = false;
            out->ops.push_back({.kind = Expression::OpKind::pushNumber, .number = 2.0 * std::numbers::pi});
            return {};
        }
        for (std::size_t index = 0; index < declarations.size(); ++index) {
            if (declarations[index].name != name) {
                continue;
            }
            if (!numericTypeWord(declarations[index].type)) {
                return std::unexpected(gr::Error(std::format("recipe_expression_parse: parameter '{}' has type {} and cannot appear in an expression ('{}')", printableEcho(name), declarations[index].type, printableEcho(text))));
            }
            allOperandsIntegral = allOperandsIntegral && integralTypeWord(declarations[index].type);
            out->ops.push_back({.kind = Expression::OpKind::pushParameter, .parameterIndex = index});
            return {};
        }
        std::string declared;
        for (const auto& declaration : declarations) {
            declared += declared.empty() ? declaration.name : (", " + declaration.name);
        }
        return std::unexpected(gr::Error(std::format("recipe_unknown_identifier: '{}' in '{}' — declared parameters: [{}]", printableEcho(name), printableEcho(text), printableEcho(declared))));
    }

    /// An identifier followed by '(' is a call. `clamp(x, low, high)` is the one function the
    /// dialect has. The arity is checked here, since a call is the only place the grammar takes
    /// more than one operand. The order of the bounds is checked at evaluation, where their
    /// values are known.
    [[nodiscard]] std::expected<void, gr::Error> parseCall(std::string_view name) {
        if (name != "clamp") {
            return fail(std::format("unknown function '{}' — the dialect declares clamp(x, low, high)", printableEcho(name)));
        }
        ++position; // the '(' that peek() found
        std::size_t arguments = 0UZ;
        while (true) {
            if (auto argument = parseExpr(); !argument.has_value()) {
                return argument;
            }
            ++arguments;
            if (peek() != ',') {
                break;
            }
            ++position;
        }
        if (peek() != ')') {
            return fail("expected ')'");
        }
        ++position;
        if (arguments != 3UZ) {
            return fail(std::format("clamp takes 3 arguments (value, low, high), not {}", arguments));
        }
        out->ops.push_back({.kind = Expression::OpKind::clamp});
        return {};
    }
};

} // namespace detail

/// Parses the text AFTER the '=' sentinel against the recipe's declarations.
[[nodiscard]] inline std::expected<Expression, gr::Error> parseExpression(std::string_view source, std::span<const ParameterDeclaration> declarations) {
    Expression     expression{.ops = {}, .source = std::string(source)};
    detail::Parser parser{.text = source, .declarations = declarations, .out = &expression};
    if (auto parsed = parser.parseExpr(); !parsed.has_value()) {
        return std::unexpected(parsed.error());
    }
    if (!parser.atEnd()) {
        return std::unexpected(parser.fail("trailing input").error());
    }
    expression.integerMode = parser.allOperandsIntegral && !parser.sawDivide;
    return expression;
}

/// Evaluates against values aligned with the declarations the expression was parsed with.
/// An integer-mode expression evaluates in int64 throughout. Every other expression evaluates
/// in float64. A non-finite result is refused, since no later conversion can make it valid.
[[nodiscard]] inline std::expected<pmt::Value, gr::Error> evaluate(const Expression& expression, std::span<const pmt::Value> parameterValues) {
    auto operandError = [&](std::size_t index) { return std::unexpected(gr::Error(std::format("recipe_expression_conversion: parameter #{} holds no numeric value for '{}'", index, detail::printableEcho(expression.source)))); };

    if (expression.integerMode) {
        std::vector<std::int64_t> stack;
        for (const auto& op : expression.ops) {
            switch (op.kind) {
            case Expression::OpKind::pushInteger: stack.push_back(op.integer); break;
            case Expression::OpKind::pushParameter: {
                if (op.parameterIndex >= parameterValues.size()) {
                    return operandError(op.parameterIndex);
                }
                const auto value = detail::integerOf(parameterValues[op.parameterIndex]);
                if (!value.has_value()) {
                    return operandError(op.parameterIndex);
                }
                stack.push_back(*value);
                break;
            }
            case Expression::OpKind::negate:
                if (stack.back() == std::numeric_limits<std::int64_t>::min()) {
                    return std::unexpected(gr::Error(std::format("recipe_expression_conversion: '{}' overflows", detail::printableEcho(expression.source))));
                }
                stack.back() = -stack.back();
                break;
            case Expression::OpKind::clamp: {
                const std::int64_t high = stack.back();
                stack.pop_back();
                const std::int64_t low = stack.back();
                stack.pop_back();
                if (low > high) {
                    return std::unexpected(gr::Error(std::format("recipe_expression_domain: clamp bounds {} and {} are not ordered low, high in '{}'", low, high, detail::printableEcho(expression.source))));
                }
                stack.back() = std::clamp(stack.back(), low, high);
                break;
            }
            default: {
                const std::int64_t right = stack.back();
                stack.pop_back();
                std::int64_t& left = stack.back();
                // Checked arithmetic. An expression that overflows is refused. It never causes
                // undefined behavior or a silently wrapped setting.
                std::int64_t result{};
                const bool   overflowed = op.kind == Expression::OpKind::add ? __builtin_add_overflow(left, right, &result) : op.kind == Expression::OpKind::subtract ? __builtin_sub_overflow(left, right, &result) : __builtin_mul_overflow(left, right, &result);
                if (overflowed) {
                    return std::unexpected(gr::Error(std::format("recipe_expression_conversion: '{}' overflows", detail::printableEcho(expression.source))));
                }
                left = result;
                break;
            }
            }
        }
        if (stack.empty()) {
            return std::unexpected(gr::Error(std::format("recipe_expression_parse: '{}' is empty", detail::printableEcho(expression.source))));
        }
        return pmt::Value(stack.back());
    }

    std::vector<double> stack;
    for (const auto& op : expression.ops) {
        switch (op.kind) {
        case Expression::OpKind::pushNumber: stack.push_back(op.number); break;
        case Expression::OpKind::pushInteger: stack.push_back(static_cast<double>(op.integer)); break;
        case Expression::OpKind::pushParameter: {
            if (op.parameterIndex >= parameterValues.size()) {
                return operandError(op.parameterIndex);
            }
            const auto value = detail::doubleOf(parameterValues[op.parameterIndex]);
            if (!value.has_value()) {
                return operandError(op.parameterIndex);
            }
            stack.push_back(*value);
            break;
        }
        case Expression::OpKind::negate: stack.back() = -stack.back(); break;
        case Expression::OpKind::clamp: {
            const double high = stack.back();
            stack.pop_back();
            const double low = stack.back();
            stack.pop_back();
            // The test is spelled with `<=`. A bound that arrived as a NaN fails it and is refused
            // here. The value does not pass through unclamped.
            if (!(low <= high)) {
                return std::unexpected(gr::Error(std::format("recipe_expression_domain: clamp bounds {} and {} are not ordered low, high in '{}'", low, high, detail::printableEcho(expression.source))));
            }
            stack.back() = std::clamp(stack.back(), low, high);
            break;
        }
        default: {
            const double right = stack.back();
            stack.pop_back();
            double& left = stack.back();
            left         = op.kind == Expression::OpKind::add ? left + right : op.kind == Expression::OpKind::subtract ? left - right : op.kind == Expression::OpKind::multiply ? left * right : left / right;
            break;
        }
        }
    }
    if (stack.empty()) {
        return std::unexpected(gr::Error(std::format("recipe_expression_parse: '{}' is empty", detail::printableEcho(expression.source))));
    }
    if (!std::isfinite(stack.back())) {
        return std::unexpected(gr::Error(std::format("recipe_expression_conversion: '{}' evaluated to a non-finite value", detail::printableEcho(expression.source))));
    }
    return pmt::Value(stack.back());
}

/// Validates a recipe's declaration list at definition load. It refuses duplicates, reserved
/// spellings (framework settings and the expression constants), unknown type words and mistyped
/// defaults.
namespace detail {

/// Whether a vector-typed declaration's default holds the vector its type word names. The element word is the
/// type word without its `[]`. A sequence reaches a recipe as a Tensor of that element type. A vector of strings
/// reaches it as a Tensor of Values.
[[nodiscard]] inline bool vectorDefaultAgrees(std::string_view type, const pmt::Value& value) noexcept {
    const std::string_view element = type.substr(0UZ, type.size() - 2UZ);
    if (element == "float64") {
        return value.get_if<Tensor<double>>() != nullptr;
    }
    if (element == "float32") {
        return value.get_if<Tensor<float>>() != nullptr;
    }
    if (element == "int64") {
        return value.get_if<Tensor<std::int64_t>>() != nullptr;
    }
    if (element == "int32") {
        return value.get_if<Tensor<std::int32_t>>() != nullptr;
    }
    if (element == "int16") {
        return value.get_if<Tensor<std::int16_t>>() != nullptr;
    }
    if (element == "int8") {
        return value.get_if<Tensor<std::int8_t>>() != nullptr;
    }
    if (element == "uint64") {
        return value.get_if<Tensor<std::uint64_t>>() != nullptr;
    }
    if (element == "uint32") {
        return value.get_if<Tensor<std::uint32_t>>() != nullptr;
    }
    if (element == "uint16") {
        return value.get_if<Tensor<std::uint16_t>>() != nullptr;
    }
    if (element == "uint8") {
        return value.get_if<Tensor<std::uint8_t>>() != nullptr;
    }
    if (element == "bool") {
        return value.get_if<Tensor<bool>>() != nullptr;
    }
    return element == "string" && value.get_if<Tensor<pmt::Value>>() != nullptr;
}

} // namespace detail

[[nodiscard]] inline std::expected<void, gr::Error> validateDeclarations(std::span<const ParameterDeclaration> declarations) {
    // A parameter may not shadow a word the grammar already spells, such as the constants and the one function.
    static constexpr std::array<std::string_view, 9> kReservedNames{"name", "compute_domain", "disconnect_on_done", "enabled", "ui_constraints", "meta_information", "pi", "tau_circle", "clamp"};
    for (std::size_t index = 0; index < declarations.size(); ++index) {
        const auto& declaration = declarations[index];
        if (!detail::knownTypeWord(declaration.type)) {
            return std::unexpected(gr::Error(std::format("recipe_default_type: parameter '{}' declares unknown type '{}'", declaration.name, declaration.type)));
        }
        if (std::ranges::find(kReservedNames, declaration.name) != kReservedNames.end()) {
            return std::unexpected(gr::Error(std::format("recipe_reserved_parameter: '{}'", declaration.name)));
        }
        for (std::size_t other = index + 1; other < declarations.size(); ++other) {
            if (declarations[other].name == declaration.name) {
                return std::unexpected(gr::Error(std::format("recipe_duplicate_parameter: '{}'", declaration.name)));
            }
        }
        if (declaration.defaultValue.has_value()) {
            const bool numericAgrees = detail::numericTypeWord(declaration.type) && (detail::integralTypeWord(declaration.type) ? detail::integerOf(*declaration.defaultValue).has_value() : detail::doubleOf(*declaration.defaultValue).has_value());
            const bool boolAgrees    = declaration.type == "bool" && declaration.defaultValue->get_if<bool>() != nullptr;
            const bool stringAgrees  = declaration.type == "string" && declaration.defaultValue->get_if<std::pmr::string>() != nullptr;
            const bool vectorAgrees  = detail::vectorTypeWord(declaration.type) && detail::vectorDefaultAgrees(declaration.type, *declaration.defaultValue);
            if (!numericAgrees && !boolAgrees && !stringAgrees && !vectorAgrees) {
                return std::unexpected(gr::Error(std::format("recipe_default_type: parameter '{}' declares type {} but its default holds another", declaration.name, declaration.type)));
            }
        }
    }
    return {};
}

namespace detail {

template<typename T>
[[nodiscard]] std::expected<pmt::Value, std::string> heldAs(bool sequence, std::string_view name, const pmt::Value& value) {
    if (sequence) {
        auto converted = gr::settings::convertParameter<std::vector<T>>(name, value);
        if (!converted.has_value()) {
            return std::unexpected(converted.error());
        }
        return pmt::Value(gr::detail::collectionToTensor(*converted));
    }
    auto converted = gr::settings::convertParameter<T>(name, value);
    if (!converted.has_value()) {
        return std::unexpected(converted.error());
    }
    return pmt::Value(std::move(*converted));
}

} // namespace detail

/// A parameter's value in the parameter's declared type. The value converts as a block setting of that type converts
/// one, with the same refusals, and a refusal names the parameter.
[[nodiscard]] inline std::expected<pmt::Value, gr::Error> heldValue(const ParameterDeclaration& declaration, const pmt::Value& value) {
    const std::string_view type     = declaration.type;
    const bool             sequence = detail::vectorTypeWord(type);
    const std::string_view element  = sequence ? type.substr(0UZ, type.size() - 2UZ) : type;
    const std::string_view name     = declaration.name;

    std::expected<pmt::Value, std::string> held = std::unexpected(std::format("the type word {} names no settings type", type));
    if (element == "bool") {
        held = detail::heldAs<bool>(sequence, name, value);
    } else if (element == "int8") {
        held = detail::heldAs<std::int8_t>(sequence, name, value);
    } else if (element == "int16") {
        held = detail::heldAs<std::int16_t>(sequence, name, value);
    } else if (element == "int32") {
        held = detail::heldAs<std::int32_t>(sequence, name, value);
    } else if (element == "int64") {
        held = detail::heldAs<std::int64_t>(sequence, name, value);
    } else if (element == "uint8") {
        held = detail::heldAs<std::uint8_t>(sequence, name, value);
    } else if (element == "uint16") {
        held = detail::heldAs<std::uint16_t>(sequence, name, value);
    } else if (element == "uint32") {
        held = detail::heldAs<std::uint32_t>(sequence, name, value);
    } else if (element == "uint64") {
        held = detail::heldAs<std::uint64_t>(sequence, name, value);
    } else if (element == "float32") {
        held = detail::heldAs<float>(sequence, name, value);
    } else if (element == "float64") {
        held = detail::heldAs<double>(sequence, name, value);
    } else if (element == "string") {
        held = detail::heldAs<std::string>(sequence, name, value);
    }
    if (!held.has_value()) {
        return std::unexpected(gr::Error(std::format("recipe_expression_conversion: parameter '{}' of type {} does not take the value: {}", detail::printableEcho(name), detail::printableEcho(type), detail::printableEcho(held.error()))));
    }
    return std::move(*held);
}

/// Overlays supplied values on declared defaults, each held in its parameter's declared type. An unknown name is
/// refused by name. Missing required parameters are refused together, all named in one error.
[[nodiscard]] inline std::expected<std::vector<pmt::Value>, gr::Error> resolveParameters(std::span<const ParameterDeclaration> declarations, const property_map& supplied) {
    for (const auto& [name, value] : supplied) {
        const bool declared = std::ranges::any_of(declarations, [&](const ParameterDeclaration& declaration) { return std::string_view(declaration.name) == std::string_view(name); });
        if (!declared) {
            return std::unexpected(gr::Error(std::format("recipe_unknown_parameter: '{}'", detail::printableEcho(name))));
        }
    }
    std::vector<pmt::Value> values;
    values.reserve(declarations.size());
    std::string missing;
    for (const auto& declaration : declarations) {
        const pmt::Value* suppliedValue = nullptr;
        for (const auto& [name, value] : supplied) {
            if (std::string_view(name) == std::string_view(declaration.name)) {
                suppliedValue = &value;
                break;
            }
        }
        if (suppliedValue != nullptr || declaration.defaultValue.has_value()) {
            auto held = heldValue(declaration, suppliedValue != nullptr ? *suppliedValue : *declaration.defaultValue);
            if (!held.has_value()) {
                return std::unexpected(held.error());
            }
            values.push_back(std::move(*held));
        } else {
            missing += missing.empty() ? declaration.name : (", " + declaration.name);
            values.emplace_back();
        }
    }
    if (!missing.empty()) {
        return std::unexpected(gr::Error(std::format("recipe_parameter_required: [{}]", missing)));
    }
    return values;
}

/// One element of a sequence-valued setting, either an expression to evaluate or a value
/// spelled literally beside the expressions in the same sequence.
struct SequenceElement {
    std::optional<Expression> expression;
    pmt::Value                literal;
};

/// One derived interior setting. It names the block by a name path from the composite's interior
/// downward through nested subgraphs. It names the setting and holds the expression that derives it.
struct Binding {
    std::vector<std::string> namePath;
    std::string              settingKey;
    Expression               expression;
    /// Set instead of `expression` when the setting takes a parameter's value unchanged. A string or a vector
    /// carries no arithmetic. A recipe can only pass one through.
    std::optional<std::size_t> substituted;
    /// Set instead of `expression` when the setting is a sequence with derived elements. A setting is staged
    /// whole. The binding therefore carries the whole sequence and rebuilds it on every change. With one binding
    /// per key, the transaction and the refusal stay exactly as they are for a scalar.
    std::vector<SequenceElement> sequence;
};

/// The value a binding produces for one set of parameter values.
[[nodiscard]] inline std::expected<pmt::Value, gr::Error> bindingValue(const Binding& binding, std::span<const pmt::Value> values) {
    if (binding.substituted.has_value()) {
        if (*binding.substituted >= values.size()) {
            return std::unexpected(gr::Error("recipe_parameter_index: a substituted parameter is out of range"));
        }
        return values[*binding.substituted];
    }
    if (!binding.sequence.empty()) {
        std::vector<pmt::Value> elements;
        elements.reserve(binding.sequence.size());
        for (const SequenceElement& element : binding.sequence) {
            if (!element.expression.has_value()) {
                elements.push_back(element.literal);
                continue;
            }
            auto evaluated = evaluate(*element.expression, values);
            if (!evaluated.has_value()) {
                return std::unexpected(evaluated.error());
            }
            elements.push_back(std::move(*evaluated));
        }
        return pmt::Value(Tensor<pmt::Value>(elements.begin(), elements.end()));
    }
    return evaluate(binding.expression, values);
}

/// True when two derived values are equal. `pmt::Value` compares a Tensor by identity. A
/// sequence is rebuilt whole on every evaluation, so it is compared element by element here.
/// Every other alternative uses the value's own equality. Where equality cannot decide, the
/// answer is "differs". The engine may then restage a key it did not need to. It never skips a
/// key it had to stage.
[[nodiscard]] inline bool derivedValuesAgree(const pmt::Value& left, const pmt::Value& right) {
    const auto* leftElements  = left.get_if<Tensor<pmt::Value>>();
    const auto* rightElements = right.get_if<Tensor<pmt::Value>>();
    if (leftElements != nullptr && rightElements != nullptr) {
        return std::ranges::equal(*leftElements, *rightElements);
    }
    return left == right;
}

/// The state of a live composite. It holds the declarations, the bindings, the current parameter
/// values and what was last staged for each binding. A staged change to an exported parameter
/// re-evaluates against a trial copy of the values. It commits only when every binding evaluated.
/// A refusal rejects the whole change, and the running values stay.
struct AttachedBindings {
    std::vector<ParameterDeclaration> declarations;
    std::vector<Binding>              bindings;
    std::vector<pmt::Value>           values;
    /// aligned with `bindings`, the value the engine last staged for each. A change stages only
    /// the keys whose values changed. Empty before the first application, which therefore stages
    /// every bound key.
    std::vector<std::optional<pmt::Value>> lastStaged;
};

} // namespace gr::recipe

#endif // GNURADIO_RECIPEPARAMETERS_HPP
