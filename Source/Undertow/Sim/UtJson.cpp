#include "Sim/UtJson.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace ut {
namespace json_detail {

struct Parser {
    const char* p;
    const char* end;
    std::string error;
    int depth = 0;

    void skipSpace() {
        while (p < end) {
            const char c = *p;
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++p;
            } else if (c == '/' && p + 1 < end && p[1] == '/') {   // line comments are allowed in hand-edited data
                while (p < end && *p != '\n') ++p;
            } else {
                break;
            }
        }
    }

    bool fail(const char* msg) {
        if (error.empty()) error = msg;
        return false;
    }

    bool literal(const char* word) {
        const char* q = p;
        while (*word) {
            if (q >= end || *q != *word) return fail("invalid literal");
            ++q;
            ++word;
        }
        p = q;
        return true;
    }

    static void appendUtf8(std::string& s, unsigned cp) {
        if (cp < 0x80U) {
            s.push_back(static_cast<char>(cp));
        } else if (cp < 0x800U) {
            s.push_back(static_cast<char>(0xC0U | (cp >> 6U)));
            s.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
        } else if (cp < 0x10000U) {
            s.push_back(static_cast<char>(0xE0U | (cp >> 12U)));
            s.push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU)));
            s.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
        } else {
            s.push_back(static_cast<char>(0xF0U | (cp >> 18U)));
            s.push_back(static_cast<char>(0x80U | ((cp >> 12U) & 0x3FU)));
            s.push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU)));
            s.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
        }
    }

    bool hex4(unsigned& out) {
        if (end - p < 4) return fail("truncated \\u escape");
        out = 0U;
        for (int i = 0; i < 4; ++i) {
            const char c = *p++;
            unsigned v;
            if (c >= '0' && c <= '9') v = static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') v = static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v = static_cast<unsigned>(c - 'A' + 10);
            else return fail("bad \\u escape");
            out = (out << 4U) | v;
        }
        return true;
    }

    bool string(std::string& out) {
        ++p;   // opening quote
        while (p < end) {
            const char c = *p++;
            if (c == '"') return true;
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (p >= end) break;
            const char e = *p++;
            switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    unsigned cp = 0U;
                    if (!hex4(cp)) return false;
                    if (cp >= 0xD800U && cp <= 0xDBFFU && end - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                        p += 2;
                        unsigned lo = 0U;
                        if (!hex4(lo)) return false;
                        cp = 0x10000U + ((cp - 0xD800U) << 10U) + (lo - 0xDC00U);
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default: return fail("bad escape");
            }
        }
        return fail("unterminated string");
    }

    bool value(Json& out) {
        if (++depth > 200) return fail("nesting too deep");
        skipSpace();
        if (p >= end) return fail("unexpected end of input");
        const char c = *p;
        bool ok = true;
        if (c == '{') {
            ++p;
            out = Json::object();
            skipSpace();
            if (p < end && *p == '}') {
                ++p;
            } else {
                for (;;) {
                    skipSpace();
                    if (p >= end || *p != '"') { ok = fail("expected object key"); break; }
                    std::string key;
                    if (!string(key)) { ok = false; break; }
                    skipSpace();
                    if (p >= end || *p != ':') { ok = fail("expected ':'"); break; }
                    ++p;
                    Json member;
                    if (!value(member)) { ok = false; break; }
                    out.set(key, std::move(member));
                    skipSpace();
                    if (p < end && *p == ',') { ++p; continue; }
                    if (p < end && *p == '}') { ++p; break; }
                    ok = fail("expected ',' or '}'");
                    break;
                }
            }
        } else if (c == '[') {
            ++p;
            out = Json::array();
            skipSpace();
            if (p < end && *p == ']') {
                ++p;
            } else {
                for (;;) {
                    Json element;
                    if (!value(element)) { ok = false; break; }
                    out.push(std::move(element));
                    skipSpace();
                    if (p < end && *p == ',') { ++p; continue; }
                    if (p < end && *p == ']') { ++p; break; }
                    ok = fail("expected ',' or ']'");
                    break;
                }
            }
        } else if (c == '"') {
            std::string s;
            ok = string(s);
            if (ok) out = Json(s);
        } else if (c == 't') {
            ok = literal("true");
            if (ok) out = Json(true);
        } else if (c == 'f') {
            ok = literal("false");
            if (ok) out = Json(false);
        } else if (c == 'n') {
            ok = literal("null");
            if (ok) out = Json();
        } else if (c == '-' || (c >= '0' && c <= '9')) {
            const char* start = p;
            if (*p == '-') ++p;
            while (p < end && ((*p >= '0' && *p <= '9') || *p == '.' || *p == 'e' || *p == 'E' || *p == '+' || *p == '-')) ++p;
            const std::string num(start, static_cast<size_t>(p - start));
            char* parsedEnd = nullptr;
            const double d = std::strtod(num.c_str(), &parsedEnd);
            if (parsedEnd == num.c_str() || *parsedEnd != '\0') ok = fail("bad number");
            else out = Json(d);
        } else {
            ok = fail("unexpected character");
        }
        --depth;
        return ok;
    }
};

