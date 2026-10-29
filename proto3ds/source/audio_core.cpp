#include "audio_core.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_SIMD
#include "minimp3.h"
#include "w3d_sound.h"

namespace snd {

namespace {
constexpr float kTile = 533.33333f;

// events without their own sound use another one's
const char* fallbackOf(const std::string& e) {
    static const std::pair<const char*, const char*> k[] = {
        {"bagsopen", "open"}, {"bagsclose", "close"}, {"spellsopen", "open"}, {"spellsclose", "close"},
        {"questsopen", "open"}, {"questsclose", "close"}, {"npcopen", "open"}, {"npcclose", "close"},
        {"auctionopen", "npcopen"}, {"auctionclose", "npcclose"}, {"questabandon", "close"}, {"mail", "click"},
        {"questaccept", "click"}, {"tab", "click"}, {"targetfriend", "targethostile"}, {"targetneutral", "targethostile"},
        {"invite", "whisper"},
    };
    for (const auto& [a, b] : k)
        if (e == a) return b;
    return nullptr;
}

std::vector<std::string> split(const char* line) {
    std::vector<std::string> out;
    std::string cur;
    for (const char* p = line; *p && *p != '\n' && *p != '\r'; ++p) {
        if (*p == '\t') { out.push_back(cur); cur.clear(); }
        else cur += *p;
    }
    out.push_back(cur);
    return out;
}
uint32_t num(const std::string& s) { return (uint32_t)strtoul(s.c_str(), nullptr, 10); }
}  // namespace

// ------------------------------------------------------------------ SfxBank
SfxBank::~SfxBank() { release(); }

void SfxBank::release() {
    if (blob_) (free_ ? free_ : ::free)(blob_);
    blob_ = nullptr;
    size_ = 0;
    clips_.clear();
}

bool SfxBank::load(const char* path, Alloc a, Free f) {
    release();
    FILE* fp = fopen(path, "rb");
    if (!fp) return false;
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (n < (long)sizeof(W3dSfxHeader)) { fclose(fp); return false; }
    blob_ = (uint8_t*)(a ? a((size_t)n) : malloc((size_t)n));
    free_ = f;
    if (!blob_) { fclose(fp); return false; }
    size_ = fread(blob_, 1, (size_t)n, fp);
    fclose(fp);
    W3dSfxHeader h;
    memcpy(&h, blob_, sizeof h);
    if (size_ != (size_t)n || h.magic != W3D_SFX_MAGIC || sizeof h + (size_t)h.count * sizeof(W3dSfxEntry) > size_) {
        release();
        return false;
    }
    for (uint32_t i = 0; i < h.count; ++i) {
        W3dSfxEntry e;
        memcpy(&e, blob_ + sizeof h + i * sizeof e, sizeof e);
        if ((e.offset & 1) || (size_t)e.offset + (size_t)e.samples * 2 > size_ || !e.samples) continue;
        clips_[e.key].push_back({(const int16_t*)(blob_ + e.offset), e.samples, e.rate ? e.rate : 22050u, e.volume / 1000.0f});
    }
    return !clips_.empty();
}

const Clip* SfxBank::pick(const char* event) {
    std::string e = event;
    for (int depth = 0; depth < 4 && !e.empty(); ++depth) {
        auto it = clips_.find(w3dSndHash(e.c_str()));
        if (it != clips_.end() && !it->second.empty()) return &it->second[rng_() % it->second.size()];
        const char* fb = fallbackOf(e);
        e = fb ? fb : "";
    }
    return nullptr;
}

bool SfxBank::synthesize(Alloc a, Free f) {
    release();
    // tone(ms, f0 -> f1 Hz, amplitude) segments per event; quiet, short, sine with a quick fade
    struct Seg { int ms; float f0, f1, amp; };
    struct Ev { const char* name; std::vector<Seg> segs; };
    const std::vector<Ev> evs = {
        {"click", {{25, 1800, 1500, 0.18f}}},
        {"tab", {{30, 1200, 1300, 0.16f}}},
        {"open", {{70, 500, 900, 0.18f}}},
        {"close", {{70, 900, 500, 0.16f}}},
        {"coin", {{60, 2400, 2400, 0.15f}, {90, 3200, 3200, 0.13f}}},
        {"levelup", {{110, 523, 523, 0.2f}, {110, 659, 659, 0.2f}, {110, 784, 784, 0.2f}, {260, 1046, 1046, 0.2f}}},
        {"questcomplete", {{120, 660, 660, 0.2f}, {200, 880, 880, 0.2f}}},
        {"discover", {{100, 784, 784, 0.18f}, {100, 988, 988, 0.18f}, {100, 1175, 1175, 0.18f}, {320, 1568, 1568, 0.18f}}},
        {"whisper", {{90, 1400, 1400, 0.16f}, {120, 1000, 1000, 0.14f}}},
        {"targethostile", {{35, 700, 600, 0.14f}}},
        {"targetlost", {{35, 500, 400, 0.10f}}},
    };
    const uint32_t rate = 22050;
    std::vector<std::pair<const char*, std::vector<int16_t>>> pcm;
    size_t total = 0;
    for (const Ev& ev : evs) {
        std::vector<int16_t> s;
        for (const Seg& g : ev.segs) {
            const int n = g.ms * (int)rate / 1000;
            float ph = 0;
            for (int i = 0; i < n; ++i) {
                float t = (float)i / n, fr = g.f0 + (g.f1 - g.f0) * t;
                ph += 6.2831853f * fr / rate;
                float env = std::min(1.0f, i / (rate * 0.003f)) * (1.0f - t) * (1.0f - t);
                s.push_back((int16_t)(sinf(ph) * env * g.amp * 32767));
            }
        }
        total += s.size() * 2;
        pcm.push_back({ev.name, std::move(s)});
    }
    blob_ = (uint8_t*)(a ? a(total) : malloc(total));
    free_ = f;
    if (!blob_) return false;
    size_ = total;
    size_t off = 0;
    for (auto& [name, s] : pcm) {
        memcpy(blob_ + off, s.data(), s.size() * 2);
        clips_[w3dSndHash(name)].push_back({(const int16_t*)(blob_ + off), (uint32_t)s.size(), rate, 1.0f});
        off += s.size() * 2;
    }
    return true;
}

// ------------------------------------------------------------------ MusicTable
bool MusicTable::load(const char* dir) {
    dir_ = dir;
    FILE* f = fopen((dir_ + "/music.txt").c_str(), "r");
    if (!f) return false;
    char line[2048];
    while (fgets(line, sizeof line, f)) {
        std::vector<std::string> c = split(line);
        if (c[0] == "A" && c.size() >= 5) areas_[num(c[1])] = {num(c[2]), num(c[3]), num(c[4])};
        else if (c[0] == "Z" && c.size() >= 6) zm_[num(c[1])] = {num(c[2]), num(c[3]), num(c[4]), num(c[5])};
        else if (c[0] == "Y" && c.size() >= 4) amb_[num(c[1])] = {num(c[2]), num(c[3])};
        else if ((c[0] == "S" || c[0] == "B") && c.size() >= 3) {
            SoundSet& s = sounds_[num(c[1])];
            s.volume = std::clamp(num(c[2]) / 100.0f, 0.05f, 1.0f);
            for (size_t k = 3; k < c.size(); ++k)
                if (!c[k].empty()) s.files.push_back(c[k]);
        } else if (c[0] == "M" && c.size() >= 3) mapArea_[num(c[1])] = num(c[2]);
        else if (c[0] == "G" && c.size() >= 3) {
            glue_.volume = std::clamp(num(c[1]) / 100.0f, 0.05f, 1.0f);
            for (size_t k = 2; k < c.size(); ++k)
                if (!c[k].empty()) glue_.files.push_back(c[k]);
        }
    }
    fclose(f);
    return !areas_.empty();
}

uint32_t MusicTable::areaAt(uint32_t map, float x, float y) {
    Grid& g = grids_[map];
    if (!g.tried) {
        g.tried = true;
        FILE* f = fopen((dir_ + "/areas_" + std::to_string(map) + ".bin").c_str(), "rb");
        if (f) {
            W3dAreaHeader h{};
            if (fread(&h, sizeof h, 1, f) == 1 && h.magic == W3D_AREA_MAGIC && h.tiles <= 4096) {
                g.slots.resize(64 * 64);
                g.areas.resize((size_t)h.tiles * 256);
                if (fread(g.slots.data(), 2, g.slots.size(), f) != g.slots.size() ||
                    fread(g.areas.data(), 2, g.areas.size(), f) != g.areas.size()) {
                    g.slots.clear();
                    g.areas.clear();
                }
            }
            fclose(f);
        }
    }
    if (!g.slots.empty()) {
        const float u = 32.0f - y / kTile, v = 32.0f - x / kTile;   // tileX along world Y, tileY along world X
        const int tx = (int)floorf(u), ty = (int)floorf(v);
        if (tx >= 0 && tx < 64 && ty >= 0 && ty < 64) {
            const uint16_t slot = g.slots[tx * 64 + ty];
            if (slot != 0xFFFF && (size_t)slot * 256 + 256 <= g.areas.size()) {
                const int cx = std::clamp((int)((u - tx) * 16), 0, 15), cy = std::clamp((int)((v - ty) * 16), 0, 15);
                const uint16_t a = g.areas[(size_t)slot * 256 + cy * 16 + cx];
                if (a) return a;
            }
        }
    }
    auto it = mapArea_.find(map);
    return it == mapArea_.end() ? 0 : it->second;
}

ZoneSound MusicTable::zone(uint32_t area) const {
    ZoneSound z;
    uint32_t a = area;
    for (int depth = 0; depth < 4 && a && (!z.music || !z.ambience); ++depth) {
        auto it = areas_.find(a);
        if (it == areas_.end()) break;
        if (!z.music) z.music = it->second.music;
        if (!z.ambience) z.ambience = it->second.ambience;
        a = it->second.parent;
    }
    auto set = [&](uint32_t id) -> const SoundSet* {
        auto it = sounds_.find(id);
        return it == sounds_.end() || it->second.files.empty() ? nullptr : &it->second;
    };
    if (auto it = zm_.find(z.music); it != zm_.end()) {
        z.silMinMs = it->second.silMin;
        z.silMaxMs = std::max(it->second.silMin, it->second.silMax);
        z.day = set(it->second.day);
        z.night = set(it->second.night);
    }
    if (auto it = amb_.find(z.ambience); it != amb_.end()) {
        z.ambDay = set(it->second.first);
        z.ambNight = set(it->second.second);
    }
    return z;
}

// ------------------------------------------------------------------ Director
std::string Director::pickFrom(const MusicTable& t, const SoundSet* s) {
    if (!s || s->files.empty()) return {};
    std::string p = t.path(s->files[rng_() % s->files.size()]);
    for (int i = 0; i < 4 && s->files.size() > 1 && p == lastTrack_; ++i) p = t.path(s->files[rng_() % s->files.size()]);   // another track than the last
    lastTrack_ = p;
    return p;
}

void Director::update(const MusicTable& t, uint32_t area, bool night, uint32_t nowMs) {
    // stage 5.43: standing on an area border made the music jump between two zones' tracks and cut every one short; the
    // music follows an area only after it held for a second (the first area at once)
    if (area != cand_) { cand_ = area; candMs_ = nowMs; }
    const bool areaChanged = area != area_ && (area_ == ~0u || (int32_t)(nowMs - candMs_) >= 1000);
    if (areaChanged || night != night_) {
        area = areaChanged ? area : area_;
        area_ = area;
        night_ = night;
        zone_ = t.zone(area);
        const SoundSet* m = night && zone_.night ? zone_.night : zone_.day ? zone_.day : zone_.night;
        const SoundSet* a = night && zone_.ambNight ? zone_.ambNight : zone_.ambDay ? zone_.ambDay : zone_.ambNight;
        const uint32_t mid = zone_.music * 2 + (m == zone_.night ? 1 : 0), aid = zone_.ambience * 2 + (a == zone_.ambNight ? 1 : 0);
        bool keep = false;   // the playing track belongs to the new music too (other ZoneMusic rows list the same files): let it finish
        if (m && !music_.empty())
            for (const std::string& f : m->files) keep |= t.path(f) == music_;
        if (mid != musicId_ && keep) {
            musicId_ = mid;
            musicVol_ = m->volume;
        } else if (mid != musicId_) {   // other music: a track of it right away (as entering a zone in the original)
            musicId_ = mid;
            music_ = pickFrom(t, m);
            musicVol_ = m ? m->volume : 1;
            pendingTrack_ = false;
        }
        if (aid != ambId_) {
            ambId_ = aid;
            ambience_ = pickFrom(t, a);
            ambVol_ = a ? a->volume : 1;
        }
    }
    if (pendingTrack_ && (int32_t)(nowMs - silenceUntil_) >= 0) {
        pendingTrack_ = false;
        const SoundSet* m = night_ && zone_.night ? zone_.night : zone_.day ? zone_.day : zone_.night;
        music_ = pickFrom(t, m);
    }
}

void Director::musicEnded(uint32_t nowMs) {
    music_.clear();
    // ZoneMusic asks for 2-5 minutes of silence between tracks; on a handheld that felt like the music had stopped
    // (stage 5.31: a quarter of it, 5..45 s; stage 5.43: an eighth, 3..20 s).
    uint32_t lo = std::clamp<uint32_t>(zone_.silMinMs / 8, 3000u, 10000u);
    uint32_t hi = std::clamp<uint32_t>(std::max(zone_.silMaxMs, zone_.silMinMs) / 8, lo, 20000u);
    silenceUntil_ = nowMs + lo + (hi > lo ? rng_() % (hi - lo + 1) : 0);
    pendingTrack_ = true;
}

// ------------------------------------------------------------------ WorldBank
WorldBank::~WorldBank() { if (f_) fclose(f_); }

bool WorldBank::load(const char* path) {
    if (f_) fclose(f_);
    f_ = fopen(path, "rb");
    clips_.clear();
    if (!f_) return false;
    W3dSfxHeader h{};
    if (fread(&h, sizeof h, 1, f_) != 1 || h.magic != W3D_WORLD_MAGIC || h.count > 200000) {
        fclose(f_);
        f_ = nullptr;
        return false;
    }
    std::vector<W3dWorldEntry> e(h.count);
    if (h.count && fread(e.data(), sizeof(W3dWorldEntry), h.count, f_) != h.count) {
        fclose(f_);
        f_ = nullptr;
        return false;
    }
    for (const W3dWorldEntry& x : e) {
        if (!x.samples || x.samples > 22050u * 10) continue;
        clips_[x.key].push_back({x.offset, x.samples, x.rate ? x.rate : 22050u, x.volume / 1000.0f,
                                 std::max(0.5f, x.minDist / 10.0f), std::max(1.0f, x.cutoff / 10.0f)});
    }
    return true;
}

int WorldBank::variants(uint32_t id) const {
    auto it = clips_.find(id);
    return it == clips_.end() ? 0 : (int)it->second.size();
}

const WorldClip* WorldBank::clip(uint32_t id, int k) const {
    auto it = clips_.find(id);
    if (it == clips_.end() || k < 0 || k >= (int)it->second.size()) return nullptr;
    return &it->second[k];
}

bool WorldBank::decode(const WorldClip& c, int16_t* out) {
    if (!f_) return false;
    const uint32_t blocks = (c.samples + kW3dImaBlock - 1) / kW3dImaBlock;
    if (fseek(f_, (long)c.offset, SEEK_SET)) return false;
    uint8_t blk[kW3dImaBlockBytes];
    int16_t pcm[kW3dImaBlock];
    for (uint32_t b = 0; b < blocks; ++b) {
        if (fread(blk, 1, sizeof blk, f_) != sizeof blk) return false;
        w3dImaDecodeBlock(blk, pcm);
        const uint32_t n = std::min<uint32_t>(kW3dImaBlock, c.samples - b * kW3dImaBlock);
        memcpy(out + (size_t)b * kW3dImaBlock, pcm, n * 2);
    }
    return true;
}

// ------------------------------------------------------------------ SoundTables
bool SoundTables::load(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) return false;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        std::vector<std::string> c = split(line);
        if (c[0] == "D" && c.size() >= 3) display_[num(c[1])] = num(c[2]);
        else if (c[0] == "K" && c.size() >= 9)
            csd_[num(c[1])] = {num(c[2]), num(c[3]), num(c[4]), num(c[5]), num(c[6]), num(c[7]), num(c[8])};
        else if (c[0] == "W" && c.size() >= 4) weapon_[num(c[1])] = {num(c[2]), num(c[3])};
        else if (c[0] == "F" && c.size() >= 3) footstep_[num(c[1])] = num(c[2]);
        else if (c[0] == "Q" && c.size() >= 7) visual_[num(c[1])] = {num(c[2]), num(c[3]), num(c[4]), num(c[5]), num(c[6])};
        else if (c[0] == "P" && c.size() >= 3) spellVisual_[num(c[1])] = num(c[2]);
        else if (c[0] == "L" && c.size() >= 3) liquid_[c[1]] = num(c[2]);
        else if (c[0] == "N" && c.size() >= 5) npcVoice_[num(c[1])] = {num(c[2]), num(c[3]), num(c[4])};
        else if (c[0] == "V" && c.size() >= 5) errorVoice_[c[1] + " " + c[2] + " " + c[3]] = num(c[4]);
        else if (c[0] == "T" && c.size() >= 5)
            emoteVoice_[((uint64_t)num(c[1]) << 16) | ((num(c[2]) & 0xFF) << 8) | (num(c[3]) & 0xFF)] = num(c[4]);
    }
    fclose(f);
    loaded_ = true;
    return true;
}

