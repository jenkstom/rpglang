// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Tom White

#include "json.h"

#include <cctype>
#include <cmath>
#include <cstdio>

namespace analyze {

namespace {

void dump_string(const std::string &s, std::string &out) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += (char)c;
                }
        }
    }
    out += '"';
}

void newline_indent(std::string &out, int indent, int depth) {
    if (indent <= 0) return;
    out += '\n';
    out.append((size_t)(indent * depth), ' ');
}

} // namespace

void Json::dump_to(std::string &out, int indent, int depth) const {
    switch (type_) {
        case Type::Null: out += "null"; break;
        case Type::Bool: out += bool_ ? "true" : "false"; break;
        case Type::Int: out += std::to_string(int_); break;
        case Type::Double: {
            if (std::isfinite(double_)) {
                char buf[64];
                std::snprintf(buf, sizeof buf, "%g", double_);
                out += buf;
            } else {
                out += "null";
            }
            break;
        }
        case Type::String: dump_string(str_, out); break;
        case Type::Array: {
            if (arr_.empty()) { out += "[]"; break; }
            out += '[';
            bool first = true;
            for (auto &v : arr_) {
                if (!first) out += ',';
                first = false;
                newline_indent(out, indent, depth + 1);
                v.dump_to(out, indent, depth + 1);
            }
            newline_indent(out, indent, depth);
            out += ']';
            break;
        }
        case Type::Object: {
            if (obj_.empty()) { out += "{}"; break; }
            out += '{';
            bool first = true;
            for (auto &kv : obj_) {
                if (!first) out += ',';
                first = false;
                newline_indent(out, indent, depth + 1);
                dump_string(kv.first, out);
                out += indent > 0 ? ": " : ":";
                kv.second.dump_to(out, indent, depth + 1);
            }
            newline_indent(out, indent, depth);
            out += '}';
            break;
        }
    }
}

std::string Json::dump(int indent) const {
    std::string out;
    dump_to(out, indent, 0);
    return out;
}

namespace {

struct Parser {
    const std::string &s;
    size_t i = 0;
    std::string err;

    explicit Parser(const std::string &text) : s(text) {}

    void skip_ws() {
        while (i < s.size() && std::isspace((unsigned char)s[i])) ++i;
    }

    bool fail(const std::string &msg) {
        err = "at offset " + std::to_string(i) + ": " + msg;
        return false;
    }

    bool parse_value(Json &out) {
        skip_ws();
        if (i >= s.size()) return fail("unexpected end of input");
        char c = s[i];
        if (c == '{') return parse_object(out);
        if (c == '[') return parse_array(out);
        if (c == '"') { std::string str; if (!parse_string(str)) return false; out = Json(str); return true; }
        if (c == 't') { if (s.compare(i, 4, "true") == 0) { i += 4; out = Json(true); return true; } return fail("bad literal"); }
        if (c == 'f') { if (s.compare(i, 5, "false") == 0) { i += 5; out = Json(false); return true; } return fail("bad literal"); }
        if (c == 'n') { if (s.compare(i, 4, "null") == 0) { i += 4; out = Json(nullptr); return true; } return fail("bad literal"); }
        if (c == '-' || std::isdigit((unsigned char)c)) return parse_number(out);
        return fail(std::string("unexpected character '") + c + "'");
    }

    bool parse_string(std::string &out) {
        if (s[i] != '"') return fail("expected string");
        ++i;
        out.clear();
        while (i < s.size() && s[i] != '"') {
            char c = s[i];
            if (c == '\\') {
                ++i;
                if (i >= s.size()) return fail("unterminated escape");
                char e = s[i];
                switch (e) {
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'u': {
                        if (i + 4 >= s.size()) return fail("bad \\u escape");
                        unsigned code = 0;
                        for (int k = 1; k <= 4; ++k) {
                            char h = s[i + k];
                            code <<= 4;
                            if (h >= '0' && h <= '9') code |= (unsigned)(h - '0');
                            else if (h >= 'a' && h <= 'f') code |= (unsigned)(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') code |= (unsigned)(h - 'A' + 10);
                            else return fail("bad \\u escape");
                        }
                        i += 4;
                        // Minimal: encode as UTF-8 for the BMP subset we need.
                        if (code < 0x80) out += (char)code;
                        else if (code < 0x800) {
                            out += (char)(0xC0 | (code >> 6));
                            out += (char)(0x80 | (code & 0x3F));
                        } else {
                            out += (char)(0xE0 | (code >> 12));
                            out += (char)(0x80 | ((code >> 6) & 0x3F));
                            out += (char)(0x80 | (code & 0x3F));
                        }
                        break;
                    }
                    default: return fail("bad escape");
                }
                ++i;
            } else {
                out += c;
                ++i;
            }
        }
        if (i >= s.size()) return fail("unterminated string");
        ++i; // closing quote
        return true;
    }

    bool parse_number(Json &out) {
        size_t start = i;
        if (i < s.size() && s[i] == '-') ++i;
        while (i < s.size() && std::isdigit((unsigned char)s[i])) ++i;
        bool is_float = false;
        if (i < s.size() && s[i] == '.') {
            is_float = true;
            ++i;
            while (i < s.size() && std::isdigit((unsigned char)s[i])) ++i;
        }
        if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
            is_float = true;
            ++i;
            if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
            while (i < s.size() && std::isdigit((unsigned char)s[i])) ++i;
        }
        std::string tok = s.substr(start, i - start);
        if (tok.empty() || tok == "-") return fail("bad number");
        if (is_float) out = Json(std::stod(tok));
        else out = Json((long long)std::stoll(tok));
        return true;
    }

    bool parse_array(Json &out) {
        out = Json::array();
        ++i; // '['
        skip_ws();
        if (i < s.size() && s[i] == ']') { ++i; return true; }
        while (true) {
            Json v;
            if (!parse_value(v)) return false;
            out.push_back(std::move(v));
            skip_ws();
            if (i >= s.size()) return fail("unterminated array");
            if (s[i] == ',') { ++i; continue; }
            if (s[i] == ']') { ++i; break; }
            return fail("expected ',' or ']'");
        }
        return true;
    }

    bool parse_object(Json &out) {
        out = Json::object();
        ++i; // '{'
        skip_ws();
        if (i < s.size() && s[i] == '}') { ++i; return true; }
        while (true) {
            skip_ws();
            std::string key;
            if (i >= s.size() || s[i] != '"') return fail("expected string key");
            if (!parse_string(key)) return false;
            skip_ws();
            if (i >= s.size() || s[i] != ':') return fail("expected ':'");
            ++i;
            Json v;
            if (!parse_value(v)) return false;
            out.set(key, std::move(v));
            skip_ws();
            if (i >= s.size()) return fail("unterminated object");
            if (s[i] == ',') { ++i; continue; }
            if (s[i] == '}') { ++i; break; }
            return fail("expected ',' or '}'");
        }
        return true;
    }
};

} // namespace

bool Json::parse(const std::string &text, Json &out, std::string &err) {
    Parser p(text);
    if (!p.parse_value(out)) { err = p.err; return false; }
    p.skip_ws();
    return true;
}

} // namespace analyze
