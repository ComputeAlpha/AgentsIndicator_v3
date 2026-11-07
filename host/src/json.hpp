// Minimal JSON value with ordered objects (so rewriting ~/.claude/settings.json
// keeps the user's key order). Numbers keep their original text.
#pragma once
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace json {

class Value {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Value() = default;
    static Value boolean(bool b) { Value v; v.type_ = Type::Bool; v.b_ = b; return v; }
    static Value number(std::string text) { Value v; v.type_ = Type::Number; v.s_ = std::move(text); return v; }
    static Value number(long long n) { return number(std::to_string(n)); }
    static Value string(std::string s) { Value v; v.type_ = Type::String; v.s_ = std::move(s); return v; }
    static Value array() { Value v; v.type_ = Type::Array; return v; }
    static Value object() { Value v; v.type_ = Type::Object; return v; }

    Type type() const { return type_; }
    bool is_null() const { return type_ == Type::Null; }
    bool is_string() const { return type_ == Type::String; }
    bool is_object() const { return type_ == Type::Object; }
    bool is_array() const { return type_ == Type::Array; }

    const std::string& str() const { return s_; }   // String or Number text
    bool as_bool() const { return b_; }

    // Arrays
    std::vector<Value>& items() { return arr_; }
    const std::vector<Value>& items() const { return arr_; }
    void push(Value v) { arr_.push_back(std::move(v)); }

    // Objects
    std::vector<std::pair<std::string, Value>>& members() { return obj_; }
    const std::vector<std::pair<std::string, Value>>& members() const { return obj_; }
    const Value* find(const std::string& key) const;
    Value* find(const std::string& key);
    Value& operator[](const std::string& key);       // inserts Null if missing
    void erase(const std::string& key);

    // Convenience: string member or "" if absent / not a string.
    std::string get_string(const std::string& key) const;

    std::string dump(int indent = 2) const;

private:
    void dump_to(std::string& out, int indent, int depth) const;

    Type type_ = Type::Null;
    bool b_ = false;
    std::string s_;
    std::vector<Value> arr_;
    std::vector<std::pair<std::string, Value>> obj_;
};

// Throws std::runtime_error with position info on malformed input.
Value parse(const std::string& text);

}  // namespace json