const CreatureSounds* SoundTables::creature(uint32_t displayId) const {
    auto d = display_.find(displayId);
    if (d == display_.end()) return nullptr;
    auto it = csd_.find(d->second);
    return it == csd_.end() ? nullptr : &it->second;
}

std::pair<uint32_t, uint32_t> SoundTables::weapon(uint32_t subclass) const {
    auto it = weapon_.find(subclass);
    return it == weapon_.end() ? std::pair<uint32_t, uint32_t>{0, 0} : it->second;
}

uint32_t SoundTables::footstep(uint32_t id) const {
    auto it = footstep_.find(id);
    if (it == footstep_.end()) it = footstep_.find(0);
    if (it == footstep_.end() && !footstep_.empty()) it = footstep_.begin();
    return it == footstep_.end() ? 0 : it->second;
}

uint32_t SoundTables::liquid(const char* kind) const {
    auto it = liquid_.find(kind);
    return it == liquid_.end() ? 0 : it->second;
}

const NpcVoice* SoundTables::npcVoice(uint32_t displayId) const {
    auto it = npcVoice_.find(displayId);
    return it == npcVoice_.end() ? nullptr : &it->second;
}

uint32_t SoundTables::errorVoice(uint32_t race, uint32_t gender, const char* token) const {
    auto it = errorVoice_.find(std::to_string(race) + " " + std::to_string(gender) + " " + token);
    return it == errorVoice_.end() ? 0 : it->second;
}

