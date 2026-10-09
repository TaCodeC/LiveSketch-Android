#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// JSON para los archivos de proyecto: escribirlo con sangría (para que se pueda leer y
// comparar) y leerlo a un árbol de valores. Los números se escriben con los dígitos justos
// para que al leerlos salga exactamente el mismo float o double, y siempre con punto.
namespace json {

class Value {
public:
    enum class Type : uint8_t { Null, Bool, Number, String, Array, Object };

    Type type() const { return m_type; }
    bool isNull() const { return m_type == Type::Null; }
    bool isBool() const { return m_type == Type::Bool; }
    bool isNumber() const { return m_type == Type::Number; }
    bool isString() const { return m_type == Type::String; }
    bool isArray() const { return m_type == Type::Array; }
    bool isObject() const { return m_type == Type::Object; }

    // El valor, o `fallback` si es de otro tipo.
    bool boolean(bool fallback = false) const { return isBool() ? m_bool : fallback; }
    double number(double fallback = 0.0) const { return isNumber() ? m_number : fallback; }
    // Vacío si no es un texto.
    const std::string& string() const { return m_string; }

    // Elementos de una lista (vacío si no lo es).
    const std::vector<Value>& items() const { return m_items; }
    // Miembros de un objeto, en el orden del texto (vacío si no lo es).
    const std::vector<std::pair<std::string, Value>>& members() const { return m_members; }
    // Miembro de un objeto (el primero con ese nombre), o un valor nulo.
    const Value& operator[](std::string_view key) const;
    // Elemento de una lista, o un valor nulo.
    const Value& operator[](size_t index) const;

private:
    friend class Parser;

    Type m_type = Type::Null;
    bool m_bool = false;
    double m_number = 0.0;
    std::string m_string;
    std::vector<Value> m_items;
    std::vector<std::pair<std::string, Value>> m_members;
};

// Límites de lo que se lee.
inline constexpr int kMaxDepth = 64;
inline constexpr size_t kMaxText = size_t{4} << 20;

// Lee `text` (UTF-8). Si no es JSON válido o pasa de los límites, devuelve false y deja en
// `error` qué falla y dónde.
bool parse(std::string_view text, Value& out, std::string* error = nullptr);

// Escribe JSON con sangría de dos espacios. Las listas que se abren con `inline` (de
// números, como un color) van en una línea.
class Writer {
public:
    Writer& beginObject();
    Writer& endObject();
    Writer& beginArray(bool inLine = false);
    Writer& endArray();
    // Nombre del siguiente miembro de un objeto.
    Writer& key(std::string_view name);

    Writer& value(std::string_view text);
    Writer& value(const char* text) { return value(std::string_view(text)); }
    Writer& value(const std::string& text) { return value(std::string_view(text)); }
    Writer& value(bool flag);
    Writer& value(int number) { return value(static_cast<int64_t>(number)); }
    Writer& value(int64_t number);
    Writer& value(uint64_t number);
    Writer& value(uint32_t number) { return value(static_cast<uint64_t>(number)); }
    // Con los dígitos justos para volver al mismo float (o double). NaN e infinito no existen
    // en JSON: van como null.
    Writer& value(float number);
    Writer& value(double number);
    Writer& null();

    const std::string& text() const { return m_text; }

private:
    struct Level {
        bool object = false;
        bool inLine = false;
        int count = 0;
    };
    void beforeValue();
    void newline();

    std::string m_text;
    std::vector<Level> m_levels;
    bool m_afterKey = false;
};

// Texto JSON de una cadena, con comillas (escapa comillas, barras y caracteres de control).
std::string quote(std::string_view text);

// Un número con los dígitos justos para volver al mismo float (o double), con punto decimal.
// Para NaN o infinito, "null".
std::string number(float value);
std::string number(double value);

} // namespace json
