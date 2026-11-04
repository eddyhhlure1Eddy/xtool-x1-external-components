#!/usr/bin/env python3
"""
machine_safety_check.py - independent machine-safety diff for xTool X1 galvo G-code.

Question answered:
    Does NEW ask the machine for anything OLD did not, apart from the one intended
    change "NEW skips the first K relief layers of OLD" (keep-top)?

Usage:
    python -I machine_safety_check.py OLD.gc NEW.gc --k K [--feather N] [--json report.json]

Output: one PASS/FAIL line per check, INFO lines, a final VERDICT line.
Exit code: 0 = every check passed, 1 = at least one check failed, 2 = usage/IO error.

Model of the file format (learned from the files themselves):
  * one command per line, comment lines start with '#';
  * galvo moves  G0A<a>B<b>            jump, laser off
                 G1A<a>B<b>[F..][S..]  A or B may be omitted (modal); S = laser power, modal;
  * gantry moves G200X<x>Y<y>[F..];  focus moves G200 V<v>[ ]F<f>;
  * blocks       '# JS001 5W BITMAP START' ... '# motion_end';
  * a relief layer starts with a block containing a V move (the first relief layer
    uses the header V move); the trailing "cleaning" passes are single blocks with
    their own V-move format and block-start feed;
  * G106 ... G107 brackets a camera ("take photo") suspension.
The spot position on the table is gantry + galvo; which galvo axis pairs with which
gantry axis is decided from OLD (the relief burn area must match the engraved image).
Relief burn segments run about 1 mm past the image along their scan direction, so
layers scanned at different angles are compared with the segment ends trimmed.

Both files are streamed once; only laser-on moves are kept, in compact arrays.
"""

import argparse
import json
import math
import re
import sys
import time
from array import array
from collections import Counter

import numpy as np

try:
    from scipy import ndimage as ndi
except ImportError:  # pragma: no cover
    ndi = None

NUM = r'-?\d+(?:\.\d+)?'
UNUM = r'\d+(?:\.\d+)?'
# hot line shapes; every other line goes through the generic tokenizer
MOVE_RE = re.compile(r'G([01])(?:A(' + NUM + r'))?(?:B(' + NUM + r'))?(?:F(' + UNUM + r'))?(?:S(' + UNUM + r'))?\Z')
GANTRY_RE = re.compile(r'G200X(' + NUM + r')Y(' + NUM + r')(?:F(' + UNUM + r'))?\Z')
VMOVE_RE = re.compile(r'G200 V(' + NUM + r')( ?)F(' + UNUM + r')\Z')
WORD_RE = re.compile(r'\s*(?:("[^"]*")|([A-Za-z])([-+]?(?:\d+(?:\.\d*)?|\.\d+)|#)|(\S))')

# parameter letters that are coordinates (their values are not vocabulary)
COORD_LETTERS = {'G0': 'AB', 'G1': 'AB', 'G200': 'XYV'}

BLOCK_START = '# JS001 5W BITMAP START'
BLOCK_END = '# motion_end'
END_MARK = '# END'
TAIL_MARK = '# JS001 5W TAIL'
INF = float('inf')
LONG_OFF = 20.0     # laser-off galvo moves longer than this (mm) are counted


# --------------------------------------------------------------------------------------
# tokenizer / templates
# --------------------------------------------------------------------------------------
def tokenize(line):
    """Words of a line: (letter, value, start, end), ('"', text, s, e) or ('?', junk, s, e).
    A value may be '#' (masked coordinate in a template)."""
    words = []
    pos = 0
    n = len(line)
    while pos < n:
        m = WORD_RE.match(line, pos)
        if m is None or m.end() == pos:
            break
        q, letter, val, bad = m.groups()
        if q is not None:
            words.append(('"', q, m.start(1), m.end(1)))
        elif letter is not None:
            words.append((letter, val, m.start(3), m.end(3)))
        else:
            words.append(('?', bad, m.start(4), m.end(4)))
        pos = m.end()
    return words


def command_of(words):
    if words and words[0][0] not in ('"', '?'):
        return words[0][0] + words[0][1]
    return '?'


def make_template(line, words, cmd):
    """Mask coordinate values with '#', keep everything else verbatim (incl. spacing)."""
    coords = COORD_LETTERS.get(cmd, '')
    out = []
    last = 0
    for w in words[1:]:
        if w[0] in coords and w[1] != '#':
            out.append(line[last:w[2]])
            out.append('#')
            last = w[3]
    out.append(line[last:])
    return ''.join(out)


def key_str(key):
    """Template keys of the hot paths are tuples; turn them into template strings."""
    if not isinstance(key, tuple):
        return key
    kind = key[0]
    if kind in ('0', '1'):
        _, ha, hb, f, s = key
        return 'G' + kind + ('A#' if ha else '') + ('B#' if hb else '') + ('F' + f if f else '') + ('S' + s if s else '')
    if kind == 'GXY':
        return 'G200X#Y#' + ('F' + key[1] if key[1] else '')
    if kind == 'GV':
        return 'G200 V#' + key[1] + 'F' + key[2]
    return str(key)


def fnum(x, nd=3):
    if x is None:
        return 'n/a'
    if isinstance(x, float) and (math.isinf(x) or math.isnan(x)):
        return str(x)
    return f'{x:.{nd}f}'


# --------------------------------------------------------------------------------------
# streaming parser
# --------------------------------------------------------------------------------------
class Parsed:
    pass


