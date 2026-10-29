// audio.cpp — the NDSP half of the sound (audio_core.cpp is the platform-free half).
// Channels: 0 music (mono / stereo stream), 1 ambience (mono stream), 2..7 event sounds (PCM16 from sfx.w3ds,
// kept in linear memory). A thread (core 2 on New 3DS, next to the streamer) decodes the streams into four wave
// buffers per channel, fades between tracks and follows the zone the main thread reports.
#include "audio.hpp"

#include <3ds.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <random>
#include <string>
#include <unordered_map>

#include "audio_core.hpp"
#include "w3d_sound.h"

namespace audio {

namespace {
constexpr int kMusicCh = 0, kAmbCh = 1, kSfxCh0 = 2, kSfxChN = 8, kWorldCh0 = 8, kWorldChN = 24;
constexpr size_t kWorldCacheBytes = 3u << 20;   // decoded world sounds kept in linear memory
constexpr int kBufs = 4, kBufFrames = 4096;

struct Voice {
    int ch = 0;
    snd::Stream s;
    std::string playing, wanted;
    int16_t* mem = nullptr;           // kBufs * kBufFrames * 2 (stereo) samples, linear memory
    ndspWaveBuf wb[kBufs];
    float vol = 0, gain = 1;          // fade 0..1 and the sound's own volume
    bool started = false, ended = false, loop = false;
};

bool g_ok = false;
uint64_t g_t0 = 0;
Thread g_thread = nullptr;
std::atomic<bool> g_quit{false};
LightLock g_lock;
snd::SfxBank g_sfx;
snd::MusicTable g_music;
snd::Director g_dir;
Voice g_voice[2];
int g_nextSfx = kSfxCh0;
// set by the main thread (under g_lock)
uint32_t g_map = 0;
float g_x = 0, g_y = 0;
bool g_inWorld = false;
float g_musicVol = 0.6f, g_sfxVol = 0.8f, g_ambVol = 0.5f;
uint32_t g_area = 0;
char g_status[128] = "sound: off";
std::string g_soundsTxt;
// world sounds
snd::WorldBank g_world;
struct Req { uint32_t sound; float x, y, z; bool self, stop; uint64_t tag; float gain; uint32_t loopMs; };
Req g_reqs[64];
int g_reqHead = 0, g_reqTail = 0;
float g_lx = 0, g_ly = 0, g_lz = 0, g_lyaw = 0;
LightEvent g_wake;
struct Cached { int16_t* pcm = nullptr; uint32_t samples = 0, rate = 0, lastUse = 0; float volume = 1; };
std::unordered_map<uint32_t, Cached> g_cache;   // (id << 3 | variant) -> samples
size_t g_cacheBytes = 0;
struct WorldVoice { uint32_t key = 0; uint64_t tag = 0; uint32_t startMs = 0, endMs = 0; ndspWaveBuf wb; };   // endMs: a looping precast's last moment
WorldVoice g_wv[kWorldChN - kWorldCh0];
// stage 6 (perf log)
int g_core = -1;
std::atomic<uint32_t> g_misses{0}, g_fails{0}, g_busyUs{0}, g_requests{0}, g_dropped{0};
std::minstd_rand g_rng(7);
uint32_t g_worldPlayed = 0;

bool keyPlaying(uint32_t key) {
    for (int i = 0; i < kWorldChN - kWorldCh0; ++i)
        if (g_wv[i].key == key && ndspChnIsPlaying(kWorldCh0 + i)) return true;
    return false;
}

const Cached* cached(uint32_t id, int k, uint32_t now) {
    const uint32_t key = id << 3 | (uint32_t)k;
    auto it = g_cache.find(key);
    if (it != g_cache.end()) { it->second.lastUse = now; return &it->second; }
    const snd::WorldClip* c = g_world.clip(id, k);
    if (!c) return nullptr;
    const size_t bytes = ((size_t)c->samples + kW3dImaBlock) * 2;
    while (g_cacheBytes + bytes > kWorldCacheBytes && !g_cache.empty()) {   // the least recently used, not playing
        auto victim = g_cache.end();
        for (auto i = g_cache.begin(); i != g_cache.end(); ++i)
            if (!keyPlaying(i->first) && (victim == g_cache.end() || i->second.lastUse < victim->second.lastUse)) victim = i;
        if (victim == g_cache.end()) { ++g_fails; return nullptr; }
        linearFree(victim->second.pcm);
        g_cacheBytes -= ((size_t)victim->second.samples + kW3dImaBlock) * 2;
        g_cache.erase(victim);
    }
    Cached e;
    ++g_misses;
    e.pcm = (int16_t*)linearAlloc(bytes);
    if (!e.pcm) { ++g_fails; return nullptr; }
    if (!g_world.decode(*c, e.pcm)) { linearFree(e.pcm); return nullptr; }
    DSP_FlushDataCache(e.pcm, c->samples * 2);
    e.samples = c->samples;
    e.rate = c->rate;
    e.volume = c->volume;
    e.lastUse = now;
    g_cacheBytes += bytes;
    return &(g_cache[key] = e);
}

void runRequest(const Req& r, uint32_t now, float sfxVol) {
    if (r.stop) {
        for (int i = 0; i < kWorldChN - kWorldCh0; ++i)
            if (g_wv[i].tag == r.tag && r.tag) { ndspChnWaveBufClear(kWorldCh0 + i); g_wv[i].tag = 0; g_wv[i].endMs = 0; }
        return;
    }
    const int n = g_world.variants(r.sound);
    if (!n || sfxVol <= 0) return;
    const snd::WorldClip* wc = g_world.clip(r.sound, 0);
    float l = 0.7f, rr = 0.7f;
    if (!r.self && !snd::spatialize(r.x, r.y, g_lx, g_ly, g_lyaw, wc->minDist, wc->cutoff, &l, &rr)) return;
    const int k = (int)(g_rng() % (uint32_t)n);
    const Cached* c = cached(r.sound, k, now);
    if (!c) return;
    int slot = -1;   // a free channel, else the one playing longest
    for (int i = 0; i < kWorldChN - kWorldCh0 && slot < 0; ++i)
        if (!ndspChnIsPlaying(kWorldCh0 + i)) slot = i;
    if (slot < 0) {
        slot = 0;
        for (int i = 1; i < kWorldChN - kWorldCh0; ++i)
            if (g_wv[i].startMs < g_wv[slot].startMs) slot = i;
    }
    const int ch = kWorldCh0 + slot;
    WorldVoice& v = g_wv[slot];
    ndspChnWaveBufClear(ch);
    ndspChnReset(ch);
    ndspChnSetInterp(ch, NDSP_INTERP_LINEAR);
    ndspChnSetRate(ch, (float)c->rate);
    ndspChnSetFormat(ch, NDSP_FORMAT_MONO_PCM16);
    float mix[12] = {};
    const float g = sfxVol * c->volume * r.gain;
    mix[0] = l * g;
    mix[1] = rr * g;
    ndspChnSetMix(ch, mix);
    memset(&v.wb, 0, sizeof v.wb);
    v.wb.data_vaddr = c->pcm;
    v.wb.nsamples = c->samples;
    v.key = r.sound << 3 | (uint32_t)k;
    v.tag = r.tag;
    v.startMs = now;
    v.endMs = 0;
    // a cast sound shorter than the cast repeats until SPELL_GO / failure stops it (the safety end: cast time + 0.8 s)
    if (r.tag && r.loopMs && (uint64_t)c->samples * 1000 < (uint64_t)c->rate * r.loopMs) {
        v.wb.looping = true;
        v.endMs = now + r.loopMs + 800;
    }
    ndspChnWaveBufAdd(ch, &v.wb);
    ++g_worldPlayed;
}

// sdmc:/3ds/w3dproto/log_music.txt (stage 5.43): when a music track starts, ends or is cut, how long it played and why -
// so that "the music ends too fast" can be told apart (end of file / decoder gave up / the zone changed / fade out).
void logMusic(const char* what, const Voice& v, const std::string& other = std::string()) {
    static int lines = 0;
    if (v.ch != kMusicCh || lines >= 300) return;
    FILE* f = fopen("sdmc:/3ds/w3dproto/log_music.txt", lines ? "ab" : "wb");
    if (!f) return;
    ++lines;
    const std::string& path = v.playing.empty() ? other : v.playing;
    const size_t slash = path.find_last_of('/');
    fprintf(f, "%8u ms  %-6s %s  played %.1f s  end=%d  vol %.2f\n", (unsigned)osGetTime() - (unsigned)g_t0, what,
            slash == std::string::npos ? path.c_str() : path.c_str() + slash + 1,
            v.s.rate() ? (double)v.s.framesRead() / v.s.rate() : 0.0, v.s.endReason(), v.vol);
    fclose(f);
}

void setMix(int ch, float v) {
    float mix[12] = {};
    mix[0] = mix[1] = v;
    ndspChnSetMix(ch, mix);
}

void stopVoice(Voice& v) {
    if (v.started) logMusic(v.ended ? "ended" : "cut", v);
    ndspChnWaveBufClear(v.ch);
    v.s.close();
    v.playing.clear();
    v.started = false;
    for (ndspWaveBuf& w : v.wb) w.status = NDSP_WBUF_FREE;
}

bool startVoice(Voice& v, const std::string& path) {
    stopVoice(v);
    if (path.empty() || !v.s.open(path)) return false;
    v.playing = path;
    ndspChnReset(v.ch);
    ndspChnSetInterp(v.ch, NDSP_INTERP_LINEAR);
    ndspChnSetRate(v.ch, (float)v.s.rate());
    ndspChnSetFormat(v.ch, v.s.channels() == 2 ? NDSP_FORMAT_STEREO_PCM16 : NDSP_FORMAT_MONO_PCM16);
    v.started = true;
    v.ended = false;
    logMusic("start", v);
    return true;
}

// Refills the free wave buffers; false when the stream ran out and nothing is queued any more.
bool feed(Voice& v) {
    if (!v.started) return false;
    bool queued = false;
    for (int i = 0; i < kBufs; ++i) {
        ndspWaveBuf& w = v.wb[i];
        if (w.status == NDSP_WBUF_QUEUED || w.status == NDSP_WBUF_PLAYING) { queued = true; continue; }
        if (v.ended) continue;
        int16_t* buf = v.mem + (size_t)i * kBufFrames * 2;
        int got = v.s.read(buf, kBufFrames);
        if (got < kBufFrames && v.loop && v.s.rewind()) got += v.s.read(buf + (size_t)got * v.s.channels(), kBufFrames - got);
        if (got <= 0) { v.ended = true; continue; }
        memset(&w, 0, sizeof w);
        w.data_pcm16 = buf;
        w.nsamples = (u32)got;
        DSP_FlushDataCache(buf, (u32)got * v.s.channels() * 2);
        ndspChnWaveBufAdd(v.ch, &w);
        queued = true;
    }
    return queued;
}

// Fades the voice toward its wanted file: out, switch, in.
void steer(Voice& v, float target, float dt) {
    const bool switching = v.wanted != v.playing;
    const float goal = switching ? 0.0f : target;
    const float speed = switching ? 1.0f / 1.5f : 1.0f / 2.5f;   // 1.5 s out, 2.5 s in
    if (v.vol < goal) v.vol = std::min(goal, v.vol + speed * dt);
    else v.vol = std::max(goal, v.vol - speed * dt);
    if (switching && (v.vol <= 0.001f || !v.started)) {
        v.vol = 0;
        if (!v.wanted.empty()) startVoice(v, v.wanted);
        else stopVoice(v);
        v.playing = v.wanted;   // a file that fails to open is not retried every frame
    }
    if (v.started) setMix(v.ch, v.vol * v.gain);
}

void threadMain(void*) {
    uint64_t last = osGetTime();
    while (!g_quit) {
        const uint64_t nowT = osGetTime();
        const float dt = std::min(0.2f, (nowT - last) / 1000.0f);
        last = nowT;
        const uint32_t now = (uint32_t)nowT;
        const u64 busy0 = svcGetSystemTick();
        LightLock_Lock(&g_lock);
        const uint32_t map = g_map;
        const float x = g_x, y = g_y, mv = g_musicVol, av = g_ambVol;
        const bool inWorld = g_inWorld;
        const float sfxVol = g_sfxVol;
        Req batch[64];
        int nb = 0;
        while (g_reqHead != g_reqTail) {
            batch[nb++] = g_reqs[g_reqTail];
            g_reqTail = (g_reqTail + 1) % 64;
        }
        LightLock_Unlock(&g_lock);
        for (int i = 0; i < nb; ++i) runRequest(batch[i], now, sfxVol);
        for (int i = 0; i < kWorldChN - kWorldCh0; ++i)   // a looping precast whose stop never came
            if (g_wv[i].endMs && (int32_t)(now - g_wv[i].endMs) >= 0) { ndspChnWaveBufClear(kWorldCh0 + i); g_wv[i].endMs = 0; g_wv[i].tag = 0; }
        if (g_music.loaded()) {
            const time_t t = time(nullptr);
            const struct tm* lt = localtime(&t);
            const bool night = lt && (lt->tm_hour < 6 || lt->tm_hour >= 21);
            const uint32_t area = inWorld ? g_music.areaAt(map, x, y) : 0;
            g_area = area;
            g_dir.update(g_music, area, night, now);
            Voice& m = g_voice[0];
            Voice& a = g_voice[1];
            m.wanted = inWorld && mv > 0 ? g_dir.music() : std::string();
            a.wanted = inWorld && av > 0 ? g_dir.ambience() : std::string();
            m.gain = mv * g_dir.musicVolume();
            if (!inWorld && mv > 0 && g_music.glue()) {   // the login / character screens: their theme, looping
                const snd::SoundSet* gs = g_music.glue();
                m.wanted = m.playing.find("/music/glue_") != std::string::npos ? m.playing : g_music.path(gs->files[0]);
                m.gain = mv * gs->volume;
            }
            m.loop = !inWorld;
            a.gain = av * g_dir.ambienceVolume();
            steer(m, 1, dt);
            steer(a, 1, dt);
            if (!feed(m) && m.started && m.ended) {   // the track is over: a pause, then another one
                stopVoice(m);
                g_dir.musicEnded(now);
                m.wanted = m.playing = g_dir.music();
            }
            feed(a);
        }
        g_busyUs += (uint32_t)((svcGetSystemTick() - busy0) * 1000000ull / SYSCLOCK_ARM11);
        LightEvent_WaitTimeout(&g_wake, 15 * 1000000LL);   // a sound request wakes it at once
    }
}
}  // namespace

bool init(const char* soundDir) {
    g_t0 = osGetTime();
    if (R_FAILED(ndspInit())) {
        snprintf(g_status, sizeof g_status, "sound: off (no sdmc:/3ds/dspfirm.cdc - run DSP1)");
        return false;
    }
    g_ok = true;
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    LightLock_Init(&g_lock);
    LightEvent_Init(&g_wake, RESET_ONESHOT);
    std::string dir = soundDir;
    g_world.load((dir + "/world.w3ds").c_str());
    g_soundsTxt = dir + "/sounds.txt";
    const bool pack = g_sfx.load((dir + "/sfx.w3ds").c_str(), [](size_t n) { return linearAlloc(n); }, linearFree);
    if (!pack) g_sfx.synthesize([](size_t n) { return linearAlloc(n); }, linearFree);
    DSP_FlushDataCache(nullptr, 0);
    const bool music = g_music.load(soundDir);
    for (int i = 0; i < 2; ++i) {
        g_voice[i].ch = i == 0 ? kMusicCh : kAmbCh;
        g_voice[i].loop = i == 1;
        g_voice[i].mem = (int16_t*)linearAlloc((size_t)kBufs * kBufFrames * 2 * sizeof(int16_t));
    }
    snprintf(g_status, sizeof g_status, "sound: %s %u, world %u, music %s", pack ? "sfx" : "built-in", (unsigned)g_sfx.count(),
             (unsigned)g_world.count(), music ? "on" : "none");
    if (g_voice[0].mem && g_voice[1].mem) {
        s32 prio = 0x30;
        svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
        bool n3ds = false;
        APT_CheckNew3DS(&n3ds);
        if (n3ds) g_thread = threadCreate(threadMain, nullptr, 48 * 1024, prio - 2, 2, false);
        if (g_thread) g_core = 2;
        if (!g_thread && R_SUCCEEDED(APT_SetAppCpuTimeLimit(30))) { g_thread = threadCreate(threadMain, nullptr, 48 * 1024, prio - 2, 1, false); if (g_thread) g_core = 1; }
        if (!g_thread) { g_thread = threadCreate(threadMain, nullptr, 48 * 1024, prio - 1, 0, false); if (g_thread) g_core = 0; }
    }
    return true;
}

void shutdown() {
    if (!g_ok) return;
    if (g_thread) {
        g_quit = true;
        LightEvent_Signal(&g_wake);
        threadJoin(g_thread, U64_MAX);
        threadFree(g_thread);
        g_thread = nullptr;
    }
    for (Voice& v : g_voice) {
        stopVoice(v);
        if (v.mem) linearFree(v.mem);
        v.mem = nullptr;
    }
    for (int ch = 0; ch < kWorldChN; ++ch) ndspChnWaveBufClear(ch);
    for (auto& [k, c] : g_cache) linearFree(c.pcm);
    g_cache.clear();
    g_cacheBytes = 0;
    ndspExit();
    g_ok = false;
}

void play(const char* event) {
    if (!g_ok || g_sfxVol <= 0) return;
    const snd::Clip* c = g_sfx.pick(event);
    if (!c) return;
    static ndspWaveBuf wb[kSfxChN];
    // a free channel, else the oldest one
    int ch = -1;
    for (int k = 0; k < kSfxChN - kSfxCh0 && ch < 0; ++k) {
        int cand = kSfxCh0 + (g_nextSfx - kSfxCh0 + k) % (kSfxChN - kSfxCh0);
        if (!ndspChnIsPlaying(cand)) ch = cand;
    }
    if (ch < 0) ch = g_nextSfx;
    g_nextSfx = kSfxCh0 + (ch - kSfxCh0 + 1) % (kSfxChN - kSfxCh0);
    ndspChnWaveBufClear(ch);
    ndspChnReset(ch);
    ndspChnSetInterp(ch, NDSP_INTERP_LINEAR);
    ndspChnSetRate(ch, (float)c->rate);
    ndspChnSetFormat(ch, NDSP_FORMAT_MONO_PCM16);
    setMix(ch, g_sfxVol * c->volume);
    ndspWaveBuf& w = wb[ch];
    memset(&w, 0, sizeof w);
    w.data_vaddr = c->pcm;
    w.nsamples = c->samples;
    ndspChnWaveBufAdd(ch, &w);
}

void setVolumes(int music, int sfx, int ambience) {
    if (!g_ok) return;
    LightLock_Lock(&g_lock);
    g_musicVol = std::clamp(music, 0, 10) / 10.0f;
    g_sfxVol = std::clamp(sfx, 0, 10) / 10.0f;
    g_ambVol = std::clamp(ambience, 0, 10) / 10.0f;
    LightLock_Unlock(&g_lock);
}

void setPlace(uint32_t map, float x, float y, bool inWorld) {
    if (!g_ok) return;
    LightLock_Lock(&g_lock);
    g_map = map;
    g_x = x;
    g_y = y;
    g_inWorld = inWorld;
    LightLock_Unlock(&g_lock);
}

void setListener(float x, float y, float z, float yaw) {
    if (!g_ok) return;
    LightLock_Lock(&g_lock);
    g_lx = x; g_ly = y; g_lz = z; g_lyaw = yaw;
    LightLock_Unlock(&g_lock);
}

static void push(const Req& r) {
    if (!g_ok || !g_thread) return;
    LightLock_Lock(&g_lock);
    const int next = (g_reqHead + 1) % 64;
    ++g_requests;
    if (next != g_reqTail) {   // full: drop
        g_reqs[g_reqHead] = r;
        g_reqHead = next;
    } else {
        ++g_dropped;
    }
    LightLock_Unlock(&g_lock);
    LightEvent_Signal(&g_wake);
}

void playAt(uint32_t sound, float x, float y, float z, bool self, uint64_t tag, float gain, uint32_t loopMs) {
    if (!sound || !g_world.loaded()) return;
    push({sound, x, y, z, self, false, tag, gain, loopMs});
}

void stopTag(uint64_t tag) { push({0, 0, 0, 0, false, true, tag, 1.0f, 0}); }

const char* soundsTxt() { return g_soundsTxt.c_str(); }

const char* status() { return g_status; }

void stats(Stats* o) {
    *o = Stats();
    o->on = g_ok;
    if (!g_ok) return;
    o->core = g_core;
    o->sfxBytes = (uint32_t)g_sfx.bytes();
    for (const Voice& v : g_voice) if (v.mem) o->streamBytes += (uint32_t)((size_t)kBufs * kBufFrames * 2 * sizeof(int16_t));
    o->cacheBytes = (uint32_t)g_cacheBytes;          // (written by the sound thread: a snapshot)
    o->cacheClips = (uint32_t)g_cache.size();
    o->cacheMisses = g_misses;
    o->cacheFails = g_fails;
    for (int i = 0; i < kWorldChN - kWorldCh0; ++i) o->worldPlaying += ndspChnIsPlaying(kWorldCh0 + i) ? 1 : 0;
    o->busyUs = g_busyUs;
    o->requests = g_requests;
    o->requestsDropped = g_dropped;
}
uint32_t area() { return g_area; }

}  // namespace audio
