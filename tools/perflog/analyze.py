#!/usr/bin/env python3
"""Сводка perf.csv (лог производительности, этап 6): где уходит кадр, худшие секунды, память, нехватки, подгрузка, сеть.

    python3 analyze.py perf.csv [perf_events.txt] [--phase world] [--top 12]

Только стандартная библиотека. Легенда столбцов: docs/PERFLOG.md.
"""
import argparse
import csv
import math
import os
import sys
from collections import defaultdict

PHASES = ["input", "net", "ui", "move", "spells", "sound", "game", "hud", "touch", "glue", "stream", "prepare", "wait", "portrait",
          "world", "huddraw", "touchdraw", "flush", "mapload", "other"]
PASSES = ["sky", "terrain", "collect", "opaque", "markers", "units", "water", "alpha", "fx"]


def load(path):
    rows, header = [], None
    with open(path, newline="", encoding="utf-8", errors="replace") as f:
        for line in csv.reader(f):
            if not line or line[0].startswith("#"):
                continue
            if line[0] == "t_s":
                header = line
                continue
            if header and len(line) == len(header):
                rows.append(dict(zip(header, line)))
    return rows


def num(r, k, d=0.0):
    try:
        return float(r.get(k, d))
    except (TypeError, ValueError):
        return d


def wavg(rows, k, w="f_frames"):
    tw = sum(num(r, w) for r in rows)
    return sum(num(r, k) * num(r, w) for r in rows) / tw if tw else 0.0


def pct(vals, p):
    if not vals:
        return 0.0
    v = sorted(vals)
    return v[min(len(v) - 1, int(round(p * (len(v) - 1))))]


def pearson(xs, ys):
    n = len(xs)
    if n < 5:
        return 0.0
    mx, my = sum(xs) / n, sum(ys) / n
    sx = math.sqrt(sum((x - mx) ** 2 for x in xs))
    sy = math.sqrt(sum((y - my) ** 2 for y in ys))
    if sx < 1e-9 or sy < 1e-9:
        return 0.0
    return sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / (sx * sy)


