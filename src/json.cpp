#include "json.h"
#include <stdio.h>
#include <stdlib.h>

// ---------------------------------------------------------------------------
// Typed reads
// ---------------------------------------------------------------------------

std::wstring Json::AsString(const std::wstring& key, const std::wstring& def) const
{
    const Json* v = Find(key);
    return (v && v->m_type == Type::String) ? v->m_str : def;
}

int Json::AsInt(const std::wstring& key, int def) const
{
    const Json* v = Find(key);
    return (v && v->m_type == Type::Number) ? (int)v->m_num : def;
}

double Json::AsNumberAt(const std::wstring& key, double def) const
{
    const Json* v = Find(key);
    return (v && v->m_type == Type::Number) ? v->m_num : def;
}

bool Json::AsBool(const std::wstring& key, bool def) const
{
    const Json* v = Find(key);
    if (!v) return def;
    if (v->m_type == Type::Bool) return v->m_bool;
    if (v->m_type == Type::Number) return v->m_num != 0;
    return def;
}

// ---------------------------------------------------------------------------
// Writing
// ---------------------------------------------------------------------------

static void EscapeTo(std::wstring& out, const std::wstring& s)
{
    out += L'"';
    for (wchar_t c : s)
    {
        switch (c)
        {
        case L'"':  out += L'\\'; out += L'"'; break;
        case L'\\': out += L'\\'; out += L'\\'; break;
        case L'\n': out += L'\\'; out += L'n'; break;
        case L'\r': out += L'\\'; out += L'r'; break;
        case L'\t': out += L'\\'; out += L't'; break;
        default:
            if (c < 0x20)
            {
                wchar_t buf[8];
                swprintf(buf, 8, L"\\u%04x", (unsigned)c);
                out += buf;
            }
            else out += c;
        }
    }
    out += L'"';
}

std::wstring Json::Serialise(int indent) const
{
    std::wstring pad((size_t)indent * 2, L' ');
    std::wstring inner((size_t)(indent + 1) * 2, L' ');
    std::wstring out;

    switch (m_type)
    {
    case Type::Null:   return L"null";
    case Type::Bool:   return m_bool ? L"true" : L"false";
    case Type::Number:
    {
        wchar_t buf[32];
        if (m_num == (double)(long long)m_num) swprintf(buf, 32, L"%lld", (long long)m_num);
        else                                   swprintf(buf, 32, L"%g", m_num);
        return buf;
    }
    case Type::String: EscapeTo(out, m_str); return out;

    case Type::Array:
        if (m_items.empty()) return L"[]";
        out = L"[\n";
        for (size_t i = 0; i < m_items.size(); ++i)
        {
            out += inner + m_items[i].Serialise(indent + 1);
            if (i + 1 < m_items.size()) out += L',';
            out += L'\n';
        }
        return out + pad + L"]";

    case Type::Object:
        if (m_keys.empty()) return L"{}";
        out = L"{\n";
        for (size_t i = 0; i < m_keys.size(); ++i)
        {
            out += inner;
            EscapeTo(out, m_keys[i]);
            out += L": ";
            out += m_members.at(m_keys[i]).Serialise(indent + 1);
            if (i + 1 < m_keys.size()) out += L',';
            out += L'\n';
        }
        return out + pad + L"}";
    }
    return L"null";
}

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------

namespace {

struct Parser
{
    const wchar_t* p;
    const wchar_t* end;

    void Skip()
    {
        while (p < end && (*p == L' ' || *p == L'\t' || *p == L'\r' || *p == L'\n' ||
                           *p == 0xFEFF))
            ++p;
    }
    bool Lit(const wchar_t* s)
    {
        const wchar_t* q = p;
        while (*s && q < end && *q == *s) { ++q; ++s; }
        if (*s) return false;
        p = q;
        return true;
    }

    bool String(std::wstring& out)
    {
        if (p >= end || *p != L'"') return false;
        ++p;
        out.clear();
        while (p < end && *p != L'"')
        {
            if (*p == L'\\')
            {
                ++p;
                if (p >= end) return false;
                switch (*p)
                {
                case L'"':  out += L'"';  break;
                case L'\\': out += L'\\'; break;
                case L'/':  out += L'/';  break;
                case L'b':  out += L'\b'; break;
                case L'f':  out += L'\f'; break;
                case L'n':  out += L'\n'; break;
                case L'r':  out += L'\r'; break;
                case L't':  out += L'\t'; break;
                case L'u':
                {
                    if (end - p < 5) return false;
                    wchar_t hex[5] = { p[1], p[2], p[3], p[4], 0 };
                    out += (wchar_t)wcstoul(hex, nullptr, 16);
                    p += 4;
                    break;
                }
                default: return false;
                }
                ++p;
            }
            else out += *p++;
        }
        if (p >= end) return false;
        ++p;   // closing quote
        return true;
    }

    bool Value(Json& out)
    {
        Skip();
        if (p >= end) return false;

        if (*p == L'{')
        {
            ++p;
            out = Json::Obj();
            Skip();
            if (p < end && *p == L'}') { ++p; return true; }
            for (;;)
            {
                Skip();
                std::wstring key;
                if (!String(key)) return false;
                Skip();
                if (p >= end || *p != L':') return false;
                ++p;
                Json v;
                if (!Value(v)) return false;
                out.Set(key, std::move(v));
                Skip();
                if (p < end && *p == L',') { ++p; continue; }
                if (p < end && *p == L'}') { ++p; return true; }
                return false;
            }
        }
        if (*p == L'[')
        {
            ++p;
            out = Json::Arr();
            Skip();
            if (p < end && *p == L']') { ++p; return true; }
            for (;;)
            {
                Json v;
                if (!Value(v)) return false;
                out.Push(std::move(v));
                Skip();
                if (p < end && *p == L',') { ++p; continue; }
                if (p < end && *p == L']') { ++p; return true; }
                return false;
            }
        }
        if (*p == L'"')
        {
            std::wstring s;
            if (!String(s)) return false;
            out = Json::Str(s);
            return true;
        }
        if (Lit(L"true"))  { out = Json::Bool(true);  return true; }
        if (Lit(L"false")) { out = Json::Bool(false); return true; }
        if (Lit(L"null"))  { out = Json();            return true; }

        wchar_t* stop = nullptr;
        double d = wcstod(p, &stop);
        if (stop == p) return false;
        p = stop;
        out = Json::Num(d);
        return true;
    }
};

} // namespace

bool Json::Parse(const std::wstring& text, Json& out)
{
    Parser ps{ text.c_str(), text.c_str() + text.size() };
    if (!ps.Value(out)) return false;
    ps.Skip();
    return true;
}
