#include "casc_source.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <mutex>

#ifdef W3D_WITH_CASC
#include <CascLib.h>
#endif

namespace w3d {

#ifdef W3D_WITH_CASC
namespace {
struct Storage {
    HANDLE h = nullptr;
    std::mutex mu;   // CascLib handles are not documented as thread safe for opens: serialise
    ~Storage() { if (h) CascCloseStorage(h); }
};
}  // namespace

bool openForever(DataDir& data, const std::string& wowDir, const std::string& wdtNames, std::string& err) {
    auto st = std::make_shared<Storage>();
    std::string used;
    for (const std::string& cand : {wowDir, wowDir + "/Data", wowDir + "\\Data"}) {
        if (CascOpenStorage(cand.c_str(), 0, &st->h)) { used = cand; break; }
    }
    if (used.empty()) { err = "cannot open the CASC storage in " + wowDir + " (CascLib error " + std::to_string(GetCascError()) + ")"; return false; }
    std::unordered_map<std::string, uint32_t> ids;
    std::vector<std::string> tries;
    if (!wdtNames.empty()) tries.push_back(wdtNames);
    tries.push_back("forever_wdt_names.csv");
    for (const std::string& t : tries) {
        std::ifstream in(t);
        if (!in) continue;
        std::string line;
        while (std::getline(in, line)) {
            // "775971;world/maps/azeroth/azeroth.wdt"
            const size_t c = line.find(';');
            if (c == std::string::npos) continue;
            const std::string path = line.substr(c + 1);
            const size_t m = path.find("world/maps/");
            if (m == std::string::npos) continue;
            const size_t e = path.find('/', m + 11);
            if (e == std::string::npos) continue;
            std::string k = path.substr(m + 11, e - m - 11);
            for (char& ch : k) if (ch >= 'A' && ch <= 'Z') ch = (char)(ch + 32);
            const uint32_t id = (uint32_t)strtoul(line.c_str(), nullptr, 10);
            if (id) ids[k] = id;
        }
        break;
    }
    if (ids.empty()) { err = "no map WDT table (forever_wdt_names.csv, lines: FileDataID;world/maps/<dir>/<dir>.wdt)"; return false; }
    data.setForever(
        [st](uint32_t id, std::vector<uint8_t>& out) {
            std::lock_guard<std::mutex> lk(st->mu);
            HANDLE f = nullptr;
            if (!CascOpenFile(st->h, CASC_FILE_DATA_ID(id), CASC_LOCALE_ALL, CASC_OPEN_BY_FILEID | CASC_OVERCOME_ENCRYPTED, &f)) return false;
            ULONGLONG size = 0;
            CascGetFileSize64(f, &size);
            bool ok = size < (512ull << 20);
            DWORD got = 0;
            if (ok) { out.resize((size_t)size); ok = size == 0 || (CascReadFile(f, out.data(), (DWORD)size, &got) && got == size); }
            CascCloseFile(f);
            return ok && size != 0;
        },
        std::move(ids));
    std::printf("  WoW Forever data (CASC) in %s\n", used.c_str());
    return true;
}
#else
bool openForever(DataDir&, const std::string&, const std::string&, std::string& err) {
    err = "this build has no CASC support (configure with -DW3D_CASC=ON)";
    return false;
}
#endif

}  // namespace w3d
