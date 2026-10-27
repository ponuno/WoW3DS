// Direct MPQ reading through StormLib, so the converter can take the client's
// Data/ folder as-is instead of a pre-extracted copy.
#include "conv.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>

#ifdef W3D_WITH_STORMLIB
#include <StormLib.h>
#endif

namespace fs = std::filesystem;

namespace w3d {

namespace {
std::string lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// Vanilla load order: base archives < patch.MPQ < patch-2.MPQ < patch-3.MPQ ...
// (private servers add patch-4..patch-Z). Locale archives in Data/<locale>/
// rank just above the base ones of the same kind.
int mpqPriority(const fs::path& p, bool inLocaleDir) {
    std::string n = lower(p.filename().string());
    int pr = inLocaleDir ? 1 : 0;
    if (n.rfind("patch", 0) == 0) {
        // "patch.mpq" -> 1, "patch-2.mpq" -> 2, "patch-a.mpq" -> 'a', "patch-enus.mpq" -> 1
        int level = 1;
        if (n.size() > 6 && n[5] == '-') {
            std::string tag = n.substr(6, n.find('.') - 6);
            if (tag.size() == 1) level = std::isdigit((unsigned char)tag[0]) ? tag[0] - '0' : tag[0];
            else if (!tag.empty() && std::all_of(tag.begin(), tag.end(), ::isdigit)) level = std::stoi(tag);
            else if (tag.size() > 4 && tag[4] == '-') {  // patch-enUS-2.mpq
                char c = tag.back();
                level = std::isdigit((unsigned char)c) ? c - '0' : c;
            }
        }
        pr = 1000 + level * 2 + (inLocaleDir ? 1 : 0);
    }
    return pr;
}
}  // namespace

DataDir::~DataDir() {
#ifdef W3D_WITH_STORMLIB
    for (void* h : archives_) SFileCloseArchive((HANDLE)h);
#endif
}

bool DataDir::openMpq(const std::string& dir) {
#ifdef W3D_WITH_STORMLIB
    std::vector<std::pair<int, fs::path>> found;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(dir, ec)) {
        if (e.is_regular_file(ec) && lower(e.path().extension().string()) == ".mpq")
            found.push_back({mpqPriority(e.path(), false), e.path()});
        else if (e.is_directory(ec) && e.path().filename().string().size() == 4)  // enUS, ruRU, ...
            for (auto& l : fs::directory_iterator(e.path(), ec))
                if (l.is_regular_file(ec) && lower(l.path().extension().string()) == ".mpq")
                    found.push_back({mpqPriority(l.path(), true), l.path()});
    }
    std::stable_sort(found.begin(), found.end(), [](auto& a, auto& b) { return a.first > b.first; });
    for (auto& [pr, path] : found) {
        HANDLE h = nullptr;
        if (SFileOpenArchive(path.string().c_str(), 0, STREAM_FLAG_READ_ONLY, &h)) {
            archives_.push_back(h);
            std::printf("  mpq [%4d] %s\n", pr, path.filename().string().c_str());
        } else {
            std::fprintf(stderr, "  cannot open %s (error %u)\n", path.string().c_str(), (unsigned)SErrGetLastError());
        }
    }
    return !archives_.empty();
#else
    (void)dir;
    std::fprintf(stderr, "this build has no MPQ support (configure with -DW3D_STORMLIB=ON)\n");
    return false;
#endif
}

bool DataDir::readMpq(const std::string& gamePath, std::vector<uint8_t>& out) const {
#ifdef W3D_WITH_STORMLIB
    std::string name = gamePath;
    std::replace(name.begin(), name.end(), '/', '\\');
    for (void* a : archives_) {
        HANDLE f = nullptr;
        if (!SFileOpenFileEx((HANDLE)a, name.c_str(), SFILE_OPEN_FROM_MPQ, &f)) continue;
        DWORD hi = 0, size = SFileGetFileSize(f, &hi);
        bool ok = false;
        if (size != SFILE_INVALID_SIZE && hi == 0) {
            out.resize(size);
            DWORD got = 0;
            ok = SFileReadFile(f, out.data(), size, &got, nullptr) || got == size;
            ok = ok && got == size;
        }
        SFileCloseFile(f);
        if (ok) return true;  // highest-priority archive that has the file wins
    }
#else
    (void)gamePath; (void)out;
#endif
    return false;
}

}  // namespace w3d
