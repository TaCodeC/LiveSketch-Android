#include "IO/Json.h"

#include <algorithm>
#include <clocale>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace json {
namespace {

const Value& nullValue() {
    static const Value value;
    return value;
}

// snprintf y strtod usan la coma decimal si alguien cambia el locale; JSON lleva punto.
char decimalPoint() {
    const lconv* conv = std::localeconv();
    return conv && conv->decimal_point && conv->decimal_point[0] ? conv->decimal_point[0] : '.';
}

void appendUtf8(std::string& out, uint32_t code) {
    if (code < 0x80) {
        out += static_cast<char>(code);
    } else if (code < 0x800) {
        out += static_cast<char>(0xC0 | (code >> 6));
        out += static_cast<char>(0x80 | (code & 0x3F));
    } else if (code < 0x10000) {
        out += static_cast<char>(0xE0 | (code >> 12));
        out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (code & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (code >> 18));
        out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (code & 0x3F));
    }
}

// El número con `precision` cifras significativas, con punto decimal.
std::string formatNumber(double value, int precision) {
    char text[40];
    std::snprintf(text, sizeof(text), "%.*g", precision, value);
    const char point = decimalPoint();
    if (point != '.') {
        for (char* c = text; *c; ++c) {
            if (*c == point) {
                *c = '.';
            }
        }
    }
    return text;
}

// Un número entero sin exponente ("300" y no "3e+02"). False si no es entero o es enorme.
bool integerText(double value, std::string& text) {
    if (value != std::floor(value) || std::fabs(value) >= 1e15) {
        return false;
    }
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.0f", value);
    text = buffer;
    return true;
}

double readNumber(const std::string& text) {
    std::string local = text;
    const char point = decimalPoint();
    if (point != '.') {
        for (char& c : local) {
            if (c == '.') {
                c = point;
            }
        }
    }
    return std::strtod(local.c_str(), nullptr);
}

} // namespace

const Value& Value::operator[](std::string_view key) const {
    for (const auto& [name, value] : m_members) {
        if (name == key) {
            return value;
        }
    }
    return nullValue();
}

const Value& Value::operator[](size_t index) const {
    return index < m_items.size() ? m_items[index] : nullValue();
}

Value Value::text(std::string text) {
    Value value;
    value.m_type = Type::String;
    value.m_string = std::move(text);
    return value;
}

Value* Value::member(std::string_view key) {
    if (!isObject()) {
        return nullptr;
    }
    for (auto& [name, value] : m_members) {
        if (name == key) {
            return &value;
        }
    }
    m_members.emplace_back(std::string(key), Value());
    return &m_members.back().second;
}

// -----------------------------------------------------------------------------
// Lectura
// -----------------------------------------------------------------------------

class Parser {
public:
    explicit Parser(std::string_view text) : m_text(text) {}

    bool run(Value& out, std::string* error) {
        if (m_text.size() > kMaxText) {
            return report("el texto es demasiado largo", error);
        }
        // Una marca BOM al principio no cuenta.
        if (m_text.substr(0, 3) == "\xEF\xBB\xBF") {
            m_at = 3;
        }
        skipSpace();
        if (!parseValue(out, 0)) {
            return report(m_problem, error);
        }
        skipSpace();
        if (m_at != m_text.size()) {
            return report("sobra texto después del final", error);
        }
        return true;
    }

private:
    bool report(const std::string& problem, std::string* error) const {
        if (error) {
            int line = 1;
            int column = 1;
            for (size_t i = 0; i < m_at && i < m_text.size(); ++i) {
                if (m_text[i] == '\n') {
                    ++line;
                    column = 1;
                } else {
                    ++column;
                }
            }
            *error = "línea " + std::to_string(line) + ", columna " + std::to_string(column) + ": " + problem;
        }
        return false;
    }

    bool fail(const char* problem) {
        m_problem = problem;
        return false;
    }

