#include "json.hpp"

#include <stdexcept>

namespace json {

const Value* Value::find(const std::string& key) const {
    for (const auto& kv : obj_)
        if (kv.first == key) return &kv.second;
    return nullptr;
}

Value* Value::find(const std::string& key) {
    for (auto& kv : obj_)
        if (kv.first == key) return &kv.second;
    return nullptr;
}

Value& Value::operator[](const std::string& key) {
    if (type_ != Type::Object) { *this = object(); }
    if (Value* v = find(key)) return *v;
    obj_.emplace_back(key, Value());
    return obj_.back().second;
}

void Value::erase(const std::string& key) {
    for (auto it = obj_.begin(); it != obj_.end(); ++it)
        if (it->first == key) { obj_.erase(it); return; }
}

std::string Value::get_string(const std::string& key) const {
    const Value* v = find(key);
    return v && v->is_string() ? v->str() : std::string();
}

// ------------------------------------------------------------------ writer

static void escape_to(std::string& out, const std::string& s) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        default:
            if (c < 0x20) {
                static const char* hex = "0123456789abcdef";
                out += "\\u00";
                out += hex[c >> 4];
                out += hex[c & 15];
            } else {
                out += (char)c;   // UTF-8 passes through
            }
        }
    }
    out += '"';
}

void Value::dump_to(std::string& out, int indent, int depth) const {
    auto newline = [&](int d) {
        if (indent <= 0) return;
        out += '\n';
        out.append((size_t)(indent * d), ' ');
    };
    switch (type_) {
    case Type::Null: out += "null"; break;
    case Type::Bool: out += b_ ? "true" : "false"; break;
    case Type::Number: out += s_; break;
    case Type::String: escape_to(out, s_); break;
    case Type::Array:
        if (arr_.empty()) { out += "[]"; break; }
        out += '[';
        for (size_t i = 0; i < arr_.size(); ++i) {
            if (i) out += ',';
            newline(depth + 1);
            arr_[i].dump_to(out, indent, depth + 1);
        }
        newline(depth);
        out += ']';
        break;
    case Type::Object:
        if (obj_.empty()) { out += "{}"; break; }
        out += '{';
        for (size_t i = 0; i < obj_.size(); ++i) {
            if (i) out += ',';
            newline(depth + 1);
            escape_to(out, obj_[i].first);
            out += indent > 0 ? ": " : ":";
            obj_[i].second.dump_to(out, indent, depth + 1);
        }
        newline(depth);
        out += '}';
        break;
    }
}

std::string Value::dump(int indent) const {
    std::string out;
    dump_to(out, indent, 0);
    return out;
}

// ------------------------------------------------------------------ parser

namespace {

class Parser {
public:
    explicit Parser(const std::string& s) : s_(s) {}

    Value parse_document() {
        skip_ws();
        Value v = parse_value(0);
        skip_ws();
        if (i_ != s_.size()) fail("trailing characters");
        return v;
    }

private:
    [[noreturn]] void fail(const char* what) {
        throw std::runtime_error(std::string("JSON parse error at offset ") + std::to_string(i_) + ": " + what);
    }

    void skip_ws() {
        while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\n' || s_[i_] == '\r' || s_[i_] == '\t')) ++i_;
    }

    bool consume(const char* lit) {
        size_t n = 0;
        while (lit[n]) ++n;
        if (s_.compare(i_, n, lit) == 0) { i_ += n; return true; }
        return false;
    }

    Value parse_value(int depth) {
        if (depth > 200) fail("nesting too deep");
        if (i_ >= s_.size()) fail("unexpected end");
        char c = s_[i_];
        if (c == '{') return parse_object(depth);
        if (c == '[') return parse_array(depth);
        if (c == '"') return Value::string(parse_string());
        if (consume("true")) return Value::boolean(true);
        if (consume("false")) return Value::boolean(false);
        if (consume("null")) return Value();
        if (c == '-' || (c >= '0' && c <= '9')) return parse_number();
        fail("unexpected character");
    }