def parse_gcode(path, label, log, ref_galvo=None):
    """Stream one file.  ref_galvo = (minA, maxA, minB, maxB): galvo endpoints outside
    it are counted (used for NEW against OLD's envelope)."""
    t0 = time.time()
    p = Parsed()
    p.path = path
    p.label = label
    rA0, rA1, rB0, rB1 = ref_galvo if ref_galvo else (-INF, INF, -INF, INF)

    tmpl = {}                 # template key -> [count, first line]
    header_lines = []         # lines before the first block
    end_lines = []            # lines from '# END' to EOF
    blocks = []
    layers = []
    suspensions = []          # camera suspensions G106..G107
    vmoves = []               # (line, V, template, where)
    hot_travel = []           # (line, text, layer, kind, suspended, right_after_gantry_move)
    hot_block_starts = []
    burns_outside_blocks = []
    burns_while_suspended = []
    unknown_start_burns = []
    moves_outside_blocks = 0
    v_outside_blocks = []
    v_after_moves = []
    unclosed_blocks = 0
    stray_block_ends = 0
    n_end_markers = 0
    s_at_end_marker = None
    m6_hot = []
    nonascii_lines = 0
    nul_lines = 0
    n_crlf = 0
    last_unterminated = False
    tail_line = None
    last_nonblank_line = None
    last_nonblank_no = 0
    density = None
    offset = None
    process_bbox = None
    galvo_center = None
    bb_map = {}               # (layer, gantry tile) -> [minA, maxA, minB, maxB] of galvo move endpoints
    out_of_ref = []           # (line, layer) of galvo endpoints outside ref_galvo (first 200)
    out_of_ref_layers = Counter()
    long_off = Counter()      # layer -> number of laser-off G1 moves longer than LONG_OFF
    long_off_max = {}         # layer -> (length, line)

    seg_tile = array('i')
    seg_a0 = array('f')
    seg_b0 = array('f')
    seg_a1 = array('f')
    seg_b1 = array('f')
    seg_layer = array('i')
    seg_line = array('i')
    seg_feed = array('f')
    seg_power = array('f')
    tiles = {}
    tile_xy = []

    # modal machine state
    S = 0.0
    F = None
    A = None
    B = None
    gx = None
    gy = None
    V = None
    suspended = False
    susp_cur = None
    in_block = False
    blk = None
    layer = -1
    tile = -1
    header_done = False
    collecting_end = False
    after_gantry = False
    ba0 = bb0 = INF
    ba1 = bb1 = -INF

    move_match = MOVE_RE.match
    gantry_match = GANTRY_RE.match
    vmove_match = VMOVE_RE.match
    tmpl_get = tmpl.get

    def flush_bbox(lay, t, a0, a1, b0, b1):
        if a0 <= a1 or b0 <= b1:
            e = bb_map.get((lay, t))
            if e is None:
                bb_map[(lay, t)] = [a0, a1, b0, b1]
            else:
                e[0] = min(e[0], a0); e[1] = max(e[1], a1)
                e[2] = min(e[2], b0); e[3] = max(e[3], b1)

    def tile_of(x, y):
        k = (x, y)
        t = tiles.get(k)
        if t is None:
            t = len(tile_xy)
            tiles[k] = t
            tile_xy.append(k)
        return t

    n = 0
    with open(path, 'r', encoding='latin-1', newline='') as fh:
        for raw in fh:
            n += 1
            if raw[-1:] == '\n':
                if raw[-2:-1] == '\r':
                    line = raw[:-2]
                    n_crlf += 1
                else:
                    line = raw[:-1]
            else:
                line = raw
                last_unterminated = True
            if not header_done:
                header_lines.append(line)
            if collecting_end:
                end_lines.append(line)
            if not line:
                continue
            last_nonblank_line = line
            last_nonblank_no = n

            c0 = line[0]
            vals = None
            if c0 == 'G':
                # ---------------- hot path 1: galvo move
                m = move_match(line)
                if m is not None:
                    g, a_s, b_s, f_s, s_s = m.groups()
                    key = (g, a_s is not None, b_s is not None, f_s, s_s)
                    t = tmpl_get(key)
                    if t is None:
                        tmpl[key] = [1, n]
                    else:
                        t[0] += 1
                    if f_s is not None:
                        F = float(f_s)
                        if in_block:
                            blk['sig'].append(key)
                            if blk['start_tmpl'] is None:
                                blk['start_tmpl'] = key
                    if s_s is not None:
                        S = float(s_s)
                    na = float(a_s) if a_s is not None else A
                    nb = float(b_s) if b_s is not None else B
                    if g == '1':
                        if S > 0.0:
                            if A is None or B is None or na is None or nb is None:
                                unknown_start_burns.append(n)
                            else:
                                seg_tile.append(tile)
                                seg_a0.append(A)
                                seg_b0.append(B)
                                seg_a1.append(na)
                                seg_b1.append(nb)
                                seg_layer.append(layer)
                                seg_line.append(n)
                                seg_feed.append(F if F is not None else 0.0)
                                seg_power.append(S)
                            if not in_block:
                                burns_outside_blocks.append(n)
                            if suspended:
                                burns_while_suspended.append(n)
                        elif A is not None and B is not None and na is not None and nb is not None:
                            if abs(na - A) + abs(nb - B) > LONG_OFF:
                                d = math.hypot(na - A, nb - B)
                                if d > LONG_OFF:
                                    long_off[layer] += 1
                                    if d > long_off_max.get(layer, (0.0, 0))[0]:
                                        long_off_max[layer] = (d, n)
                    elif S > 0.0 and len(hot_travel) < 100000:
                        hot_travel.append((n, line, layer, 'G0 jump', suspended, after_gantry))
                    after_gantry = False
                    if suspended and susp_cur is not None:
                        susp_cur['moves'].append((g, na, nb, n))
                    if na is not None:
                        if na < ba0:
                            ba0 = na
                        if na > ba1:
                            ba1 = na
                        if na < rA0 or na > rA1:
                            out_of_ref_layers[layer] += 1
                            if len(out_of_ref) < 200:
                                out_of_ref.append((n, layer))
                    if nb is not None:
                        if nb < bb0:
                            bb0 = nb
                        if nb > bb1:
                            bb1 = nb
                        if (nb < rB0 or nb > rB1) and not (na is not None and (na < rA0 or na > rA1)):
                            out_of_ref_layers[layer] += 1
                            if len(out_of_ref) < 200:
                                out_of_ref.append((n, layer))
                    A = na
                    B = nb
                    if not in_block and (a_s is not None or b_s is not None):
                        moves_outside_blocks += 1
                    continue
                # ---------------- hot path 2: gantry move
                m = gantry_match(line)
                if m is not None:
                    x_s, y_s, f_s = m.groups()
                    key = ('GXY', f_s)
                    t = tmpl_get(key)
                    if t is None:
                        tmpl[key] = [1, n]
                    else:
                        t[0] += 1
                    if S > 0.0 and len(hot_travel) < 100000:
                        hot_travel.append((n, line, layer, 'gantry move', suspended, False))
                    nx = float(x_s)
                    ny = float(y_s)
                    if nx != gx or ny != gy:
                        flush_bbox(layer, tile, ba0, ba1, bb0, bb1)
                        ba0 = bb0 = INF
                        ba1 = bb1 = -INF
                        gx = nx
                        gy = ny
                        tile = tile_of(gx, gy)
                    after_gantry = True
                    if in_block:
                        blk['sig'].append(key)
                        blk['n_gantry'] += 1
                    if suspended and susp_cur is not None:
                        susp_cur['gantry'].append((nx, ny, n, line))
                    continue
                # ---------------- hot path 3: focus (V) move -> shared G200 handling below
                m = vmove_match(line)
                if m is not None:
                    v_s, sp, f_s = m.groups()
                    tkey = ('GV', sp, f_s)
                    t = tmpl_get(tkey)
                    if t is None:
                        tmpl[tkey] = [1, n]
                    else:
                        t[0] += 1
                    vals = {'V': v_s, 'F': f_s}
                    cmd = 'G200'

            # ---------------- comments
            if c0 == '#':
                if not line.isascii():
                    nonascii_lines += 1
                key = '# date=*' if line.startswith('# date=') else line
                t = tmpl_get(key)
                if t is None:
                    tmpl[key] = [1, n]
                else:
                    t[0] += 1
                if in_block:
                    blk['sig'].append(key)
                if suspended and susp_cur is not None:
                    susp_cur['lines'].append(line)
                if line == BLOCK_START:
                    if not header_done:
                        header_done = True
                        header_lines.pop()
                    if in_block:
                        unclosed_blocks += 1
                        blk['end'] = n - 1
                    if S > 0.0:
                        hot_block_starts.append(n)
                    if layer < 0:
                        flush_bbox(layer, tile, ba0, ba1, bb0, bb1)
                        ba0 = bb0 = INF
                        ba1 = bb1 = -INF
                        layer = 0
                        layers.append(dict(V=V, v_line=None, v_tmpl='HEADER', start_tmpl=None,
                                           first_block=len(blocks), n_blocks=0, line=n))
                    in_block = True
                    blk = dict(start=n, end=None, layer=layer, sig=[BLOCK_START], start_tmpl=None,
                               n_gantry=0, starts_layer=False)
                    blocks.append(blk)
                    layers[layer]['n_blocks'] += 1
                elif line == BLOCK_END:
                    if not in_block:
                        stray_block_ends += 1
                    else:
                        blk['end'] = n
                        in_block = False
                elif line == END_MARK:
                    n_end_markers += 1
                    s_at_end_marker = S
                    if in_block:
                        unclosed_blocks += 1
                        in_block = False
                    if not collecting_end:
                        collecting_end = True
                        end_lines.append(line)
                elif line == TAIL_MARK:
                    tail_line = n
                elif line.startswith('# blockConfig=') and density is None:
                    md = re.search(r'"density":"?([0-9.]+)', line)
                    if md:
                        density = float(md.group(1))
                    mo = re.search(r'"offset":\{"x":(' + NUM + r'),"y":(' + NUM + r')\}', line)
                    if mo:
                        offset = (float(mo.group(1)), float(mo.group(2)))
                elif line.startswith('# WorkingInfo={"process_bbox"'):
                    mb = re.search(r'\[([^\]]*)\]', line)
                    if mb:
                        try:
                            process_bbox = [float(v) for v in mb.group(1).split(',')]
                        except ValueError:
                            process_bbox = None
                continue

            # ---------------- generic path (rare lines)
            if vals is None:
                if not line.isascii():
                    nonascii_lines += 1
                if '\x00' in line:
                    nul_lines += 1
                if not line.strip():
                    tkey = 'WHITESPACE:' + repr(line)
                    t = tmpl_get(tkey)
                    if t is None:
                        tmpl[tkey] = [1, n]
                    else:
                        t[0] += 1
                    continue
                words = tokenize(line)
                cmd = command_of(words)
                tkey = make_template(line, words, cmd)
                t = tmpl_get(tkey)
                if t is None:
                    tmpl[tkey] = [1, n]
                else:
                    t[0] += 1
                vals = {}
                for w in words[1:]:
                    if w[0] not in ('"', '?'):
                        vals[w[0]] = w[1]
            if in_block:
                blk['sig'].append(tkey)
            if suspended and susp_cur is not None and cmd not in ('G0', 'G1', 'G200'):
                susp_cur['lines'].append(line)

            if cmd in ('G0', 'G1'):
                f_s = vals.get('F')
                s_s = vals.get('S')
                moved = ('A' in vals) or ('B' in vals)
                if f_s is not None:
                    F = float(f_s)
                    if in_block and moved and blk['start_tmpl'] is None:
                        blk['start_tmpl'] = tkey
                if s_s is not None:
                    S = float(s_s)
                na = float(vals['A']) if 'A' in vals else A
                nb = float(vals['B']) if 'B' in vals else B
                if cmd == 'G1' and S > 0.0 and moved:
                    if A is None or B is None or na is None or nb is None:
                        unknown_start_burns.append(n)
                    else:
                        seg_tile.append(tile)
                        seg_a0.append(A)
                        seg_b0.append(B)
                        seg_a1.append(na)
                        seg_b1.append(nb)
                        seg_layer.append(layer)
                        seg_line.append(n)
                        seg_feed.append(F if F is not None else 0.0)
                        seg_power.append(S)
                    if not in_block:
                        burns_outside_blocks.append(n)
                    if suspended:
                        burns_while_suspended.append(n)
                elif cmd == 'G0' and S > 0.0 and moved and len(hot_travel) < 100000:
                    hot_travel.append((n, line, layer, 'G0 jump', suspended, after_gantry))
                if moved:
                    after_gantry = False
                    if suspended and susp_cur is not None:
                        susp_cur['moves'].append((cmd[1], na, nb, n))
                    if na is not None:
                        ba0 = min(ba0, na)
                        ba1 = max(ba1, na)
                    if nb is not None:
                        bb0 = min(bb0, nb)
                        bb1 = max(bb1, nb)
                    if (na is not None and (na < rA0 or na > rA1)) or (nb is not None and (nb < rB0 or nb > rB1)):
                        out_of_ref_layers[layer] += 1
                        if len(out_of_ref) < 200:
                            out_of_ref.append((n, layer))
                    if not in_block:
                        moves_outside_blocks += 1
                A = na
                B = nb
            elif cmd == 'G200':
                motion = any(k in vals for k in 'XYVUAB')
                if motion and S > 0.0 and len(hot_travel) < 100000:
                    kind = 'V move' if 'V' in vals else ('gantry move' if ('X' in vals or 'Y' in vals) else 'G200 move')
                    hot_travel.append((n, line, layer, kind, suspended, False))
                if not header_done and 'A' in vals and 'B' in vals and galvo_center is None:
                    try:
                        galvo_center = (float(vals['A']), float(vals['B']))
                    except ValueError:
                        pass
                if 'A' in vals:
                    A = float(vals['A'])
                if 'B' in vals:
                    B = float(vals['B'])
                if 'X' in vals or 'Y' in vals:
                    nx = float(vals['X']) if 'X' in vals else gx
                    ny = float(vals['Y']) if 'Y' in vals else gy
                    if nx != gx or ny != gy:
                        flush_bbox(layer, tile, ba0, ba1, bb0, bb1)
                        ba0 = bb0 = INF
                        ba1 = bb1 = -INF
                        gx = nx
                        gy = ny
                        if gx is not None and gy is not None:
                            tile = tile_of(gx, gy)
                    after_gantry = True
                    if in_block:
                        blk['n_gantry'] += 1
                    if suspended and susp_cur is not None:
                        susp_cur['gantry'].append((nx, ny, n, line))
                if 'V' in vals:
                    try:
                        v = float(vals['V'])
                    except ValueError:
                        v = None
                    if collecting_end:
                        where = 'end'
                    elif not header_done:
                        where = 'header'
                    elif in_block:
                        where = 'block'
                    else:
                        where = 'between-blocks'
                        v_outside_blocks.append(n)
                    vt = key_str(tkey)
                    vmoves.append((n, v, vt, where))
                    V = v
                    if where in ('block', 'between-blocks') and layer >= 0:
                        # a V move after the first block starts a new layer
                        flush_bbox(layer, tile, ba0, ba1, bb0, bb1)
                        ba0 = bb0 = INF
                        ba1 = bb1 = -INF
                        layer += 1
                        layers.append(dict(V=v, v_line=n, v_tmpl=vt, start_tmpl=None,
                                           first_block=len(blocks) - (1 if in_block else 0),
                                           n_blocks=1 if in_block else 0, line=n))
                        if in_block:
                            layers[layer - 1]['n_blocks'] -= 1
                            blk['layer'] = layer
                            blk['starts_layer'] = True
                            if blk['start_tmpl'] is not None:
                                v_after_moves.append(n)
            elif cmd == 'G106':
                suspended = True
                susp_cur = dict(start=n, gantry_before=(gx, gy), galvo_before=(A, B), S_before=S,
                                lines=[line], gantry=[], moves=[], end=None)
                suspensions.append(susp_cur)
            elif cmd == 'G107':
                if susp_cur is not None:
                    susp_cur['end'] = n
                suspended = False
                susp_cur = None
            elif cmd == 'M6':
                if S > 0.0:
                    m6_hot.append(n)

    flush_bbox(layer, tile, ba0, ba1, bb0, bb1)
    if in_block:
        unclosed_blocks += 1

    p.n_lines = n
    p.templates = {}
    for k, v in tmpl.items():
        sk = key_str(k)
        if sk in p.templates:
            p.templates[sk][0] += v[0]
            p.templates[sk][1] = min(p.templates[sk][1], v[1])
        else:
            p.templates[sk] = [v[0], v[1]]
    p.header_lines = header_lines if header_done else []
    p.end_lines = end_lines
    p.blocks = blocks
    p.layers = layers
    p.suspensions = suspensions
    p.vmoves = vmoves
    p.hot_travel = hot_travel
    p.hot_block_starts = hot_block_starts
    p.burns_outside_blocks = burns_outside_blocks
    p.burns_while_suspended = burns_while_suspended
    p.unknown_start_burns = unknown_start_burns
    p.moves_outside_blocks = moves_outside_blocks
    p.v_outside_blocks = v_outside_blocks
    p.v_after_moves = v_after_moves
    p.unclosed_blocks = unclosed_blocks
    p.stray_block_ends = stray_block_ends
    p.n_end_markers = n_end_markers
    p.s_at_end_marker = s_at_end_marker
    p.s_at_eof = S
    p.m6_hot = m6_hot
    p.nonascii_lines = nonascii_lines
    p.nul_lines = nul_lines
    p.n_crlf = n_crlf
    p.last_unterminated = last_unterminated
    p.tail_line = tail_line
    p.last_nonblank_line = last_nonblank_line
    p.last_nonblank_no = last_nonblank_no
    p.density = density
    p.offset = offset
    p.process_bbox = process_bbox
    p.galvo_center = galvo_center
    p.bb_map = bb_map
    p.out_of_ref = out_of_ref
    p.out_of_ref_layers = out_of_ref_layers
    p.long_off = long_off
    p.long_off_max = long_off_max
    p.tile_xy = np.array(tile_xy, dtype=np.float64).reshape(-1, 2)

    def f64(a):
        return np.frombuffer(a, dtype=np.float32).astype(np.float64) if len(a) else np.zeros(0)

    def i32(a):
        return np.frombuffer(a, dtype=np.int32).copy() if len(a) else np.zeros(0, np.int32)
    p.seg_tile = i32(seg_tile)
    p.seg_a0 = f64(seg_a0)
    p.seg_b0 = f64(seg_b0)
    p.seg_a1 = f64(seg_a1)
    p.seg_b1 = f64(seg_b1)
    p.seg_layer = i32(seg_layer)
    p.seg_line = i32(seg_line)
    p.seg_feed = f64(seg_feed)
    p.seg_power = f64(seg_power)
    p.seg_len = np.hypot(p.seg_a1 - p.seg_a0, p.seg_b1 - p.seg_b0)
    p.parse_seconds = time.time() - t0
    log(f'[{label}] parsed {n:,} lines, {len(blocks)} blocks, {len(layers)} layers, '
        f'{len(p.seg_len):,} laser-on moves in {p.parse_seconds:.1f} s')
    return p