    void skipSpace() {
        while (m_at < m_text.size() &&
               (m_text[m_at] == ' ' || m_text[m_at] == '\t' || m_text[m_at] == '\n' || m_text[m_at] == '\r')) {
            ++m_at;
        }
    }

    bool literal(std::string_view word) {
        if (m_text.substr(m_at, word.size()) != word) {
            return fail("valor no válido");
        }
        m_at += word.size();
        return true;
    }

    bool parseValue(Value& out, int depth) {
        if (depth > kMaxDepth) {
            return fail("demasiados niveles anidados");
        }
        if (m_at >= m_text.size()) {
            return fail("el texto se acaba antes de tiempo");
        }
        const char c = m_text[m_at];
        switch (c) {
        case '{':
            return parseObject(out, depth);
        case '[':
            return parseArray(out, depth);
        case '"':
            out.m_type = Value::Type::String;
            return parseString(out.m_string);
        case 't':
            out.m_type = Value::Type::Bool;
            out.m_bool = true;
            return literal("true");
        case 'f':
            out.m_type = Value::Type::Bool;
            out.m_bool = false;
            return literal("false");
        case 'n':
            out.m_type = Value::Type::Null;
            return literal("null");
        default:
            if (c == '-' || (c >= '0' && c <= '9')) {
                return parseNumber(out);
            }
            return fail("valor no válido");
        }
    }

    bool parseObject(Value& out, int depth) {
        out.m_type = Value::Type::Object;
        ++m_at;   // {
        skipSpace();
        if (m_at < m_text.size() && m_text[m_at] == '}') {
            ++m_at;
            return true;
        }
        while (true) {
            skipSpace();
            if (m_at >= m_text.size() || m_text[m_at] != '"') {
                return fail("falta el nombre de un miembro");
            }
            std::string name;
            if (!parseString(name)) {
                return false;
            }
            skipSpace();
            if (m_at >= m_text.size() || m_text[m_at] != ':') {
                return fail("faltan los dos puntos");
            }
            ++m_at;
            skipSpace();
            out.m_members.emplace_back(std::move(name), Value());
            if (!parseValue(out.m_members.back().second, depth + 1)) {
                return false;
            }
            skipSpace();
            if (m_at >= m_text.size()) {
                return fail("el texto se acaba antes de tiempo");
            }
            if (m_text[m_at] == ',') {
                ++m_at;
                continue;
            }
            if (m_text[m_at] == '}') {
                ++m_at;
                return true;
            }
            return fail("falta una coma o la llave de cierre");
        }
    }

    bool parseArray(Value& out, int depth) {
        out.m_type = Value::Type::Array;
        ++m_at;   // [
        skipSpace();
        if (m_at < m_text.size() && m_text[m_at] == ']') {
            ++m_at;
            return true;
        }
        while (true) {
            skipSpace();
            out.m_items.emplace_back();
            if (!parseValue(out.m_items.back(), depth + 1)) {
                return false;
            }
            skipSpace();
            if (m_at >= m_text.size()) {
                return fail("el texto se acaba antes de tiempo");
            }
            if (m_text[m_at] == ',') {
                ++m_at;
                continue;
            }
            if (m_text[m_at] == ']') {
                ++m_at;
                return true;
            }
            return fail("falta una coma o el corchete de cierre");
        }
    }

