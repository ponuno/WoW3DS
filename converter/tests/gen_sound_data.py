"""Synthetic client data for w3dconv --sound: SoundEntries / AreaTable / ZoneMusic / SoundAmbience / Map DBCs, sounds
in every format the converter reads (WAV PCM16, WAV 8-bit, IMA ADPCM WAV, MS ADPCM WAV, MP3) as sine tones of known
pitch, and a WDT + two ADTs whose MCNK headers name areas, so tests/sound_test.cpp can check pitch, length, zones.
  gen_sound_data.py outdir        (needs ffmpeg with libmp3lame)"""
import math, os, struct, subprocess, sys, wave

out = sys.argv[1]

def path(p):
    full = os.path.join(out, *p.split("\\"))
    os.makedirs(os.path.dirname(full), exist_ok=True)
    return full

def tone_wav(p, hz, secs, rate=44100, ch=1, width=2):
    n = int(secs * rate)
    with wave.open(p, "wb") as w:
        w.setnchannels(ch); w.setsampwidth(width); w.setframerate(rate)
        fr = bytearray()
        for i in range(n):
            v = math.sin(2 * math.pi * hz * i / rate) * 0.5
            for c in range(ch):
                fr += struct.pack("<h", int(v * 32767)) if width == 2 else bytes((int(128 + v * 127),))
        w.writeframes(bytes(fr))

def ffmpeg(src, dst, *args):
    subprocess.run(["ffmpeg", "-loglevel", "error", "-y", "-i", src, *args, dst], check=True)

tmp = path("tmp\\x")
os.makedirs(os.path.dirname(tmp), exist_ok=True)
# interface sounds (Sound\Interface)
tone_wav(path("Sound\\Interface\\click.wav"), 1000, 0.2)                      # PCM16 44.1 kHz -> resampled
tone_wav(path("Sound\\Interface\\open8.wav"), 500, 0.3, rate=22050, width=1)  # 8-bit
src = os.path.join(out, "tmp", "tab_src.wav"); tone_wav(src, 800, 0.25, rate=22050)
ffmpeg(src, path("Sound\\Interface\\tab.wav"), "-c:a", "adpcm_ima_wav")       # IMA ADPCM
src = os.path.join(out, "tmp", "coin_src.wav"); tone_wav(src, 1500, 0.25, rate=22050, ch=2)
ffmpeg(src, path("Sound\\Interface\\coin.wav"), "-c:a", "adpcm_ms")           # MS ADPCM stereo
src = os.path.join(out, "tmp", "lvl_src.wav"); tone_wav(src, 600, 0.6)
ffmpeg(src, path("Sound\\Interface\\levelup.mp3"), "-c:a", "libmp3lame", "-b:a", "96k")
tone_wav(path("Sound\\Interface\\discover.wav"), 1200, 0.3)                    # found by the "*discover" name search
# music: two day tracks, one night track (stereo MP3s), ambience WAV loop
for name, hz in (("day1", 440), ("day2", 330), ("night1", 220)):
    src = os.path.join(out, "tmp", name + ".wav"); tone_wav(src, hz, 3.0, ch=2)
    ffmpeg(src, path("Sound\\Music\\ZoneMusic\\" + name + ".mp3"), "-c:a", "libmp3lame", "-b:a", "128k")
tone_wav(path("Sound\\Ambience\\forest.wav"), 300, 2.0, rate=44100)
src = os.path.join(out, "tmp", "glue.wav"); tone_wav(src, 550, 2.0, ch=2)   # the login screen theme
ffmpeg(src, path("Sound\\Music\\GlueScreenMusic\\wow_main_theme.mp3"), "-c:a", "libmp3lame", "-b:a", "128k")

S = [b"\0"]
def s(t):
    if not t: return 0
    off = sum(len(x) for x in S); S.append(t.encode() + b"\0"); return off

