#pragma once
// Minimal JSON writer — avoids external dependencies.
// Produces compact or pretty-printed UTF-8 JSON.

#include <string>
#include <sstream>
#include <vector>
#include <cstdint>
#include <cmath>

namespace ego_offline::json {

// ── Primitive formatters ─────────────────────────────────────────────────────

inline std::string str(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    out += '"';
    for (char c : s) {
        if (c == '"')       out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else                out += c;
    }
    out += '"';
    return out;
}

inline std::string num(int64_t  v) { return std::to_string(v); }
inline std::string num(uint64_t v) { return std::to_string(v); }
inline std::string num(int32_t  v) { return std::to_string(v); }
inline std::string num(uint32_t v) { return std::to_string(v); }

inline std::string num(double v, int prec = 9) {
    if (std::isnan(v) || std::isinf(v)) return "null";
    std::ostringstream oss;
    oss.precision(prec);
    oss << std::defaultfloat << v;
    std::string s = oss.str();
    // ensure there's a decimal point for doubles
    if (s.find('.') == std::string::npos && s.find('e') == std::string::npos)
        s += ".0";
    return s;
}

inline std::string boolean(bool v) { return v ? "true" : "false"; }
inline std::string null_val()       { return "null"; }

// ── Builder ──────────────────────────────────────────────────────────────────

class ObjectBuilder {
public:
    explicit ObjectBuilder(int indent = 2, int depth = 0)
        : indent_(indent), depth_(depth) {}

    template<typename V>
    ObjectBuilder& field(const std::string& key, V&& val) {
        append_key(key);
        buf_ += std::forward<V>(val);
        return *this;
    }

    // Nested object via lambda: build.object("key", [](ObjectBuilder& o){ o.field(...); });
    template<typename Fn>
    ObjectBuilder& object(const std::string& key, Fn&& fn) {
        append_key(key);
        ObjectBuilder child(indent_, depth_ + 1);
        fn(child);
        buf_ += child.build();
        return *this;
    }

    // Array of strings / numbers (pre-formatted)
    ObjectBuilder& array(const std::string& key, const std::vector<std::string>& items) {
        append_key(key);
        if (items.empty()) { buf_ += "[]"; return *this; }
        buf_ += "[\n";
        std::string pad(static_cast<size_t>((depth_ + 2) * indent_), ' ');
        std::string close_pad(static_cast<size_t>((depth_ + 1) * indent_), ' ');
        for (size_t i = 0; i < items.size(); ++i) {
            buf_ += pad + items[i];
            if (i + 1 < items.size()) buf_ += ',';
            buf_ += '\n';
        }
        buf_ += close_pad + ']';
        return *this;
    }

    // Raw array of pre-built objects
    ObjectBuilder& raw_array(const std::string& key, const std::vector<std::string>& objs) {
        append_key(key);
        if (objs.empty()) { buf_ += "[]"; return *this; }
        buf_ += "[\n";
        std::string close_pad(static_cast<size_t>((depth_ + 1) * indent_), ' ');
        for (size_t i = 0; i < objs.size(); ++i) {
            // indent each line of obj
            std::string indented;
            std::istringstream ss(objs[i]);
            std::string line;
            std::string obj_pad(static_cast<size_t>((depth_ + 2) * indent_), ' ');
            bool first_line = true;
            while (std::getline(ss, line)) {
                if (!first_line) indented += '\n';
                indented += obj_pad + line;
                first_line = false;
            }
            buf_ += indented;
            if (i + 1 < objs.size()) buf_ += ',';
            buf_ += '\n';
        }
        buf_ += close_pad + ']';
        return *this;
    }

    std::string build() const {
        std::string pad(static_cast<size_t>(depth_ * indent_), ' ');
        if (empty_) return "{}";
        std::string result = "{\n" + buf_;
        // Remove trailing comma+newline from last field, replace with newline
        // buf_ ends with ",\n" or just "\n"
        if (result.size() >= 2 && result[result.size()-2] == ',')
            result[result.size()-2] = '\n', result.resize(result.size()-1);
        result += pad + "}";
        return result;
    }

private:
    void append_key(const std::string& key) {
        empty_ = false;
        std::string pad(static_cast<size_t>((depth_ + 1) * indent_), ' ');
        buf_ += pad + json::str(key) + ": ";
    }

    int         indent_;
    int         depth_;
    std::string buf_;
    bool        empty_ = true;

    // Need std::istringstream for raw_array
    static void dummy_use() { std::istringstream ss(""); (void)ss; }
};

// helper: build a top-level object
template<typename Fn>
std::string object(Fn&& fn, int indent = 2) {
    ObjectBuilder b(indent, 0);
    fn(b);
    return b.build();
}

} // namespace ego_offline::json