    bool hex4(uint32_t& code) {
        if (m_text.size() - m_at < 4) {
            return fail("escape \\u incompleto");
        }
        code = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = m_text[m_at++];
            code <<= 4;
            if (c >= '0' && c <= '9') {
                code |= static_cast<uint32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                code |= static_cast<uint32_t>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                code |= static_cast<uint32_t>(c - 'A' + 10);
            } else {
                return fail("escape \\u no válido");
            }
        }
        return true;
    }

    bool parseString(std::string& out) {
        ++m_at;   // "
        while (true) {
            if (m_at >= m_text.size()) {
                return fail("falta cerrar un texto");
            }
            const char c = m_text[m_at++];
            if (c == '"') {
                return true;
            }
            if (static_cast<unsigned char>(c) < 0x20) {
                return fail("carácter de control en un texto");
            }
            if (c != '\\') {
                out += c;
                continue;
            }
            if (m_at >= m_text.size()) {
                return fail("falta cerrar un texto");
            }
            const char e = m_text[m_at++];
            switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                uint32_t code = 0;
                if (!hex4(code)) {
                    return false;
                }
                if (code >= 0xD800 && code < 0xDC00) {
                    // Primera mitad de un par: si sigue la segunda, forman un carácter.
                    uint32_t low = 0;
                    if (m_text.substr(m_at, 2) == "\\u") {
                        const size_t back = m_at;
                        m_at += 2;
                        if (!hex4(low)) {
                            return false;
                        }
                        if (low >= 0xDC00 && low < 0xE000) {
                            code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                        } else {
                            m_at = back;
                            code = 0xFFFD;
                        }
                    } else {
                        code = 0xFFFD;
                    }
                } else if (code >= 0xDC00 && code < 0xE000) {
                    code = 0xFFFD;   // segunda mitad suelta
                }
                appendUtf8(out, code);
                break;
            }
            default:
                return fail("escape no válido en un texto");
            }
        }
    }

    bool parseNumber(Value& out) {
        const size_t start = m_at;
        auto digit = [this] { return m_at < m_text.size() && m_text[m_at] >= '0' && m_text[m_at] <= '9'; };
        if (m_text[m_at] == '-') {
            ++m_at;
        }
        if (!digit()) {
            return fail("número no válido");
        }
        if (m_text[m_at] == '0') {
            ++m_at;
        } else {
            while (digit()) {
                ++m_at;
            }
        }
        if (m_at < m_text.size() && m_text[m_at] == '.') {
            ++m_at;
            if (!digit()) {
                return fail("número no válido");
            }
            while (digit()) {
                ++m_at;
            }
        }
        if (m_at < m_text.size() && (m_text[m_at] == 'e' || m_text[m_at] == 'E')) {
            ++m_at;
            if (m_at < m_text.size() && (m_text[m_at] == '+' || m_text[m_at] == '-')) {
                ++m_at;
            }
            if (!digit()) {
                return fail("número no válido");
            }
            while (digit()) {
                ++m_at;
            }
        }
        if (m_at - start > 64) {
            return fail("número demasiado largo");
        }
        const double value = readNumber(std::string(m_text.substr(start, m_at - start)));
        if (!std::isfinite(value)) {
            return fail("número fuera de rango");
        }
        out.m_type = Value::Type::Number;
        out.m_number = value;
        return true;
    }

    std::string_view m_text;
    size_t m_at = 0;
    std::string m_problem;
};

bool parse(std::string_view text, Value& out, std::string* error) {
    out = Value();
    Parser parser(text);
    if (!parser.run(out, error)) {
        out = Value();
        return false;
    }
    return true;
}

// -----------------------------------------------------------------------------
// Escritura
// -----------------------------------------------------------------------------

std::string quote(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 2);
    out += '"';
    for (const char c : text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) {
                char escape[8];
                std::snprintf(escape, sizeof(escape), "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
                out += escape;
            } else {
                out += c;
            }
        }
    }
    out += '"';
    return out;
}

void Writer::newline() {
    m_text += '\n';
    m_text.append(m_levels.size() * 2, ' ');
}

void Writer::beforeValue() {
    if (m_afterKey) {
        m_afterKey = false;
        return;
    }
    if (m_levels.empty()) {
        return;
    }
    Level& level = m_levels.back();
    if (level.count > 0) {
        m_text += ',';
    }
    if (level.inLine) {
        if (level.count > 0) {
            m_text += ' ';
        }
    } else {
        newline();
    }
    ++level.count;
}