def dbc(name, fields, rows, strings):
    recs = b"".join(struct.pack("<" + "".join("f" if isinstance(v, float) else "I" for v in r), *r) for r in rows)
    open(path("DBFilesClient\\" + name), "wb").write(
        b"WDBC" + struct.pack("<IIII", len(rows), fields, fields * 4, len(strings)) + recs + strings)

def strtab():
    return b"".join(S)

# SoundEntries: ID, type, Name, File[10], Freq[10], DirectoryBase, Volume, Flags, MinDist, Cutoff, EAX = 29 fields
def se(id_, name, files, base, vol=1.0):
    f = [s(x) for x in files] + [0] * (10 - len(files))
    return [id_, 1, s(name)] + f + [1] * 10 + [s(base), vol, 0, 0, 0, 0]
rows = [
    se(1, "igMainMenuOptionCheckBoxOn", ["click.wav"], "Sound\\Interface"),
    se(2, "igCharacterInfoOpen", ["open8.wav"], "Sound\\Interface", 0.5),
    se(3, "igCharacterInfoTab", ["tab.wav"], "Sound\\Interface"),
    se(4, "LOOTWINDOWCOINSOUND", ["coin.wav"], "Sound\\Interface"),
    se(5, "LevelUp", ["levelup.mp3"], "Sound\\Interface"),
    se(6, "ZoneDiscoveredChime", ["discover.wav"], "Sound\\Interface"),
    se(100, "Zone-ElwynnDay", ["day1.mp3", "day2.mp3"], "Sound\\Music\\ZoneMusic", 0.7),
    se(101, "Zone-ElwynnNight", ["night1.mp3"], "Sound\\Music\\ZoneMusic"),
    se(200, "ForestDay", ["forest.wav"], "Sound\\Ambience"),
    se(400, "GS_LoginScreen", ["wow_main_theme.mp3"], "Sound\\Music\\GlueScreenMusic", 0.8),
]
# world sounds: a wolf's cries, weapon impacts, a fire spell, footsteps (pitches 2000 + k * 100 Hz)
WORLD = [(300, "WolfAggro"), (301, "WolfDeath"), (302, "WolfWoundA"), (303, "WolfWoundCrit"), (304, "WolfAttackA"),
         (305, "WolfAttackCrit"), (310, "m1hSwordHitFlesh"), (311, "m1hSwordHitFleshCrit"), (312, "m1hSwordHitMetal"),
         (320, "PreCastFireHigh"), (321, "FireCast"), (322, "FireBallImpact"), (330, "FootStepsMediumDirt"),
         (331, "FootStepsMediumGrass"),
         # liquids (stage 5.26): found by keyword (splash, swim, wade) and by the id the server plays at a bite (3355)
         (350, "EnterWaterMediumA"), (351, "SwimSlowMedium"), (352, "FootStepsWaterSmall"), (3355, "FishBite")]
for k, (i, n) in enumerate(WORLD):
    tone_wav(path("Sound\\Creature\\%s.wav" % n), 2000 + k * 100, 0.4 if i != 320 else 5.0, rate=22050)
    rows.append(se(i, n, [n + ".wav", n + ".wav"], "Sound\\Creature", 0.8))