# --------------------------------------------------------------------------------------
# derived structure
# --------------------------------------------------------------------------------------
def layer_key(L):
    return (L['v_tmpl'], key_str(L['start_tmpl']))


def finish_structure(p):
    for L in p.layers:
        fb = L['first_block']
        if 0 <= fb < len(p.blocks):
            L['start_tmpl'] = p.blocks[fb]['start_tmpl']


def learn_kinds(old):
    """From OLD: relief layer key (layer 1) and cleaning key (key of the last layer)."""
    keys = [layer_key(L) for L in old.layers]
    relief_key = keys[1] if len(keys) > 1 else None
    clean_key = keys[-1] if len(keys) > 1 and keys[-1] != relief_key else None
    return relief_key, clean_key


def classify(p, relief_key, clean_key):
    kinds = []
    for i, L in enumerate(p.layers):
        k = layer_key(L)
        if i == 0:
            kinds.append('relief')
        elif relief_key is not None and k == relief_key:
            kinds.append('relief')
        elif clean_key is not None and k == clean_key:
            kinds.append('cleaning')
        else:
            kinds.append('unknown')
    p.kinds = kinds
    p.relief_layers = [i for i, k in enumerate(kinds) if k == 'relief']
    p.clean_layers = [i for i, k in enumerate(kinds) if k == 'cleaning']
    p.unknown_layers = [i for i, k in enumerate(kinds) if k == 'unknown']
    nL = len(p.layers)
    lay = p.seg_layer
    valid = lay >= 0
    p.layer_len = np.bincount(lay[valid], weights=p.seg_len[valid], minlength=nL) if nL else np.zeros(0)
    p.layer_non = np.bincount(lay[valid], minlength=nL) if nL else np.zeros(0, np.int64)
    with np.errstate(divide='ignore', invalid='ignore'):
        tsec = np.where(p.seg_feed > 0, p.seg_len / (p.seg_feed / 60.0), 0.0)
    p.layer_time = np.bincount(lay[valid], weights=tsec[valid], minlength=nL) if nL else np.zeros(0)
    kind_of_layer = np.array([{'relief': 0, 'cleaning': 1}.get(k, 2) for k in kinds] + [3], dtype=np.int8)
    p.seg_kind = kind_of_layer[np.where(valid, lay, nL)]
    order = np.argsort(lay, kind='stable')
    p.layer_index = {}
    if len(order):
        sl = lay[order]
        bounds = np.flatnonzero(np.diff(sl)) + 1
        starts = np.concatenate([[0], bounds])
        ends = np.concatenate([bounds, [len(sl)]])
        for s, e in zip(starts, ends):
            p.layer_index[int(sl[s])] = order[s:e]


def kind_of(p, lay):
    return p.kinds[lay] if 0 <= lay < len(p.kinds) else ('before first block' if lay < 0 else 'outside')


def layer_sel(p, lay):
    return p.layer_index.get(lay, np.zeros(0, np.int64))


def table_xy(p, sel, pairing):
    """Table coordinates (x0, y0, x1, y1) of the laser-on moves selected by sel (mask
    or index array).  pairing 'AX': x = X + A, y = Y + B;  'AY': x = X + B, y = Y + A."""
    g = p.tile_xy[p.seg_tile[sel]]
    gx = g[:, 0]
    gy = g[:, 1]
    a0 = p.seg_a0[sel]; b0 = p.seg_b0[sel]; a1 = p.seg_a1[sel]; b1 = p.seg_b1[sel]
    if pairing == 'AX':
        return gx + a0, gy + b0, gx + a1, gy + b1
    return gx + b0, gy + a0, gx + b1, gy + a1


def trim_ends(segs, e):
    """Shorten every segment by e at both ends; segments shorter than 2e are dropped.
    Returns the trimmed segments and the share of path that remains under test."""
    x0, y0, x1, y1 = segs
    L = np.hypot(x1 - x0, y1 - y0)
    if e <= 0:
        return segs, 1.0
    k = L > 2 * e
    ux = (x1 - x0)[k] / L[k]
    uy = (y1 - y0)[k] / L[k]
    out = (x0[k] + ux * e, y0[k] + uy * e, x1[k] - ux * e, y1[k] - uy * e)
    tot = L.sum()
    return out, (float((L[k] - 2 * e).sum() / tot) if tot > 0 else 0.0)


# --------------------------------------------------------------------------------------
# raster geometry
# --------------------------------------------------------------------------------------
def iter_samples(x0, y0, x1, y1, step, chunk=4_000_000):
    """Points spaced <= step along each segment, both endpoints included.  Yields
    (xs, ys, start, end, offs, rep): samples of segments start..end-1, offs = first
    sample of each segment within the chunk, rep = local segment index per sample."""
    nseg = len(x0)
    if nseg == 0:
        return
    L = np.hypot(x1 - x0, y1 - y0)
    nint = np.maximum(1, np.ceil(L / step)).astype(np.int64)
    cnt = nint + 1
    cum = np.cumsum(cnt)
    start = 0
    while start < nseg:
        base = cum[start - 1] if start > 0 else 0
        end = int(np.searchsorted(cum, base + chunk, side='right'))
        end = max(end, start + 1)
        c = cnt[start:end]
        offs = np.cumsum(c) - c
        rep = np.repeat(np.arange(end - start), c)
        k = np.arange(int(c.sum()), dtype=np.int64) - offs[rep]
        t = k / nint[start:end][rep]
        sx0 = x0[start:end]; sy0 = y0[start:end]
        xs = sx0[rep] + (x1[start:end] - sx0)[rep] * t
        ys = sy0[rep] + (y1[start:end] - sy0)[rep] * t
        yield xs, ys, start, end, offs, rep
        start = end


class Raster:
    """Grid that stores which cells a set of segments touches, and the distance (mm) from
    every cell centre to the nearest touched cell centre."""

    def __init__(self, xmin, ymin, xmax, ymax, cell, margin=0.0):
        self.cell = cell
        self.x0 = xmin
        self.y0 = ymin
        self.w = int(math.ceil((xmax - xmin) / cell)) + 1
        self.h = int(math.ceil((ymax - ymin) / cell)) + 1
        self.margin = margin
        self.mask = np.zeros((self.h, self.w), dtype=bool)
        self.dist = None

    def cells(self, xs, ys):
        ix = np.floor((xs - self.x0) / self.cell).astype(np.int64)
        iy = np.floor((ys - self.y0) / self.cell).astype(np.int64)
        inside = (ix >= 0) & (ix < self.w) & (iy >= 0) & (iy < self.h)
        return ix, iy, inside

    def mark(self, segs, step):
        flat = self.mask.reshape(-1)
        for xs, ys, *_ in iter_samples(*segs, step):
            ix, iy, inside = self.cells(xs, ys)
            flat[iy[inside] * self.w + ix[inside]] = True

    def build_distance(self):
        self.dist = ndi.distance_transform_edt(~self.mask).astype(np.float32)
        self.dist *= np.float32(self.cell)

    def point_dist(self, xs, ys):
        ix, iy, inside = self.cells(xs, ys)
        d = np.empty(len(xs), dtype=np.float64)
        d[inside] = self.dist[iy[inside], ix[inside]]
        if not inside.all():
            out = ~inside
            xe = self.x0 + self.w * self.cell
            ye = self.y0 + self.h * self.cell
            dx = np.maximum(np.maximum(self.x0 - xs[out], xs[out] - xe), 0)
            dy = np.maximum(np.maximum(self.y0 - ys[out], ys[out] - ye), 0)
            d[out] = np.hypot(dx, dy) + self.margin   # lower bound
        return d

    def segment_max_dist(self, segs, step):
        """Largest sample distance per segment, and the number of samples."""
        seg_max = np.zeros(len(segs[0]), dtype=np.float64)
        ns = 0
        for xs, ys, start, end, offs, _ in iter_samples(*segs, step):
            d = self.point_dist(xs, ys)
            seg_max[start:end] = np.maximum.reduceat(d, offs)
            ns += len(xs)
        return seg_max, ns

    def fraction_beyond(self, segs, step, thr, keep_above=None, keep=0):
        """Share of sample points (~ path length) farther than thr, the max distance, and
        (if keep > 0) up to `keep` points with raster distance > keep_above, largest first,
        as an array of rows (x, y, raster distance, segment index)."""
        tot = 0
        bad = 0
        dmax = 0.0
        kept = []
        for xs, ys, start, end, offs, rep in iter_samples(*segs, step):
            d = self.point_dist(xs, ys)
            tot += len(d)
            bad += int(np.count_nonzero(d > thr))
            dmax = max(dmax, float(d.max()))
            if keep and keep_above is not None:
                m = np.flatnonzero(d > keep_above)
                if len(m):
                    if len(m) > keep:
                        m = m[np.argsort(-d[m])[:keep]]
                    kept.append(np.column_stack([xs[m], ys[m], d[m], (start + rep[m]).astype(np.float64)]))
                    if sum(len(k) for k in kept) > 4 * keep:
                        allk = np.concatenate(kept)
                        kept = [allk[np.argsort(-allk[:, 2])[:keep]]]
        pts = np.zeros((0, 4))
        if kept:
            allk = np.concatenate(kept)
            pts = allk[np.argsort(-allk[:, 2])[:keep]]
        return (bad / tot if tot else 0.0), dmax, pts