uint32_t SoundTables::emoteVoice(uint32_t textEmote, uint32_t race, uint32_t gender) const {
    auto it = emoteVoice_.find(((uint64_t)textEmote << 16) | ((race & 0xFF) << 8) | (gender & 0xFF));
    return it == emoteVoice_.end() ? 0 : it->second;
}

const SpellSounds* SoundTables::spell(uint32_t spellId) const {
    auto p = spellVisual_.find(spellId);
    if (p == spellVisual_.end()) return nullptr;
    auto it = visual_.find(p->second);
    return it == visual_.end() ? nullptr : &it->second;
}

bool spatialize(float x, float y, float lx, float ly, float yaw, float minDist, float cutoff, float* left, float* right) {
    const float dx = x - lx, dy = y - ly, d = sqrtf(dx * dx + dy * dy);
    if (d >= cutoff) return false;
    float g = d <= minDist ? 1.0f : 1.0f - (d - minDist) / std::max(0.1f, cutoff - minDist);
    g = g * g;   // a softer tail
    float pan = 0;
    if (d > 0.5f) {
        // angle of the source relative to the facing; positive = to the left (counter-clockwise)
        const float rel = atan2f(dy, dx) - yaw;
        pan = -sinf(rel) * std::min(1.0f, d / 4.0f);   // close sources stay centred
    }
    *left = g * sqrtf(0.5f * (1.0f - pan));
    *right = g * sqrtf(0.5f * (1.0f + pan));
    return g > 0.01f;
}