rows.append(se(340, "igSpellPrecast", [], ""))   # no files: must be skipped
# UI stage 8 voices: an NPC voice set (NPCSounds), our error lines listed in SoundEntries (human male), a generic race
# voice (human female), an emote voice; more files that only exist in the archives (orc female errors with sparse numbers,
# dwarf male generic NPC greetings) are found by their names
VOICE = [(500, "GuardGreetings", "Sound\\Creature\\Guard", ["GuardGreet01.wav", "GuardGreet02.wav", "GuardGreet03.wav"]),
         (501, "GuardFarewell", "Sound\\Creature\\Guard", ["GuardBye01.wav"]),
         (502, "GuardPissed", "Sound\\Creature\\Guard", ["GuardPissed01.wav"]),
         (503, "BakerGreetings", "Sound\\Creature\\Baker", ["BakerGreet01.wav"]),
         (504, "SmithGreetings", "Sound\\Creature\\Smith", ["SmithGreet01.wav"]),
         (600, "HumanMaleErrorNoRage", "Sound\\Character\\Human\\MaleErrorMessages", ["HumanMale_err_norage01.wav", "HumanMale_err_norage02.wav"]),
         (601, "HumanMaleErrorOutOfRange", "Sound\\Character\\Human\\MaleErrorMessages", ["HumanMale_err_outofrange01.wav"]),
         (700, "HumanFemaleStandardNPCGreetings", "Sound\\Creature\\HumanFemaleStandardNPC", ["HumanFemaleStandardNPCGreeting01.wav"]),
         (800, "HumanMaleCheer", "Sound\\Character\\Human", ["HumanMaleCheer01.wav"])]
for k, (i, n, d, files) in enumerate(VOICE):
    for f in files: tone_wav(path(d + "\\" + f), 1200 + k * 50, 0.6, rate=22050)
    rows.append(se(i, n, files, d))
for f in ("OrcFemale_err_nomana01.wav", "OrcFemale_err_nomana03.wav"):
    tone_wav(path("Sound\\Character\\Orc\\OrcFemaleErrorMessages\\" + f), 900, 0.5, rate=22050)
tone_wav(path("Sound\\Creature\\DwarfMaleStandardNPC\\DwarfMaleStandardNPCGreeting01.wav"), 950, 0.5, rate=22050)
dbc("SoundEntries.dbc", 29, rows, strtab())

def zrow(n, vals):
    r = [0] * n
    for c, v in vals.items(): r[c] = v
    return r
# CreatureSoundData (1.12-like): ID, exertion 1, exertionCrit 2, injury 3, injuryCrit 4, crushing 5, death 6, stun 7,
# stand 8, footstep 9, aggro 10, ... (30 fields); two creatures share the wolf's sounds
csd = [zrow(30, {0: i, 1: 304, 2: 305, 3: 302, 4: 303, 6: 301, 9: 7, 10: 300}) for i in (1, 2)]
dbc("CreatureSoundData.dbc", 30, csd, b"\0")
# CreatureDisplayInfo: ID, model, sound 2, extra 3, ..., NPCSounds 11 (UI stage 8)
dbc("CreatureDisplayInfo.dbc", 12, [zrow(12, {0: 100, 1: 1, 2: 1}), zrow(12, {0: 101, 1: 2, 2: 2}), zrow(12, {0: 102, 1: 3, 2: 0}),
    zrow(12, {0: 103, 1: 4, 11: 7}), zrow(12, {0: 104, 1: 4, 11: 8}), zrow(12, {0: 105, 1: 4, 11: 9}),
    zrow(12, {0: 106, 1: 5, 3: 50}), zrow(12, {0: 107, 1: 5, 3: 51})], b"\0")
dbc("NPCSounds.dbc", 5, [[7, 500, 501, 502, 0], [8, 503, 0, 0, 0], [9, 504, 0, 0, 0]], b"\0")
dbc("CreatureDisplayInfoExtra.dbc", 20, [zrow(20, {0: 50, 1: 1, 2: 1}), zrow(20, {0: 51, 1: 3, 2: 0})], b"\0")
dbc("EmotesTextSound.dbc", 5, [[1, 21, 1, 0, 800], [2, 21, 1, 1, 9999]], b"\0")   # (9999: no such sound - skipped)
dbc("FootstepTerrainLookup.dbc", 5, [[1, 7, 3, 331, 0], [2, 7, 1, 330, 0]], b"\0")
S[:] = [b"\0"]
dbc("TerrainType.dbc", 6, [[1, s("Dirt"), 0, 0, 0, 0], [3, s("Grass"), 0, 0, 0, 0]], strtab())
dbc("WeaponImpactSounds.dbc", 23, [zrow(23, {0: 1, 1: 7, 2: 0, 3: 312}), zrow(23, {0: 2, 1: 7, 2: 1, 3: 310, 13: 311})], b"\0")
# SpellVisualKit: ID, StartAnim 1, Anim 2 (small ids that are also SoundEntries ids: 1..5), ..., Sound 15 (20 fields)
dbc("SpellVisualKit.dbc", 20, [zrow(20, {0: 10, 1: 1, 2: 2, 15: 320}), zrow(20, {0: 11, 1: 3, 2: 4, 15: 321}),
                               zrow(20, {0: 12, 1: 5, 2: 1, 15: 322})], b"\0")
