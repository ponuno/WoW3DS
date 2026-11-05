// fbase.cpp — see fbase.hpp
#include "fbase.hpp"

#include <cstdio>
#include <cstdlib>

#include "miniz.h"

namespace wf {

std::string hex(const uint8_t* p, size_t n, size_t max) {
    std::string s;
    char b[4];
    for (size_t i = 0; i < n && i < max; ++i) { snprintf(b, sizeof b, "%02x", p[i]); s += b; }
    if (n > max) s += "...";
    return s;
}

namespace {
struct JsonParser {
    const std::string& t;
    size_t i = 0;
    std::string err;
    explicit JsonParser(const std::string& text) : t(text) {}
    void ws() { while (i < t.size() && (t[i] == ' ' || t[i] == '\n' || t[i] == '\r' || t[i] == '\t')) ++i; }
    bool fail(const char* m) { if (err.empty()) { err = m; err += " at "; err += std::to_string(i); } return false; }
    bool str(std::string& out) {
        if (i >= t.size() || t[i] != '"') return fail("string expected");
        ++i;
        while (i < t.size() && t[i] != '"') {
            char c = t[i++];
            if (c == '\\') {
                if (i >= t.size()) return fail("bad escape");
                char e = t[i++];
                switch (e) {
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'u': {
                        if (i + 4 > t.size()) return fail("bad \\u");
                        unsigned cp = (unsigned)strtoul(t.substr(i, 4).c_str(), nullptr, 16);
                        i += 4;
                        if (cp < 0x80) out += (char)cp;
                        else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
                        else { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
                        break;
                    }
                    default: out += e; break;
                }
            } else {
                out += c;
            }
        }
        if (i >= t.size()) return fail("unterminated string");
        ++i;
        return true;
    }
    bool value(Json& v, int depth) {
        if (depth > 64) return fail("too deep");
        ws();
        if (i >= t.size()) return fail("value expected");
        const char c = t[i];
        if (c == '{') {
            v.type = Json::Obj;
            ++i;
            ws();
            if (i < t.size() && t[i] == '}') { ++i; return true; }
            for (;;) {
                ws();
                std::string k;
                if (!str(k)) return false;
                ws();
                if (i >= t.size() || t[i] != ':') return fail("':' expected");
                ++i;
                v.obj.emplace_back(k, Json());
                if (!value(v.obj.back().second, depth + 1)) return false;
                ws();
                if (i < t.size() && t[i] == ',') { ++i; continue; }
                if (i < t.size() && t[i] == '}') { ++i; return true; }
                return fail("',' or '}' expected");
            }
        }
        if (c == '[') {
            v.type = Json::Arr;
            ++i;
            ws();
            if (i < t.size() && t[i] == ']') { ++i; return true; }
            for (;;) {
                v.arr.emplace_back();
                if (!value(v.arr.back(), depth + 1)) return false;
                ws();
                if (i < t.size() && t[i] == ',') { ++i; continue; }
                if (i < t.size() && t[i] == ']') { ++i; return true; }
                return fail("',' or ']' expected");
            }
        }
        if (c == '"') { v.type = Json::Str; return str(v.s); }
        if (t.compare(i, 4, "true") == 0) { v.type = Json::Bool; v.b = true; i += 4; return true; }
        if (t.compare(i, 5, "false") == 0) { v.type = Json::Bool; v.b = false; i += 5; return true; }
        if (t.compare(i, 4, "null") == 0) { v.type = Json::Null; i += 4; return true; }
        const char* s = t.c_str() + i;
        char* e = nullptr;
        v.num = strtod(s, &e);
        if (e == s) return fail("bad value");
        v.type = Json::Num;
        i += (size_t)(e - s);
        return true;
    }
};
}  // namespace

bool Json::parse(const std::string& text, Json& out, std::string* err) {
    JsonParser p(text);
    out = Json();
    const bool ok = p.value(out, 0);
    if (!ok && err) *err = p.err;
    return ok;
}

bool unpackJsonBlob(const Bytes& blob, std::string& json, std::string* prefix) {
    if (blob.size() < 6) return false;
    uint32_t len;
    memcpy(&len, blob.data(), 4);
    if (len == 0 || len > (16u << 20)) return false;
    Bytes out(len);
    mz_ulong outLen = len;
    if (mz_uncompress(out.data(), &outLen, blob.data() + 4, (mz_ulong)(blob.size() - 4)) != MZ_OK) return false;
    std::string s((const char*)out.data(), outLen);
    while (!s.empty() && s.back() == '\0') s.pop_back();
    const size_t colon = s.find(':');
    if (colon == std::string::npos) return false;
    if (prefix) *prefix = s.substr(0, colon);
    json = s.substr(colon + 1);
    return true;
}

}  // namespace wf
