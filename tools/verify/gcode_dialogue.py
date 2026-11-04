"""Compare what two relief G-code files tell the machine.

usage: gcode_dialogue.py <old galvo.gc> <new galvo.gc> [--k K] [--feather N] [--json out.json]

Parses both files line by line and reports, for each: command vocabulary, parameter values (power S, feed F,
M-code arguments), V-axis positions, gantry (G200 X/Y) and galvo (A/B) ranges, laser-on path per layer, camera /
defocus blocks, head/tail lines. Then lists every difference between old and new, and checks that the new file
stays inside what the old one already asked the machine to do.
"""
import argparse, collections, json, math, re, sys

CMD = re.compile(rb"^([GM]\d+)(.*)$")
PAR = re.compile(rb"([A-Z])(-?\d+(?:\.\d*)?)")


def parse(path):
    r = {"path": path, "bytes": 0, "lines": 0, "cmd_count": collections.Counter(), "cmd_params": collections.defaultdict(set),
         "values": collections.defaultdict(collections.Counter), "comments": collections.Counter(), "info_lines": [],
         "v": [], "gantry_x": [math.inf, -math.inf], "gantry_y": [math.inf, -math.inf],
         "a": [math.inf, -math.inf], "b": [math.inf, -math.inf], "segments": [], "head": [], "tail": collections.deque(maxlen=12),
         "g0_with_laser": 0, "max_s": 0.0, "photo": collections.Counter(), "eol_crlf": 0}
    seg = None
    s_state, a_pos, b_pos = 0.0, 0.0, 0.0
    r["prefix"] = []          # lines before the first block (header, camera / defocus block)
    in_prefix = True

    def new_seg(vline, v):
        nonlocal seg
        seg = {"v": v, "vline": vline, "on_mm": 0.0, "on_moves": 0, "blocks": 0, "s": collections.Counter(), "f": collections.Counter(),
               "a": [math.inf, -math.inf], "b": [math.inf, -math.inf], "gx": [math.inf, -math.inf], "gy": [math.inf, -math.inf],
               # table position of the spot = gantry + galvo, for both possible axis pairings (A->X or A->Y)
               "on1": [math.inf, -math.inf, math.inf, -math.inf], "on2": [math.inf, -math.inf, math.inf, -math.inf],
               "off1": [math.inf, -math.inf, math.inf, -math.inf], "off2": [math.inf, -math.inf, math.inf, -math.inf]}
        r["segments"].append(seg)

    def grow(box, x, y):
        box[0] = min(box[0], x); box[1] = max(box[1], x); box[2] = min(box[2], y); box[3] = max(box[3], y)
    gx = gy = 0.0
    r["end_seq"] = []
    in_end = False

    with open(path, "rb") as fh:
        for raw in fh:
            r["bytes"] += len(raw)
            r["lines"] += 1
            if raw.endswith(b"\r\n"):
                r["eol_crlf"] += 1
            line = raw.rstrip(b"\r\n")
            if len(r["head"]) < 40:
                r["head"].append(line)
            if line:
                r["tail"].append(line)
                if line == b"# END":
                    in_end = True
                if in_end:
                    r["end_seq"].append(line)
            if in_prefix:
                if b"BITMAP START" in line:
                    in_prefix = False
                else:
                    r["prefix"].append(line)
            if not line:
                continue
            if line.startswith(b"#"):
                key = line[:28]
                r["comments"][key] += 1
                if line.startswith((b"# WorkingInfo", b"# gc=")):
                    r["info_lines"].append(line)
                if b"BITMAP START" in line and seg is not None:
                    seg["blocks"] += 1
                if b"photo" in line or b"defocus" in line:
                    r["photo"][line[:40]] += 1
                continue
            m = CMD.match(line)
            if not m:
                r["cmd_count"][b"?" + line[:12]] += 1
                continue
            cmd, rest = m.group(1), m.group(2)
            r["cmd_count"][cmd] += 1
            pars = {k: float(v) for k, v in PAR.findall(rest)}
            for k in pars:
                r["cmd_params"][cmd].add(k)
            # values worth comparing exactly (everything except coordinates)
            for k, v in pars.items():
                if cmd in (b"G0", b"G1") and k in (b"A", b"B"):
                    continue
                if cmd == b"G200" and k in (b"X", b"Y", b"V"):
                    continue
                r["values"][(cmd + b" " + k).decode()][v] += 1
            if cmd == b"G200":
                if b"V" in pars:
                    r["v"].append((pars[b"V"], pars.get(b"F")))
                    if pars.get(b"F") == 600 or seg is None:
                        new_seg(len(r["v"]) - 1, pars[b"V"])
                if b"X" in pars:
                    x = gx = pars[b"X"]
                    r["gantry_x"] = [min(r["gantry_x"][0], x), max(r["gantry_x"][1], x)]
                    if seg is not None:
                        seg["gx"] = [min(seg["gx"][0], x), max(seg["gx"][1], x)]
                if b"Y" in pars:
                    y = gy = pars[b"Y"]
                    r["gantry_y"] = [min(r["gantry_y"][0], y), max(r["gantry_y"][1], y)]
                    if seg is not None:
                        seg["gy"] = [min(seg["gy"][0], y), max(seg["gy"][1], y)]
                continue
            if cmd in (b"G0", b"G1"):
                if b"S" in pars:
                    s_state = pars[b"S"]
                    r["max_s"] = max(r["max_s"], s_state)
                na, nb = pars.get(b"A", a_pos), pars.get(b"B", b_pos)
                r["a"] = [min(r["a"][0], na), max(r["a"][1], na)]
                r["b"] = [min(r["b"][0], nb), max(r["b"][1], nb)]
                if seg is not None:
                    seg["a"] = [min(seg["a"][0], na), max(seg["a"][1], na)]
                    seg["b"] = [min(seg["b"][0], nb), max(seg["b"][1], nb)]
                    burning = cmd == b"G1" and s_state > 0
                    for p_a, p_b in ((a_pos, b_pos), (na, nb)) if burning else ((na, nb),):
                        grow(seg["on1" if burning else "off1"], gx + p_a, gy + p_b)
                        grow(seg["on2" if burning else "off2"], gx + p_b, gy + p_a)
                if cmd == b"G0" and s_state > 0:
                    r["g0_with_laser"] += 1
                    if seg is not None:
                        seg["g0_laser"] = seg.get("g0_laser", 0) + 1
                    else:
                        r["g0_laser_prefix"] = r.get("g0_laser_prefix", 0) + 1
                if cmd == b"G1" and s_state > 0 and seg is not None and (b"A" in pars or b"B" in pars):
                    seg["on_mm"] += math.hypot(na - a_pos, nb - b_pos)
                    seg["on_moves"] += 1
                    seg["s"][s_state] += 1
                if seg is not None and b"F" in pars:
                    seg["f"][pars[b"F"]] += 1
                a_pos, b_pos = na, nb
    return r