# SpellVisual: ID, precast 1, cast 2, impact 3, state 4, stateDone 5, channel 6, ..., missile sound 11 (16 fields)
dbc("SpellVisual.dbc", 16, [zrow(16, {0: 5, 1: 10, 2: 11, 3: 12})], b"\0")
S[:] = [b"\0"]
spells = [zrow(173, {0: 133, 115: 5, 120: s("Fireball")}), zrow(173, {0: 78, 115: 0, 120: s("Heroic Strike")})]
dbc("Spell.dbc", 173, spells, strtab())

S[:] = [b"\0"]
# AreaTable (1.12): ID, map, parent, bit, flags, prov, provUW, ambience, zoneMusic, intro, explore, name[9], ... = 25
def area(id_, map_, parent, amb, zm, name):
    return [id_, map_, parent, 0, 0, 0, 0, amb, zm, 0, 0] + [s(name)] + [0] * 8 + [0, 0, 0, 0, 0]
rows = [area(12, 0, 0, 1, 1, "Elwynn Forest"), area(87, 0, 12, 0, 0, "Goldshire"), area(1519, 0, 0, 0, 2, "Stormwind"),
        area(209, 33, 0, 0, 1, "Shadowfang Keep")]
dbc("AreaTable.dbc", 25, rows, strtab())
# ZoneMusic: ID, SetName, silMin[2], silMax[2], sound[2]
S[:] = [b"\0"]
dbc("ZoneMusic.dbc", 8, [[1, s("Elwynn"), 1000, 1000, 2000, 2000, 100, 101], [2, s("Stormwind"), 500, 500, 500, 500, 100, 0]], strtab())
S[:] = [b"\0"]
dbc("SoundAmbience.dbc", 3, [[1, 200, 200]], b"\0")
dbc("Map.dbc", 3, [[0, s("Azeroth"), 0], [33, s("Shadowfang"), 0]], strtab())

# WDT with tiles 32_48 and 32_49 (MAIN index = y * 64 + x), ADTs: MCNK headers only
main = bytearray(64 * 64 * 8)
for tx, ty in ((32, 48), (32, 49)):
    struct.pack_into("<I", main, (ty * 64 + tx) * 8, 1)
open(path("World\\Maps\\Azeroth\\Azeroth.wdt"), "wb").write(b"REVM" + struct.pack("<II", 4, 18) + b"NIAM" +
                                                            struct.pack("<I", len(main)) + bytes(main))
def adt(tx, ty, area_of):
    b = bytearray(b"REVM" + struct.pack("<II", 4, 18))
    for cy in range(16):
        for cx in range(16):
            h = bytearray(128)
            struct.pack_into("<III", h, 0, 0, cx, cy)
            struct.pack_into("<I", h, 0x34, area_of(cx, cy))
            b += b"KNCM" + struct.pack("<I", len(h)) + h
    open(path("World\\Maps\\Azeroth\\Azeroth_%d_%d.adt" % (tx, ty)), "wb").write(bytes(b))
adt(32, 48, lambda cx, cy: 12 if cx < 8 else 87)
adt(32, 49, lambda cx, cy: 1519)
