#pragma once
// Minimal JSON reader -- no external dependencies
// Supports: objects, arrays, strings, integers, doubles, booleans, null
// Used to load manifest and metadata files.

#include <string>
#include <vector>
#include <map>
#include <stdexcept>
#include <cstdint>
#include <sstream>
#include <fstream>
#include <cstdlib>

namespace ego_offline {

struct JsonValue {
    enum class Type { Null, Bool, Int, Double, String, Array, Object };

    Type type = Type::Null;
    bool        b   = false;
    int64_t     i   = 0;
    double      d   = 0.0;
    std::string s;
    std::vector<JsonValue>              arr;
    std::map<std::string, JsonValue>    obj;

    bool is_null()   const { return type == Type::Null; }
    bool is_bool()   const { return type == Type::Bool; }
    bool is_int()    const { return type == Type::Int; }
    bool is_double() const { return type == Type::Double || type == Type::Int; }
    bool is_string() const { return type == Type::String; }
    bool is_array()  const { return type == Type::Array; }
    bool is_object() const { return type == Type::Object; }

    bool        get_bool(bool def = false)               const { return is_bool() ? b : def; }
    int64_t     get_int(int64_t def = 0)                 const { return is_int() ? i : def; }
    double      get_double(double def = 0.0)             const {
        if (is_int()) return static_cast<double>(i);
        return is_double() ? d : def;
    }
    std::string get_string(const std::string& def = {})  const { return is_string() ? s : def; }

    bool contains(const std::string& key) const {
        return is_object() && obj.count(key) > 0;
    }
    const JsonValue& operator[](const std::string& key) const {
        static JsonValue null_val;
        auto it = obj.find(key);
        return it != obj.end() ? it->second : null_val;
    }
    const JsonValue& operator[](size_t idx) const {
        static JsonValue null_val;
        return (is_array() && idx < arr.size()) ? arr[idx] : null_val;
    }

    template<typename T>
    T value(const std::string& key, T def) const;
};

template<> inline bool JsonValue::value<bool>(const std::string& k, bool def) const
    { return contains(k) ? (*this)[k].get_bool(def) : def; }
template<> inline int64_t JsonValue::value<int64_t>(const std::string& k, int64_t def) const
    { return contains(k) ? (*this)[k].get_int(def) : def; }
template<> inline uint64_t JsonValue::value<uint64_t>(const std::string& k, uint64_t def) const
    { return contains(k) ? static_cast<uint64_t>((*this)[k].get_int(static_cast<int64_t>(def))) : def; }
template<> inline int JsonValue::value<int>(const std::string& k, int def) const
    { return contains(k) ? static_cast<int>((*this)[k].get_int(def)) : def; }
template<> inline double JsonValue::value<double>(const std::string& k, double def) const
    { return contains(k) ? (*this)[k].get_double(def) : def; }
template<> inline std::string JsonValue::value<std::string>(const std::string& k, std::string def) const
    { return contains(k) ? (*this)[k].get_string(def) : def; }

// ---- Parser ----

namespace json_detail {

struct Parser {
    const std::string& src;
    size_t pos = 0;

    char peek() const { return pos < src.size() ? src[pos] : '\0'; }
    char next() { return pos < src.size() ? src[pos++] : '\0'; }

    void skip_ws() {
        while (pos < src.size() && (src[pos]==' '||src[pos]=='\t'||src[pos]=='\n'||src[pos]=='\r'))
            ++pos;
    }

    std::string parse_string() {
        if (next() != '"') throw std::runtime_error("expected '\"'");
        std::string out;
        while (pos < src.size()) {
            char c = next();
            if (c == '"') return out;
            if (c == '\\') {
                char e = next();
                switch(e) {
                    case '"':  out += '"';  break;
                    case '\\': out += '\\'; break;
                    case '/':  out += '/';  break;
                    case 'n':  out += '\n'; break;
                    case 'r':  out += '\r'; break;
                    case 't':  out += '\t'; break;
                    default:   out += e;    break;
                }
            } else {
                out += c;
            }
        }
        throw std::runtime_error("unterminated string");
    }

    JsonValue parse_number() {
        size_t start = pos;
        bool is_float = false;
        if (peek()=='-') ++pos;
        while (pos<src.size() && src[pos]>='0' && src[pos]<='9') ++pos;
        if (pos<src.size() && src[pos]=='.') { is_float = true; ++pos; }
        while (pos<src.size() && src[pos]>='0' && src[pos]<='9') ++pos;
        if (pos<src.size() && (src[pos]=='e'||src[pos]=='E')) {
            is_float = true; ++pos;
            if (pos<src.size() && (src[pos]=='+'||src[pos]=='-')) ++pos;
            while (pos<src.size() && src[pos]>='0' && src[pos]<='9') ++pos;
        }
        std::string tok = src.substr(start, pos - start);
        JsonValue v;
        if (is_float) {
            v.type = JsonValue::Type::Double;
            v.d = std::stod(tok);
        } else {
            v.type = JsonValue::Type::Int;
            v.i = std::stoll(tok);
        }
        return v;
    }

    JsonValue parse_value() {
        skip_ws();
        char c = peek();
        if (c == '"') {
            JsonValue v; v.type = JsonValue::Type::String; v.s = parse_string(); return v;
        }
        if (c == '{') return parse_object();
        if (c == '[') return parse_array();
        if (c=='-' || (c>='0' && c<='9')) return parse_number();
        // true/false/null
        if (src.compare(pos,4,"true")==0)  { pos+=4; JsonValue v; v.type=JsonValue::Type::Bool; v.b=true;  return v; }
        if (src.compare(pos,5,"false")==0) { pos+=5; JsonValue v; v.type=JsonValue::Type::Bool; v.b=false; return v; }
        if (src.compare(pos,4,"null")==0)  { pos+=4; return {}; }
        throw std::runtime_error(std::string("unexpected char '") + c + "' at pos " + std::to_string(pos));
    }

    JsonValue parse_object() {
        if (next() != '{') throw std::runtime_error("expected '{'");
        JsonValue v; v.type = JsonValue::Type::Object;
        skip_ws();
        if (peek() == '}') { ++pos; return v; }
        while (pos < src.size()) {
            skip_ws();
            std::string key = parse_string();
            skip_ws();
            if (next() != ':') throw std::runtime_error("expected ':'");
            skip_ws();
            v.obj[key] = parse_value();
            skip_ws();
            char sep = peek();
            if (sep == ',') { ++pos; continue; }
            if (sep == '}') { ++pos; break; }
            throw std::runtime_error("expected ',' or '}'");
        }
        return v;
    }

    JsonValue parse_array() {
        if (next() != '[') throw std::runtime_error("expected '['");
        JsonValue v; v.type = JsonValue::Type::Array;
        skip_ws();
        if (peek() == ']') { ++pos; return v; }
        while (pos < src.size()) {
            skip_ws();
            v.arr.push_back(parse_value());
            skip_ws();
            char sep = peek();
            if (sep == ',') { ++pos; continue; }
            if (sep == ']') { ++pos; break; }
            throw std::runtime_error("expected ',' or ']'");
        }
        return v;
    }
};

} // namespace json_detail

inline JsonValue json_parse(const std::string& text) {
    json_detail::Parser p{text, 0};
    p.skip_ws();
    return p.parse_value();
}

inline JsonValue json_load(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("cannot open: " + path);
    std::string text((std::istreambuf_iterator<char>(f)),
                      std::istreambuf_iterator<char>());
    return json_parse(text);
}

} // namespace ego_offline
