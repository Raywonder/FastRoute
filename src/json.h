#pragma once
#include <string>
#include <vector>
#include <map>
#include <memory>

// A deliberately small JSON value type: enough for the session document and
// nothing more. No dependencies, UTF-16 in memory, UTF-8 on disk.
class Json
{
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Json() : m_type(Type::Null) {}
    static Json Obj()   { Json j; j.m_type = Type::Object; return j; }
    static Json Arr()   { Json j; j.m_type = Type::Array;  return j; }
    static Json Str(const std::wstring& v) { Json j; j.m_type = Type::String; j.m_str = v; return j; }
    static Json Num(double v)              { Json j; j.m_type = Type::Number; j.m_num = v; return j; }
    static Json Bool(bool v)               { Json j; j.m_type = Type::Bool;   j.m_bool = v; return j; }

    Type type() const { return m_type; }
    bool IsObject() const { return m_type == Type::Object; }
    bool IsArray() const  { return m_type == Type::Array; }

    // Object access
    void Set(const std::wstring& key, Json v) { m_keys.push_back(key); m_members[key] = std::move(v); }
    const Json* Find(const std::wstring& key) const
    {
        auto it = m_members.find(key);
        return it == m_members.end() ? nullptr : &it->second;
    }

    // Array access
    void Push(Json v) { m_items.push_back(std::move(v)); }
    const std::vector<Json>& Items() const { return m_items; }

    // Typed reads with defaults
    // Scalar form, for array elements, which have no key to look up by.
    double       AsNumber(double def = 0.0) const { return m_type == Type::Number ? m_num : def; }
    std::wstring AsText(const std::wstring& def = L"") const { return m_type == Type::String ? m_str : def; }
    std::wstring AsString(const std::wstring& key, const std::wstring& def = L"") const;
    int          AsInt(const std::wstring& key, int def = 0) const;
    // For a value too large for an int: minutes since 1601 does not fit.
    double       AsNumberAt(const std::wstring& key, double def = 0.0) const;
    bool         AsBool(const std::wstring& key, bool def = false) const;

    std::wstring Serialise(int indent = 0) const;
    static bool  Parse(const std::wstring& text, Json& out);

private:
    Type m_type;
    bool m_bool = false;
    double m_num = 0;
    std::wstring m_str;
    std::vector<Json> m_items;                  // array
    std::vector<std::wstring> m_keys;           // object, insertion order
    std::map<std::wstring, Json> m_members;     // object
};
