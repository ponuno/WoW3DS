// sound_test — w3dconv --sound output (from gen_sound_data.py) read back by the 3DS-side code (audio_core.cpp):
// every source format decodes to the right pitch and length, the zone grid gives the right areas, music / ambience
// follow the zone with pauses between tracks, and the built-in blips exist.
//   sound_test <sound dir>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "audio_core.hpp"

static int g_fail = 0, g_ok = 0;
#define CHECK(c, ...) do { if (c) ++g_ok; else { ++g_fail; std::printf("FAIL %s:%d ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

// pitch by zero crossings (upward) over the middle of the signal
static double pitch(const int16_t* s, size_t n, uint32_t rate, int stride = 1) {
    size_t a = n / 8, b = n - n / 8, ups = 0, first = 0, last = 0;
    for (size_t i = a + 1; i < b; ++i)
        if (s[(i - 1) * stride] < 0 && s[i * stride] >= 0) {
            if (!ups) first = i;
            last = i;
            ++ups;
        }
    return ups > 1 ? (double)(ups - 1) * rate / (double)(last - first) : 0;
}

static void clipCheck(snd::SfxBank& bank, const char* ev, double hz, double secs) {
    const snd::Clip* c = bank.pick(ev);
    CHECK(c != nullptr, "event %s missing", ev);
    if (!c) return;
    double p = pitch(c->pcm, c->samples, c->rate);
    CHECK(std::fabs(p - hz) < hz * 0.03, "%s: pitch %.1f Hz, want %.0f", ev, p, hz);
    double len = (double)c->samples / c->rate;
    CHECK(std::fabs(len - secs) < 0.06, "%s: %.3f s, want %.2f", ev, len, secs);
    CHECK(c->rate <= 22050, "%s: rate %u", ev, c->rate);
    std::printf("  %-8s %5u Hz %.3f s pitch %.1f vol %.2f\n", ev, c->rate, len, p, c->volume);
}

