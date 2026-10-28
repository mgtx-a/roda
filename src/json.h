// A small JSON reader and writer for jobs and worker messages: objects, arrays, strings,
// numbers, true/false/null. Enough for what Roda sends to itself; not a general library.
#pragma once
#include "common.h"

#include <map>

namespace rd {

struct Json {
    enum class Type { Null, Bool, Number, String, Array, Object } type = Type::Null;
    bool b = false;
    double num = 0;
    wstring str;
    std::vector<Json> arr;
    std::map<wstring, Json> obj;

    Json() = default;
    Json(const wchar_t* s) : type(Type::String), str(s) {}
    Json(const wstring& s) : type(Type::String), str(s) {}
    Json(double n) : type(Type::Number), num(n) {}
    Json(int n) : type(Type::Number), num(n) {}
    Json(bool v) : type(Type::Bool), b(v) {}
    static Json Array() { Json j; j.type = Type::Array; return j; }
    static Json Object() { Json j; j.type = Type::Object; return j; }

    /// Member access; a missing member reads as null.
    const Json& operator[](const wchar_t* key) const;
    Json& Set(const wchar_t* key, Json v) { type = Type::Object; obj[key] = std::move(v); return *this; }
    void Push(Json v) { type = Type::Array; arr.push_back(std::move(v)); }

    bool has(const wchar_t* key) const { return type == Type::Object && obj.count(key) != 0; }
    wstring Str(const wstring& fallback = L"") const { return type == Type::String ? str : fallback; }
    double Num(double fallback = 0) const;
    bool Bool(bool fallback = false) const { return type == Type::Bool ? b : fallback; }

    std::string Dump() const;                       // compact, one line, UTF-8
    static bool Parse(std::string_view text, Json& out);
};

}  // namespace rd