// ------------------------------------------------------------------ Stream
struct Stream::Mp3 {
    mp3dec_t dec;
    std::vector<uint8_t> in;          // undecoded bytes
    size_t inPos = 0;
    bool eof = false;
};

Stream::Stream() = default;
Stream::~Stream() { close(); }

void Stream::close() {
    if (f_) fclose(f_);
    f_ = nullptr;
    m_.reset();
    pcm_.clear();
    pcmPos_ = 0;
    framesRead_ = 0;
    endReason_ = 0;
}

bool Stream::open(const std::string& path) {
    close();
    path_ = path;
    f_ = fopen(path.c_str(), "rb");
    if (!f_) return false;
    const bool ima = path.size() > 4 && path.compare(path.size() - 4, 4, ".ima") == 0;
    if (ima) {
        W3dImaHeader h{};
        if (fread(&h, sizeof h, 1, f_) != 1 || h.magic != W3D_IMA_MAGIC || !h.rate) { close(); return false; }
        mp3_ = false;
        channels_ = 1;
        rate_ = h.rate;
        imaLeft_ = h.samples;
        return true;
    }
    mp3_ = true;
    m_ = std::make_unique<Mp3>();
    mp3dec_init(&m_->dec);
    {   // an ID3v2 tag in front (cover art can be hundreds of KB and holds byte runs that look like frame headers): skip it
        uint8_t id[10];
        if (fread(id, 1, 10, f_) == 10 && id[0] == 'I' && id[1] == 'D' && id[2] == '3' && !((id[6] | id[7] | id[8] | id[9]) & 0x80)) {
            const long size = ((long)id[6] << 21) | ((long)id[7] << 14) | ((long)id[8] << 7) | (long)id[9];
            fseek(f_, 10 + size + ((id[5] & 0x10) ? 10 : 0), SEEK_SET);
        } else {
            fseek(f_, 0, SEEK_SET);
        }
    }
    // the first frame tells the rate and the channels
    channels_ = 0;
    if (!fillMp3() || !channels_) { close(); return false; }
    return true;
}

