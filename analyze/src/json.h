// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Tom White

/* ========================================================================== *
 * json.h -- a minimal, self-contained JSON value type (no external deps).
 *
 * Used both as the module Section payload type (§6.2) and, later, as the
 * input format for `portfolio --from` / `migrate` (which consume per-file
 * `--json` output). Object keys preserve insertion order, matching how the
 * report schema in TOOLS_IDEAS.md §6.2 is laid out.
 * ========================================================================== */
#ifndef RPGANALYZE_JSON_H
#define RPGANALYZE_JSON_H

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace analyze {

class Json {
public:
    enum class Type { Null, Bool, Int, Double, String, Array, Object };

    Json() : type_(Type::Null) {}
    Json(std::nullptr_t) : type_(Type::Null) {}
    Json(bool b) : type_(Type::Bool), bool_(b) {}
    Json(int i) : type_(Type::Int), int_(i) {}
    Json(long i) : type_(Type::Int), int_(i) {}
    Json(long long i) : type_(Type::Int), int_(i) {}
    Json(size_t i) : type_(Type::Int), int_(static_cast<long long>(i)) {}
    Json(double d) : type_(Type::Double), double_(d) {}
    Json(const char *s) : type_(Type::String), str_(s) {}
    Json(std::string s) : type_(Type::String), str_(std::move(s)) {}

    static Json array() { Json j; j.type_ = Type::Array; return j; }
    static Json object() { Json j; j.type_ = Type::Object; return j; }

    Type type() const { return type_; }
    bool is_null() const { return type_ == Type::Null; }

    /* Array building. */
    Json &push_back(Json v) {
        arr_.push_back(std::move(v));
        return *this;
    }

    /* Object building; preserves insertion order, last write wins on repeat key. */
    Json &set(const std::string &key, Json v) {
        for (auto &kv : obj_) {
            if (kv.first == key) { kv.second = std::move(v); return *this; }
        }
        obj_.emplace_back(key, std::move(v));
        return *this;
    }

    const std::vector<Json> &items() const { return arr_; }
    const std::vector<std::pair<std::string, Json>> &fields() const { return obj_; }

    bool           as_bool() const { return bool_; }
    long long      as_int() const { return int_; }
    double         as_double() const { return type_ == Type::Int ? (double)int_ : double_; }
    const std::string &as_string() const { return str_; }

    /* Serialize. `indent` <= 0 emits compact single-line JSON. */
    std::string dump(int indent = 2) const;

    /* Parse minimal JSON text (object/array/string/number/bool/null). Returns
     * false with `err` set on malformed input. */
    static bool parse(const std::string &text, Json &out, std::string &err);

private:
    void dump_to(std::string &out, int indent, int depth) const;

    Type type_ = Type::Null;
    bool bool_ = false;
    long long int_ = 0;
    double double_ = 0.0;
    std::string str_;
    std::vector<Json> arr_;
    std::vector<std::pair<std::string, Json>> obj_;
};

} // namespace analyze

#endif // RPGANALYZE_JSON_H