void escapeTo(std::string& out, const std::string& s) {
    out.push_back('"');
    for (const char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20U) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                    out += buf;
                } else {
                    out.push_back(ch);
                }
        }
    }
    out.push_back('"');
}

void newline(std::string& out, int indent, int level) {
    if (indent <= 0) return;
    out.push_back('\n');
    out.append(static_cast<size_t>(indent * level), ' ');
}

void dumpTo(const Json& j, std::string& out, int indent, int level) {
    switch (j.type()) {
        case Json::Type::Null: out += "null"; break;
        case Json::Type::Bool: out += j.asBool() ? "true" : "false"; break;
        case Json::Type::Number: {
            const double d = j.asDouble();
            char buf[40];
            if (std::isfinite(d) && d == std::floor(d) && std::fabs(d) < 1e15) {
                std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(d));
            } else if (std::isfinite(d)) {
                // Shortest form that reads back exactly: 9 digits is enough for anything that started life as a
                // float; latitudes and longitudes need up to 17.
                for (const int digits : {9, 15, 16, 17}) {
                    std::snprintf(buf, sizeof(buf), "%.*g", digits, d);
                    if (std::strtod(buf, nullptr) == d) break;
                }
            } else {
                std::snprintf(buf, sizeof(buf), "0");   // JSON has no NaN/Inf
            }
            out += buf;
            break;
        }
        case Json::Type::String: escapeTo(out, j.asString()); break;
        case Json::Type::Array: {
            out.push_back('[');
            for (int i = 0; i < j.size(); ++i) {
                if (i) out.push_back(',');
                newline(out, indent, level + 1);
                dumpTo(j.at(i), out, indent, level + 1);
            }
            if (j.size()) newline(out, indent, level);
            out.push_back(']');
            break;
        }
        case Json::Type::Object: {
            out.push_back('{');
            for (int i = 0; i < j.size(); ++i) {
                if (i) out.push_back(',');
                newline(out, indent, level + 1);
                escapeTo(out, j.keyAt(i));
                out.push_back(':');
                if (indent > 0) out.push_back(' ');
                dumpTo(j.at(i), out, indent, level + 1);
            }
            if (j.size()) newline(out, indent, level);
            out.push_back('}');
            break;
        }
    }
}

const Json& nullJson() {
    static const Json n;
    return n;
}

const std::string& emptyString() {
    static const std::string s;
    return s;
}

}  // namespace json_detail

bool Json::parse(const std::string& text, Json& out, std::string* error) {
    json_detail::Parser parser{text.data(), text.data() + text.size(), std::string(), 0};
    Json result;
    bool ok = parser.value(result);
    if (ok) {
        parser.skipSpace();
        if (parser.p != parser.end) ok = parser.fail("trailing characters after value");
    }
    if (!ok) {
        if (error) {
            char where[48];
            std::snprintf(where, sizeof(where), " at offset %lld", static_cast<long long>(parser.p - text.data()));
            *error = parser.error + where;
        }
        out = Json();
        return false;
    }
    out = std::move(result);
    return true;
}

std::string Json::dump(int indent) const {
    std::string out;
    json_detail::dumpTo(*this, out, indent, 0);
    return out;
}

const std::string& Json::asString() const { return type_ == Type::String ? str_ : json_detail::emptyString(); }

const Json& Json::at(int index) const {
    if (index < 0 || index >= size()) return json_detail::nullJson();
    return items_[static_cast<size_t>(index)];
}

const std::string& Json::keyAt(int index) const {
    if (type_ != Type::Object || index < 0 || index >= static_cast<int>(keys_.size())) return json_detail::emptyString();
    return keys_[static_cast<size_t>(index)];
}

const Json& Json::operator[](const char* key) const {
    if (type_ == Type::Object) {
        for (size_t i = 0; i < keys_.size(); ++i) {
            if (keys_[i] == key) return items_[i];
        }
    }
    return json_detail::nullJson();
}

bool Json::has(const char* key) const {
    if (type_ != Type::Object) return false;
    for (const std::string& k : keys_) {
        if (k == key) return true;
    }
    return false;
}

Json& Json::push(Json value) {
    if (type_ != Type::Array) {
        *this = Json::array();
    }
    items_.push_back(std::move(value));
    return *this;
}

Json& Json::set(const std::string& key, Json value) {
    if (type_ != Type::Object) {
        *this = Json::object();
    }
    for (size_t i = 0; i < keys_.size(); ++i) {
        if (keys_[i] == key) {
            items_[i] = std::move(value);
            return *this;
        }
    }
    keys_.push_back(key);
    items_.push_back(std::move(value));
    return *this;
}

}  // namespace ut