def exact_dist_to_segments(px, py, segs, reach):
    """Exact distance from each point to the nearest of the segments; only segments whose
    bounding box comes within `reach` of the point are considered (others -> inf)."""
    x0, y0, x1, y1 = segs
    sxmin = np.minimum(x0, x1); sxmax = np.maximum(x0, x1)
    symin = np.minimum(y0, y1); symax = np.maximum(y0, y1)
    out = np.full(len(px), np.inf)
    for i in range(len(px)):
        r = reach[i] if np.ndim(reach) else reach
        m = np.flatnonzero((sxmin <= px[i] + r) & (sxmax >= px[i] - r) & (symin <= py[i] + r) & (symax >= py[i] - r))
        if len(m):
            dx = x1[m] - x0[m]
            dy = y1[m] - y0[m]
            L2 = dx * dx + dy * dy
            t = np.clip(((px[i] - x0[m]) * dx + (py[i] - y0[m]) * dy) / np.where(L2 > 0, L2, 1.0), 0.0, 1.0)
            out[i] = float(np.hypot(px[i] - (x0[m] + t * dx), py[i] - (y0[m] + t * dy)).min())
    return out


def seg_box(*seg_sets, margin):
    xs = np.concatenate([np.concatenate([s[0], s[2]]) for s in seg_sets if len(s[0])])
    ys = np.concatenate([np.concatenate([s[1], s[3]]) for s in seg_sets if len(s[0])])
    return float(xs.min()) - margin, float(ys.min()) - margin, float(xs.max()) + margin, float(ys.max()) + margin


def scan_angle_deg(x0, y0, x1, y1):
    """Length-weighted dominant direction of the segments (degrees, mod 180)."""
    dx = x1 - x0
    dy = y1 - y0
    L = np.hypot(dx, dy)
    if len(L) == 0 or L.sum() <= 0:
        return None
    th = np.arctan2(dy, dx)
    c = np.sum(L * np.cos(2 * th))
    s = np.sum(L * np.sin(2 * th))
    return math.degrees(0.5 * math.atan2(s, c)) % 180.0


# --------------------------------------------------------------------------------------
# report
# --------------------------------------------------------------------------------------
class Report:
    def __init__(self):
        self.checks = []
        self.info = []

    def check(self, name, ok, detail, data=None):
        status = 'N/A' if ok is None else ('PASS' if ok else 'FAIL')
        self.checks.append(dict(name=name, status=status, detail=detail, data=data))
        print(f'{status:4s}  {name}: {detail}', flush=True)

    def note(self, name, detail, data=None):
        self.info.append(dict(name=name, detail=detail, data=data))
        print(f'INFO  {name}: {detail}', flush=True)