bool Stream::rewind() { return open(path_); }

bool Stream::fillMp3() {   // decodes at least one more frame into pcm_ (false at the end)
    Mp3& m = *m_;
    mp3d_sample_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    // Only the end of the file ends a track. (It used to give up after 64 frames without samples and drop the whole
    // buffer on the first stretch it could not sync on, which cut tracks short.)
    for (int tries = 0; tries < 20000; ++tries) {
        if (m.in.size() - m.inPos < 16384 && !m.eof) {
            m.in.erase(m.in.begin(), m.in.begin() + (long)m.inPos);
            m.inPos = 0;
            size_t old = m.in.size();
            m.in.resize(old + 32768);
            size_t got = fread(m.in.data() + old, 1, 32768, f_);
            m.in.resize(old + got);
            if (!got) m.eof = true;
        }
        if (m.inPos >= m.in.size()) { endReason_ = 1; return false; }
        mp3dec_frame_info_t info;
        int n = mp3dec_decode_frame(&m.dec, m.in.data() + m.inPos, (int)(m.in.size() - m.inPos), pcm, &info);
        if (!info.frame_bytes) {
            if (m.eof) { endReason_ = 1; return false; }
            m.inPos += std::min<size_t>(1024, m.in.size() - m.inPos);   // no frame here: step on, the buffer is refilled below
            continue;
        }
        m.inPos += info.frame_bytes;
        if (!n) continue;
        if (!channels_) {
            channels_ = std::min(2, info.channels);
            rate_ = (uint32_t)info.hz;
        }
        if (pcmPos_ > 0) {
            pcm_.erase(pcm_.begin(), pcm_.begin() + (long)pcmPos_);
            pcmPos_ = 0;
        }
        for (int i = 0; i < n; ++i) {
            if (channels_ == 2) {
                pcm_.push_back(pcm[i * info.channels]);
                pcm_.push_back(pcm[i * info.channels + (info.channels > 1 ? 1 : 0)]);
            } else {
                int v = 0;
                for (int c = 0; c < info.channels; ++c) v += pcm[i * info.channels + c];
                pcm_.push_back((int16_t)(v / info.channels));
            }
        }
        return true;
    }
    endReason_ = 2;
    return false;
}

int Stream::read(int16_t* out, int frames) {
    if (!f_) return 0;
    int done = 0;
    while (done < frames) {
        size_t avail = (pcm_.size() - pcmPos_) / channels_;
        if (!avail) {
            if (mp3_) {
                if (!fillMp3()) break;
            } else {
                if (!imaLeft_) break;
                uint8_t blk[kW3dImaBlockBytes];
                if (fread(blk, 1, sizeof blk, f_) != sizeof blk) { imaLeft_ = 0; break; }
                pcm_.resize(kW3dImaBlock);
                pcmPos_ = 0;
                w3dImaDecodeBlock(blk, pcm_.data());
                if (imaLeft_ < (uint32_t)kW3dImaBlock) pcm_.resize(imaLeft_);
                imaLeft_ -= (uint32_t)pcm_.size();
            }
            continue;
        }
        size_t take = std::min<size_t>(avail, (size_t)(frames - done));
        memcpy(out + (size_t)done * channels_, pcm_.data() + pcmPos_, take * channels_ * 2);
        pcmPos_ += take * channels_;
        done += (int)take;
    }
    framesRead_ += (uint64_t)done;
    return done;
}

}  // namespace snd