    Value parse_number() {
        size_t start = i_;
        if (s_[i_] == '-') ++i_;
        auto digits = [&] { size_t b = i_; while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') ++i_; return i_ > b; };
        if (!digits()) fail("bad number");
        if (i_ < s_.size() && s_[i_] == '.') { ++i_; if (!digits()) fail("bad fraction"); }
        if (i_ < s_.size() && (s_[i_] == 'e' || s_[i_] == 'E')) {
            ++i_;
            if (i_ < s_.size() && (s_[i_] == '+' || s_[i_] == '-')) ++i_;
            if (!digits()) fail("bad exponent");
        }
        return Value::number(s_.substr(start, i_ - start));
    }

    unsigned hex4() {
        if (i_ + 4 > s_.size()) fail("short \\u escape");
        unsigned v = 0;
        for (int k = 0; k < 4; ++k) {
            char h = s_[i_++];
            v <<= 4;
            if (h >= '0' && h <= '9') v |= (unsigned)(h - '0');
            else if (h >= 'a' && h <= 'f') v |= (unsigned)(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') v |= (unsigned)(h - 'A' + 10);
            else fail("bad hex digit");
        }
        return v;
    }

    static void put_utf8(std::string& out, unsigned cp) {
        if (cp < 0x80) out += (char)cp;
        else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) {
            out += (char)(0xE0 | (cp >> 12));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        } else {
            out += (char)(0xF0 | (cp >> 18));
            out += (char)(0x80 | ((cp >> 12) & 0x3F));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        }
    }

    std::string parse_string() {
        ++i_;   // opening quote
        std::string out;
        while (true) {
            if (i_ >= s_.size()) fail("unterminated string");
            char c = s_[i_++];
            if (c == '"') return out;
            if (c != '\\') { out += c; continue; }
            if (i_ >= s_.size()) fail("bad escape");
            char e = s_[i_++];
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
                unsigned cp = hex4();
                if (cp >= 0xD800 && cp <= 0xDBFF && s_.compare(i_, 2, "\\u") == 0) {
                    size_t save = i_;
                    i_ += 2;
                    unsigned lo = hex4();
                    if (lo >= 0xDC00 && lo <= 0xDFFF) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    else i_ = save;
                }
                put_utf8(out, cp);
                break;
            }
            default: fail("bad escape");
            }
        }
    }

    Value parse_array(int depth) {
        ++i_;
        Value v = Value::array();
        skip_ws();
        if (i_ < s_.size() && s_[i_] == ']') { ++i_; return v; }
        while (true) {
            skip_ws();
            v.push(parse_value(depth + 1));
            skip_ws();
            if (i_ >= s_.size()) fail("unterminated array");
            if (s_[i_] == ',') { ++i_; continue; }
            if (s_[i_] == ']') { ++i_; return v; }
            fail("expected , or ]");
        }
    }

    Value parse_object(int depth) {
        ++i_;
        Value v = Value::object();
        skip_ws();
        if (i_ < s_.size() && s_[i_] == '}') { ++i_; return v; }
        while (true) {
            skip_ws();
            if (i_ >= s_.size() || s_[i_] != '"') fail("expected key");
            std::string key = parse_string();
            skip_ws();
            if (i_ >= s_.size() || s_[i_] != ':') fail("expected :");
            ++i_;
            skip_ws();
            Value item = parse_value(depth + 1);
            if (Value* existing = v.find(key)) *existing = std::move(item);
            else v.members().emplace_back(std::move(key), std::move(item));
            skip_ws();
            if (i_ >= s_.size()) fail("unterminated object");
            if (s_[i_] == ',') { ++i_; continue; }
            if (s_[i_] == '}') { ++i_; return v; }
            fail("expected , or }");
        }
    }

    const std::string& s_;
    size_t i_ = 0;
};

}  // namespace

Value parse(const std::string& text) { return Parser(text).parse_document(); }

}  // namespace json