def choose_pairing(old, rep):
    """Decide which galvo axis pairs with which gantry axis from OLD's relief."""
    sel = old.seg_kind == 0
    res = {}
    first = layer_sel(old, old.relief_layers[0])
    for pr in ('AX', 'AY'):
        x0, y0, x1, y1 = table_xy(old, sel, pr)
        xs = np.concatenate([x0, x1])
        ys = np.concatenate([y0, y1])
        r = dict(xmin=float(xs.min()), xmax=float(xs.max()), ymin=float(ys.min()), ymax=float(ys.max()))
        r['w'] = r['xmax'] - r['xmin']
        r['h'] = r['ymax'] - r['ymin']
        # first relief layer: share of 0.5 mm cells burnt from more than one gantry position
        fx0, fy0, fx1, fy1 = table_xy(old, first, pr)
        tl = old.seg_tile[first].astype(np.int64)
        cell = 0.5
        xm = min(fx0.min(), fx1.min())
        ym = min(fy0.min(), fy1.min())
        W = int((max(fx0.max(), fx1.max()) - xm) / cell) + 2
        nt = len(old.tile_xy) + 1
        keys = []
        for xs_, ys_, start, end, offs, rep_ in iter_samples(fx0, fy0, fx1, fy1, cell * 0.5):
            ck = np.floor((ys_ - ym) / cell).astype(np.int64) * W + np.floor((xs_ - xm) / cell).astype(np.int64)
            keys.append(np.unique(ck * nt + tl[start:end][rep_]))
        k = np.unique(np.concatenate(keys))
        _, per_cell = np.unique(k // nt, return_counts=True)
        r['overlap_frac'] = float(np.count_nonzero(per_cell > 1) / len(per_cell))
        res[pr] = r
    pb = old.process_bbox
    exp = (pb[2] - pb[0], pb[3] - pb[1]) if pb and len(pb) == 4 else None
    if exp:
        score = {pr: abs(res[pr]['w'] - exp[0]) + abs(res[pr]['h'] - exp[1]) for pr in res}
    else:
        score = {pr: res[pr]['w'] * res[pr]['h'] for pr in res}
    best = min(score, key=score.get)
    extra = ''
    if pb and old.offset and old.galvo_center:
        gc = old.galvo_center
        off = old.offset
        r = res[best]
        ca, cb = (gc[0], gc[1]) if best == 'AX' else (gc[1], gc[0])
        extra = (f'; chosen bbox - galvo centre {gc} + blockConfig offset ({off[0]:.3f}, {off[1]:.3f}) = '
                 f'X {r["xmin"] - ca + off[0]:.2f}..{r["xmax"] - ca + off[0]:.2f}, '
                 f'Y {r["ymin"] - cb + off[1]:.2f}..{r["ymax"] - cb + off[1]:.2f} vs header process_bbox {pb}')
    rep.note('axis_pairing',
             f'chosen {best} (galvo A pairs with gantry {"X" if best == "AX" else "Y"}). Old relief laser-on bbox: '
             f'AX {res["AX"]["w"]:.2f} x {res["AX"]["h"]:.2f} mm, tiles of the first layer overlap in '
             f'{100 * res["AX"]["overlap_frac"]:.1f}% of 0.5 mm cells; AY {res["AY"]["w"]:.2f} x {res["AY"]["h"]:.2f} mm, '
             f'overlap {100 * res["AY"]["overlap_frac"]:.1f}%; expected image size '
             f'{"%.1f x %.1f mm (header process_bbox)" % exp if exp else "unknown"}' + extra,
             data=dict(candidates=res, expected=exp, chosen=best))
    return best


def galvo_envelope(p, kinds=None):
    r = [INF, -INF, INF, -INF]
    for (lay, t), e in p.bb_map.items():
        if kinds is not None and kind_of(p, lay) not in kinds:
            continue
        r = [min(r[0], e[0]), max(r[1], e[1]), min(r[2], e[2]), max(r[3], e[3])]
    return r


# --------------------------------------------------------------------------------------
# main comparison
# --------------------------------------------------------------------------------------
def run(old_path, new_path, K, args):
    rep = Report()

    def log(s):
        print(s, file=sys.stderr, flush=True)
    old = parse_gcode(old_path, 'old', log)
    env_old = galvo_envelope(old)
    new = parse_gcode(new_path, 'new', log, ref_galvo=tuple(env_old))
    finish_structure(old)
    finish_structure(new)
    relief_key, clean_key = learn_kinds(old)
    classify(old, relief_key, clean_key)
    classify(new, relief_key, clean_key)

    # ------------------------------------------------------------ completeness
    def completeness(p):
        probs = []
        if p.last_unterminated:
            probs.append('last line has no line ending (truncated)')
        if p.tail_line is None:
            probs.append(f'tail marker "{TAIL_MARK}" missing')
        elif p.last_nonblank_no != p.tail_line:
            probs.append(f'last non-blank line {p.last_nonblank_no} is {p.last_nonblank_line[:60]!r}, not the tail marker')
        if p.n_end_markers != 1:
            probs.append(f'{p.n_end_markers} "{END_MARK}" markers (expected 1)')
        if p.nonascii_lines or p.nul_lines:
            probs.append(f'{p.nonascii_lines} non-ASCII lines, {p.nul_lines} lines with NUL')
        if p.unclosed_blocks or p.stray_block_ends:
            probs.append(f'{p.unclosed_blocks} unclosed blocks, {p.stray_block_ends} stray block ends')
        return probs
    po = completeness(old)
    pn = completeness(new)
    if (new.n_crlf == 0) != (old.n_crlf == 0):
        pn.append(f'line endings differ (CRLF lines: old {old.n_crlf}, new {new.n_crlf})')
    rep.check('file_complete', not pn,
              ('new: ' + '; '.join(pn)) if pn else
              f'new: {new.n_lines:,} lines, last line terminated, ends with "{TAIL_MARK}", one "{END_MARK}", '
              f'all {len(new.blocks)} blocks closed, ASCII only' + (f' [OLD itself: {"; ".join(po)}]' if po else ''))

    # ------------------------------------------------------------ header
    def no_date(lines):
        return [l for l in lines if not l.startswith('# date=')]
    ho = no_date(old.header_lines)
    hn = no_date(new.header_lines)
    do = [l for l in old.header_lines if l.startswith('# date=')]
    dn = [l for l in new.header_lines if l.startswith('# date=')]
    if ho and ho == hn:
        rep.check('header_identical', True,
                  f'all {len(new.header_lines)} lines before the first block identical'
                  + (f' except the date comment ({do} vs {dn})' if do != dn else ' (date comment included)'))
    else:
        i = next((i for i in range(max(len(ho), len(hn))) if i >= len(ho) or i >= len(hn) or ho[i] != hn[i]), 0)
        rep.check('header_identical', False,
                  f'header differs ({len(ho)} vs {len(hn)} lines, date ignored); first difference at header line {i + 1}: '
                  f'old {ho[i] if i < len(ho) else None!r}, new {hn[i] if i < len(hn) else None!r}')

    # ------------------------------------------------------------ camera block
    def susp_desc(p):
        out = []
        for s in p.suspensions:
            g = s['gantry']
            mv = [m for m in s['moves'] if m[1] is not None]
            out.append(dict(lines=s['lines'], cam=[(x, y) for (x, y, _, _) in g[:-1]],
                            ret_gantry=(g[-1][0], g[-1][1]) if g else None, gantry_before=s['gantry_before'],
                            ret_galvo=(mv[-1][1], mv[-1][2]) if mv else None, galvo_before=s['galvo_before'],
                            closed=s['end'] is not None, start=s['start']))
        return out
    so = susp_desc(old)
    sn = susp_desc(new)
    cp = []
    if len(so) != len(sn):
        cp.append(f'{len(sn)} camera suspensions (G106..G107) in new, {len(so)} in old')
    for i, (a, b) in enumerate(zip(so, sn)):
        if a['lines'] != b['lines']:
            cp.append(f'suspension {i + 1}: command/comment lines {b["lines"]} vs old {a["lines"]}')
        if a['cam'] != b['cam']:
            cp.append(f'suspension {i + 1}: camera positions {b["cam"]} vs old {a["cam"]}')
        if not b['closed']:
            cp.append(f'suspension {i + 1} (line {b["start"]}) not closed by G107')
        if b['ret_gantry'] != b['gantry_before']:
            cp.append(f'suspension {i + 1}: gantry returns to {b["ret_gantry"]}, left from {b["gantry_before"]}')
        gb = b['galvo_before']
        rg = b['ret_galvo']
        if rg is not None and gb[0] is not None and (abs(rg[0] - gb[0]) > 1e-9 or abs(rg[1] - gb[1]) > 1e-9):
            cp.append(f'suspension {i + 1}: galvo returns to {rg}, left from {gb}')
    tpo = sorted((k, v[0]) for k, v in old.templates.items() if 'take photo' in k)
    tpn = sorted((k, v[0]) for k, v in new.templates.items() if 'take photo' in k)
    if tpo != tpn:
        cp.append(f'"take photo" comment lines differ: {tpn} vs old {tpo}')
    m4703_o = sorted((k, v[0]) for k, v in old.templates.items() if k.startswith('M4703'))
    m4703_n = sorted((k, v[0]) for k, v in new.templates.items() if k.startswith('M4703'))
    if m4703_o != m4703_n:
        cp.append(f'M4703 lines differ: {m4703_n} vs old {m4703_o}')
    rep.check('camera_block', not cp,
              '; '.join(cp) if cp else
              f'{len(sn)} camera suspensions (lines {[s["start"] for s in sn]}), same commands '
              f'({", ".join(l for s in sn for l in s["lines"] if l.startswith("M4703"))}) and camera positions '
              f'{[s["cam"] for s in sn]} as old; each returns to the gantry and galvo position it left from')

    # ------------------------------------------------------------ end sequence
    if old.end_lines and old.end_lines == new.end_lines and old.last_unterminated == new.last_unterminated:
        rep.check('end_sequence_identical', True,
                  f'{len(new.end_lines)} lines from "{END_MARK}" to EOF identical: ' + ' | '.join(l for l in new.end_lines if l))
    else:
        rep.check('end_sequence_identical', False,
                  f'new {[l for l in new.end_lines if l]} vs old {[l for l in old.end_lines if l]} '
                  f'(last line unterminated: new {new.last_unterminated}, old {old.last_unterminated})')

    # ------------------------------------------------------------ vocabulary
    def vocab(p):
        cmds = {}
        letters = {}
        values = {}
        for t, (cnt, first) in p.templates.items():
            if t.startswith('#') or t.startswith('WHITESPACE:'):
                continue
            words = tokenize(t)
            cmd = command_of(words)
            cmds[cmd] = min(cmds.get(cmd, first), first)
            coords = COORD_LETTERS.get(cmd, '')
            for w in words[1:]:
                if w[0] == '?':
                    k = (cmd, 'junk ' + w[1])
                    letters[k] = min(letters.get(k, first), first)
                elif w[0] == '"':
                    k = (cmd, 'quoted', w[1])
                    values[k] = min(values.get(k, first), first)
                else:
                    k = (cmd, w[0])
                    letters[k] = min(letters.get(k, first), first)
                    if w[0] not in coords and w[1] != '#':
                        try:
                            v = float(w[1])
                        except ValueError:
                            v = w[1]
                        k = (cmd, w[0], v)
                        values[k] = min(values.get(k, first), first)
        return cmds, letters, values
    co, lo, vo = vocab(old)
    cn, ln, vn = vocab(new)
    nc = {k: v for k, v in cn.items() if k not in co}
    nl = {k: v for k, v in ln.items() if k not in lo}
    nv = {k: v for k, v in vn.items() if k not in vo}
    nt = {k: v for k, v in new.templates.items() if k not in old.templates}
    rep.check('vocabulary_commands', not nc,
              f'command words never used in old: {[(k, "first line %d" % v) for k, v in nc.items()]}' if nc else
              f'all {len(cn)} command words of new occur in old: {" ".join(sorted(cn))}')
    rep.check('vocabulary_parameter_letters', not nl,
              f'(command, letter) pairs never used in old: {[(k, "first line %d" % v) for k, v in nl.items()]}' if nl else
              f'all {len(ln)} (command, parameter letter) pairs of new occur in old')
    rep.check('vocabulary_parameter_values', not nv,
              f'non-coordinate values never used in old: {[(k, "first line %d" % v) for k, v in list(nv.items())[:20]]}'
              if nv else f'all {len(vn)} non-coordinate (command, letter, value) triples of new occur in old')
    rep.check('vocabulary_line_formats', not nt,
              f'{len(nt)} line formats never seen in old (coordinates masked): '
              f'{[(k[:80], "x%d, first line %d" % (v[0], v[1])) for k, v in list(nt.items())[:15]]}' if nt else
              f'all {len(new.templates)} line formats of new (coordinates masked, comments verbatim, spacing kept) occur in old')

    # ------------------------------------------------------------ block / layer structure
    def collapse(sig):
        out = []
        for s in sig:
            s = key_str(s)
            if not out or out[-1] != s:
                out.append(s)
        return tuple(out)
    sig_old = Counter(collapse(b['sig']) for b in old.blocks)
    sig_new = Counter(collapse(b['sig']) for b in new.blocks)
    unk = [s for s in sig_new if s not in sig_old]
    sp = []
    if unk:
        first_bad = min(b['start'] for b in new.blocks if collapse(b['sig']) in set(unk))
        sp.append(f'{sum(sig_new[s] for s in unk)} blocks with a layout never seen in old (first at line {first_bad}): '
                  f'{list(unk[0])[:24]}')
    if new.unknown_layers:
        sp.append(f'layers of unknown type starting at lines {[new.layers[i]["line"] for i in new.unknown_layers][:10]}, '
                  f'keys {[layer_key(new.layers[i]) for i in new.unknown_layers][:3]}')
    ks = [k for k in new.kinds if k != 'unknown']
    if 'cleaning' in ks and 'relief' in ks[ks.index('cleaning'):]:
        sp.append('a relief layer comes after a cleaning pass')
    bpc_o = [old.layers[i]['n_blocks'] for i in old.clean_layers]
    bpc_n = [new.layers[i]['n_blocks'] for i in new.clean_layers]
    if set(bpc_n) - set(bpc_o):
        sp.append(f'blocks per cleaning pass new {bpc_n} vs old {bpc_o}')
    if new.moves_outside_blocks > old.moves_outside_blocks:
        sp.append(f'{new.moves_outside_blocks} galvo moves outside blocks (old {old.moves_outside_blocks})')
    if len(new.burns_outside_blocks) > len(old.burns_outside_blocks):
        sp.append(f'laser-on moves outside blocks at lines {new.burns_outside_blocks[:10]}')
    if len(new.v_outside_blocks) > len(old.v_outside_blocks):
        sp.append(f'V moves outside blocks at lines {new.v_outside_blocks[:10]}')
    if len(new.v_after_moves) > len(old.v_after_moves):
        sp.append(f'V moves after galvo moves inside a block at lines {new.v_after_moves[:10]}')
    if new.unknown_start_burns:
        sp.append(f'laser-on moves from an unknown galvo position at lines {new.unknown_start_burns[:10]}')
    rep.check('block_structure', not sp,
              '; '.join(sp) if sp else
              f'{len(new.blocks)} blocks use {len(sig_new)} block layouts, all seen in old ({len(sig_old)} layouts); '
              f'{len(new.relief_layers)} relief layers then {len(new.clean_layers)} cleaning passes; '
              f'{sorted(set(bpc_n))} block(s) per cleaning pass as in old; no galvo moves or V moves outside blocks')

    # ------------------------------------------------------------ power and feed
    def s_values(p):
        out = {}
        for t, (cnt, first) in p.templates.items():
            if t.startswith('G0') or t.startswith('G1'):
                words = tokenize(t)
                if command_of(words) in ('G0', 'G1'):
                    for w in words[1:]:
                        if w[0] == 'S' and w[1] != '#':
                            v = float(w[1])
                            out[v] = min(out.get(v, first), first)
        return out
    svo = s_values(old)
    svn = s_values(new)
    mo_ = max(svo) if svo else 0.0
    mn_ = max(svn) if svn else 0.0
    rep.check('max_power_not_higher', mn_ <= mo_,
              f'max laser power S on G0/G1 lines: new {mn_:g} (first at line {svn.get(mn_)}), old {mo_:g}')

    def part_counter(p, arr, kind):
        sel = p.seg_kind == kind
        v, c = np.unique(arr[sel], return_counts=True)
        return {float(a): int(b) for a, b in zip(v, c)}
    pr_o, pr_n = part_counter(old, old.seg_power, 0), part_counter(new, new.seg_power, 0)
    pc_o, pc_n = part_counter(old, old.seg_power, 1), part_counter(new, new.seg_power, 1)
    px_n = {**part_counter(new, new.seg_power, 2), **part_counter(new, new.seg_power, 3)}
    rep.check('relief_power', set(pr_n) <= set(pr_o) and not px_n,
              f'laser-on S in relief layers: new {pr_n}, old {pr_o}'
              + (f'; laser-on in unknown layers/outside layers: {px_n}' if px_n else ''))
    if old.clean_layers or new.clean_layers:
        rep.check('cleaning_power', set(pc_n) <= set(pc_o), f'laser-on S in cleaning passes: new {pc_n}, old {pc_o}')
    else:
        rep.check('cleaning_power', None, 'no cleaning passes in either file')
    fr_o, fr_n = part_counter(old, old.seg_feed, 0), part_counter(new, new.seg_feed, 0)
    fc_o, fc_n = part_counter(old, old.seg_feed, 1), part_counter(new, new.seg_feed, 1)
    rep.check('laser_on_feeds', set(fr_n) <= set(fr_o) and set(fc_n) <= set(fc_o),
              f'feed F (mm/min) in effect at laser-on moves: relief new {sorted(fr_n)} old {sorted(fr_o)}; '
              f'cleaning new {sorted(fc_n)} old {sorted(fc_o)}')

    # ------------------------------------------------------------ laser state at travel moves
    def travel_groups(p):
        g = Counter()
        ex = {}
        for (n, line, lay, kind, susp, after_g) in p.hot_travel:
            key = (kind_of(p, lay), kind + (' right after a gantry move' if after_g else ''),
                   'inside camera suspension' if susp else 'outside camera suspension')
            g[key] += 1
            ex.setdefault(key, (n, line))
        return g, ex
    tg_o, _ = travel_groups(old)
    tg_n, tex_n = travel_groups(new)
    lp = []
    for key, c in tg_n.items():
        if tg_o.get(key, 0) == 0:
            lp.append(f'{c} x {key[1]} with modal S>0 in {key[0]} ({key[2]}), never in old; e.g. line {tex_n[key][0]} '
                      f'{tex_n[key][1]!r}')
        elif key[2] == 'inside camera suspension' and c > tg_o[key]:
            lp.append(f'{c} x {key[1]} with modal S>0 inside camera suspensions in {key[0]} (old {tg_o[key]})')
    if len(new.burns_while_suspended) > len(old.burns_while_suspended):
        lp.append(f'laser-on G1 inside a camera suspension at lines {new.burns_while_suspended[:5]}')
    n_g0 = sum(v[0] for k, v in new.templates.items() if k.startswith('G0') and ('A#' in k or 'B#' in k))
    desc = '; '.join(f'{k[0]}: {k[1]} {k[2]} new {tg_n.get(k, 0)} / old {tg_o.get(k, 0)}'
                     for k in sorted(set(tg_n) | set(tg_o)))
    rep.check('laser_off_at_G0_and_travel', not lp,
              ('; '.join(lp) + f' [all groups: {desc}]') if lp else
              f'of {n_g0:,} G0 jumps and all gantry/V moves, those executed while the modal S is > 0 occur only in '
              f'situations old has too: {desc or "none"}')

    le = []
    if new.hot_block_starts:
        le.append(f'laser on at block starts, lines {new.hot_block_starts[:5]}')
    if new.s_at_end_marker is None or new.s_at_end_marker > 0:
        le.append(f'modal S at "{END_MARK}" = {new.s_at_end_marker}')
    if new.s_at_eof > 0:
        le.append(f'modal S at end of file = {new.s_at_eof:g}')
    if new.m6_hot:
        le.append(f'M6 with laser on at {new.m6_hot}')
    if 'G1 S0' not in new.end_lines:
        le.append('no "G1 S0" in the end sequence')
    rep.check('laser_off_at_blocks_and_end', not le,
              '; '.join(le) if le else
              f'S = 0 at all {len(new.blocks)} block starts, at "{END_MARK}" and at end of file; "G1 S0" before M6')

    # ------------------------------------------------------------ V axis
    rv_o = [old.layers[i]['V'] for i in old.relief_layers]
    rv_n = [new.layers[i]['V'] for i in new.relief_layers]
    so_ = np.diff([v for v in rv_o if v is not None]) if len(rv_o) > 1 else np.zeros(0)
    step = float(np.median(so_)) if len(so_) else None
    uniform = bool(len(so_) == 0 or np.all(np.abs(so_ - step) < 1e-6))
    vo_ = [v for (_, v, _, w) in old.vmoves if w != 'end' and v is not None]
    vn_ = [v for (_, v, _, w) in new.vmoves if w != 'end' and v is not None]
    vmin, vmax = (min(vo_), max(vo_)) if vo_ else (None, None)
    outr = sorted({v for v in vn_ if vmin is None or v < vmin - 1e-9 or v > vmax + 1e-9})
    never = sorted({round(v, 6) for v in vn_} - {round(v, 6) for v in vo_})
    rep.check('V_within_old_range', bool(vn_) and not outr,
              f'new V {min(vn_) if vn_ else None}..{max(vn_) if vn_ else None} ({len(set(vn_))} distinct values), '
              f'old range {vmin}..{vmax}' + (f'; outside: {outr[:10]}' if outr else '')
              + (f'; values old never used: {never[:10]}' if never else '; every new V value also occurs in old'))
    ok1 = bool(rv_o and rv_n and rv_o[0] is not None and rv_n[0] is not None and abs(rv_n[0] - rv_o[0]) < 1e-9)
    rep.check('V_first_relief_layer', ok1,
              f'first relief layer V: new {rv_n[0] if rv_n else None}, old {rv_o[0] if rv_o else None} (original surface)')
    if rv_n and step is not None and rv_n[0] is not None:
        bad = [j for j, v in enumerate(rv_n) if v is None or abs(v - (rv_n[0] + j * step)) > 1e-6]
        dev = max((abs(v - (rv_n[0] + j * step)) for j, v in enumerate(rv_n) if v is not None), default=0.0)
        rep.check('V_step_per_layer', not bad and uniform,
                  f'old step {step:+.4f} per relief layer ({"uniform" if uniform else "NOT uniform"} in old); '
                  f'new relief V = {rv_n[0]} + j*({step:+.4f}) for {len(rv_n) - len(bad)}/{len(rv_n)} layers '
                  f'(max deviation {dev:.1e})' + (f'; deviating layers {bad[:10]}' if bad else ''))
    else:
        rep.check('V_step_per_layer', False, 'cannot determine relief V values')
    n_o = len(old.relief_layers)
    n_n = len(new.relief_layers)
    rep.check('relief_layer_count', n_n == n_o - K, f'new {n_n} relief layers; old {n_o}; old - K = {n_o - K}')

    if old.clean_layers or new.clean_layers:
        def roles(p, rv):
            surf = rv[0] if rv else None
            bot = rv[-1] + step if rv and step is not None and rv[-1] is not None else None
            out = []
            for i in p.clean_layers:
                v = p.layers[i]['V']
                if surf is not None and v is not None and abs(v - surf) < 1e-6:
                    out.append('surface')
                elif bot is not None and v is not None and abs(v - bot) < 1e-6:
                    out.append('bottom')
                else:
                    out.append(f'V={v}')
            return out
        ro = roles(old, rv_o)
        rn = roles(new, rv_n)
        keep = getattr(args, 'feather', None)
        if keep is None:
            rep.check('cleaning_pass_count', len(new.clean_layers) == len(old.clean_layers),
                      f'new {len(new.clean_layers)} cleaning passes, old {len(old.clean_layers)}')
        else:
            # feather_passes N: NEW keeps the first N of OLD's passes (same order, same surface / bottom roles)
            rep.check('cleaning_pass_count', len(new.clean_layers) == keep <= len(old.clean_layers),
                      f'new {len(new.clean_layers)} cleaning passes, old {len(old.clean_layers)}, expected the first {keep}')
            ro = ro[:keep]
        rep.check('cleaning_V_roles', ro == rn,
                  f'cleaning V (surface = first relief V, bottom = last relief V + step): new '
                  f'{[new.layers[i]["V"] for i in new.clean_layers]} = {rn}; old '
                  f'{[old.layers[i]["V"] for i in old.clean_layers]} = {ro}')
    else:
        rep.check('cleaning_pass_count', None, 'no cleaning passes in either file')

    # ------------------------------------------------------------ per-layer laser-on path
    lo_len = old.layer_len
    ln_len = new.layer_len
    pairs = [(j, new.relief_layers[j], old.relief_layers[j + K]) for j in range(n_n) if j + K < n_o]
    if pairs:
        ratios = np.array([ln_len[a] / lo_len[b] if lo_len[b] > 0 else np.inf for (_, a, b) in pairs])
        w = int(np.argmax(np.abs(ratios - 1)))
        ok = bool(np.all(np.abs(ratios - 1) <= args.path_tol)) and len(pairs) == n_n
        rep.check('per_layer_laser_on_path', ok,
                  f'new layer j vs old layer j+{K}: ratio {ratios.min():.4f}..{ratios.max():.4f} over {len(pairs)} layers '
                  f'(limit +/-{100 * args.path_tol:g}%); worst j={pairs[w][0]}: new {ln_len[pairs[w][1]]:.1f} mm vs old '
                  f'{lo_len[pairs[w][2]]:.1f} mm' + ('' if len(pairs) == n_n else f'; {n_n - len(pairs)} new layers have no partner'),
                  data=dict(ratios=[float(r) for r in ratios]))
    else:
        rep.check('per_layer_laser_on_path', False, 'no layer pairs to compare')
    scores = {}
    for kk in range(0, n_o):
        pr = [(new.relief_layers[j], old.relief_layers[j + kk]) for j in range(n_n) if j + kk < n_o]
        if len(pr) < max(3, min(n_n, 5)):
            continue
        rr = np.array([ln_len[a] / lo_len[b] for a, b in pr if lo_len[b] > 0 and ln_len[a] > 0])
        if len(rr):
            scores[kk] = float(np.median(np.abs(np.log(rr))))
    if scores:
        bk = min(scores, key=scores.get)
        rep.check('K_consistency', bk == K,
                  f'shift that best matches the per-layer laser-on path: K={bk} (declared {K}); median |log ratio| '
                  + ', '.join(f'K={k}: {100 * scores[k]:.2f}%' for k in sorted(scores) if abs(k - K) <= 2))
    else:
        rep.check('K_consistency', None, 'not enough layers to estimate K')

    # ------------------------------------------------------------ geometry on the table
    pairing = choose_pairing(old, rep)
    pitch = 1.0 / old.density if old.density else 0.1
    h = pitch / 2.0
    tol = args.tol
    cell = args.cell
    o_rel = table_xy(old, old.seg_kind == 0, pairing)
    n_rel = table_xy(new, new.seg_kind == 0, pairing)
    t1 = time.time()
    margin = max(1.0, 5 * (h + tol))
    area = Raster(*seg_box(o_rel, margin=margin), cell, margin)
    area.mark(o_rel, cell)
    area.build_distance()
    log(f'[geom] old relief area raster {area.w}x{area.h} cells of {cell} mm in {time.time() - t1:.1f} s')
    acc = cell / 2 + cell * math.sqrt(2)
    smax, nsmp = area.segment_max_dist(n_rel, cell)
    exc = smax - h
    nlines = new.seg_line[new.seg_kind == 0]
    definite = exc > tol + acc
    unsure = np.flatnonzero((exc > tol - acc) & ~definite)
    refined = {}
    if len(unsure):
        # borderline moves: exact point-to-segment distances (up to 300 moves, 2000 points)
        budget = 2000
        for i in unsure[np.argsort(-exc[unsure])][:300]:
            seg = tuple(np.array([s[i]]) for s in n_rel)
            xs, ys = [np.concatenate(v) for v in zip(*[(a, b) for a, b, *_ in iter_samples(*seg, cell / 4)])]
            d = area.point_dist(xs, ys)
            m = np.flatnonzero(d - h > tol - acc)
            if len(m) == 0:
                refined[int(i)] = float(d.max()) - h
                continue
            m = m[np.argsort(-d[m])[:max(1, min(budget, 200))]]
            budget -= len(m)
            refined[int(i)] = float(exact_dist_to_segments(xs[m], ys[m], o_rel, d[m] + 0.3).max()) - h
            if budget <= 0:
                break
    exc_best = exc.copy()
    for i, v in refined.items():
        exc_best[i] = v
    bad = np.flatnonzero(definite | (exc_best > tol))
    wi = int(np.argmax(exc_best)) if len(exc_best) else None
    rep.check('relief_burn_inside_old_area', len(exc) > 0 and len(bad) == 0,
              f'{len(exc):,} new relief laser-on moves ({nsmp:,} points every {cell} mm) vs the union of all old relief '
              f'laser-on moves widened by half the line pitch ({h:.4f} mm): largest excursion '
              f'{fnum(float(exc_best.max()) if len(exc) else None)} mm (limit {tol} mm; raster accuracy +/-{acc:.3f} mm, '
              f'{len(refined)} borderline moves recomputed exactly), {len(bad)} moves beyond the limit'
              + (f', e.g. lines {nlines[bad[:8]].tolist()}; worst line {int(nlines[wi])} at {float(exc_best[wi]):.3f} mm'
                 if len(bad) else ''),
              data=dict(max_excursion_mm=float(exc_best.max()) if len(exc) else None, n_bad=int(len(bad)),
                        bad_lines=nlines[bad[:200]].tolist()))
    if old.clean_layers and new.clean_layers:
        sel_oc = old.seg_kind == 1
        sel_nc = new.seg_kind == 1
        smo, _ = area.segment_max_dist(table_xy(old, sel_oc, pairing), cell)
        smn, _ = area.segment_max_dist(table_xy(new, sel_nc, pairing), cell)
        exo = float((smo - h).max())
        exn = float((smn - h).max())
        lim = max(tol, exo)
        badc = np.flatnonzero(smn - h > lim + 1e-9)
        cl = new.seg_line[sel_nc]
        rep.check('cleaning_burn_inside_old_area', len(badc) == 0,
                  f'{len(smn):,} new cleaning laser-on dots: largest excursion beyond the old relief area {exn:.3f} mm '
                  f'(negative = inside); old cleaning dots reach {exo:.3f} mm; limit max({tol}, old) = {lim:.3f} mm; '
                  f'{len(badc)} dots beyond' + (f', e.g. lines {cl[badc[:8]].tolist()}' if len(badc) else ''))
        lo_c = old.seg_len[sel_oc]
        ln_c = new.seg_len[sel_nc]
        rep.check('cleaning_dot_mode', len(ln_c) > 0 and float(ln_c.max()) <= float(lo_c.max()) + 0.002,
                  f'cleaning laser-on move length min/median/max: new {ln_c.min():.3f}/{np.median(ln_c):.3f}/'
                  f'{ln_c.max():.3f} mm, old {lo_c.min():.3f}/{np.median(lo_c):.3f}/{lo_c.max():.3f} mm')
    del area

    # ------------------------------------------------------------ per-layer area checks
    t2 = time.time()
    lc = args.layer_cell
    err = lc * math.sqrt(2) + lc / 4          # raster error bound (snapping + marking step)
    thr_a = h + tol + err
    sub = []
    for j in range(min(n_n, n_o)):
        sa = layer_sel(new, new.relief_layers[j])
        A_ = table_xy(new, sa, pairing)
        B_ = table_xy(old, layer_sel(old, old.relief_layers[j]), pairing)
        if len(A_[0]) == 0:
            sub.append((j, 0.0, 0.0, None, 0.0))
            continue
        if len(B_[0]) == 0:
            sub.append((j, 1.0, INF, None, INF))
            continue
        r = Raster(*seg_box(A_, B_, margin=1.0), lc, 1.0)
        r.mark(B_, lc / 2)
        r.build_distance()
        f, d, pts = r.fraction_beyond(A_, lc / 2, thr_a, keep_above=h + tol - err, keep=200)
        del r
        if len(pts):
            ex = exact_dist_to_segments(pts[:, 0], pts[:, 1], B_, pts[:, 2] + 0.3)
            k = int(np.argmax(ex))
            sub.append((j, f, d, int(new.seg_line[sa[int(pts[k, 3])]]), float(ex[k])))
        else:
            sub.append((j, f, d, None, None))     # every point within h + tol
    lim_far = h + tol + args.same_v_slack
    worst = max(sub, key=lambda t: t[1]) if sub else None
    exs = [t for t in sub if t[4] is not None]
    far = max(exs, key=lambda t: t[4]) if exs else None
    ok_sv = bool(sub) and all(t[1] <= 1e-4 for t in sub) and (far is None or far[4] <= lim_far)
    rep.check('per_layer_inside_old_same_V', ok_sv,
              (f'new relief layer j vs old relief layer j (same V, same scan angle) over {len(sub)} layers: share of new '
               f'path farther than {thr_a:.3f} mm (half pitch + {tol} + raster error) from old layer j: max '
               f'{100 * worst[1]:.4f}% (j={worst[0]}, limit 0.01%); farthest point (exact) '
               + (f'{far[4]:.3f} mm in j={far[0]} at line {far[3]}' if far else f'<= {h + tol:.3f} mm')
               + f' (limit {lim_far:.3f} mm = half pitch + {tol} + {args.same_v_slack} line-end allowance)')
              if sub else 'no layers',
              data=[dict(j=t[0], outside_frac=t[1], raster_far_mm=t[2], exact_far_line=t[3], exact_far_mm=t[4])
                    for t in sub])

    lc2 = args.layer_cell2
    e = args.end_ext
    thr_b = h + tol + lc2 * math.sqrt(2) + lc2 / 4
    per = []
    for (j, a, b) in pairs:
        A_ = table_xy(new, layer_sel(new, a), pairing)
        B_ = table_xy(old, layer_sel(old, b), pairing)
        if len(A_[0]) == 0 or len(B_[0]) == 0:
            per.append((j, 1.0, INF, 1.0, INF, 0.0, 0.0))
            continue
        box = seg_box(A_, B_, margin=1.0)
        At, kA = trim_ends(A_, e)
        Bt, kB = trim_ends(B_, e)
        rB = Raster(*box, lc2, 1.0)
        rB.mark(B_, lc2 / 2)
        rB.build_distance()
        fn, dn, _ = rB.fraction_beyond(At, lc2 / 2, thr_b) if len(At[0]) else (0.0, 0.0, None)
        del rB
        rA = Raster(*box, lc2, 1.0)
        rA.mark(A_, lc2 / 2)
        rA.build_distance()
        fo, do_, _ = rA.fraction_beyond(Bt, lc2 / 2, thr_b) if len(Bt[0]) else (0.0, 0.0, None)
        del rA
        per.append((j, fn, dn, fo, do_, kA, kB))
    log(f'[geom] per-layer area checks of {len(sub)} + {len(pairs)} layer pairs in {time.time() - t2:.1f} s')
    if per:
        jn = max(per, key=lambda t: t[1])
        jo = max(per, key=lambda t: t[3])
        ok = jn[1] <= args.layer_area_frac and jo[3] <= args.layer_area_frac
        rep.check('per_layer_area_match', ok,
                  f'new layer j vs old layer j+{K} (their scan angles generally differ, so each move is trimmed by {e} mm '
                  f'at both ends to remove the ~1 mm along-scan end extension; {100 * min(t[5] for t in per):.0f}-'
                  f'{100 * max(t[5] for t in per):.0f}% of the path stays under test): share of new path farther than '
                  f'{thr_b:.3f} mm from old layer j+{K}: max {100 * jn[1]:.3f}% (j={jn[0]}); share of old layer j+{K} path '
                  f'outside new layer j: max {100 * jo[3]:.3f}% (j={jo[0]}); farthest {max(max(t[2], t[4]) for t in per):.2f} mm; '
                  f'limit {100 * args.layer_area_frac:g}% per layer and direction',
                  data=[dict(j=t[0], new_outside_frac=t[1], new_far_mm=t[2], old_outside_frac=t[3], old_far_mm=t[4],
                             tested_new=t[5], tested_old=t[6]) for t in per])
    else:
        rep.check('per_layer_area_match', False, 'no layer pairs')

    # ------------------------------------------------------------ galvo envelope
    env_new_r = galvo_envelope(new, ('relief',))
    env_new_c = galvo_envelope(new, ('cleaning',))
    env_old_r = galvo_envelope(old, ('relief',))
    env_old_c = galvo_envelope(old, ('cleaning',))
    oor_by_kind = Counter()
    for lay, c in new.out_of_ref_layers.items():
        oor_by_kind[kind_of(new, lay)] += c

    def env_txt(r):
        return f'A {fnum(r[0])}..{fnum(r[1])}, B {fnum(r[2])}..{fnum(r[3])}'
    for part, envn, envo in (('relief', env_new_r, env_old_r), ('cleaning', env_new_c, env_old_c)):
        if part == 'cleaning' and not (old.clean_layers or new.clean_layers):
            continue
        cnt = oor_by_kind.get(part, 0)
        ex = [n for (n, lay) in new.out_of_ref if kind_of(new, lay) == part][:12]
        lay_counts = {new.layers[l]['line']: c for l, c in sorted(new.out_of_ref_layers.items())
                      if kind_of(new, l) == part}
        rep.check(f'galvo_within_old_envelope_{part}', cnt == 0,
                  f'new {part} galvo endpoints {env_txt(envn)}; old envelope (all moves of old) {env_txt(env_old)} '
                  f'(old {part} alone {env_txt(envo)}); {cnt} new {part} galvo moves end outside the old envelope'
                  + (f', e.g. lines {ex}; per pass/layer (by start line): {lay_counts}' if cnt else ''))

    # ================================================================ INFO
    for p in (old, new):
        for part in ('relief', 'cleaning'):
            g = galvo_envelope(p, (part,))
            tl = sorted({t for (lay, t) in p.bb_map if kind_of(p, lay) == part and t >= 0})
            if tl:
                txy = p.tile_xy[tl]
                gt = (f'gantry X {txy[:, 0].min():.3f}..{txy[:, 0].max():.3f}, Y {txy[:, 1].min():.3f}..'
                      f'{txy[:, 1].max():.3f} ({len(tl)} positions)')
            else:
                gt = 'no gantry positions'
            nl_ = sum(c for l, c in p.long_off.items() if kind_of(p, l) == part)
            ml_ = max([v for l, v in p.long_off_max.items() if kind_of(p, l) == part], default=(0.0, 0))
            rep.note(f'ranges_{p.label}_{part}', f'{gt}; galvo endpoints A {fnum(g[0])}..{fnum(g[1])}, '
                                                 f'B {fnum(g[2])}..{fnum(g[3])}; laser-off G1 moves longer than '
                                                 f'{LONG_OFF:g} mm: {nl_} (longest {ml_[0]:.2f} mm at line {ml_[1]})')
    bx0 = float(min(o_rel[0].min(), o_rel[2].min())); bx1 = float(max(o_rel[0].max(), o_rel[2].max()))
    by0 = float(min(o_rel[1].min(), o_rel[3].min())); by1 = float(max(o_rel[1].max(), o_rel[3].max()))
    nbx0 = float(min(n_rel[0].min(), n_rel[2].min())); nbx1 = float(max(n_rel[0].max(), n_rel[2].max()))
    nby0 = float(min(n_rel[1].min(), n_rel[3].min())); nby1 = float(max(n_rel[1].max(), n_rel[3].max()))
    rep.note('relief_burn_bbox', f'old X {bx0:.3f}..{bx1:.3f}, Y {by0:.3f}..{by1:.3f} ({bx1 - bx0:.2f} x {by1 - by0:.2f} mm); '
                                 f'new X {nbx0:.3f}..{nbx1:.3f}, Y {nby0:.3f}..{nby1:.3f} ({nbx1 - nbx0:.2f} x '
                                 f'{nby1 - nby0:.2f} mm) (table = gantry + galvo, pairing {pairing})')
    for p in (old, new):
        for part in ('relief', 'cleaning'):
            ext = [0.0, 0.0, 0.0, 0.0]
            ntl = 0
            nbe = 0
            for (lay, t), en in p.bb_map.items():
                if kind_of(p, lay) != part or t < 0 or en[0] > en[1] or en[2] > en[3]:
                    continue
                gx, gy = p.tile_xy[t]
                if pairing == 'AX':
                    tx0, tx1, ty0, ty1 = gx + en[0], gx + en[1], gy + en[2], gy + en[3]
                else:
                    tx0, tx1, ty0, ty1 = gx + en[2], gx + en[3], gy + en[0], gy + en[1]
                d = (bx0 - tx0, tx1 - bx1, by0 - ty0, ty1 - by1)
                ntl += 1
                nbe += max(d) > tol
                ext = [max(ext[i], d[i]) for i in range(4)]
            rep.note(f'laser_off_excursion_{p.label}_{part}',
                     f'galvo moves (laser off; includes the galvo park at A0 B0 that starts every block) reach beyond '
                     f'the old relief burn bbox by up to -X {ext[0]:.2f}, +X {ext[1]:.2f}, -Y {ext[2]:.2f}, '
                     f'+Y {ext[3]:.2f} mm; {nbe} of {ntl} (layer, gantry position) groups go beyond it by more than {tol} mm')
    cam = sorted({c for s in sn for c in s['cam']})
    rep.note('gantry_travel_outside_burn_area', f'camera position(s) {cam} (laser suspended by G106/G107), end park '
                                                f'"G200 X0 Y0" - identical in old and new (header/camera/end checks)')
    for p in (old, new):
        rl = p.relief_layers
        cl = p.clean_layers
        rlen = float(p.layer_len[rl].sum()) if rl else 0.0
        clen = float(p.layer_len[cl].sum()) if cl else 0.0
        rt = float(p.layer_time[rl].sum()) if rl else 0.0
        ct = float(p.layer_time[cl].sum()) if cl else 0.0
        rep.note(f'laser_on_{p.label}',
                 f'relief {len(rl)} layers: {rlen / 1000:.2f} m laser-on path, {int(p.layer_non[rl].sum()) if rl else 0:,} '
                 f'moves, ~{rt / 60:.1f} min at the programmed feed; cleaning {len(cl)} passes: {clen / 1000:.2f} m, '
                 f'{int(p.layer_non[cl].sum()) if cl else 0:,} moves, ~{ct / 60:.1f} min; total laser-on ~{(rt + ct) / 60:.1f} min '
                 f'(path / feed only; acceleration, jumps and dwells not included)',
                 data=dict(relief_path_mm=rlen, cleaning_path_mm=clen, relief_s=rt, cleaning_s=ct))
    if old.clean_layers and new.clean_layers and old.relief_layers and new.relief_layers:
        cro = [float(old.layer_len[i]) for i in old.clean_layers]
        crn = [float(new.layer_len[i]) for i in new.clean_layers]
        rr = [x / y for x, y in zip(crn, cro) if y > 0]
        rep.note('cleaning_pass_path', f'cleaning pass laser-on path new/old {min(rr):.4f}..{max(rr):.4f}; first relief '
                                       f'layer path new/old {ln_len[new.relief_layers[0]] / lo_len[old.relief_layers[0]]:.4f}')
    rows = []
    for (j, a, b) in pairs:
        an = scan_angle_deg(*table_xy(new, layer_sel(new, a), pairing))
        aoj = scan_angle_deg(*table_xy(old, layer_sel(old, old.relief_layers[j]), pairing)) if j < n_o else None
        aok = scan_angle_deg(*table_xy(old, layer_sel(old, b), pairing))
        rows.append(dict(j=j, V_new=new.layers[a]['V'], V_old_jK=old.layers[b]['V'], path_new=float(ln_len[a]),
                         path_old_jK=float(lo_len[b]), ratio=float(ln_len[a] / lo_len[b]) if lo_len[b] else None,
                         blocks_new=new.layers[a]['n_blocks'],
                         blocks_old_j=old.layers[old.relief_layers[j]]['n_blocks'] if j < n_o else None,
                         blocks_old_jK=old.layers[b]['n_blocks'], angle_new=an, angle_old_j=aoj, angle_old_jK=aok))

    def same(x, y):
        return x is not None and y is not None and abs(((x - y + 90) % 180) - 90) < 0.5
    sa = sum(1 for r in rows if same(r['angle_new'], r['angle_old_j']))
    sk = sum(1 for r in rows if same(r['angle_new'], r['angle_old_jK']))
    sb = sum(1 for r in rows if r['blocks_new'] == r['blocks_old_j'])
    rep.note('per_layer_table', f'{len(rows)} rows (full table in JSON); new layer j has the scan angle of old layer j in '
                                f'{sa}/{len(rows)} layers and that of old layer j+K in {sk}/{len(rows)}; same block count as '
                                f'old layer j in {sb}/{len(rows)}', data=rows)
    print('      j  V_new   V_old(j+K)  path_new(mm) path_old(j+K)  ratio   scan angle new / old j / old j+K', flush=True)
    for r in rows:
        print(f'    {r["j"]:3d}  {r["V_new"]:.2f}  {r["V_old_jK"]:.2f}     {r["path_new"]:10.1f}  {r["path_old_jK"]:10.1f}  '
              f'{fnum(r["ratio"], 4)}  {fnum(r["angle_new"], 1)} / {fnum(r["angle_old_j"], 1)} / {fnum(r["angle_old_jK"], 1)}',
              flush=True)

    fails = [c['name'] for c in rep.checks if c['status'] == 'FAIL']
    nas = [c['name'] for c in rep.checks if c['status'] == 'N/A']
    npass = sum(1 for c in rep.checks if c['status'] == 'PASS')
    verdict = 'PASS' if not fails else 'FAIL'
    print(f'VERDICT: {verdict} - {npass} passed, {len(fails)} failed' + (f' ({", ".join(fails)})' if fails else '')
          + (f', {len(nas)} not applicable ({", ".join(nas)})' if nas else ''), flush=True)
    return dict(old=old_path, new=new_path, K=K, pairing=pairing, verdict=verdict, checks=rep.checks,
                info=rep.info, parameters=vars(args)), (0 if not fails else 1)


def _json_default(o):
    if isinstance(o, np.integer):
        return int(o)
    if isinstance(o, np.floating):
        return float(o)
    if isinstance(o, np.ndarray):
        return o.tolist()
    if isinstance(o, (set, frozenset)):
        return sorted(o)
    return str(o)


def main(argv=None):
    ap = argparse.ArgumentParser(description='Independent machine-safety check: does NEW galvo G-code ask the machine '
                                             'for anything OLD did not, apart from skipping the first K relief layers?')
    ap.add_argument('old', help='original galvo G-code')
    ap.add_argument('new', help='modified galvo G-code')
    ap.add_argument('--k', type=int, required=True, help='number of old relief layers that NEW skips')
    ap.add_argument('--feather', type=int, default=None,
                    help='NEW keeps only the first N of OLD\'s cleaning (feather) passes (feather_passes 1..7)')
    ap.add_argument('--json', help='write the full report as JSON')
    ap.add_argument('--tol', type=float, default=0.1, help='area tolerance in mm (default 0.1)')
    ap.add_argument('--path-tol', type=float, default=0.03,
                    help='allowed relative difference of per-layer laser-on path (default 0.03)')
    ap.add_argument('--cell', type=float, default=0.025, help='raster cell (mm) of the union area check (default 0.025)')
    ap.add_argument('--layer-cell', type=float, default=0.05,
                    help='raster cell (mm) of the same-V subset check (default 0.05)')
    ap.add_argument('--same-v-slack', type=float, default=0.2,
                    help='extra line-end allowance (mm) of the same-V subset check (default 0.2)')
    ap.add_argument('--layer-cell2', type=float, default=0.1,
                    help='raster cell (mm) of the new j / old j+K area match (default 0.1)')
    ap.add_argument('--end-ext', type=float, default=1.2,
                    help='trim (mm) at both ends of every move in the j / j+K area match (default 1.2)')
    ap.add_argument('--layer-area-frac', type=float, default=0.01,
                    help='max share of a layer path outside the partner layer in the j / j+K match (default 0.01)')
    args = ap.parse_args(argv)
    if ndi is None:
        print('ERROR: scipy (scipy.ndimage) is required', file=sys.stderr)
        return 2
    if args.k < 0:
        print('ERROR: --k must be >= 0', file=sys.stderr)
        return 2
    t0 = time.time()
    try:
        result, code = run(args.old, args.new, args.k, args)
    except OSError as e:
        print(f'ERROR: {e}', file=sys.stderr)
        return 2
    result['seconds'] = round(time.time() - t0, 1)
    if args.json:
        with open(args.json, 'w', encoding='utf-8') as fh:
            json.dump(result, fh, indent=1, default=_json_default)
    print(f'(finished in {time.time() - t0:.0f} s)', file=sys.stderr, flush=True)
    return code


if __name__ == '__main__':
    sys.exit(main())