Writer& Writer::beginObject() {
    beforeValue();
    m_text += '{';
    m_levels.push_back({true, false, 0});
    return *this;
}

Writer& Writer::endObject() {
    const Level level = m_levels.back();
    m_levels.pop_back();
    if (level.count > 0) {
        newline();
    }
    m_text += '}';
    if (m_levels.empty()) {
        m_text += '\n';
    }
    return *this;
}

Writer& Writer::beginArray(bool inLine) {
    // Dentro de una lista en una línea, todo va en la línea.
    inLine = inLine || (!m_levels.empty() && m_levels.back().inLine);
    beforeValue();
    m_text += '[';
    m_levels.push_back({false, inLine, 0});
    return *this;
}

Writer& Writer::endArray() {
    const Level level = m_levels.back();
    m_levels.pop_back();
    if (level.count > 0 && !level.inLine) {
        newline();
    }
    m_text += ']';
    if (m_levels.empty()) {
        m_text += '\n';
    }
    return *this;
}

Writer& Writer::key(std::string_view name) {
    Level& level = m_levels.back();
    if (level.count > 0) {
        m_text += ',';
    }
    newline();
    ++level.count;
    m_text += quote(name);
    m_text += ": ";
    m_afterKey = true;
    return *this;
}

Writer& Writer::value(std::string_view text) {
    beforeValue();
    m_text += quote(text);
    return *this;
}

Writer& Writer::value(bool flag) {
    beforeValue();
    m_text += flag ? "true" : "false";
    return *this;
}

Writer& Writer::value(int64_t number) {
    beforeValue();
    m_text += std::to_string(number);
    return *this;
}

Writer& Writer::value(uint64_t number) {
    beforeValue();
    m_text += std::to_string(number);
    return *this;
}

std::string number(float value) {
    if (!std::isfinite(value)) {
        return "null";
    }
    std::string text;
    if (integerText(value, text)) {
        return text;
    }
    for (int precision = 1; precision <= 9; ++precision) {
        text = formatNumber(value, precision);
        if (static_cast<float>(readNumber(text)) == value) {
            break;
        }
    }
    return text;
}

std::string number(double value) {
    if (!std::isfinite(value)) {
        return "null";
    }
    std::string text;
    if (integerText(value, text)) {
        return text;
    }
    for (int precision = 1; precision <= 17; ++precision) {
        text = formatNumber(value, precision);
        if (readNumber(text) == value) {
            break;
        }
    }
    return text;
}

Writer& Writer::value(float value) {
    beforeValue();
    m_text += number(value);
    return *this;
}

Writer& Writer::value(double value) {
    beforeValue();
    m_text += number(value);
    return *this;
}

Writer& Writer::null() {
    beforeValue();
    m_text += "null";
    return *this;
}

namespace {

void writeValue(Writer& writer, const Value& value) {
    switch (value.type()) {
    case Value::Type::Null:
        writer.null();
        break;
    case Value::Type::Bool:
        writer.value(value.boolean());
        break;
    case Value::Type::Number:
        writer.value(value.number());
        break;
    case Value::Type::String:
        writer.value(value.string());
        break;
    case Value::Type::Array: {
        const std::vector<Value>& items = value.items();
        const bool numbers = !items.empty() && std::all_of(items.begin(), items.end(),
                                                           [](const Value& item) { return item.isNumber(); });
        writer.beginArray(numbers);
        for (const Value& item : items) {
            writeValue(writer, item);
        }
        writer.endArray();
        break;
    }
    case Value::Type::Object:
        writer.beginObject();
        for (const auto& [name, member] : value.members()) {
            writer.key(name);
            writeValue(writer, member);
        }
        writer.endObject();
        break;
    }
}

} // namespace

std::string write(const Value& value) {
    Writer writer;
    writeValue(writer, value);
    return writer.text();
}

} // namespace json