int main(int argc, char** argv) {
    if (argc < 2) { std::puts("usage: sound_test <sound dir>"); return 2; }
    const std::string dir = argv[1];
    snd::SfxBank bank;
    CHECK(bank.load((dir + "/sfx.w3ds").c_str()), "sfx.w3ds");
    clipCheck(bank, "click", 1000, 0.2);      // PCM16 44.1 kHz
    clipCheck(bank, "open", 500, 0.3);        // 8-bit
    clipCheck(bank, "tab", 800, 0.25);        // IMA ADPCM
    clipCheck(bank, "coin", 1500, 0.25);      // MS ADPCM stereo
    clipCheck(bank, "levelup", 600, 0.6);     // MP3 (decoder delay / padding: a few ms)
    clipCheck(bank, "discover", 1200, 0.3);   // stage 5.45: an area explored - found by the "*discover" search
    CHECK(bank.pick("open")->volume < 0.6f, "open: SoundEntries volume 0.5 kept");
    CHECK(bank.pick("bagsopen") == bank.pick("open") || bank.pick("bagsopen") != nullptr, "bagsopen falls back to open");
    CHECK(bank.pick("nosuchevent") == nullptr, "unknown event");

    snd::MusicTable mt;
    CHECK(mt.load(dir.c_str()), "music.txt");
    const float TS = 533.33333f;
    auto at = [&](uint32_t map, int tx, int ty, int cx, int cy) {
        float u = tx + (cx + 0.5f) / 16.0f, v = ty + (cy + 0.5f) / 16.0f;
        return mt.areaAt(map, (32.0f - v) * TS, (32.0f - u) * TS);
    };
    CHECK(at(0, 32, 48, 2, 5) == 12, "Elwynn chunk: %u", at(0, 32, 48, 2, 5));
    CHECK(at(0, 32, 48, 12, 5) == 87, "Goldshire chunk: %u", at(0, 32, 48, 12, 5));
    CHECK(at(0, 32, 49, 7, 15) == 1519, "Stormwind tile: %u", at(0, 32, 49, 7, 15));
    CHECK(at(33, 0, 0, 0, 0) == 209, "dungeon map without terrain: its area (%u)", at(33, 0, 0, 0, 0));
    CHECK(at(0, 10, 10, 0, 0) == 12, "outside the grid: the map's first zone (%u)", at(0, 10, 10, 0, 0));
    CHECK(at(1, 10, 10, 0, 0) == 0, "unknown map: no area");
    snd::ZoneSound gs = mt.zone(87);
    CHECK(gs.music == 1 && gs.day && gs.day->files.size() == 2 && gs.night && gs.ambDay, "Goldshire inherits Elwynn's music + ambience");
    CHECK(gs.silMinMs == 1000 && gs.silMaxMs == 2000, "silence %u..%u", gs.silMinMs, gs.silMaxMs);
    CHECK(gs.day && std::fabs(gs.day->volume - 0.7f) < 0.02f, "music volume");

    snd::Director d(7);
    d.update(mt, 12, false, 1000);
    CHECK(d.music().find("/music/m100_") != std::string::npos, "day music: %s", d.music().c_str());
    CHECK(d.ambience().find("/music/a200_0.ima") != std::string::npos, "ambience: %s", d.ambience().c_str());
    const std::string first = d.music();
    d.update(mt, 87, false, 1100);   // same music set inside Goldshire: no change
    CHECK(d.music() == first, "same set keeps the track");
    d.update(mt, 87, true, 1200);    // night
    CHECK(d.music().find("/music/m101_0.mp3") != std::string::npos, "night music: %s", d.music().c_str());
    d.musicEnded(5000);
    CHECK(d.music().empty(), "silence after a track");
    d.update(mt, 87, true, 7500);    // an eighth of silMin, at least 3 s (stage 5.43)
    CHECK(d.music().empty(), "still silent before the pause ends");
    d.update(mt, 87, true, 10100);   // pause 3..? s from 5000: over by 8000 at the latest (silMax 2000 -> 3 s)
    CHECK(!d.music().empty(), "next track after the pause");
    CHECK(mt.glue() && mt.glue()->files.size() == 1 && mt.path(mt.glue()->files[0]).find("/music/glue_0.mp3") != std::string::npos,
          "login screen music");
    d.update(mt, 1519, false, 11000);   // a new area is followed after it held for 1 s
    CHECK(d.area() == 87, "an area border is not followed at once");
    d.update(mt, 1519, false, 12100);
    CHECK(d.ambience().empty() && !d.music().empty(), "Stormwind: own music, no ambience");

    // streams
    snd::Stream s;
    CHECK(s.open(dir + "/music/m100_0.mp3"), "open mp3");
    CHECK(s.channels() == 2 && s.rate() == 44100, "mp3 %d ch %u Hz", s.channels(), s.rate());
    std::vector<int16_t> all, buf(4096 * 2);
    for (int n; (n = s.read(buf.data(), 4096)) > 0;) all.insert(all.end(), buf.begin(), buf.begin() + n * s.channels());
    size_t frames = all.size() / 2;
    double p440 = pitch(all.data(), frames, 44100, 2);
    CHECK(std::fabs(frames / 44100.0 - 3.0) < 0.08, "mp3 length %.3f s", frames / 44100.0);
    CHECK(std::fabs(p440 - 440) < 440 * 0.02, "mp3 pitch %.1f", p440);
    CHECK(s.rewind() && s.read(buf.data(), 100) == 100, "rewind");
    snd::Stream a;
    CHECK(a.open(dir + "/music/a200_0.ima"), "open ima");
    CHECK(a.channels() == 1 && a.rate() == 22050, "ima %d ch %u Hz", a.channels(), a.rate());
    all.clear();
    for (int n; (n = a.read(buf.data(), 1000)) > 0;) all.insert(all.end(), buf.begin(), buf.begin() + n);
    double p300 = pitch(all.data(), all.size(), 22050);
    CHECK(std::fabs(all.size() / 22050.0 - 2.0) < 0.01, "ima length %.3f s", all.size() / 22050.0);
    CHECK(std::fabs(p300 - 300) < 300 * 0.02, "ima pitch %.1f", p300);
    // IMA quality: compare with the ideal tone (SNR)
    double sig = 0, err = 0;
    for (size_t i = 2000; i < all.size() - 2000; ++i) {
        double ref = std::sin(2 * M_PI * 300 * (i + 0.5) * 2 / 44100.0) * 0.5 * 32767;   // box-filtered 44.1 -> 22.05
        sig += ref * ref;
        err += (all[i] - ref) * (all[i] - ref);
    }
    double snr = 10 * std::log10(sig / std::max(err, 1.0));
    CHECK(snr > 18, "ima SNR %.1f dB", snr);
    std::printf("  streams: mp3 %.2f s %.1f Hz, ima %.2f s %.1f Hz SNR %.1f dB\n", frames / 44100.0, p440, all.size() / 22050.0, p300, snr);

    // world sounds: tables (columns found from the data) and the bank
    snd::SoundTables st;
    CHECK(st.load((dir + "/sounds.txt").c_str()), "sounds.txt");
    const snd::CreatureSounds* wolf = st.creature(100);
    CHECK(wolf && wolf->aggro == 300 && wolf->death == 301 && wolf->wound == 302 && wolf->woundCrit == 303 &&
          wolf->attack == 304 && wolf->attackCrit == 305 && wolf->footstep == 7, "creature sound columns");
    CHECK(st.creature(101) && !st.creature(102), "display without sound data");
    CHECK(st.weapon(7).first == 310 && st.weapon(7).second == 311, "weapon impact on flesh: %u %u", st.weapon(7).first, st.weapon(7).second);
    CHECK(st.footstep(7) == 330, "footstep on dirt: %u", st.footstep(7));
    const snd::SpellSounds* fb = st.spell(133);
    CHECK(fb && fb->precast == 320 && fb->cast == 321 && fb->impact == 322, "spell visual kit sounds");
    CHECK(!st.spell(78), "spell without a visual");
    snd::WorldBank wb;
    CHECK(wb.load((dir + "/world.w3ds").c_str()), "world.w3ds");
    CHECK(wb.variants(300) == 2 && wb.variants(340) == 0 && wb.variants(312) == 0, "variants: %d %d %d", wb.variants(300), wb.variants(340), wb.variants(312));
    const int kPitch[] = {300, 301, 302, 303, 304, 305, 310, 311, 312, 320, 321, 322, 330, 331};
    for (int k = 0; k < 14; ++k) {
        const snd::WorldClip* c = wb.clip(kPitch[k], 1);
        if (!c) continue;
        std::vector<int16_t> pcm(c->samples + 2048);
        CHECK(wb.decode(*c, pcm.data()), "decode %d", kPitch[k]);
        const double want = 2000 + k * 100, p = pitch(pcm.data(), c->samples, c->rate);
        CHECK(std::fabs(p - want) < want * 0.02, "world %d pitch %.1f want %.0f", kPitch[k], p, want);
        CHECK(std::fabs(c->volume - 0.8f) < 0.01f && c->minDist == 8 && c->cutoff == 40, "world %d volume / distances", kPitch[k]);
        if (kPitch[k] == 320) CHECK(c->samples == 22050 * 5, "precast loop kept whole under the 12 s cap, was cut to 3 s (%u)", c->samples);
    }
    {   // 3D: facing +X, a source to the left (+Y) is louder on the left; beyond the cutoff silent
        float l, r;
        CHECK(snd::spatialize(0, 10, 0, 0, 0, 8, 40, &l, &r) && l > r * 2, "left source: %.2f %.2f", l, r);
        CHECK(snd::spatialize(10, 0, 0, 0, 1.5707963f, 8, 40, &l, &r) && r > l * 2, "facing +Y, a source at +X is right: %.2f %.2f", l, r);
        CHECK(snd::spatialize(5, 0, 0, 0, 0, 8, 40, &l, &r) && std::fabs(l - r) < 0.01f && l > 0.7f, "ahead, close: centred, loud");
        float l2, r2;
        snd::spatialize(30, 0, 0, 0, 0, 8, 40, &l2, &r2);
        CHECK(l2 < l * 0.5f, "farther is quieter");
        CHECK(!snd::spatialize(45, 0, 0, 0, 0, 8, 40, &l, &r), "beyond the cutoff");
    }

    snd::SfxBank synth;
    CHECK(synth.synthesize(), "built-in sounds");
    for (const char* e : {"click", "tab", "open", "close", "coin", "levelup", "questcomplete", "whisper", "targethostile",
                          "bagsopen", "npcclose", "questaccept", "invite", "targetfriend"})
        CHECK(synth.pick(e) != nullptr, "built-in %s", e);

    std::printf("SOUND: %d checks, %d failed -> %s\n", g_ok + g_fail, g_fail, g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