def bar(v, total, width=24):
    n = int(round(width * v / total)) if total > 0 else 0
    return "#" * max(0, min(width, n))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csv")
    ap.add_argument("events", nargs="?")
    ap.add_argument("--phase", default="world", help="какие секунды разбирать (world, loading, login, bench, all)")
    ap.add_argument("--top", type=int, default=12)
    a = ap.parse_args()
    rows = load(a.csv)
    if not rows:
        print("в файле нет строк")
        return 1
    out = []
    p = out.append
    by_phase = defaultdict(int)
    for r in rows:
        by_phase[r["phase"]] += 1
    sel = rows if a.phase == "all" else [r for r in rows if r["phase"] == a.phase]
    p("== perf.csv: %d с записано (%s)" % (len(rows), ", ".join("%s %d" % kv for kv in sorted(by_phase.items(), key=lambda x: -x[1]))))
    if not sel:
        p("секунд с phase=%s нет; возьму все" % a.phase)
        sel = rows
    frames = sum(num(r, "f_frames") for r in sel)
    secs = sum(num(r, "t_win_ms") for r in sel) / 1000.0
    fps_s = [num(r, "f_fps") for r in sel if num(r, "f_frames") > 0]
    p("\n== Кадры (%s, %d с)" % (a.phase, len(sel)))
    p("средний FPS %.1f; по секундам: медиана %.1f, худшие 10%% %.1f, минимум %.1f" % (
        frames / secs if secs else 0, pct(fps_s, 0.5), pct(fps_s, 0.1), min(fps_s) if fps_s else 0))
    p("секунд ниже 20 FPS: %d, ниже 15: %d; кадров > 33 мс: %d, > 50: %d, > 100: %d; рывков: %d" % (
        sum(1 for v in fps_s if v < 20), sum(1 for v in fps_s if v < 15), sum(num(r, "f_over33") for r in sel),
        sum(num(r, "f_over50") for r in sel), sum(num(r, "f_over100") for r in sel), sum(num(r, "f_hitches") for r in sel)))
    p("CPU кадра %.2f мс (без ожидания), ожидание %.2f мс, GPU %.2f мс (макс %.2f); буфер команд до %.0f%%" % (
        wavg(sel, "f_cpu_avg"), wavg(sel, "f_wait_avg"), wavg(sel, "f_gpu_avg"), max(num(r, "f_gpu_max") for r in sel),
        max(num(r, "f_cmdbuf_max") for r in sel)))
    if "f_kick_lead" in sel[0] and wavg(sel, "f_kicks") > 0:
        p("ранний старт GPU (6.3): %.1f пинка на кадр, GPU начинает за %.2f мс до FrameEnd; GPU всего ≈ %.2f мс; цена пинков %.2f мс CPU" % (
            wavg(sel, "f_kicks"), wavg(sel, "f_kick_lead"), wavg(sel, "f_gpu_span"), wavg(sel, "f_kick_ms")))
    bound = defaultdict(int)
    for r in sel:
        bound[r.get("f_bound", "")] += 1
    p("упор: " + ", ".join("%s %d с" % kv for kv in sorted(bound.items(), key=lambda x: -x[1])) +
      "  (vsync = 57+ FPS; gpu = GPU дольше CPU; cpu = наоборот)")

    p("\n== Куда уходит кадр (мс на кадр, среднее)")
    ph = [(n, wavg(sel, "ph_" + n)) for n in PHASES]
    tot = sum(v for _, v in ph)
    for n, v in sorted(ph, key=lambda x: -x[1]):
        if v >= 0.05:
            p("  %-10s %6.2f  %5.1f%%  %s  (макс %.1f)" % (n, v, 100 * v / tot if tot else 0, bar(v, tot),
                                                         max(num(r, "phx_" + n) for r in sel)))
    p("  wait = ожидание GPU (и кадровой синхронизации, если VSync включён: w_vsync); prepare = видимость до ожидания (6.4)")
    p("\n== Отрисовка мира по проходам (CPU мс / вызовов / треугольников на кадр)")
    for n in PASSES:
        ms_, d, t = wavg(sel, "r_%s_ms" % n), wavg(sel, "r_%s_draws" % n), wavg(sel, "r_%s_tris" % n)
        if ms_ >= 0.01 or d >= 1:
            p("  %-8s %6.2f мс %6.0f вызовов %8.0f треуг." % (n, ms_, d, t))
    p("  всего: вызовов %.0f (макс %.0f), треугольников %.0f (макс %.0f), смен текстур %.0f; юнитов %.1f, костей %.0f, поза %.2f мс" % (
        wavg(sel, "r_draws"), max(num(r, "r_draws_max") for r in sel), wavg(sel, "r_tris"), max(num(r, "r_tris_max") for r in sel),
        wavg(sel, "r_binds"), wavg(sel, "r_units"), wavg(sel, "r_bone_uploads"), wavg(sel, "r_pose_ms")))
    p("  частицы: квадов %.0f, живых до %.0f, отброшено %.0f/с; эффекты %.2f мс" % (
        wavg(sel, "r_fx_quads"), max(num(r, "r_fx_alive") for r in sel), max(num(r, "r_fx_drop_s") for r in sel), wavg(sel, "r_fxsim_ms")))
    p("  интерфейс: квадов %.0f, пакетов %.0f на кадр; страниц прочитано с SD %d (%.0f мс), из них в основном потоке %d (%.0f мс); не влезло квадов %d" % (
        wavg(sel, "ui_quads"), wavg(sel, "ui_batches"), sum(num(r, "ui_loads") for r in sel), sum(num(r, "ui_load_ms") for r in sel),
        sum(num(r, "ui_sync_loads" if "ui_sync_ms" in sel[0] else "ui_loads") for r in sel),
        sum(num(r, "ui_sync_ms" if "ui_sync_ms" in sel[0] else "ui_load_ms") for r in sel),   # (before 6.2 every page was read there)
        sum(num(r, "ui_overflow") for r in sel)))

    p("\n== Худшие секунды")
    worst = sorted([r for r in sel if num(r, "f_frames") > 0], key=lambda r: num(r, "f_fps"))[:a.top]
    for r in worst:
        top3 = sorted(((n, num(r, "ph_" + n)) for n in PHASES), key=lambda x: -x[1])[:3]
        p("  %7.1f с  %5.1f FPS  макс %5.1f мс (%s)  карта %s тайл %s_%s  [%s] gpu %.1f  draws %.0f tris %.0f units %.0f  io %.0f%%  upload %.1f мс" % (
            num(r, "t_s"), num(r, "f_fps"), num(r, "f_ms_max"), r.get("f_worst_phase", ""), r["map"], r["tile_x"], r["tile_y"],
            " ".join("%s %.1f" % kv for kv in top3), num(r, "f_gpu_avg"), num(r, "r_draws"), num(r, "r_tris"), num(r, "r_units"),
            num(r, "io_busy_pct"), num(r, "st_upload_max")))

    p("\n== Места (карта/тайл): секунд, FPS средний / худший, вызовов, треугольников, юнитов")
    tiles = defaultdict(list)
    for r in sel:
        tiles[(r["map"], r["tile_x"], r["tile_y"])].append(r)
    for k, rs in sorted(tiles.items(), key=lambda kv: wavg(kv[1], "f_fps"))[:a.top]:
        fr = sum(num(r, "f_frames") for r in rs)
        sc = sum(num(r, "t_win_ms") for r in rs) / 1000.0
        p("  карта %s тайл %s_%s: %3d с  %5.1f / %5.1f FPS  %5.0f  %7.0f  %4.1f" % (
            k[0], k[1], k[2], len(rs), fr / sc if sc else 0, min(num(r, "f_fps") for r in rs), wavg(rs, "r_draws"),
            wavg(rs, "r_tris"), wavg(rs, "r_units")))

    p("\n== Что связано с длинными кадрами (корреляция с f_ms_avg по секундам)")
    ys = [num(r, "f_ms_avg") for r in sel]
    cands = ["r_draws", "r_tris", "r_binds", "r_units", "r_doodads", "r_buildings", "r_fx_quads", "w_units_drawn", "ph_game",
             "ph_net", "ph_hud", "ph_touch", "ph_stream", "st_upload_ms", "st_done_ms", "st_players_ms", "io_busy_pct", "net_pk_in",
             "ui_quads", "f_gpu_avg", "r_pose_ms", "lin_free", "vram_free"]
    cors = [(c, pearson([num(r, c) for r in sel], ys)) for c in cands if c in sel[0]]
    for c, v in sorted(cors, key=lambda x: -abs(x[1]))[:10]:
        p("  %-14s r = %+.2f" % (c, v))

    p("\n== Память (КБ)")
    rmin = min(rows, key=lambda r: num(r, "lin_free"))
    p("linear: всего %.0f, свободно мин %.0f (на %.0f с, тайл %s_%s); в тот момент:" % (
        num(rmin, "lin_total"), num(rmin, "lin_free"), num(rmin, "t_s"), rmin["tile_x"], rmin["tile_y"]))
    parts = ["lin_pool", "lin_water", "lin_buildings", "lin_doodads", "lin_unit_models", "lin_models_loading", "lin_creatures",
             "lin_tex", "lin_staging", "lin_coll", "lin_sound", "lin_ui", "lin_cmdbuf", "lin_other"]
    used = num(rmin, "lin_used")
    for k in sorted(parts, key=lambda k: -num(rmin, k)):
        if num(rmin, k) > 0:
            p("    %-20s %8.0f  %5.1f%%  %s" % (k[4:], num(rmin, k), 100 * num(rmin, k) / used if used else 0, bar(num(rmin, k), used)))
    vmin = min(rows, key=lambda r: num(r, "vram_largest"))
    p("VRAM: свободно мин %.0f, наибольший блок мин %.0f (на %.0f с); текстуры %.0f, цели %.0f, резерв %.0f, портреты %.0f, прочее %.0f" % (
        min(num(r, "vram_free") for r in rows), num(vmin, "vram_largest"), num(vmin, "t_s"), num(vmin, "vram_tex"),
        num(vmin, "vram_targets"), num(vmin, "vram_reserve"), num(vmin, "vram_portraits"), num(vmin, "vram_other")))
    cls = ["unit", "tile", "coll", "terrtex", "object", "bldtex", "doodtex"]
    p("  текстуры по назначению (VRAM / linear): " + ", ".join(
        "%s %.0f/%.0f" % (c, num(vmin, "vram_tex_" + c), num(vmin, "lin_tex_" + c)) for c in cls) +
      ", тела игроков %.0f/%.0f" % (num(vmin, "vram_tex_bodies"), num(vmin, "lin_tex_bodies")))
    hmin = min(rows, key=lambda r: num(r, "heap_free"))
    p("куча: %.0f, свободно мин %.0f; COLL %.0f, высоты %.0f, экземпляры %.0f, слои игроков %.0f, индекс %.0f, прочее %.0f" % (
        num(hmin, "heap_size"), num(hmin, "heap_free"), num(hmin, "heap_coll"), num(hmin, "heap_heights"), num(hmin, "heap_instances"),
        num(hmin, "heap_charimg"), num(hmin, "heap_index"), num(hmin, "heap_other")))

    p("\n== Нехватки (за всё время)")
    keys = [("oom_staging", "буфер текстуры"), ("oom_model", "модели (пропали до перезагрузки тайла)"), ("oom_creature", "модели существ"),
            ("oom_water", "вода"), ("oom_upload", "загрузка текстуры"), ("oom_coll", "коллизия"),
            ("tex_vram_miss", "текстур ушло в linear (VRAM полна)"), ("tex_waited", "текстур ждали памяти (белые)"),
            ("tex_not_in_pack", "текстур нет в паке"), ("tex_read_failed", "чтений текстур с ошибкой"), ("io_fails", "ошибок чтения SD"),
            ("snd_fails", "звуков без памяти"), ("snd_dropped", "звуков потеряно"), ("ui_overflow", "квадов интерфейса не влезло"),
            ("net_update_fails", "пакетов обновлений с ошибкой"), ("log_dropped", "строк лога потеряно")]
    any_ = False
    for k, label in keys:
        v = sum(num(r, k) for r in rows) if k != "log_dropped" else max(num(r, k) for r in rows)
        if v:
            any_ = True
            p("  %-45s %d" % (label, v))
    if not any_:
        p("  нет")

    p("\n== Подгрузка и SD-карта")
    gl = [(num(r, "st_ground_ms"), num(r, "st_ground_n")) for r in rows if num(r, "st_ground_n") > 0]
    fl = [(num(r, "st_full_ms"), num(r, "st_full_n")) for r in rows if num(r, "st_full_n") > 0]
    if gl:
        p("земля тайла: %d тайлов, в среднем %.0f мс, худшая секунда %.0f мс" % (
            sum(n for _, n in gl), sum(m * n for m, n in gl) / sum(n for _, n in gl), max(m for m, _ in gl)))
    if fl:
        p("тайл целиком: %d, в среднем %.0f мс, худшая секунда %.0f мс" % (
            sum(n for _, n in fl), sum(m * n for m, n in fl) / sum(n for _, n in fl), max(m for m, _ in fl)))
    speeds = [num(r, "io_kbps_busy") for r in rows if num(r, "io_kb") > 256]
    p("SD: прочитано %.1f МБ за %d чтений; скорость при чтении медиана %.0f КБ/с; занятость потока до %.0f%%; самое долгое чтение %.0f мс" % (
        sum(num(r, "io_kb") for r in rows) / 1024, sum(num(r, "io_reads") for r in rows), pct(speeds, 0.5),
        max(num(r, "io_busy_pct") for r in rows), max(num(r, "io_max_read_ms") for r in rows)))
    kinds = ["terr", "water", "plcm", "coll", "model", "tex", "disp", "crt", "chri", "chimg", "item", "liqd"]
    tot_kb = sum(sum(num(r, "io_kb_" + k) for r in rows) for k in kinds)
    p("  по видам (МБ): " + ", ".join("%s %.1f" % (k, sum(num(r, "io_kb_" + k) for r in rows) / 1024) for k in kinds
                                    if sum(num(r, "io_kb_" + k) for r in rows) > 0) + "  (всего %.1f)" % (tot_kb / 1024))
    p("очередь чтения до %d (%.0f КБ); загрузки текстур на основном потоке до %.1f мс/кадр, разбор прочитанного до %.1f мс, тела игроков до %.1f мс" % (
        max(num(r, "st_queue") for r in rows), max(num(r, "st_queue_kb") for r in rows), max(num(r, "st_upload_max") for r in rows),
        max(num(r, "st_done_max") for r in rows), max(num(r, "st_players_max") for r in rows)))

    on = [r for r in rows if num(r, "online") > 0]
    if on:
        p("\n== Сеть")
        p("приём %.2f КБ/с (макс %.2f), отправка %.2f КБ/с; пакетов %.0f/с (макс %.0f); пинг %s..%s мс; объектов до %d; не разобрано до %.1f КБ; Wi-Fi %s..%s" % (
            sum(num(r, "net_in_kbps") for r in on) / len(on), max(num(r, "net_in_kbps") for r in on),
            sum(num(r, "net_out_kbps") for r in on) / len(on), sum(num(r, "net_pk_in") for r in on) / len(on),
            max(num(r, "net_pk_in") for r in on), int(min(num(r, "net_ping") for r in on)), int(max(num(r, "net_ping") for r in on)),
            max(num(r, "net_objects") for r in on), max(num(r, "net_pending_kb") for r in on),
            int(min(num(r, "wifi") for r in on)), int(max(num(r, "wifi") for r in on))))
    p("\nзвук: поток занят до %.1f%%, кэш до %.0f КБ; запись лога: %.1f мс/с в среднем, сам лог %.2f мс/с" % (
        max(num(r, "snd_busy_pct") for r in rows), max(num(r, "snd_cache_kb") for r in rows),
        sum(num(r, "log_write_ms") for r in rows) / len(rows), sum(num(r, "log_cost_ms") for r in rows) / len(rows)))

    ev = a.events or os.path.join(os.path.dirname(os.path.abspath(a.csv)), "perf_events.txt")
    if os.path.exists(ev):
        lines = open(ev, encoding="utf-8", errors="replace").read().splitlines()
        hitches = [l for l in lines if "] hitch " in l]
        important = [l for l in lines if l.startswith("[") and any(w in l for w in ("LOW", "OUT OF MEMORY", "VRAM:", "command buffer", "dropped", "failed", "map "))]
        p("\n== События (%s): рывков %d" % (os.path.basename(ev), len(hitches)))
        for l in sorted(hitches, key=lambda l: -float(l.split("hitch ")[1].split(" ")[0]))[:8]:
            p("  " + l)
        for l in important[:20]:
            p("  " + l)
        if "== session summary" in lines:
            p("")
            p("\n".join(lines[lines.index("== session summary"):]))
    print("\n".join(out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
