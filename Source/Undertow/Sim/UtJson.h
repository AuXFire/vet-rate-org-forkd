// Small JSON reader/writer for game data and save files. No exceptions: parse() reports errors by return value.
#pragma once

#include <string>
#include <vector>

namespace ut {

class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Json() = default;
    Json(bool v) : type_(Type::Bool), num_(v ? 1.0 : 0.0) {}
    Json(int v) : type_(Type::Number), num_(static_cast<double>(v)) {}
    Json(long long v) : type_(Type::Number), num_(static_cast<double>(v)) {}
    Json(float v) : type_(Type::Number), num_(static_cast<double>(v)) {}
    Json(double v) : type_(Type::Number), num_(v) {}
    Json(const char* v) : type_(Type::String), str_(v) {}
    Json(const std::string& v) : type_(Type::String), str_(v) {}

    static Json array() { Json j; j.type_ = Type::Array; return j; }
    static Json object() { Json j; j.type_ = Type::Object; return j; }

    // Returns false and fills `error` (if given) on malformed input; `out` is then Null.
    static bool parse(const std::string& text, Json& out, std::string* error = nullptr);
    std::string dump(int indent = 0) const;

    Type type() const { return type_; }
    bool isNull() const { return type_ == Type::Null; }
    bool isArray() const { return type_ == Type::Array; }
    bool isObject() const { return type_ == Type::Object; }
    bool isNumber() const { return type_ == Type::Number; }
    bool isString() const { return type_ == Type::String; }

    // Readers never fail: a missing key or wrong type yields the default.
    bool asBool(bool def = false) const { return type_ == Type::Bool || type_ == Type::Number ? num_ != 0.0 : def; }
    double asDouble(double def = 0.0) const { return type_ == Type::Number || type_ == Type::Bool ? num_ : def; }
    float asFloat(float def = 0.0f) const { return static_cast<float>(asDouble(static_cast<double>(def))); }
    int asInt(int def = 0) const { return static_cast<int>(asDouble(static_cast<double>(def))); }
    long long asInt64(long long def = 0) const { return static_cast<long long>(asDouble(static_cast<double>(def))); }
    const std::string& asString() const;
    std::string asString(const std::string& def) const { return type_ == Type::String ? str_ : def; }

    int size() const { return static_cast<int>(items_.size()); }
    const Json& at(int index) const;                 // array element or object value by position; Null if out of range
    const std::string& keyAt(int index) const;       // object key by position
    const Json& operator[](const char* key) const;   // object member; Null if absent
    const Json& operator[](const std::string& key) const { return (*this)[key.c_str()]; }
    bool has(const char* key) const;

    Json& push(Json value);                          // array append; turns Null into an array
    Json& set(const std::string& key, Json value);   // object insert/replace; turns Null into an object

private:
    Type type_ = Type::Null;
    double num_ = 0.0;
    std::string str_;
    std::vector<std::string> keys_;   // parallel to items_ for objects
    std::vector<Json> items_;
};

}  // namespace ut