def split_layers(r):
    """Relief layers are the segments burnt at the relief power; the trailing ones at another power are cleaning."""
    segs = [s for s in r["segments"] if s["on_moves"]]
    if not segs:
        return [], []
    relief_s = segs[0]["s"].most_common(1)[0][0]
    relief = [s for s in segs if s["s"].most_common(1)[0][0] == relief_s]
    clean = [s for s in segs if s["s"].most_common(1)[0][0] != relief_s]
    return relief, clean


def rng(x):
    return "%.3f .. %.3f" % (x[0], x[1])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("old")
    ap.add_argument("new")
    ap.add_argument("--k", type=int, default=None, help="layers the new file is expected to skip")
    ap.add_argument("--feather", type=int, default=None,
                    help="feather passes the new file is expected to keep: the first N of the old file's (feather_passes)")
    ap.add_argument("--json")
    a = ap.parse_args()
    old, new = parse(a.old), parse(a.new)
    ok = True
    findings = []

    def check(name, cond, detail=""):
        nonlocal ok
        ok &= bool(cond)
        findings.append((("PASS" if cond else "FAIL"), name, detail))

    for tag, r in (("old", old), ("new", new)):
        rel, cl = split_layers(r)
        print("== %s: %s" % (tag, r["path"]))
        print("   %d bytes, %d lines (%d CRLF), relief layers %d, cleaning passes %d" % (r["bytes"], r["lines"], r["eol_crlf"], len(rel), len(cl)))
        print("   commands:", ", ".join("%s x%d" % (k.decode(), v) for k, v in sorted(r["cmd_count"].items())))
        print("   V: %d positions, first %s, relief %s, last %s" % (len(r["v"]), r["v"][:1], rng([min(s["v"] for s in rel), max(s["v"] for s in rel)]) if rel else "-", r["v"][-1:]))
        print("   gantry X %s, Y %s; galvo A %s, B %s; max S %g; G0 while laser on %d" % (rng(r["gantry_x"]), rng(r["gantry_y"]), rng(r["a"]), rng(r["b"]), r["max_s"], r["g0_with_laser"]))
    rel_o, cl_o = split_layers(old)
    rel_n, cl_n = split_layers(new)

    # 1. vocabulary and values
    new_cmds = set(new["cmd_count"]) - set(old["cmd_count"])
    check("no command type the old file did not use", not new_cmds, ", ".join(c.decode() for c in new_cmds))
    extra_params = {c.decode(): sorted(p.decode() for p in new["cmd_params"][c] - old["cmd_params"].get(c, set())) for c in new["cmd_params"]}
    extra_params = {c: p for c, p in extra_params.items() if p}
    check("no parameter letter the old file did not use", not extra_params, str(extra_params))
    val_diff = {}
    for key in sorted(set(old["values"]) | set(new["values"])):
        vo, vn = set(old["values"].get(key, {})), set(new["values"].get(key, {}))
        if vn - vo:
            val_diff[key] = sorted(vn - vo)
    check("no power / feed / argument value the old file did not use", not val_diff, str(val_diff)[:400])
    check("highest laser power not above the old file", new["max_s"] <= old["max_s"], "old %g new %g" % (old["max_s"], new["max_s"]))
    # A G0 is a galvo jump; it can follow a burning G1 without an explicit S0. Count where that happens.
    def g0_where(r, rel, cl):
        first = r["segments"][0] if r["segments"] else None
        return {"before layers": r.get("g0_laser_prefix", 0),
                "relief": sum(s.get("g0_laser", 0) for s in rel),
                "cleaning": sum(s.get("g0_laser", 0) for s in cl),
                "first segment": first.get("g0_laser", 0) if first else 0}
    go, gn = g0_where(old, rel_o, cl_o), g0_where(new, rel_n, cl_n)
    print("   G0 jumps right after a burning move: old %s, new %s" % (go, gn))
    check("relief layers: no more G0 jumps after a burning move than the old file", gn["relief"] <= go["relief"],
          "old %d new %d (cleaning passes: old %d new %d, re-planned on every run)" % (go["relief"], gn["relief"], go["cleaning"], gn["cleaning"]))

    # 2. motion envelope of the relief layers (the cleaning passes are planned anew on every run and are listed apart)
    def inside(n, o, tol=1e-6):
        return n[0] >= o[0] - tol and n[1] <= o[1] + tol

    def part(segs, key):
        lo = [s[key][0] for s in segs if s[key][0] != math.inf]
        hi = [s[key][1] for s in segs if s[key][1] != -math.inf]
        return [min(lo), max(hi)] if lo else [math.inf, -math.inf]
    def box(segs, key):
        bs = [s[key] for s in segs if s[key][0] != math.inf]
        return [min(b[0] for b in bs), max(b[1] for b in bs), min(b[2] for b in bs), max(b[3] for b in bs)] if bs else None

    def bstr(b):
        return "X %.3f..%.3f Y %.3f..%.3f" % tuple(b) if b else "-"

    def over(b, ref):
        """how far box b reaches outside box ref (mm, worst side)"""
        return max(ref[0] - b[0], b[1] - ref[1], ref[2] - b[2], b[3] - ref[3], 0.0)
    for key, name in (("gx", "gantry X"), ("gy", "gantry Y"), ("a", "galvo A"), ("b", "galvo B")):
        print("   coordinates (info) %-8s relief old %s new %s | cleaning old %s new %s" % (
            name, rng(part(rel_o, key)), rng(part(rel_n, key)), rng(part(cl_o, key)), rng(part(cl_n, key))))
    # the real axis pairing is the one that packs the old burn area tightest (the other one smears the blocks apart)
    area = {m: (b[1] - b[0]) * (b[3] - b[2]) for m in ("1", "2") for b in [box(rel_o, "on" + m)]}
    real = min(area, key=area.get)
    for m in ("1", "2"):
        on_o, on_n = box(rel_o, "on" + m), box(rel_n, "on" + m)
        pairing = "A->X, B->Y" if m == "1" else "A->Y, B->X"
        detail = "old %s | new %s | outside by %.3f mm" % (bstr(on_o), bstr(on_n), over(on_n, on_o))
        if m == real:
            check("laser-on spot on the table stays inside the old burn area (%s)" % pairing, over(on_n, on_o) <= 0.1, detail)
        else:
            print("   (other axis pairing %s, not physical: old area %.0f mm2 vs %.0f) %s" % (pairing, area[m], area[real], detail))
        for tag, so, sn in (("relief", rel_o, rel_n), ("cleaning", cl_o, cl_n)):
            fo, fn = box(so, "off" + m), box(sn, "off" + m)
            if fo and fn:
                print("   laser-off travel beyond the old burn area (%s, %s): old %.3f mm, new %.3f mm" % (
                    pairing, tag, over(fo, on_o), over(fn, on_o)))
    vo = [v for v, f in old["v"]]
    vn = [v for v, f in new["v"]]
    check("V positions within the old range", min(vn) >= min(vo) - 1e-9 and max(vn) <= max(vo) + 1e-9,
          "old %.3f..%.3f new %.3f..%.3f" % (min(vo), max(vo), min(vn), max(vn)))
    check("file header and camera / defocus block identical (everything before the first block)", old["prefix"] == new["prefix"],
          "%d vs %d lines" % (len(old["prefix"]), len(new["prefix"])))

    # 3. layer structure
    if rel_o and rel_n:
        k = a.k if a.k is not None else len(rel_o) - len(rel_n)
        check("relief layers = old - k", len(rel_n) == len(rel_o) - k, "old %d new %d k %d" % (len(rel_o), len(rel_n), k))
        check("first relief layer at the old first-layer V (original surface)", abs(rel_n[0]["v"] - rel_o[0]["v"]) < 1e-9,
              "old %.3f new %.3f" % (rel_o[0]["v"], rel_n[0]["v"]))
        steps_o = sorted({round(rel_o[i + 1]["v"] - rel_o[i]["v"], 6) for i in range(len(rel_o) - 1)})
        steps_n = sorted({round(rel_n[i + 1]["v"] - rel_n[i]["v"], 6) for i in range(len(rel_n) - 1)})
        check("same V step per layer", steps_o == steps_n, "old %s new %s" % (steps_o, steps_n))
        pairs = [(rel_n[j]["on_mm"], rel_o[j + k]["on_mm"]) for j in range(len(rel_n)) if j + k < len(rel_o) and rel_o[j + k]["on_mm"] > 0]
        ratios = [n / o for n, o in pairs]
        worst = max(ratios, key=lambda x: abs(x - 1)) if ratios else 1
        # a layer scanned at another angle differs by a line or two: 5% relative, or 0.5 m absolute for near-empty layers
        bad = [(n, o) for n, o in pairs if abs(n - o) > max(0.05 * o, 500.0)]
        check("laser-on path of each new layer matches old layer j+k (5% or 0.5 m)", not bad,
              "ratio range %.4f .. %.4f (worst %.4f), largest difference %.2f m, layers off: %d" % (
                  min(ratios), max(ratios), worst, max(abs(n - o) for n, o in pairs) / 1000, len(bad)))
        skipped = sum(s["on_mm"] for s in rel_o[:k])
        print("   laser-on path: old relief %.1f m (first %d layers %.1f m), new relief %.1f m" % (
            sum(s["on_mm"] for s in rel_o) / 1000, k, skipped / 1000, sum(s["on_mm"] for s in rel_n) / 1000))
        print("   per-layer laser-on path, old layer -> new layer (m):")
        for j in range(0, len(rel_o)):
            line = "     old %2d V%.3f %8.2f" % (j, rel_o[j]["v"], rel_o[j]["on_mm"] / 1000)
            if j >= k and j - k < len(rel_n):
                n = rel_n[j - k]
                line += "  -> new %2d V%.3f %8.2f  ratio %.4f" % (j - k, n["v"], n["on_mm"] / 1000, n["on_mm"] / rel_o[j]["on_mm"] if rel_o[j]["on_mm"] else 0)
            else:
                line += "  -> (not engraved)"
            if j < k + 3 or j > len(rel_o) - 3 or j % 8 == 0:
                print(line)
    if a.feather is None:
        check("feather (cleaning) passes: same count as old, or none (featherPower 0 declared)", len(cl_n) in (0, len(cl_o)),
              "old %d new %d" % (len(cl_o), len(cl_n)))
    else:
        # passes are re-planned on every run, so only their count and focus heights (V) can be compared across runs
        check("feather (cleaning) passes: the first %d of the old file's, in order (V of each pass)" % a.feather,
              len(cl_n) == a.feather <= len(cl_o) and all(abs(cl_n[i]["v"] - cl_o[i]["v"]) < 1e-9 for i in range(len(cl_n))),
              "old %d V %s | new %d V %s" % (len(cl_o), [s["v"] for s in cl_o], len(cl_n), [s["v"] for s in cl_n]))

    # 4. camera / defocus detection, head info, end sequence
    check("camera / defocus blocks unchanged", old["photo"] == new["photo"], "old %s new %s" % (dict(old["photo"]), dict(new["photo"])))
    info_o, info_n = set(old["info_lines"]), set(new["info_lines"])
    print("   head info lines only in old:", [x.decode()[:160] for x in info_o - info_n])
    print("   head info lines only in new:", [x.decode()[:160] for x in info_n - info_o])
    to = [x for x in old["tail"]]
    tn = [x for x in new["tail"]]
    check("end sequence identical (laser off, V/X/Y/U home, M6)", old["end_seq"] == new["end_seq"] and len(new["end_seq"]) > 3,
          " / ".join(x.decode() for x in new["end_seq"]))
    check("file complete (ends with the tail block)", bool(tn) and tn[-1].startswith(b"# JS001") and b"TAIL" in tn[-1], tn[-1].decode() if tn else "")
    check("same line endings", (old["eol_crlf"] == 0) == (new["eol_crlf"] == 0))

    print()
    for st, name, detail in findings:
        print("%s  %s%s" % (st, name, ("  | " + detail) if detail else ""))
    print("RESULT:", "ALL PASS" if ok else "SOME FAILED")
    if a.json:
        json.dump({"ok": ok, "findings": findings}, open(a.json, "w", encoding="utf-8"), ensure_ascii=False, indent=1)


if __name__ == "__main__":
    main()
