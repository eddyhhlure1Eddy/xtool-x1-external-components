"""Relief depth estimator for xTool Studio (galvo relief / "Embossment" mode).

Reads the relief parameters that xTool Studio keeps in its local store and the source image
of the selected canvas object, and shows the expected depth before the job is sent:

  * expected depth: every layer is assumed to remove as much as the V axis rises for it
    (V rise / layers per rise), so a pixel engraved by m layers ends at m * that depth;
  * a pixel with darkness D = 255 - gray is engraved by the layers i (0..N-1) with D > i*256/N,
    i.e. m = ceil(D * N / 256) layers: black is the deepest, white is not engraved;
  * per layer: covered area, laser-on path (area / line spacing) and laser-on time (path / speed);
  * laser figures: line spacing, pulse pitch along the line, pulse energy, energy per layer;
  * presets: "★ Save this job" keeps a good job's values with the material, board thickness, measured depth,
    a photo (the machine's own photo at the end of the job, Studio's camera picture, a file or a pasted
    screenshot) and a note, in %LOCALAPPDATA%\\xtool-relief-wrapper\\presets; "Presets" lists them and, per
    material and laser, fits the measured µm per layer against the energy per layer, to estimate this job's
    depth or what a target depth needs (layers, or speed);
  * live progress: while the machine runs a relief job, the layer (or feather pass) it is on, with that layer's
    area on the picture, from Studio's logs (the machine reports no line number) and Studio's own time
    estimator run layer by layer on the G-code that was sent;
  * the window belongs to Studio's main window, as Studio's own dialogs do: it stays above Studio but not above
    other programs, hides while Studio is minimised and closes when Studio quits; 收起 / Collapse shrinks it to
    one line (the expected depth, and the layer the machine is on while a job runs), a click on it opens it again.

Usage:  pythonw relief_estimator.py [--data-dir DIR] [--studio-exe PATH] [--lang zh|en]
  --data-dir    Studio user data folder (default: %APPDATA%\\xTool Studio)
  --studio-exe  only follow this Studio executable (default: any "xTool Studio.exe")
  --lang        UI language at start (default: the last choice of the window's EN / 中文 button,
                kept in %LOCALAPPDATA%\\xtool-relief-wrapper\\estimator.json, else the Windows UI language)
"""
import argparse, bisect, ctypes, glob, hashlib, json, math, os, re, shutil, tempfile, threading, time
import ctypes.wintypes as wt
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

import numpy as np
from PIL import Image, ImageGrab, ImageTk

user32 = ctypes.windll.user32
kernel32 = ctypes.windll.kernel32

TEXT = {
    "zh": {
        "title": "浮雕深度估算", "lang_btn": "EN", "object": "对象：", "none": "画布上没有浮雕对象，或 Studio 还没保存参数",
        "depth": "预计深度", "deepest": "最深", "mean": "平均", "deep_share": "最深处占面积 {p:.1f}%",
        "depth_rule": "= {N} 层 × {d:.4g} mm/层（V 每层抬多少，就按每层刻多深算）",
        "no_v": "V 轴上升是关的：没有按层下降的设定，深度只能实测",
        "keep_off": "保留顶部没生效：包装 DLL 没装（XTOOL_RELIEF_WRAPPER=0），这单按 Studio 原样刻",
        "keep_switch_off": "保留顶部关着：这单按 Studio 原样刻",
        "sw_keep": "保留顶部（前几层不刨）", "sw_feather": "羽化遍数", "sw_feather_tip": "（8＝厂家原样，0＝不加）",
        "sw_transp": "透明背景按图里的颜色（不当白色）",
        "sw_on": "包装 DLL 已装：改了以后下一次点「加工」生效", "sw_none": "包装 DLL 没装：开关不起作用",
        "keep_rule": "保留顶部：前 {k} 层不刻（每层都要刻掉 ≥{t:.0f}% 面积），最亮的 {u:.1f}% 面积保持原表面",
        "skipped": "不刻",
        "measured": "实测最深（mm）：", "measured_out": "每层实际刻掉 {a:.1f} µm，设定 {b:.1f} µm（{c:+.0f}%）",
        "params": "参数（从 Studio 读，自动刷新）", "more": "其他估算", "layers": "逐层",
        "head": "光源", "size": "尺寸", "N": "层数", "vmove": "V 轴上升", "every": "每隔几层升一次",
        "step": "每次上升", "power": "功率", "speed": "速度", "density": "密度", "freq": "频率",
        "pulse": "脉宽", "angle": "扫描角度", "repeat": "加工次数", "on": "开", "off": "关",
        "v_travel": "V 轴总行程", "v_travel_f": "(层数 − 1) ÷ 每隔几层 × 每次上升",
        "res": "深度分辨率", "res_f": "最深 ÷ 256 级灰度",
        "pitch_line": "线距", "pitch_line_f": "10 ÷ 密度",
        "pitch_pulse": "沿线脉冲间距", "pitch_pulse_f": "速度 ÷ 频率",
        "p_out": "出光功率", "p_out_f": "额定功率 × 功率%",
        "e_pulse": "单脉冲能量", "e_pulse_f": "出光功率 ÷ 频率",
        "flu": "每层能量密度", "flu_f": "出光功率 ÷ (速度 × 线距)",
        "flu_t": "最深处累计能量", "flu_t_f": "每层能量密度 × 层数 × 加工次数",
        "path": "出光路程", "path_f": "各层 刻的面积 ÷ 线距 之和",
        "t_on": "出光时间", "t_on_f": "出光路程 ÷ 速度（不含空走、换位）",
        "cols": ("层", "刻完深度 mm", "刻的灰度", "面积 %", "出光路程 m", "出光时间 s"),
        "noimg": "没找到原图，深度分布和逐层面积没算",
        "status": "参数保存于 {t}　对象 {id}",
        # preset library
        "rec_btn": "★ 记录这次", "lib_btn": "参数库",
        "lib_pred_label": "按参数库实测推算：", "lib_pred": "每层约 {z:.1f} µm{vtxt} → 最深约 {d:.2f} mm（{n} 条实测）",
        "lib_pred_v": "（V 每层设 {v:.1f} µm）",
        "lib_pred_none": "参数库里还没有「{m}」用 {h} 的实测记录", "lib_empty": "参数库还没有带实测深度的记录：刻好后点「★ 记录这次」",
        "no_job": "先在画布上选一个浮雕对象", "saved_ok": "已记录：{m}，{t:g} mm（参数库第 {n} 条）",
        "rec_title": "记录这次的参数（刻得好的）", "rec_job": "这次的参数（从 Studio 读的）",
        "material": "材料", "thickness": "板厚 mm", "measured_mm": "实测最深 mm", "measured_tip": "量不了就空着",
        "note": "备注", "photo": "照片",
        "ph_machine": "机器完工照", "ph_camera": "Studio 相机照", "ph_file": "选文件…", "ph_paste": "粘贴截图",
        "ph_none": "不要照片", "ph_from": "{src}（{t}）", "ph_nothing": "没有照片",
        "ph_no_machine": "最近 2 小时没有机器完工照", "ph_no_camera": "没找到 Studio 相机照", "ph_no_clip": "剪贴板里没有图片",
        "save": "保存", "cancel": "取消", "need_material": "请填材料", "need_thickness": "板厚要填数字（mm）",
        "bad_measured": "实测最深要填数字（mm），量不了就空着",
        "job_l1": "{head} · {w:.1f} × {h:.1f} mm · {N} 层（刻 {kept}）· V 每层 {v} mm",
        "job_l2": "功率 {p:g}% · 速度 {s:g} mm/s · 密度 {d:g} /cm · 频率 {f:g} kHz · 加工次数 {r}",
        "job_l3": "每层能量密度 {e} · 预计最深 {dp} · 保留顶部 {kt} · 羽化 {fp} 遍 · 透明背景按颜色 {tr}",
        "lib_title": "参数库", "all": "全部", "open_folder": "打开记录文件夹", "open_photo": "看照片", "delete": "删除这条",
        "lib_cols": ("记录时间", "材料", "板厚", "光源", "层数", "功率", "速度", "密度", "频率", "每层能量", "实测最深", "每层实刻"),
        "confirm_delete": "删除这条记录（{m}，{t}）？它的照片也一起删掉。",
        "fit_pick": "选一种材料，就按它的实测记录推算", "fit_none": "「{m}」用 {h} 还没有带实测深度的记录",
        "fit_line": "{n} 条实测（{h}）：每层实刻 ≈ {model}", "fit_model_line": "{a:.3g} µm × 每层能量 {b:+.2f} µm",
        "fit_model_origin": "{a:.3g} µm × 每层能量（J/cm²）",
        "pred_line": "当前任务每层能量 {e:.3g} J/cm²：每层约 {z:.1f} µm，{L} 层约 {d:.2f} mm",
        "pred_nojob": "画布上没有浮雕对象，推算不了当前任务", "target": "目标深度 mm",
        "target_out": "参数不变约要 {L} 层；或 {N} 层不变、速度改到约 {s:.0f} mm/s",
        "target_layers": "参数不变约要 {L} 层", "target_bad": "按这条关系算不出这个深度",
        "detail": "{m} · 板厚 {t:g} mm · 实测最深 {md} · 每层实刻 {z}\n{job}\n备注：{note}",
        "calc": "按实测推算（同材料、同光源）", "hint_layers": "{n} 层",
        # live job
        "live": "机器实时进度（按时间推算）", "live_run": "正在刻", "live_pause": "已暂停", "live_done": "刻完了",
        "live_cancel": "已取消", "live_ready": "等待开始", "live_approx": "（约）",
        "live_layer": "第 {i} / {n} 层", "live_feather": "羽化第 {i} / {n} 遍",
        "live_time": "已刻 {el}，预计还要 {rem}（Studio 估 {est}{corr}）", "live_corr": "，按以往实际 ×{f:.2f}",
        "live_over": "已刻 {el}，比预估多了 {over}（Studio 估 {est}{corr}）", "live_took": "用时 {el}（Studio 估 {est}）",
        "live_time_only": "已刻 {el}", "live_reading": "正在读这单的 G 代码…",
        "live_note": "机器不报刻到哪一行：按开始时间、暂停和 Studio 的预估时间推算，每层占多少时间用厂家自己的估算逐层算",
        "live_rough": "厂家的时间估算用不了（{e}），只能按 G 代码指令条数粗分，羽化会被算长",
        "live_nofile": "任务文件夹里的 G 代码已经不是发给机器的那份，只能显示时间",
        "live_noest": "没找到 Studio 对这单的预估时间，只能显示时间",
        # collapsed window
        "fold_btn": "收起", "unfold_btn": "展开", "strip_left": "预计还要 {rem}", "strip_none": "画布上没有浮雕对象",
    },
    "en": {
        "title": "Relief depth estimate", "lang_btn": "中文", "object": "Object:", "none": "No relief object on the canvas, or Studio has not saved yet",
        "depth": "Expected depth", "deepest": "Deepest", "mean": "Mean", "deep_share": "{p:.1f}% of the area at the deepest",
        "depth_rule": "= {N} layers × {d:.4g} mm/layer (each layer assumed to remove what V rises for it)",
        "no_v": "V-axis rise is off: no per-layer descent is set, depth can only be measured",
        "keep_off": "Keep top not active: the plugin wrapper is not installed (XTOOL_RELIEF_WRAPPER=0); this job is engraved as Studio made it",
        "keep_switch_off": "Keep top switched off: this job is engraved as Studio made it",
        "sw_keep": "Keep top (first layers not planed)", "sw_feather": "Feather passes", "sw_feather_tip": "(8 = as shipped, 0 = none)",
        "sw_transp": "Transparent background in its own colour (not white)",
        "sw_on": "Plugin wrapper installed: a change applies from the next Process click", "sw_none": "Plugin wrapper not installed: switches have no effect",
        "keep_rule": "Keep top: first {k} layers not engraved (each would burn >= {t:.0f}% of the area); brightest {u:.1f}% of the area stays at the surface",
        "skipped": "skipped",
        "measured": "Measured deepest (mm):", "measured_out": "{a:.1f} µm removed per layer, set {b:.1f} µm ({c:+.0f}%)",
        "params": "Parameters (read from Studio, live)", "more": "More estimates", "layers": "Per layer",
        "head": "Laser", "size": "Size", "N": "Layers", "vmove": "V-axis rise", "every": "Rise every N layers",
        "step": "Rise per step", "power": "Power", "speed": "Speed", "density": "Density", "freq": "Frequency",
        "pulse": "Pulse width", "angle": "Scan angle", "repeat": "Passes", "on": "on", "off": "off",
        "v_travel": "V-axis travel", "v_travel_f": "(layers − 1) ÷ every N × rise per step",
        "res": "Depth resolution", "res_f": "deepest ÷ 256 gray levels",
        "pitch_line": "Line spacing", "pitch_line_f": "10 ÷ density",
        "pitch_pulse": "Pulse pitch on line", "pitch_pulse_f": "speed ÷ frequency",
        "p_out": "Output power", "p_out_f": "rated power × power %",
        "e_pulse": "Pulse energy", "e_pulse_f": "output power ÷ frequency",
        "flu": "Energy per layer", "flu_f": "output power ÷ (speed × line spacing)",
        "flu_t": "Energy at deepest", "flu_t_f": "energy per layer × layers × passes",
        "path": "Laser-on path", "path_f": "sum over layers of area ÷ line spacing",
        "t_on": "Laser-on time", "t_on_f": "laser-on path ÷ speed (no travel moves)",
        "cols": ("Layer", "Depth after mm", "Gray engraved", "Area %", "Path m", "Time s"),
        "noimg": "Source image not found: depth map and per-layer area skipped",
        "status": "Parameters saved {t}   object {id}",
        # preset library
        "rec_btn": "★ Save this job", "lib_btn": "Presets",
        "lib_pred_label": "From measured presets:", "lib_pred": "about {z:.1f} µm per layer{vtxt} → deepest about {d:.2f} mm ({n} measured)",
        "lib_pred_v": " (V set to {v:.1f} µm)",
        "lib_pred_none": "No measured preset for \"{m}\" with {h} yet", "lib_empty": "No preset with a measured depth yet: after a good job, click \"★ Save this job\"",
        "no_job": "Select a relief object on the canvas first", "saved_ok": "Saved: {m}, {t:g} mm (preset {n})",
        "rec_title": "Save this job's parameters (a good result)", "rec_job": "This job (read from Studio)",
        "material": "Material", "thickness": "Thickness mm", "measured_mm": "Measured depth mm", "measured_tip": "leave empty if not measured",
        "note": "Note", "photo": "Photo",
        "ph_machine": "Machine photo", "ph_camera": "Studio camera", "ph_file": "File…", "ph_paste": "Paste",
        "ph_none": "No photo", "ph_from": "{src} ({t})", "ph_nothing": "No photo",
        "ph_no_machine": "No machine photo from the last 2 hours", "ph_no_camera": "No Studio camera photo found", "ph_no_clip": "No image on the clipboard",
        "save": "Save", "cancel": "Cancel", "need_material": "Enter the material", "need_thickness": "Thickness must be a number (mm)",
        "bad_measured": "Measured depth must be a number (mm), or empty",
        "job_l1": "{head} · {w:.1f} × {h:.1f} mm · {N} layers ({kept} engraved) · V {v} mm per layer",
        "job_l2": "power {p:g}% · speed {s:g} mm/s · density {d:g} /cm · frequency {f:g} kHz · passes {r}",
        "job_l3": "energy per layer {e} · expected deepest {dp} · keep top {kt} · {fp} feather passes · transparent in colour {tr}",
        "lib_title": "Preset library", "all": "All", "open_folder": "Open folder", "open_photo": "Open photo", "delete": "Delete",
        "lib_cols": ("Saved", "Material", "Thick.", "Laser", "Layers", "Power", "Speed", "Density", "Freq.", "Energy", "Measured", "µm/layer"),
        "confirm_delete": "Delete this preset ({m}, {t})? Its photo is deleted too.",
        "fit_pick": "Pick a material to calculate from its measured presets", "fit_none": "No measured preset for \"{m}\" with {h} yet",
        "fit_line": "{n} measured ({h}): µm per layer ≈ {model}", "fit_model_line": "{a:.3g} µm × energy per layer {b:+.2f} µm",
        "fit_model_origin": "{a:.3g} µm × energy per layer (J/cm²)",
        "pred_line": "Current job, {e:.3g} J/cm² per layer: about {z:.1f} µm per layer, {L} layers about {d:.2f} mm",
        "pred_nojob": "No relief object on the canvas: nothing to calculate for", "target": "Target depth mm",
        "target_out": "about {L} layers with these settings; or keep {N} layers and set the speed to about {s:.0f} mm/s",
        "target_layers": "about {L} layers with these settings", "target_bad": "This relation cannot reach that depth",
        "detail": "{m} · thickness {t:g} mm · measured {md} · {z} per layer\n{job}\nNote: {note}",
        "calc": "From the measured presets (same material, same laser)", "hint_layers": "{n} layers",
        # live job
        "live": "Machine progress (estimated from time)", "live_run": "Engraving", "live_pause": "Paused", "live_done": "Finished",
        "live_cancel": "Cancelled", "live_ready": "Ready", "live_approx": " (approx.)",
        "live_layer": "layer {i} / {n}", "live_feather": "feather pass {i} / {n}",
        "live_time": "running {el}, about {rem} left (Studio estimate {est}{corr})", "live_corr": ", ×{f:.2f} from past jobs",
        "live_over": "running {el}, {over} past the estimate (Studio estimate {est}{corr})", "live_took": "took {el} (Studio estimate {est})",
        "live_time_only": "running {el}", "live_reading": "Reading this job's G-code…",
        "live_note": "The machine reports no line number: estimated from the start time, pauses and Studio's estimate; each layer's share comes from Studio's own estimator, run layer by layer",
        "live_rough": "Studio's estimator is not available ({e}): shared out by G-code moves, which overweights the feather passes",
        "live_nofile": "The G-code in the task folder is no longer the one sent to the machine: time only",
        "live_noest": "No Studio time estimate found for this job: time only",
        # collapsed window
        "fold_btn": "Collapse", "unfold_btn": "Expand", "strip_left": "about {rem} left", "strip_none": "No relief object on the canvas",
    },
}
HEADS = {"uv": "UV", "blue": "Blue", "co2": "CO2", "ir": "IR", "fiber": "Fiber", "mopa": "MOPA"}


def ui_lang():
    lid = kernel32.GetUserDefaultUILanguage() & 0x3FF
    return "zh" if lid == 0x04 else "en"


def settings_path():
    base = os.environ.get("LOCALAPPDATA") or os.environ.get("APPDATA") or os.path.expanduser("~")
    return os.path.join(base, "xtool-relief-wrapper", "estimator.json")


def load_settings():
    try:
        with open(settings_path(), encoding="utf-8") as f:
            s = json.load(f)
    except (OSError, ValueError):
        return {}
    return s if isinstance(s, dict) else {}


def save_settings(**kw):
    """Merge keys into estimator.json (language, learnt time ratios)."""
    s = load_settings()
    s.update(kw)
    path = settings_path()
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path + ".tmp", "w", encoding="utf-8") as f:
        json.dump(s, f, ensure_ascii=False)
    os.replace(path + ".tmp", path)


def saved_lang():
    """The language last picked with the window's EN / 中文 button, or None."""
    lang = load_settings().get("lang")
    return lang if lang in TEXT else None


def save_lang(lang):
    save_settings(lang=lang)


# ---------------------------------------------------------------- Studio window
def exe_of_window(hwnd):
    pid = wt.DWORD()
    user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
    h = kernel32.OpenProcess(0x1000, False, pid.value)
    if not h:
        return ""
    buf = ctypes.create_unicode_buffer(520)
    n = wt.DWORD(520)
    ok = kernel32.QueryFullProcessImageNameW(h, 0, buf, ctypes.byref(n))
    kernel32.CloseHandle(h)
    return buf.value.lower() if ok else ""


def plugin_dir(studio_exe):
    app = os.path.dirname(studio_exe) if studio_exe else os.path.join(os.environ.get("LOCALAPPDATA", ""), "Programs", "xTool Studio")
    return os.path.join(app, "resources", "tools", "algo-server", "plugins", "algo-path-planning-plugin")


def wrapper_installed(studio_exe):
    """The relief wrapper DLL sits in front of the vendor path planning plugin (relief_wrapper_ctl.ps1 -Mode 1)."""
    return os.path.isfile(os.path.join(plugin_dir(studio_exe), "lib_path_planning_orig.dll"))


def read_switches(studio_exe):
    """{"keep_top": float, "keep_feather": bool, "feather_passes": 0..8, "transparent_as_color": bool} from
    relief_wrapper.ini next to the wrapper, keys that are there."""
    out = {}
    try:
        with open(os.path.join(plugin_dir(studio_exe), "relief_wrapper.ini"), encoding="ascii", errors="replace") as f:
            for line in f:
                key, eq, val = line.split("#", 1)[0].partition("=")
                key, val = key.strip(), val.strip()
                if eq and key == "keep_top":
                    out["keep_top"] = float(val)
                elif eq and key == "keep_feather" and val in ("0", "1"):
                    out["keep_feather"] = val == "1"
                elif eq and key == "feather_passes" and val in tuple("012345678"):
                    out["feather_passes"] = int(val)
                elif eq and key == "transparent_as_color" and val in ("0", "1"):
                    out["transparent_as_color"] = val == "1"
    except (OSError, ValueError):
        pass
    return out


def write_switches(studio_exe, keep_top, feather_passes, transparent_as_color=True):
    """The wrapper reads this file on every relief generation, so a change applies from the next Process click.
    feather_passes: how many of the slicer's 8 feather passes to keep (8 = as shipped, 0 = none, 1..7 = the first
    ones; True / False mean 8 / 0).  keep_feather is written too, for a wrapper that predates feather_passes."""
    if isinstance(feather_passes, bool):
        feather_passes = 8 if feather_passes else 0
    feather_passes = min(8, max(0, int(feather_passes)))
    path = os.path.join(plugin_dir(studio_exe), "relief_wrapper.ini")
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="ascii", newline="\r\n") as f:
        f.write("# relief wrapper switches, read on every relief generation (written by relief_estimator.py)\n"
                f"keep_top={keep_top:g}\nkeep_feather={1 if feather_passes else 0}\nfeather_passes={feather_passes}\n"
                f"transparent_as_color={1 if transparent_as_color else 0}\n")
    os.replace(tmp, path)


def is_studio(exe, studio_exe):
    return exe == studio_exe if studio_exe else os.path.basename(exe) == "xtool studio.exe"


GWLP_HWNDPARENT, GW_OWNER, GW_HWNDPREV, SW_HIDE, SW_SHOWNOACTIVATE, SYNCHRONIZE = -8, 4, 3, 0, 4, 0x00100000
GWL_EXSTYLE, WS_EX_TOPMOST = -20, 0x8
SWP_NOSIZE, SWP_NOMOVE, SWP_NOACTIVATE, SWP_NOOWNERZORDER, HWND_TOP, HWND_NOTOPMOST = 0x1, 0x2, 0x10, 0x200, 0, -2
SWP_ONLY_ORDER = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER     # restack this window only, never Studio's
get_window = ctypes.WINFUNCTYPE(wt.HWND, wt.HWND, wt.UINT)(("GetWindow", user32))
set_window_long_ptr = ctypes.WINFUNCTYPE(ctypes.c_ssize_t, wt.HWND, ctypes.c_int, ctypes.c_ssize_t)(
    ("SetWindowLongPtrW" if ctypes.sizeof(ctypes.c_void_p) == 8 else "SetWindowLongW", user32))
set_window_pos = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, ctypes.c_ssize_t, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                                    wt.UINT)(("SetWindowPos", user32))      # insert-after as a full-width -2 (HWND_NOTOPMOST)
monitor_from_window = ctypes.WINFUNCTYPE(wt.HANDLE, wt.HWND, wt.DWORD)(("MonitorFromWindow", user32))


def is_topmost(hwnd):
    return bool(user32.GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST)


class WINDOWPLACEMENT(ctypes.Structure):
    _fields_ = [("length", wt.UINT), ("flags", wt.UINT), ("showCmd", wt.UINT), ("ptMinPosition", wt.POINT),
                ("ptMaxPosition", wt.POINT), ("rcNormalPosition", wt.RECT)]


class MONITORINFO(ctypes.Structure):
    _fields_ = [("cbSize", wt.DWORD), ("rcMonitor", wt.RECT), ("rcWork", wt.RECT), ("dwFlags", wt.DWORD)]


get_monitor_info = ctypes.WINFUNCTYPE(wt.BOOL, wt.HANDLE, ctypes.POINTER(MONITORINFO))(("GetMonitorInfoW", user32))


def studio_window(studio_exe):
    """Studio's main window: of the visible, titled windows of Studio that no other window owns (not its dialogs), the
    biggest in its normal (not minimised) size, so not a splash screen either; None if there is none.  The title is
    read with GetWindowTextW, which does not wait on Studio when Studio does not respond."""
    found = []
    title = ctypes.create_unicode_buffer(2)

    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def cb(hwnd, _):
        if user32.IsWindowVisible(hwnd) and not get_window(hwnd, GW_OWNER) and user32.GetWindowTextW(hwnd, title, 2) > 0 \
                and is_studio(exe_of_window(hwnd), studio_exe):
            wp = WINDOWPLACEMENT()
            wp.length = ctypes.sizeof(wp)
            user32.GetWindowPlacement(hwnd, ctypes.byref(wp))
            r = wp.rcNormalPosition
            found.append(((r.right - r.left) * (r.bottom - r.top), hwnd))
        return True

    user32.EnumWindows(cb, 0)
    return max(found)[1] if found else None


# ---------------------------------------------------------------- Studio local store
class Store:
    """Studio keeps the project in a Chromium file-system store: <data>/File System/000/t/<n>/<file number>."""

    def __init__(self, data_dir):
        self.root = os.path.join(data_dir, "File System", "000", "t")
        self.kind = {}
        self.img = {}

    @staticmethod
    def _kind(path):
        try:
            with open(path, "rb") as f:
                head = f.read(400)
        except OSError:
            return None
        if head.startswith(b'{"canvasId"') and b'"displays"' in head:
            return "displays"
        if b'"deviceCode"' in head and b'"processing"' in head:
            return "params"
        if head.lstrip().startswith(b"{") and b'"profiles"' in head[:40]:
            return "profiles"
        if head[:8] == b"\x89PNG\r\n\x1a\n" or head[:2] == b"\xff\xd8":
            return "image"
        return "other"

    def scan(self):
        newest, files = {}, []
        try:
            subs = [e for e in os.scandir(self.root) if e.is_dir() and e.name.isdigit()]
        except OSError:
            return newest, files
        for sub in subs:
            for e in os.scandir(sub.path):
                if not e.is_file():
                    continue
                st = e.stat()
                if st.st_size < 20:
                    continue
                rec = self.kind.get(e.path)
                if not rec or rec[0] != st.st_mtime_ns:
                    rec = (st.st_mtime_ns, self._kind(e.path))
                    self.kind[e.path] = rec
                files.append((e.path, st.st_mtime_ns, rec[1]))
                k = rec[1]
                if k in ("displays", "params", "profiles") and (k not in newest or st.st_mtime_ns > newest[k][1]):
                    newest[k] = (e.path, st.st_mtime_ns)
        return newest, files

    def find_image(self, sha, size, files):
        """Images are stored under their SHA-256 (resourcePath resources/<sha256>.png)."""
        for path, mt, k in files:
            if k != "image":
                continue
            rec = self.img.get(path)
            if not rec or rec[0] != mt:
                try:
                    with Image.open(path) as im:
                        dims = im.size
                    digest = None
                    if size is None or dims == tuple(size):
                        with open(path, "rb") as f:
                            digest = hashlib.sha256(f.read()).hexdigest()
                    rec = (mt, digest)
                except Exception:
                    rec = (mt, None)
                self.img[path] = rec
            if rec[1] == sha:
                return path
        return None


def load_json(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def relief_items(params, displays, profiles):
    """Relief objects on the canvas with their effective values (profile values, then patch overrides)."""
    out = []
    disp = {d.get("id"): d for d in displays.get("displays", [])}
    for proc in params.get("processing", {}).values():
        mode = proc.get("modes", {}).get("RELIEF_PROCESS")
        if not mode:
            continue
        ignored = set(mode.get("ignoredDisplayIds", []))
        for b in mode.get("bindings", []):
            prof = profiles.get(b.get("baseProfileId"), {})
            for did in b.get("displayIds", []):
                if did in disp and did not in ignored:
                    vals = dict(prof.get("values", {}))
                    for pid in b.get("patchIds", []):
                        vals.update(mode.get("patches", {}).get(pid, {}).get("overrides", {}))
                    out.append({"id": did, "display": disp[did], "values": vals, "type": prof.get("processingType", "")})
    return out


# ---------------------------------------------------------------- estimates
def darkness(im, transparent_as_color=False):
    """255 - gray as int32 (16-bit images scaled to 8 bit). Transparent pixels count as white (not engraved, what
    Studio hands the slicer), or with transparent_as_color as the colour stored in them (the wrapper's switch)."""
    if im.mode == "P":
        im = im.convert("RGBA")
    if im.mode in ("I;16", "I;16B", "I;16L", "I;16N", "I"):
        a = np.asarray(im).astype(np.float64)
        top = 65535.0 if a.size and a.max() > 255 else 255.0
        gray = np.clip(np.round(a * 255.0 / top), 0, 255).astype(np.int32)
    else:
        gray = np.asarray(im.convert("L"), dtype=np.int32)
    dark = 255 - gray
    if im.mode in ("RGBA", "LA", "PA") and not transparent_as_color:
        dark = np.where(np.asarray(im.getchannel("A")) < 128, 0, dark)
    return dark


def layer_thresholds(n):
    """Smallest darkness each layer burns, as measured on the slicer's own output: pure white is never burned,
    layer 0 burns every other pixel, layer i >= 1 burns gray <= floor(256 * (n - i) / n)."""
    return [1] + [max(1, 255 - (256 * (n - i)) // n) for i in range(1, n)]


def choose_k(covs, pct):
    """covs[i] = share of the image burnt by layer i. Smallest k after which the brightest pct% of the engraved
    area stays at the surface. k = 0 if no k <= N-1 gets there (flat-tone image: every layer burns the same
    area) or if more than half of the engraved area would stay (a large flat top: left as the slicer made it)."""
    n, c0 = len(covs), (covs[0] if covs else 0.0)
    if pct <= 0 or c0 <= 0:
        return 0, ""
    k = 0
    while k < n - 1 and covs[k] >= (1 - pct / 100.0) * c0:
        k += 1
    kept_share = 1 - covs[k] / c0
    if kept_share < pct / 100.0:
        return 0, "flat-tone image"
    if kept_share > 0.5:
        return 0, "large flat top"
    return k, ""


def v_lost_layers(n, w_px, h_px, bw_px, bh_px, every=1):
    """Layers of an n-layer job that the slicer leaves without their V line (it writes it only at the layer's
    block row 0 / col 0, which does not exist when the layer's grid shift is a multiple of the block size)."""
    if n < 2 or not (w_px and h_px and bw_px and bh_px):
        return []
    s = max(w_px // n, 5) + 1
    return [j for j in range(1, n) if (j + 1) % every == 0 and
            ((w_px > bw_px and (j * s) % bw_px == 0) or (h_px > bh_px and (j * s) % bh_px == 0))]


def avoid_v_loss(k, n, covs, lost):
    """k as chosen, or the first of k+1..k+3, k-1..k-3 whose N-k loses no V line (fewest if none is clean),
    keeping at most half of the engraved area at the surface. lost(n) -> list of layers without V."""
    if k <= 0 or not lost(n - k):
        return k
    best, best_n = k, len(lost(n - k))
    for kk in (k + 1, k + 2, k + 3, k - 1, k - 2, k - 3):
        if not 1 <= kk <= n - 1 or 1 - covs[kk] / covs[0] > 0.5:
            continue
        m = len(lost(n - kk))
        if m < best_n:
            best, best_n = kk, m
        if m == 0:
            break
    return best


def estimate(item, img_path, keep_top_pct=0.0, transparent_as_color=False):
    v, d = item["values"], item["display"]
    num = lambda key, default=0.0: float(v.get(key, default) or default)
    N = int(num("sliceNumber"))
    every = max(1, int(num("vLayers", 1)))
    step = num("vDecline")
    vmove = bool(v.get("zAxisMove"))
    power, speed, density, freq = num("power"), num("speed"), num("density"), num("frequency")
    passes = max(1, int(num("repeat", 1)))
    m = re.match(r"([\d.]+)@(\w+)", str(v.get("laser", "")))
    rated = float(m.group(1)) if m else None
    W, H = float(d.get("width", 0)), float(d.get("height", 0))
    line = 10.0 / density if density else None
    r = {"N": N, "every": every, "step": step, "vmove": vmove, "power": power, "speed": speed, "density": density,
         "freq": freq, "passes": passes, "rated": rated, "head": m.group(2) if m else "", "W": W, "H": H, "line": line}
    r["per_layer"] = step / every if vmove else None
    r["deepest"] = N * r["per_layer"] if vmove else None
    r["v_travel"] = (N - 1) // every * step if vmove and N else 0.0
    r["p_out"] = rated * power / 100 if rated else None
    if r["p_out"] and freq:
        r["e_pulse"] = r["p_out"] / (freq * 1000) * 1e6
    if speed and freq:
        r["pitch"] = speed / (freq * 1000)
    if r["p_out"] and speed and line:
        r["flu"] = r["p_out"] / (speed * line) * 100
        r["flu_t"] = r["flu"] * N * passes
    r["rows"], r["map"] = [], None
    if not (img_path and N and line):
        return r
    dark = darkness(Image.open(img_path), transparent_as_color)
    thr = layer_thresholds(N)
    per_dark = np.zeros(256, np.int32)                                     # layers that engrave each darkness
    for x in thr:
        per_dark[x:] += 1
    hit = per_dark[dark]
    cnt = np.bincount(dark.ravel(), minlength=256)[:256]
    ge = np.cumsum(cnt[::-1])[::-1]                                        # pixels with darkness >= x
    total, area, path = dark.size, W * H, 0.0
    covs = [ge[x] / total for x in thr]
    # keep-top: leading layers that each burn >= (100 - pct)% of what layer 1 burns are not engraved,
    # so the brightest part stays at the original surface and the rest starts from there
    k, why = choose_k(covs, keep_top_pct)
    r["k_rule"] = k
    if k and vmove and step:
        # same choice as the plugin wrapper: avoid an N-k that makes the slicer drop a layer's V line
        dpi = int(round(density * 2.54))
        w_px, h_px = int(round(W * density / 10.0)), int(round(H * density / 10.0))
        bpx = int(np.float32((30 if r["head"] == "uv" else 50) * dpi) / np.float32(25.4))
        k = avoid_v_loss(k, N, covs, lambda n: v_lost_layers(n, w_px, h_px, bpx, bpx, every))
    r["k"], r["keep_top_pct"], r["keep_why"] = k, keep_top_pct, why
    for i in range(N):
        x = thr[i]
        cov = covs[i]
        meters = 0.0 if i < k else cov * area / line / 1000.0 * passes
        path += meters
        after = (i - k + 1) * r["per_layer"] if vmove and i >= k else None
        r["rows"].append((i + 1, after, 255 - x if x <= 255 else None, cov, meters,
                          meters * 1000 / speed if speed else 0.0, i < k))
    kept = N - k
    if vmove:
        r["deepest"] = kept * r["per_layer"]
        r["v_travel"] = (kept - 1) // every * step
    if r.get("flu"):
        r["flu_t"] = r["flu"] * kept * passes
    r["path"] = path
    r["t_on"] = path * 1000 / speed if speed else None
    hit = np.maximum(0, hit - k)
    r["mean_layers"] = float(hit.mean())
    r["deepest_share"] = float((hit == kept).mean())
    r["untouched"] = float((hit == 0).mean())
    r["hit"], r["kept"] = hit, kept
    return r


def depth_colors(frac):
    """0 = surface (pale) .. 1 = deepest (dark brown)."""
    stops = np.array([[248, 240, 214], [240, 190, 110], [205, 120, 50], [140, 60, 25], [60, 25, 10]], dtype=np.float32)
    pos = np.clip(frac, 0, 1) * (len(stops) - 1)
    lo = np.floor(pos).astype(np.int32)
    hi = np.minimum(lo + 1, len(stops) - 1)
    t = (pos - lo)[..., None]
    return (stops[lo] * (1 - t) + stops[hi] * t).astype(np.uint8)


def hms(s):
    s = int(round(s))
    return f"{s // 3600}:{s % 3600 // 60:02d}:{s % 60:02d}"


# ---------------------------------------------------------------- preset library (good jobs, kept on this PC)
# presets.jsonl (one record per line), photos\<id>.jpg, thumbs\<id>.png under %LOCALAPPDATA%\xtool-relief-wrapper\presets
MATERIALS = {"zh": ["椴木层板", "胡桃木", "樱桃木", "竹板", "亚克力", "皮革"],
             "en": ["Basswood plywood", "Walnut", "Cherry", "Bamboo", "Acrylic", "Leather"]}


def presets_dir():
    return os.path.join(os.path.dirname(settings_path()), "presets")


def load_presets():
    """All records, oldest first; unreadable lines are skipped."""
    out = []
    try:
        with open(os.path.join(presets_dir(), "presets.jsonl"), encoding="utf-8") as f:
            for line in f:
                try:
                    rec = json.loads(line)
                except ValueError:
                    continue
                if isinstance(rec, dict) and rec.get("id"):
                    out.append(rec)
    except OSError:
        pass
    return out


def preset_materials(recs, measured_only=False):
    """Materials of the records, the most recently used first."""
    seen = []
    for rec in reversed(recs):
        m = str(rec.get("material") or "").strip()
        if m and m not in seen and (not measured_only or isinstance(rec.get("um_per_layer"), (int, float))):
            seen.append(m)
    return seen


def job_record(item, r, switches):
    """What a preset keeps about the job: the values the estimate used, the wrapper switches and Studio's values."""
    e = r["flu"] * r["passes"] if r.get("flu") else None
    return {"object_id": item["id"], "laser": r["head"], "rated_w": r["rated"], "size_mm": [round(r["W"], 3), round(r["H"], 3)],
            "layers": r["N"], "layers_engraved": r.get("kept", r["N"]), "keep_top_k": r.get("k", 0),
            "v_per_layer_mm": r["per_layer"], "power_pct": r["power"], "speed_mm_s": r["speed"], "density_per_cm": r["density"],
            "freq_khz": r["freq"], "passes": r["passes"], "pulse_width": item["values"].get("pulseWidth"),
            "scan_angle": item["values"].get("processAngle"), "energy_per_layer_j_cm2": e, "expected_deepest_mm": r["deepest"],
            "switches": switches, "values": item["values"]}


def save_preset(material, thickness, measured, note, job, photo=None, photo_source=None, relief_img=None, now=None):
    """Append one record. photo (a PIL image) is kept as a JPEG of at most 2000 px, the relief picture as a small
    gray thumbnail. measured = deepest point in mm (None if not measured)."""
    d = presets_dir()
    for sub in ("photos", "thumbs"):
        os.makedirs(os.path.join(d, sub), exist_ok=True)
    t = now or time.localtime()
    rid, ids, i = time.strftime("%Y%m%d-%H%M%S", t), {r["id"] for r in load_presets()}, 2
    base = rid
    while rid in ids:
        rid, i = f"{base}-{i}", i + 1
    kept = job.get("layers_engraved")
    rec = {"id": rid, "saved": time.strftime("%Y-%m-%d %H:%M:%S", t), "material": material, "thickness_mm": thickness,
           "measured_mm": measured, "um_per_layer": measured * 1000 / kept if measured is not None and kept else None,
           "note": note, "photo": None, "photo_source": photo_source if photo is not None else None, "relief_thumb": None,
           "job": job}
    if photo is not None:
        im = photo.convert("RGB")
        im.thumbnail((2000, 2000))
        rec["photo"] = "photos/" + rid + ".jpg"
        im.save(os.path.join(d, rec["photo"]), "JPEG", quality=90)
    if relief_img:
        try:
            with Image.open(relief_img) as im:
                im = im.convert("L")
                im.thumbnail((240, 240))
                rec["relief_thumb"] = "thumbs/" + rid + ".png"
                im.save(os.path.join(d, rec["relief_thumb"]))
        except (OSError, ValueError):
            rec["relief_thumb"] = None
    with open(os.path.join(d, "presets.jsonl"), "a", encoding="utf-8") as f:
        f.write(json.dumps(rec, ensure_ascii=False) + "\n")
    return rec


def delete_preset(rid):
    """Remove one record and its own photo / thumbnail. True if it was there."""
    d = presets_dir()
    path = os.path.join(d, "presets.jsonl")
    with open(path, encoding="utf-8") as f:
        lines = f.readlines()
    keep, gone = [], None
    for line in lines:
        try:
            rec = json.loads(line)
        except ValueError:
            keep.append(line)
            continue
        if gone is None and isinstance(rec, dict) and rec.get("id") == rid:
            gone = rec
        else:
            keep.append(line)
    if gone is None:
        return False
    with open(path + ".tmp", "w", encoding="utf-8") as f:
        f.writelines(keep)
    os.replace(path + ".tmp", path)
    for key in ("photo", "relief_thumb"):
        rel = gone.get(key)
        if isinstance(rel, str) and re.fullmatch(r"(photos|thumbs)/[\w-]+\.(jpg|png)", rel):
            try:
                os.remove(os.path.join(d, rel))
            except OSError:
                pass
    return True


def depth_points(recs, material, laser):
    """(energy per layer J/cm², measured µm per layer) of the records of one material and laser."""
    pts = []
    for rec in recs:
        j = rec.get("job") or {}
        e, z = j.get("energy_per_layer_j_cm2"), rec.get("um_per_layer")
        if rec.get("material") == material and j.get("laser") == laser and isinstance(e, (int, float)) and \
                isinstance(z, (int, float)) and e > 0 and z >= 0:
            pts.append((float(e), float(z)))
    return pts


def fit_depth(pts):
    """µm per layer as a function of the energy per layer: a straight line by least squares when there are 3 or
    more points spread over at least 20 % in energy, else a line through zero. None without points."""
    if not pts:
        return None
    n = len(pts)
    es = [e for e, _ in pts]
    if n >= 3 and max(es) >= 1.2 * min(es):
        me, mz = sum(es) / n, sum(z for _, z in pts) / n
        a = sum((e - me) * (z - mz) for e, z in pts) / sum((e - me) ** 2 for e in es)
        if a > 0:
            return {"kind": "line", "a": a, "b": mz - a * me, "n": n}
    return {"kind": "origin", "a": sum(e * z for e, z in pts) / sum(e * e for e in es), "b": 0.0, "n": n}


def fit_um(fit, e):
    return max(0.0, fit["a"] * e + fit["b"])


def target_plan(fit, r, target_mm):
    """For a target deepest point: (layers needed with the current settings, speed in mm/s that reaches it with
    the current number of layers); None for what this relation cannot reach."""
    kept = r.get("kept") or r.get("N")
    e0 = r["flu"] * r["passes"] if r.get("flu") else None
    if not (fit and e0 and kept and target_mm and target_mm > 0):
        return None, None
    z0 = fit_um(fit, e0)
    layers = math.ceil(target_mm * 1000 / z0) if z0 > 0 else None
    en = (target_mm * 1000 / kept - fit["b"]) / fit["a"] if fit["a"] > 0 else 0.0
    speed = r["p_out"] / (en / r["passes"] * r["line"]) * 100 if en > 0 and r.get("p_out") and r.get("line") else None
    return layers, speed


def newest_file(pattern):
    best = None
    for p in glob.glob(pattern):
        try:
            mt = os.path.getmtime(p)
        except OSError:
            continue
        if best is None or mt > best[1]:
            best = (p, mt)
    return best


def machine_photo(data_dir, max_age=7200):
    """The photo the machine takes when a job completes (Studio keeps it under reportData), if it is recent."""
    hit = newest_file(os.path.join(data_dir, "atomm", "*", "*", "*", "reportData", "*", "processComplete.jpg"))
    return hit if hit and time.time() - hit[1] <= max_age else None


def camera_photo(data_dir):
    """The newest picture of the overview camera (what Studio shows on the canvas)."""
    return newest_file(os.path.join(data_dir, "atomm", "*", "*", "camera", "global_correct.jpg"))


# ---------------------------------------------------------------- live job (from Studio's logs)
# While it engraves, the machine reports no line number: only process events (ready, start, pausing, pause, resume,
# finish, cancel) and its state, which names the size of the galvo file it runs; Studio's own progress is its time
# estimate.  Here the working time since "start" (pauses taken out) is spread over the layers and feather passes of
# the G-code that was sent, in proportion to their motion lines, and scaled to Studio's estimate (get_time_from_gcode
# in the algo-server log) times what finished jobs took compared with their estimate.
LOG_T = r"\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d)\.(\d+)\]"
RE_EVENT = re.compile(LOG_T + r".*Event report: /v1/platform/device/process', \{ content: \{ action: '(\w+)', taskId: '([^']+)'")
RE_STATE = re.compile(LOG_T + r".*Property report: /v1/platform/device/state'.*galvo_file_size: (\d+).*taskId: '([^']+)'")
RE_SRV_T = re.compile(r"^(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d)\.(\d+) ")
RE_EST = re.compile(r"get_time_from_gcode end: path='[^']*?(atomm[/\\]preview[/\\][^']+?[/\\]gcode[/\\]galvo\.gc)' type=\d+ time=([\d.]+) s")
RE_BLOCK = re.compile(rb"\n# [^\n]* BITMAP START\n")
RE_VLINE = re.compile(rb"\nG200 V(-?\d+(?:\.\d+)?)F600\n")


def log_time(day_time, frac):
    return time.mktime(time.strptime(day_time, "%Y-%m-%d %H:%M:%S")) + float("0." + frac)


class LogTail:
    """Lines appended to the newest copy of one of Studio's logs (<logs>\\<date>\\<rel>), read a piece at a time."""

    def __init__(self, logs_dir, rel, first_max=8 << 20):
        self.logs_dir, self.rel, self.first_max = logs_dir, rel, first_max
        self.path, self.pos, self.rest = None, 0, b""

    def newest(self):
        try:
            for d in sorted(os.listdir(self.logs_dir), reverse=True):
                p = os.path.join(self.logs_dir, d, self.rel)
                if os.path.isfile(p):
                    return p
        except OSError:
            pass
        return None

    def read(self):
        p = self.newest()
        try:
            size = os.path.getsize(p) if p else -1
        except OSError:
            return []
        if size < 0:
            return []
        if p != self.path or size < self.pos:
            self.path, self.pos, self.rest = p, max(0, size - self.first_max), b""
        if size <= self.pos:
            return []
        try:
            with open(p, "rb") as f:
                f.seek(self.pos)
                data = f.read(size - self.pos)
        except OSError:
            return []
        self.pos += len(data)
        lines = (self.rest + data).split(b"\n")
        self.rest = lines.pop()
        return [ln.decode("utf-8", "replace") for ln in lines]


class JobWatch:
    """The machine's latest relief job, as Studio's logs tell it."""

    def __init__(self, data_dir):
        logs = os.path.join(data_dir, "logs")
        self.data_dir = data_dir
        self.main = LogTail(logs, "main.log")
        self.srv = LogTail(logs, os.path.join("algo-server", "server.log"))
        self.estimates, self.srv_t, self.job = [], None, None

    def poll(self):
        for line in self.srv.read():                      # estimates first: they come before the job starts
            m = RE_SRV_T.match(line)
            if m:
                self.srv_t = log_time(m.group(1), m.group(2))
                continue
            m = RE_EST.search(line)
            if m and self.srv_t:
                self.estimates = (self.estimates + [(self.srv_t, m.group(1).replace("\\", "/"), float(m.group(2)))])[-50:]
        for line in self.main.read():
            m = RE_EVENT.search(line)
            if m:
                self.event(log_time(m.group(1), m.group(2)), m.group(3), m.group(4))
                continue
            m = RE_STATE.search(line)
            if m and self.job and self.job["task"] == m.group(4):
                self.job["size"] = int(m.group(3))
        return self.job

    def event(self, t, action, task):
        if "_RELIEF" not in task:
            return
        j = self.job
        if action in ("ready", "start") and (j is None or j["task"] != task or j["end"]):
            head = re.search(r"_RELIEF_PROCESS_(.+?)_RELIEF", task)
            j = self.job = {"task": task, "head": head.group(1) if head else "", "seen": t, "start": None, "pauses": [],
                            "paused_at": None, "end": None, "end_action": None, "size": None, "gcode": None, "est": None}
        if j is None or j["task"] != task:
            return
        if action == "start" and j["start"] is None:
            j["start"] = t
            for et, rel, sec in reversed(self.estimates):  # Studio's last estimate before the start: the file it sent
                if et <= t:
                    j["gcode"], j["est"] = os.path.join(self.data_dir, *rel.split("/")), sec
                    break
        elif action in ("pausing", "pause") and j["paused_at"] is None and j["start"] is not None and not j["end"]:
            j["paused_at"] = t
        elif action == "resume" and j["paused_at"] is not None:
            j["pauses"].append((j["paused_at"], t))
            j["paused_at"] = None
        elif action in ("finish", "cancel") and not j["end"]:
            if j["paused_at"] is not None:
                j["pauses"].append((j["paused_at"], t))
                j["paused_at"] = None
            j["end"], j["end_action"] = t, action

    @staticmethod
    def working(j, now=None):
        """Seconds of engraving so far, pauses taken out."""
        if not j or j["start"] is None:
            return 0.0
        end = j["end"] or j["paused_at"] or (time.time() if now is None else now)
        return max(0.0, end - j["start"] - sum(b - a for a, b in j["pauses"]))


def gcode_stretches(data, n_layers, lost, every, n_feather):
    """Relief stretches and feather passes of a galvo.gc in machine order, cut at block starts:
    [(kind, first number, last number, start, end)], numbers from 1.  Layer 1 starts at the first block.  The
    slicer writes a V line for layer j+1 (j = 1..n_layers-1) when (j+1) % every == 0, except for the `lost` j (its
    V-line bug; the V values after a lost line are one step too shallow, so they cannot number the layers): the
    k-th V line belongs to the k-th of those layers, and a stretch without a V line inside holds several layers.
    Feather passes are the last n_feather blocks."""
    end = data.rfind(b"\n# END\n")
    starts = [m.start() for m in RE_BLOCK.finditer(data, 0, end)] if end >= 0 else []
    if not starts:
        return []
    relief_end = starts[-n_feather] if 0 < n_feather < len(starts) else end
    vpos = [m.start() for m in RE_VLINE.finditer(data, starts[0], relief_end)]
    lost = set(lost or ())
    with_v = [j + 1 for j in range(1, n_layers) if (j + 1) % every == 0 and j not in lost]
    if len(with_v) != len(vpos):                             # the lost set does not fit: take every line in order
        with_v = list(range(2, 2 + len(vpos)))
    block_of = lambda p: starts[max(0, bisect.bisect_right(starts, p) - 1)]
    marks = [(starts[0], 1)] + [(block_of(p), n) for p, n in zip(vpos, with_v)]
    out = []
    for i, (pos, n) in enumerate(marks):
        nxt, last = (marks[i + 1][0], marks[i + 1][1] - 1) if i + 1 < len(marks) else (relief_end, max(n, n_layers))
        out.append(("layer", n, max(n, last), pos, nxt))
    if 0 < n_feather < len(starts):
        fs = starts[len(starts) - n_feather:] + [end]
        out += [("feather", i + 1, i + 1, fs[i], fs[i + 1]) for i in range(n_feather)]
    return out


class VendorTimer:
    """Studio's own time estimate (js001GetGCodeTimeSync of the path planning plugin, type 2 as Studio uses for
    relief) of a piece of G-code.  The plugin DLL and its runtime DLLs are copied to a folder of ours first, so the
    plugin folder is never held open; the job's manifest.json goes next to the piece (the estimate reads it)."""

    def __init__(self, studio_exe, manifest):
        pd = plugin_dir(studio_exe)
        name = "lib_path_planning_orig.dll" if os.path.isfile(os.path.join(pd, "lib_path_planning_orig.dll")) else "lib_path_planning.dll"
        own = {"relief_fix_core.dll", "lib_path_planning.dll"} if name.endswith("_orig.dll") else {"relief_fix_core.dll"}
        dst = os.path.join(os.path.dirname(settings_path()), "vendor-dll")
        os.makedirs(dst, exist_ok=True)
        for n in os.listdir(pd):
            if n.lower().endswith(".dll") and n.lower() not in own:
                s, d = os.path.join(pd, n), os.path.join(dst, n)
                if not os.path.isfile(d) or os.path.getsize(d) != os.path.getsize(s) or os.path.getmtime(d) < os.path.getmtime(s):
                    shutil.copy2(s, d)
        os.add_dll_directory(dst)
        self.fn = ctypes.CDLL(os.path.join(dst, name)).js001GetGCodeTimeSync
        self.fn.argtypes = [ctypes.POINTER(TimeIn), ctypes.POINTER(TimeOut)]
        self.fn.restype = ctypes.c_int
        self.dir = tempfile.mkdtemp(prefix="relief-live-")
        os.makedirs(os.path.join(self.dir, "gcode"))
        shutil.copyfile(manifest, os.path.join(self.dir, "manifest.json"))
        self.path = os.path.join(self.dir, "gcode", "galvo.gc")

    def __call__(self, blob):
        with open(self.path, "wb") as f:
            f.write(blob)
        out = TimeOut()
        self.fn(ctypes.byref(TimeIn(self.path.encode("utf-8"), 2)), ctypes.byref(out))
        if out.code != 0 or not out.data:
            raise RuntimeError("js001GetGCodeTimeSync: code %d %s" % (out.code, (out.message or b"").decode("utf-8", "replace")))
        return float(json.loads(out.data.decode("utf-8"))["time"])

    def close(self):
        shutil.rmtree(self.dir, ignore_errors=True)


class TimeIn(ctypes.Structure):
    _fields_ = [("file", ctypes.c_char_p), ("type", ctypes.c_int32)]


class TimeOut(ctypes.Structure):
    _fields_ = [("code", ctypes.c_int32), ("message", ctypes.c_char_p), ("data", ctypes.c_char_p)]


def layer_seconds(data, stretches, timer=None):
    """[(kind, number, weight)] for every layer and feather pass.  With a timer: seconds by the vendor's estimate of
    each stretch (header + stretch + end sequence, minus header + end alone; checked on a 128-layer job: within
    0.4 % of the estimate of the whole file at every quarter).  Without: motion lines (G0 / G1 / G200), which give
    the many short dot moves of the feather passes far too much weight."""
    end = data.rfind(b"\n# END\n")
    if timer is not None:
        head, tail = data[:stretches[0][3]], data[end:]
        base = timer(head + tail)
        ws = [max(0.001, timer(head + data[a:b] + tail) - base) for _, _, _, a, b in stretches]
    else:
        ws = [max(1, data.count(b"\nG1", a, b) + data.count(b"\nG0", a, b) + data.count(b"\nG200", a, b)) for _, _, _, a, b in stretches]
    out = []
    for (kind, n0, n1, _, _), w in zip(stretches, ws):
        out += [(kind, n, w / (n1 - n0 + 1)) for n in range(n0, n1 + 1)]
    return out


def slice_tiles(task_dir):
    """The slicer's tiles from manifest_sliced.json: {layer from 1: [(file, x mm, y mm)]}, feather tiles, origin, px/mm."""
    ms = load_json(os.path.join(task_dir, "manifest_sliced.json"))
    layers, feather, xs, ys, ppm = {}, [], [], [], None
    for e in ms.get("elements", []):
        fn, x, y = e.get("filename") or "", e.get("x"), e.get("y")
        if not isinstance(x, (int, float)) or not isinstance(y, (int, float)):
            continue
        if e.get("isCleaningLayer"):
            feather.append((fn, x, y))
            continue
        m = re.search(r"_slice_(\d+)/", fn)
        if m:
            layers.setdefault(int(m.group(1)) + 1, []).append((fn, x, y))
            xs.append(x)
            ys.append(y)
            if ppm is None and e.get("dpi"):
                ppm = float(e["dpi"][0]) / 25.4
    return layers, feather, ((min(xs), min(ys)) if xs else (0.0, 0.0)), ppm or 10.0


def layer_mask(task_dir, tiles, origin, ppm, size, scale):
    """Where one layer burns (black in the slicer's tiles), scaled: mode "L", 255 = burnt."""
    out = Image.new("L", (max(1, int(size[0] * scale)), max(1, int(size[1] * scale))), 0)
    for fn, x, y in tiles:
        try:
            with Image.open(os.path.join(task_dir, *fn.split("/"))) as im:
                t = im.convert("L")
        except (OSError, ValueError):
            continue
        burnt = t.point(lambda v: 255 if v < 128 else 0).resize(
            (max(1, int(round(t.width * scale))), max(1, int(round(t.height * scale)))), Image.BOX)
        out.paste(burnt, (int(round((x - origin[0]) * ppm * scale)), int(round((y - origin[1]) * ppm * scale))))
    return out


def time_ratio(head):
    """Median of (actual working time / Studio's estimate) of the last finished jobs with this laser, else 1."""
    r = [x for x in ((load_settings().get("time_ratio") or {}).get(head) or []) if isinstance(x, (int, float)) and 0.3 <= x <= 5]
    return sorted(r)[len(r) // 2] if r else 1.0


def learn_time_ratio(job_key, head, ratio):
    """Keep one finished job's ratio (once per job, also across restarts of this window)."""
    s = load_settings()
    seen = s.get("time_ratio_jobs") or []
    if job_key in seen or not 0.3 <= ratio <= 5:
        return False
    tr = s.get("time_ratio") or {}
    tr[head] = ((tr.get(head) or [])[-9:]) + [round(ratio, 4)]
    save_settings(time_ratio=tr, time_ratio_jobs=(seen + [job_key])[-50:])
    return True


def num_or_none(s):
    try:
        return float(str(s).strip().replace(",", "."))
    except ValueError:
        return None


def job_text(T, j):
    """Three lines about a stored job (the dialog and the library both show it this way)."""
    sw = j.get("switches") or {}
    num = lambda x, fmt: fmt % x if isinstance(x, (int, float)) else "—"
    head = f"{HEADS.get(j.get('laser'), j.get('laser') or '?')} {j['rated_w']:g} W" if j.get("rated_w") else str(j.get("laser") or "?")
    w, h = ((j.get("size_mm") or []) + [0, 0])[:2]
    return "\n".join([
        T["job_l1"].format(head=head, w=w or 0, h=h or 0, N=j.get("layers", "?"), kept=j.get("layers_engraved", "?"),
                           v=num(j.get("v_per_layer_mm"), "%g")),
        T["job_l2"].format(p=j.get("power_pct") or 0, s=j.get("speed_mm_s") or 0, d=j.get("density_per_cm") or 0,
                           f=j.get("freq_khz") or 0, r=j.get("passes") or 1),
        T["job_l3"].format(e=num(j.get("energy_per_layer_j_cm2"), "%.3g J/cm²"), dp=num(j.get("expected_deepest_mm"), "%.2f mm"),
                           kt=T["on"] if sw.get("keep_top") else T["off"], fp=sw.get("feather_passes", "?"),
                           tr=T["on"] if sw.get("transparent_as_color") else T["off"])])


class RecordDialog:
    """★ Save this job: the job as Studio has it now, plus material, thickness, measured depth, a photo and a note."""

    def __init__(self, app, item, r):
        self.app, self.T, self.item = app, app.T, item
        T = self.T
        self.job = job_record(item, r, app.switch_values())
        self.photo, self.photo_key, self.photo_tk, self.auto_thick = None, None, None, ""
        w = self.win = tk.Toplevel(app.root)
        w.title(T["rec_title"])
        w.transient(app.root)
        w.resizable(False, False)
        f = ttk.Frame(w, padding=12)
        f.pack(fill="both", expand=True)
        ttk.Label(f, text=T["rec_job"], font=("Microsoft YaHei UI", 9, "bold")).grid(row=0, column=0, columnspan=4, sticky="w")
        ttk.Label(f, text=job_text(T, self.job), foreground="#555", justify="left").grid(row=1, column=0, columnspan=4, sticky="w",
                                                                                           pady=(2, 10))
        mats = preset_materials(app.presets)
        self.mat = tk.StringVar(value=mats[0] if mats else "")
        self.thick, self.meas, self.note = tk.StringVar(), tk.StringVar(value=app.meas.get().strip()), tk.StringVar()
        self.mat_box = ttk.Combobox(f, textvariable=self.mat, values=mats + [m for m in MATERIALS[app.lang] if m not in mats], width=26)
        rows = ((T["material"], self.mat_box, None), (T["thickness"], ttk.Entry(f, textvariable=self.thick, width=10), None),
                (T["measured_mm"], ttk.Entry(f, textvariable=self.meas, width=10), T["measured_tip"]),
                (T["note"], ttk.Entry(f, textvariable=self.note, width=46), None))
        for i, (lab, wid, tip) in enumerate(rows):
            ttk.Label(f, text=lab).grid(row=2 + i, column=0, sticky="w", pady=2, padx=(0, 8))
            wid.grid(row=2 + i, column=1, columnspan=3 if tip is None else 1, sticky="w", pady=2)
            if tip:
                ttk.Label(f, text=tip, foreground="#888").grid(row=2 + i, column=2, columnspan=2, sticky="w", padx=(6, 0))
        self.mat_box.bind("<<ComboboxSelected>>", lambda e: self.fill_thickness())
        self.fill_thickness()
        pf = ttk.LabelFrame(f, text=T["photo"], padding=8)
        pf.grid(row=6, column=0, columnspan=4, sticky="we", pady=(10, 0))
        self.ph_view = ttk.Label(pf)
        self.ph_view.grid(row=0, column=0, rowspan=5, sticky="nw", padx=(0, 10))
        for i, (key, cmd) in enumerate((("ph_machine", self.use_machine), ("ph_camera", self.use_camera), ("ph_file", self.use_file),
                                        ("ph_paste", self.use_clipboard), ("ph_none", self.no_photo))):
            ttk.Button(pf, text=T[key], width=14, command=cmd).grid(row=i, column=1, sticky="w", pady=1)
        self.ph_text = ttk.Label(pf, foreground="#888", wraplength=320, justify="left")
        self.ph_text.grid(row=5, column=0, columnspan=2, sticky="w", pady=(4, 0))
        self.msg = ttk.Label(f, foreground="#b00020")
        self.msg.grid(row=7, column=0, columnspan=3, sticky="w", pady=(10, 0))
        bf = ttk.Frame(f)
        bf.grid(row=7, column=3, sticky="e", pady=(10, 0))
        ttk.Button(bf, text=T["cancel"], command=w.destroy).pack(side="right")
        ttk.Button(bf, text=T["save"], command=self.save).pack(side="right", padx=(0, 6))
        self.use_machine(first=True)

    def fill_thickness(self):
        """The thickness last saved for this material, unless something else was typed."""
        if self.thick.get().strip() not in ("", self.auto_thick):
            return
        m = self.mat.get().strip()
        for rec in reversed(self.app.presets):
            if rec.get("material") == m and isinstance(rec.get("thickness_mm"), (int, float)):
                self.auto_thick = f"{rec['thickness_mm']:g}"
                self.thick.set(self.auto_thick)
                return

    def set_photo(self, im, key, text):
        self.photo, self.photo_key = im, key if im is not None else None
        if im is None:
            self.photo_tk = None
            self.ph_view.config(image="")
        else:
            th = im.copy()
            th.thumbnail((320, 180))
            self.photo_tk = ImageTk.PhotoImage(th)
            self.ph_view.config(image=self.photo_tk)
        self.ph_text.config(text=text)

    def load_path(self, path, key, text):
        try:
            with Image.open(path) as im:
                im.load()
                self.set_photo(im.copy(), key, text)
        except (OSError, ValueError) as e:
            self.ph_text.config(text=str(e))

    def use_machine(self, first=False):
        hit = machine_photo(self.app.data_dir)
        if not hit:
            if first:
                self.set_photo(None, None, self.T["ph_no_machine"])
            else:
                self.ph_text.config(text=self.T["ph_no_machine"])
            return
        t = time.strftime("%m-%d %H:%M", time.localtime(hit[1]))
        self.load_path(hit[0], "machine " + time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(hit[1])),
                       self.T["ph_from"].format(src=self.T["ph_machine"], t=t))

    def use_camera(self):
        hit = camera_photo(self.app.data_dir)
        if not hit:
            self.ph_text.config(text=self.T["ph_no_camera"])
            return
        t = time.strftime("%m-%d %H:%M", time.localtime(hit[1]))
        self.load_path(hit[0], "camera " + time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(hit[1])),
                       self.T["ph_from"].format(src=self.T["ph_camera"], t=t))

    def use_file(self):
        p = filedialog.askopenfilename(parent=self.win, filetypes=[("Images", "*.jpg *.jpeg *.png *.bmp *.webp"), ("*", "*.*")])
        if p:
            self.load_path(p, "file " + os.path.basename(p), os.path.basename(p))

    def use_clipboard(self):
        try:
            got = ImageGrab.grabclipboard()
        except Exception:
            got = None
        if isinstance(got, list):                         # files copied in Explorer
            got = next((x for x in got if str(x).lower().endswith((".jpg", ".jpeg", ".png", ".bmp", ".webp"))), None)
            if got:
                self.load_path(got, "file " + os.path.basename(got), os.path.basename(got))
                return
        if isinstance(got, Image.Image):
            self.set_photo(got, "clipboard", self.T["ph_paste"])
        else:
            self.ph_text.config(text=self.T["ph_no_clip"])

    def no_photo(self):
        self.set_photo(None, None, self.T["ph_nothing"])

    def save(self):
        T = self.T
        m = self.mat.get().strip()
        th = num_or_none(self.thick.get())
        ms = self.meas.get().strip()
        md = num_or_none(ms) if ms else None
        if not m:
            self.msg.config(text=T["need_material"])
        elif th is None or th <= 0:
            self.msg.config(text=T["need_thickness"])
        elif ms and (md is None or md < 0):
            self.msg.config(text=T["bad_measured"])
        else:
            try:
                rec = save_preset(m, th, md, self.note.get().strip(), self.job, self.photo, self.photo_key, self.item.get("img"))
            except OSError as e:
                self.msg.config(text=str(e))
                return
            self.win.destroy()
            self.app.preset_saved(rec)


class LibraryWindow:
    """Presets: the list, one record's photo and values, and what the measured ones say about the current job."""
    COLS = ("saved", "material", "thick", "laser", "layers", "power", "speed", "density", "freq", "energy", "measured", "um")

    def __init__(self, app):
        self.app, self.T, self.fit, self.recs, self.imgs = app, app.T, None, [], []
        T = self.T
        w = self.win = tk.Toplevel(app.root)
        w.title(T["lib_title"])
        w.geometry("1000x680")
        top = ttk.Frame(w, padding=(10, 8))
        top.pack(fill="x")
        ttk.Label(top, text=T["material"]).pack(side="left")
        self.mat = tk.StringVar(value=T["all"])
        self.mat_box = ttk.Combobox(top, textvariable=self.mat, state="readonly", width=24)
        self.mat_box.pack(side="left", padx=(6, 0))
        self.mat_box.bind("<<ComboboxSelected>>", lambda e: self.refresh())
        ttk.Button(top, text=T["open_folder"], command=self.open_folder).pack(side="right")
        mid = ttk.Frame(w, padding=(10, 0))
        self.tree = ttk.Treeview(mid, columns=self.COLS, show="headings", height=12, selectmode="browse")
        for c, name, wd in zip(self.COLS, T["lib_cols"], (135, 120, 50, 60, 50, 55, 70, 60, 55, 80, 75, 70)):
            self.tree.heading(c, text=name)
            self.tree.column(c, width=wd, anchor="w" if c in ("saved", "material", "laser") else "e")
        sb = ttk.Scrollbar(mid, orient="vertical", command=self.tree.yview)
        self.tree.configure(yscrollcommand=sb.set)
        self.tree.pack(side="left", fill="both", expand=True)
        sb.pack(side="right", fill="y")
        self.tree.bind("<<TreeviewSelect>>", lambda e: self.show())
        det = ttk.Frame(w, padding=(10, 8))
        self.ph_view = ttk.Label(det)
        self.ph_view.pack(side="left", padx=(0, 10))
        self.rel_view = ttk.Label(det)
        self.rel_view.pack(side="left", padx=(0, 10))
        bf = ttk.Frame(det)
        bf.pack(side="right", anchor="n")
        ttk.Button(bf, text=T["open_photo"], command=self.open_photo).pack(fill="x")
        ttk.Button(bf, text=T["delete"], command=self.delete).pack(fill="x", pady=(4, 0))
        self.det_text = ttk.Label(det, justify="left", wraplength=520, foreground="#333")
        self.det_text.pack(side="left", fill="x", expand=True, anchor="n")
        ff = ttk.LabelFrame(w, text=T["calc"], padding=8)
        # the calculation and the selected record stay at the bottom; a short window shortens the list
        ff.pack(side="bottom", fill="x", padx=10, pady=(0, 10))
        det.pack(side="bottom", fill="x")
        mid.pack(fill="both", expand=True)
        self.fit_text = ttk.Label(ff, justify="left", foreground="#224")
        self.fit_text.pack(anchor="w")
        tf = ttk.Frame(ff)
        tf.pack(anchor="w", pady=(6, 0))
        ttk.Label(tf, text=T["target"]).pack(side="left")
        self.target = tk.StringVar()
        ttk.Entry(tf, textvariable=self.target, width=8).pack(side="left", padx=(6, 0))
        self.target_out = ttk.Label(tf, foreground="#224")
        self.target_out.pack(side="left", padx=(10, 0))
        self.target.trace_add("write", lambda *a: self.show_target())
        self.refresh()

    def refresh(self):
        T = self.T
        self.recs = load_presets()
        self.mat_box["values"] = [T["all"]] + preset_materials(self.recs)
        if self.mat.get() not in self.mat_box["values"]:
            self.mat.set(T["all"])
        m = self.mat.get()
        f = lambda x, fmt: fmt % x if isinstance(x, (int, float)) else ""
        self.tree.delete(*self.tree.get_children())
        for rec in reversed(self.recs):
            if m != T["all"] and rec.get("material") != m:
                continue
            j = rec.get("job") or {}
            self.tree.insert("", "end", iid=rec["id"], values=(
                rec.get("saved", ""), rec.get("material", ""), f(rec.get("thickness_mm"), "%g"), HEADS.get(j.get("laser"), j.get("laser") or ""),
                j.get("layers_engraved", ""), f(j.get("power_pct"), "%g%%"), f(j.get("speed_mm_s"), "%g"), f(j.get("density_per_cm"), "%g"),
                f(j.get("freq_khz"), "%g"), f(j.get("energy_per_layer_j_cm2"), "%.3g"), f(rec.get("measured_mm"), "%.2f"),
                f(rec.get("um_per_layer"), "%.1f")))
        kids = self.tree.get_children()
        if kids:
            self.tree.selection_set(kids[0])
        self.show()
        self.show_fit()

    def selected(self):
        sel = self.tree.selection()
        return next((rec for rec in self.recs if rec["id"] == sel[0]), None) if sel else None

    def show(self):
        T, rec = self.T, self.selected()
        self.imgs = []
        if not rec:
            for lab in (self.ph_view, self.rel_view, self.det_text):
                lab.config(image="", text="")
            return
        for key, lab, size in (("photo", self.ph_view, (240, 135)), ("relief_thumb", self.rel_view, (110, 110))):
            img = None
            if rec.get(key):
                try:
                    with Image.open(os.path.join(presets_dir(), rec[key])) as im:
                        im.thumbnail(size)
                        img = ImageTk.PhotoImage(im.copy())
                except (OSError, ValueError):
                    img = None
            self.imgs.append(img)
            lab.config(image=img if img else "")
        z, md = rec.get("um_per_layer"), rec.get("measured_mm")
        self.det_text.config(text=T["detail"].format(
            m=rec.get("material", ""), t=rec.get("thickness_mm") or 0, md=f"{md:.2f} mm" if isinstance(md, (int, float)) else "—",
            z=f"{z:.1f} µm" if isinstance(z, (int, float)) else "—", job=job_text(T, rec.get("job") or {}), note=rec.get("note") or "—"))

    def show_fit(self):
        T = self.T
        m = self.mat.get()
        item, r = self.app.current()
        self.fit = None
        if m == T["all"]:
            self.fit_text.config(text=T["fit_pick"])
            self.show_target()
            return
        laser = r["head"] if r else next((rec["job"].get("laser") for rec in reversed(self.recs) if rec.get("material") == m and
                                          isinstance(rec.get("um_per_layer"), (int, float)) and rec.get("job")), None)
        hname = HEADS.get(laser, laser or "?")
        self.fit = fit_depth(depth_points(self.recs, m, laser))
        if not self.fit:
            self.fit_text.config(text=T["fit_none"].format(m=m, h=hname))
            self.show_target()
            return
        model = (T["fit_model_line"] if self.fit["kind"] == "line" else T["fit_model_origin"]).format(a=self.fit["a"], b=self.fit["b"])
        lines = [T["fit_line"].format(n=self.fit["n"], h=hname, model=model)]
        if r and r.get("flu"):
            e, L = r["flu"] * r["passes"], r.get("kept") or r["N"]
            z = fit_um(self.fit, e)
            lines.append(T["pred_line"].format(e=e, z=z, L=L, d=z * L / 1000))
        else:
            lines.append(T["pred_nojob"])
        self.fit_text.config(text="\n".join(lines))
        self.show_target()

    def show_target(self):
        T = self.T
        tv = num_or_none(self.target.get())
        item, r = self.app.current()
        if not (self.fit and r and tv and tv > 0):
            self.target_out.config(text="")
            return
        layers, speed = target_plan(self.fit, r, tv)
        if layers and speed:
            txt = T["target_out"].format(L=layers, N=r.get("kept") or r["N"], s=speed)
        elif layers:
            txt = T["target_layers"].format(L=layers)
        else:
            txt = T["target_bad"]
        self.target_out.config(text=txt)

    def open_photo(self):
        rec = self.selected()
        if rec and rec.get("photo") and os.path.isfile(os.path.join(presets_dir(), rec["photo"])):
            os.startfile(os.path.join(presets_dir(), rec["photo"]))

    def open_folder(self):
        os.makedirs(presets_dir(), exist_ok=True)
        os.startfile(presets_dir())

    def delete(self):
        rec = self.selected()
        if rec and messagebox.askyesno(self.T["lib_title"], self.T["confirm_delete"].format(m=rec.get("material", ""), t=rec.get("saved", "")),
                                       parent=self.win):
            delete_preset(rec["id"])
            self.app.presets_changed()


# ---------------------------------------------------------------- window
class Section:
    """A titled box whose body folds away when its title line is clicked (▼ open, ▶ folded).  While folded the title
    line carries a one-line summary; the state is kept in estimator.json."""

    def __init__(self, app, master, key, expanded=True, padding=8):
        self.app, self.key, self.summary = app, key, ""
        self.head = ttk.Frame(master)
        self.arrow = ttk.Label(self.head, width=2)
        self.title = ttk.Label(self.head, font=("Microsoft YaHei UI", 9, "bold"))
        self.hint = ttk.Label(self.head, foreground="#888")
        self.arrow.pack(side="left")
        self.title.pack(side="left")
        self.hint.pack(side="left", padx=(10, 0))
        self.box = ttk.LabelFrame(master, labelwidget=self.head)
        self.body = ttk.Frame(self.box, padding=padding)
        for w in (self.head, self.arrow, self.title, self.hint):
            w.configure(cursor="hand2")
            w.bind("<Button-1>", self.toggle)
        self.open = bool((load_settings().get("folds") or {}).get(key, expanded))
        self.show()

    def show(self):
        self.arrow.config(text="▼" if self.open else "▶")
        self.hint.config(text="" if self.open else self.summary)
        if self.open:
            self.body.pack(fill="both", expand=True)
        else:
            self.body.pack_forget()

    def set_title(self, text):
        self.title.config(text=text)

    def set_summary(self, text):
        self.summary = text if len(text) <= 90 else text[:89] + "…"
        if not self.open:
            self.hint.config(text=self.summary)

    def toggle(self, event=None):
        self.open = not self.open
        self.show()
        folds = load_settings().get("folds") or {}
        folds[self.key] = self.open
        try:
            save_settings(folds=folds)
        except OSError:
            pass                                       # folds this time; opens as before next time
        self.app.fit_window()


class ScrollArea:
    """A vertically scrolling area; the inner frame follows the window's width and the scrollbar only shows when what
    is open is taller than the window.  on_width(px) is told the usable width (for text wrapping)."""

    def __init__(self, master, on_width):
        self.on_width, self.bar_shown, self.last_w = on_width, False, 0
        self.outer = ttk.Frame(master)
        self.canvas = tk.Canvas(self.outer, highlightthickness=0, borderwidth=0)
        self.vsb = ttk.Scrollbar(self.outer, orient="vertical", command=self.canvas.yview)
        self.canvas.configure(yscrollcommand=self.vsb.set)
        self.inner = ttk.Frame(self.canvas)
        self.item = self.canvas.create_window((0, 0), window=self.inner, anchor="nw")
        self.canvas.pack(side="left", fill="both", expand=True)
        self.inner.bind("<Configure>", self.update)
        self.canvas.bind("<Configure>", self.update)

    def update(self, event=None):
        w, h, need = self.canvas.winfo_width(), self.canvas.winfo_height(), self.inner.winfo_reqheight()
        if w > 1 and w != self.last_w:
            self.last_w = w
            self.canvas.itemconfigure(self.item, width=w)
            self.on_width(w)
        self.canvas.configure(scrollregion=(0, 0, max(1, w), need))
        if not self.bar_shown and need > h + 1:
            self.bar_shown = True
            self.vsb.pack(side="right", fill="y", before=self.canvas)
        elif self.bar_shown and need < h - 20:                # some slack, so the bar does not flicker
            self.bar_shown = False
            self.vsb.pack_forget()
            self.canvas.yview_moveto(0)

    def wheel(self, delta):
        if self.bar_shown:
            self.canvas.yview_scroll(-1 if delta > 0 else 1, "units")


class App:
    def __init__(self, args):
        try:
            ctypes.windll.shcore.SetProcessDpiAwareness(1)
        except Exception:
            pass
        self.lang = args.lang or saved_lang() or ui_lang()
        self.T = TEXT[self.lang]
        self.keep_top = max(0.0, float(args.keep_top or 0))
        self.studio_exe = os.path.normcase(os.path.abspath(args.studio_exe)).lower() if args.studio_exe else None
        self.store = Store(args.data_dir)
        self.data_dir = args.data_dir
        self.presets, self.lib_win, self.cur_item = load_presets(), None, None
        self.watch, self.live_shown = JobWatch(args.data_dir), False
        self.live_data, self.live_busy, self.live_img_ready, self.live_img_busy, self.live_img_key, self.live_photo = \
            None, False, None, False, None, None
        self.sig, self.items, self.result, self.photo = None, [], None, None
        self.last_seen, self.last_check, self.placed = time.time(), 0.0, False
        # Studio's main window, which owns this one (attach()), a handle on its process, and whether this window is hidden
        # because Studio hid that window
        self.owner, self.owner_proc, self.hid_with_owner = None, None, False
        self.collapsed, self.strip_w, self.live_short = False, 0, ""

        self.root = tk.Tk()
        self.root.attributes("-toolwindow", True)
        self.root.option_add("*Font", ("Microsoft YaHei UI", 9))
        self.sel, self.meas = tk.StringVar(), tk.StringVar()
        # every fixed text is set by apply_texts(), so the EN / 中文 button can switch them all

        top = ttk.Frame(self.root, padding=(8, 6))
        top.pack(fill="x")
        self.fold_btn = ttk.Button(top, command=lambda: self.set_collapsed(True))
        self.fold_btn.pack(side="right", padx=(8, 0))
        self.lang_btn = ttk.Button(top, width=6, command=self.toggle_lang)
        self.lang_btn.pack(side="right", padx=(8, 0))
        self.lib_btn = ttk.Button(top, command=self.open_library)
        self.lib_btn.pack(side="right", padx=(6, 0))
        self.rec_btn = ttk.Button(top, command=self.open_record)
        self.rec_btn.pack(side="right", padx=(8, 0))
        self.lbl_object = ttk.Label(top)
        self.lbl_object.pack(side="left")
        self.combo = ttk.Combobox(top, textvariable=self.sel, state="readonly")
        self.combo.pack(side="left", fill="x", expand=True)
        self.combo.bind("<<ComboboxSelected>>", lambda e: self.render())

        # switches of the plugin wrapper (relief_wrapper.ini); start from the file, else from the launcher variables
        sw = read_switches(self.studio_exe)
        if self.keep_top <= 0 and sw.get("keep_top", 0) > 0:
            self.keep_top = sw["keep_top"]
        self.sw_keep = tk.BooleanVar(value=sw.get("keep_top", self.keep_top) > 0)
        # boxes clear and 8 feather passes = Studio as shipped; a ticked box or fewer passes = the wrapper's change
        fp = sw.get("feather_passes")
        if fp is None:
            fp = 8 if sw.get("keep_feather", os.environ.get("XTOOL_STRIP_CLEANING") != "1") else 0
        self.sw_feather = tk.IntVar(value=fp)
        self.sw_transp = tk.BooleanVar(value=sw.get("transparent_as_color", True))
        # two rows, so the longer English texts fit the window as well
        sf = ttk.Frame(self.root, padding=(8, 0, 8, 0))
        sf.pack(fill="x")
        self.cb_keep = ttk.Checkbutton(sf, variable=self.sw_keep, command=self.switched)
        self.cb_keep.pack(side="left")
        self.lbl_feather = ttk.Label(sf)
        self.lbl_feather.pack(side="left", padx=(12, 0))
        ttk.Spinbox(sf, from_=0, to=8, increment=1, width=3, textvariable=self.sw_feather, state="readonly", wrap=False,
                    command=self.switched).pack(side="left", padx=(4, 0))
        self.lbl_feather_tip = ttk.Label(sf, foreground="#888")
        self.lbl_feather_tip.pack(side="left", padx=(2, 0))
        sf2 = ttk.Frame(self.root, padding=(8, 2, 8, 4))
        sf2.pack(fill="x")
        self.cb_transp = ttk.Checkbutton(sf2, variable=self.sw_transp, command=self.switched)
        self.cb_transp.pack(side="left")
        self.sw_state = ttk.Label(sf2, text="", foreground="#888")
        self.sw_state.pack(side="left", padx=(12, 0))

        # below the switches: boxes that fold, in an area that scrolls when the window is shorter than what is open;
        # the status line stays at the bottom
        self.status = ttk.Label(self.root, text="", foreground="#888", padding=(8, 2, 8, 6))
        self.status.pack(side="bottom", fill="x")
        self.scroll = ScrollArea(self.root, self.on_width)
        self.scroll.outer.pack(fill="both", expand=True)
        body = self.scroll.inner
        self.root.bind_all("<MouseWheel>", self.on_wheel)
        self.root.bind_class("TCombobox", "<MouseWheel>", self.on_wheel_field)  # the wheel scrolls the window,
        self.root.bind_class("TSpinbox", "<MouseWheel>", self.on_wheel_field)   # it never changes a value

        self.sec_depth = Section(self, body, "depth")
        self.sec_depth.box.pack(fill="x", padx=8)
        df = self.sec_depth.body
        left = ttk.Frame(df)
        left.pack(side="left", fill="both", expand=True)
        self.big = ttk.Label(left, text="", font=("Microsoft YaHei UI", 20, "bold"))
        self.big.pack(anchor="w")
        self.sub = ttk.Label(left, text="", font=("Microsoft YaHei UI", 10))
        self.sub.pack(anchor="w")
        self.rule = ttk.Label(left, text="", foreground="#666", wraplength=430, justify="left")
        self.rule.pack(anchor="w", pady=(4, 6))
        mf = ttk.Frame(left)
        mf.pack(anchor="w")
        self.lbl_measured = ttk.Label(mf)
        self.lbl_measured.pack(side="left")
        ttk.Entry(mf, textvariable=self.meas, width=8).pack(side="left")
        self.mout = ttk.Label(left, text="", foreground="#555")
        self.mout.pack(anchor="w", pady=(2, 0))
        self.meas.trace_add("write", lambda *a: self.show_measured())
        # what the measured presets of one material say about this job
        pr = ttk.Frame(left)
        pr.pack(anchor="w", pady=(8, 0))
        self.lbl_pred = ttk.Label(pr)
        self.lbl_pred.pack(side="left")
        self.pred_mat = tk.StringVar()
        self.pred_box = ttk.Combobox(pr, textvariable=self.pred_mat, state="readonly", width=16)
        self.pred_box.pack(side="left", padx=(4, 0))
        self.pred_box.bind("<<ComboboxSelected>>", lambda e: self.update_prediction(self.result))
        self.pred_out = ttk.Label(left, text="", foreground="#224", wraplength=430, justify="left")
        self.pred_out.pack(anchor="w", pady=(2, 0))
        right = ttk.Frame(df)
        right.pack(side="right")
        self.map_label = ttk.Label(right)
        self.map_label.pack(side="left")
        self.bar = tk.Canvas(right, width=70, height=200, highlightthickness=0)
        self.bar.pack(side="left", padx=(6, 0))

        # what the machine engraves now (shown while a relief job runs, and for two hours after it ended)
        self.sec_live = Section(self, body, "live")
        self.live_img = ttk.Label(self.sec_live.body)
        self.live_img.pack(side="left", padx=(0, 10))
        lr = ttk.Frame(self.sec_live.body)
        lr.pack(side="left", fill="both", expand=True)
        self.live_big = ttk.Label(lr, font=("Microsoft YaHei UI", 14, "bold"))
        self.live_big.pack(anchor="w")
        self.live_time = ttk.Label(lr, foreground="#333")
        self.live_time.pack(anchor="w", pady=(2, 6))
        self.live_bar = tk.Canvas(lr, height=16, highlightthickness=0)
        self.live_bar.pack(fill="x")
        self.live_note = ttk.Label(lr, foreground="#888", wraplength=540, justify="left")
        self.live_note.pack(anchor="w", pady=(6, 0))

        self.sec_params = Section(self, body, "params")
        self.sec_params.box.pack(fill="x", padx=8, pady=(6, 0))
        self.pf = self.sec_params.body
        self.sec_more = Section(self, body, "more", expanded=False)
        self.sec_more.box.pack(fill="x", padx=8, pady=(6, 0))
        self.ef = self.sec_more.body
        self.sec_layers = Section(self, body, "layers", expanded=False, padding=4)
        self.sec_layers.box.pack(fill="x", padx=8, pady=(6, 8))
        tf = self.sec_layers.body
        self.cols = ("layer", "depth", "gray", "area", "path", "time")    # column ids; headings come from TEXT
        self.tree = ttk.Treeview(tf, columns=self.cols, show="headings", height=10)
        for c, w in zip(self.cols, (50, 110, 90, 80, 100, 90)):
            self.tree.column(c, width=w, anchor="e")
        sb = ttk.Scrollbar(tf, orient="vertical", command=self.tree.yview)
        self.tree.configure(yscrollcommand=sb.set)
        self.tree.pack(side="left", fill="both", expand=True)
        sb.pack(side="right", fill="y")
        self.header = (top, sf, sf2)

        # the collapsed window: one line instead of all the above; a click on it or on 展开 / Expand opens it again
        self.strip = ttk.Frame(self.root, padding=(8, 5))
        self.strip_btn = ttk.Button(self.strip, command=lambda: self.set_collapsed(False))
        self.strip_btn.pack(side="right", padx=(12, 0))
        self.strip_depth = ttk.Label(self.strip, font=("Microsoft YaHei UI", 10, "bold"), cursor="hand2")
        self.strip_depth.pack(side="left")
        self.strip_live = ttk.Label(self.strip, cursor="hand2")
        self.strip_live.pack(side="left", padx=(16, 0))
        for w in (self.strip, self.strip_depth, self.strip_live):
            w.bind("<Button-1>", lambda e: self.set_collapsed(False))

        self.apply_texts()
        if load_settings().get("collapsed"):
            self.set_collapsed(True, save=False)
        self.place()
        self.root.after(100, self.tick)
        self.root.after(300, self.follow_studio)
        self.root.after(800, self.live_tick)

    def apply_texts(self):
        """All fixed texts in the current language; the computed ones are redrawn by render()."""
        T = self.T
        self.root.title(T["title"])
        self.fold_btn.config(text=T["fold_btn"])
        self.strip_btn.config(text=T["unfold_btn"])
        self.lang_btn.config(text=T["lang_btn"])
        self.rec_btn.config(text=T["rec_btn"])
        self.lib_btn.config(text=T["lib_btn"])
        self.lbl_pred.config(text=T["lib_pred_label"])
        self.lbl_object.config(text=T["object"])
        self.cb_keep.config(text=T["sw_keep"])
        self.lbl_feather.config(text=T["sw_feather"])
        self.lbl_feather_tip.config(text=T["sw_feather_tip"])
        self.cb_transp.config(text=T["sw_transp"])
        self.lbl_measured.config(text=T["measured"])
        for sec, key in ((self.sec_depth, "depth"), (self.sec_live, "live"), (self.sec_params, "params"), (self.sec_more, "more"),
                         (self.sec_layers, "layers")):
            sec.set_title(T[key])
        for c, name in zip(self.cols, T["cols"]):
            self.tree.heading(c, text=name)
        self.render()
        if self.live_shown:
            self.live_update(poll=False)

    def toggle_lang(self):
        """EN / 中文 button: switch every text now and keep the choice for the next start."""
        self.lang = "en" if self.lang == "zh" else "zh"
        self.T = TEXT[self.lang]
        try:
            save_lang(self.lang)
        except OSError:
            pass                                   # this window still switches; the next start uses the old choice
        self.apply_texts()

    def switched(self):
        if wrapper_installed(self.studio_exe):
            try:
                write_switches(self.studio_exe, (self.keep_top or 5.0) if self.sw_keep.get() else 0.0, self.sw_feather.get(),
                               self.sw_transp.get())
            except OSError as e:
                self.sw_state.config(text=str(e))
                return
        self.render()

    def place(self, hwnd=None):
        """Over Studio's canvas (or near the screen's top left), sized by fit_window()."""
        hwnd = hwnd or studio_window(self.studio_exe)
        self.placed = bool(hwnd)
        if hwnd:
            rc = wt.RECT()
            user32.GetWindowRect(hwnd, ctypes.byref(rc))
            self.avail_h, x, y = rc.bottom - rc.top - 160, rc.left + 40, rc.top + 110
        else:
            self.avail_h, x, y = self.root.winfo_screenheight() - 140, 60, 60
        self.root.geometry(f"+{x}+{y}")
        self.fit_window()

    def fit_window(self):
        """As wide as the header needs (at least 820 px), as tall as what is open, at most the height available;
        the area scrolls when that is not enough.  Collapsed: as big as its one line (it only widens while collapsed,
        so it does not jump with every change of the text)."""
        self.root.update_idletasks()
        if self.collapsed:
            self.strip_w = max(self.strip_w, 300, self.strip.winfo_reqwidth())
            self.root.geometry(f"{min(self.strip_w, self.root.winfo_screenwidth() - 40)}x{self.strip.winfo_reqheight()}")
            return
        need = sum(f.winfo_reqheight() for f in self.header) + self.scroll.inner.winfo_reqheight() + self.status.winfo_reqheight() + 6
        h = max(360, min(need, getattr(self, "avail_h", 900)))
        w = min(max(820, max(f.winfo_reqwidth() for f in self.header) + 20), self.root.winfo_screenwidth() - 40)
        self.fit_size = (w, h, need)
        self.root.geometry(f"{w}x{h}")

    def on_width(self, w):
        """Long lines wrap to the window's width."""
        self.rule.config(wraplength=max(260, w - 340))
        self.pred_out.config(wraplength=max(260, w - 340))
        self.live_note.config(wraplength=max(260, w - 260))

    def on_wheel(self, e):
        """The wheel scrolls the window: not over the per-layer table (it scrolls itself), not in other windows."""
        w = e.widget
        if not isinstance(w, tk.Misc):
            return
        try:
            if w.winfo_toplevel() is not self.root:
                return
        except tk.TclError:
            return
        while w is not None:
            if w is self.tree:
                return
            w = w.master
        self.scroll.wheel(e.delta)

    def on_wheel_field(self, e):
        self.on_wheel(e)
        return "break"                                   # a combobox or spin box never changes its value by the wheel

    def own_hwnd(self):
        """This window's frame window (the one Windows moves, orders and hides), 0 before it exists."""
        try:
            f = self.root.wm_frame()
            return int(f, 16) if f else 0
        except (tk.TclError, ValueError):
            return 0

    def attach(self, hwnd):
        """Make this window one of Studio's own windows (owned by Studio's main window), as Studio's dialogs are: Windows
        then keeps it above Studio but not above other programs, hides it while Studio is minimised and shows it again
        with Studio; it has no taskbar button of its own."""
        own = self.own_hwnd()
        if not own:
            return
        set_window_long_ptr(own, GWLP_HWNDPARENT, hwnd)
        if self.owner_proc:
            kernel32.CloseHandle(self.owner_proc)
        pid = wt.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        self.owner, self.owner_proc = hwnd, kernel32.OpenProcess(SYNCHRONIZE, False, pid.value)
        if user32.GetForegroundWindow() == hwnd:                 # Studio in front: over it now, not at its next click
            set_window_pos(own, HWND_TOP, 0, 0, 0, 0, SWP_ONLY_ORDER)
        if not self.placed:
            self.place(hwnd)

    def keep_owned(self):
        """Own a new frame window too, should Tk make one; and when Studio hides its main window (not minimises it,
        Windows does that part), hide this one too and show it again when Studio's window is back."""
        own = self.own_hwnd()
        if not own:
            return
        if get_window(own, GW_OWNER) != self.owner:
            set_window_long_ptr(own, GWLP_HWNDPARENT, self.owner)
        if is_topmost(own) and not is_topmost(self.owner):
            self.drop_topmost(own)
        hidden = not user32.IsWindowVisible(self.owner)
        if hidden and not self.hid_with_owner:
            self.hid_with_owner = True
            user32.ShowWindow(own, SW_HIDE)
        elif not hidden and self.hid_with_owner and not user32.IsIconic(self.owner):
            self.hid_with_owner = False
            user32.ShowWindow(own, SW_SHOWNOACTIVATE)

    def drop_topmost(self, own):
        """Studio's launch window is always on top (its start-up code calls setAlwaysOnTop), and Windows puts the
        windows it owns on top with it; it leaves that on when this window then belongs to Studio's main window, which
        is not on top.  Take it off, right above Studio: under the window just above Studio, or, when nothing but
        always-on-top windows are above Studio, at the top of the other windows."""
        w = get_window(self.owner, GW_HWNDPREV)
        while w and w == own:
            w = get_window(w, GW_HWNDPREV)
        set_window_pos(own, w if w and not is_topmost(w) else HWND_NOTOPMOST, 0, 0, 0, 0, SWP_ONLY_ORDER)

    def follow_studio(self):
        """Belong to Studio's main window (attach); quit as soon as Studio has quit, or a minute after Studio has no
        window any more."""
        alive = True
        try:
            alive = self.follow_step()
        finally:
            if alive:
                self.root.after(400, self.follow_studio)

    def follow_step(self):
        now = time.time()
        if self.owner and not user32.IsWindow(self.owner):     # Studio closed its main window: it quit, or made a new one
            self.owner, self.last_check = None, 0.0
        if not self.owner and self.owner_proc and kernel32.WaitForSingleObject(self.owner_proc, 0) == 0:
            kernel32.CloseHandle(self.owner_proc)                 # that Studio has quit
            self.owner_proc = None
            if not studio_window(self.studio_exe):
                self.root.destroy()
                return False
        if now - self.last_check > (5 if self.owner else 1):
            self.last_check = now
            hwnd = studio_window(self.studio_exe)
            if hwnd:
                self.last_seen = now
                if hwnd != self.owner:
                    self.attach(hwnd)
            elif now - self.last_seen > 60:
                self.root.destroy()
                return False
        if self.owner:
            self.keep_owned()
        return True

    def set_collapsed(self, on, save=True):
        """收起 / Collapse: the window shrinks to one line, the expected depth and, while a job is shown, the layer the
        machine is on; a click on that line or on 展开 / Expand brings everything back.  Kept for the next start."""
        if on == self.collapsed:
            return
        self.collapsed = on
        if on:
            for f in (*self.header, self.status, self.scroll.outer):
                f.pack_forget()
            self.strip_w = 0
            self.update_strip()
            self.strip.pack(fill="both", expand=True)
        else:
            self.strip.pack_forget()
            for f in self.header:                                 # the order of __init__
                f.pack(fill="x")
            self.status.pack(side="bottom", fill="x")
            self.scroll.outer.pack(fill="both", expand=True)
        if save:
            try:
                save_settings(collapsed=on)
            except OSError:
                pass                                              # collapses this time; opens as before next time
        self.fit_window()
        if not on:
            self.keep_on_screen()

    def update_strip(self):
        """The collapsed window's line, from the depth box's summary and the live box's state."""
        depth = self.sec_depth.summary or self.T["strip_none"]
        live = self.live_short if self.live_shown else ""
        if (depth, live) != (self.strip_depth.cget("text"), self.strip_live.cget("text")):
            self.strip_depth.config(text=depth)
            self.strip_live.config(text=live)
            if self.collapsed:
                self.fit_window()

    def keep_on_screen(self):
        """Opened again where the one line was (say near the bottom of the screen): move it up or left until all of it
        is on the screen."""
        own = self.own_hwnd()
        mi = MONITORINFO()
        mi.cbSize = ctypes.sizeof(mi)
        if not own or not get_monitor_info(monitor_from_window(own, 2), ctypes.byref(mi)):    # 2: nearest monitor
            return
        self.root.update_idletasks()
        rc, wa = wt.RECT(), mi.rcWork
        user32.GetWindowRect(own, ctypes.byref(rc))
        x = max(wa.left, min(rc.left, wa.right - (rc.right - rc.left)))
        y = max(wa.top, min(rc.top, wa.bottom - (rc.bottom - rc.top)))
        if (x, y) != (rc.left, rc.top):
            self.root.geometry(f"+{x}+{y}")

    def tick(self):
        try:
            newest, files = self.store.scan()
            sig = tuple(sorted((k, v[1]) for k, v in newest.items()))
            if sig != self.sig and all(k in newest for k in ("displays", "params", "profiles")):
                items = relief_items(load_json(newest["params"][0]), load_json(newest["displays"][0]),
                                     load_json(newest["profiles"][0]).get("profiles", {}))
                for it in items:
                    dsp = it["display"]
                    m = re.search(r"([0-9a-f]{64})", str(dsp.get("resourcePath", "")))
                    it["img"] = self.store.find_image(m.group(1), (dsp.get("originWidth"), dsp.get("originHeight")), files) if m else None
                self.items, self.sig = items, sig
                self.saved = time.strftime("%H:%M:%S", time.localtime(newest["params"][1] / 1e9))
                names = [f"{it['id'][:8]}   {it['display'].get('width', 0):.1f} × {it['display'].get('height', 0):.1f} mm" for it in items]
                self.combo["values"] = names
                if names and self.sel.get() not in names:
                    self.sel.set(names[0])
                self.render()
        except (ValueError, OSError):
            pass
        self.root.after(1500, self.tick)

    def grid_rows(self, frame, rows, cols):
        for w in frame.winfo_children():
            w.destroy()
        for i, row in enumerate(rows):
            r, c = divmod(i, cols)
            for j, (txt, style) in enumerate(zip(row, ("label", "value", "formula"))):
                fg = {"label": "#555", "value": "#000", "formula": "#888"}[style]
                font = ("Microsoft YaHei UI", 9, "bold") if style == "value" else None
                lab = ttk.Label(frame, text=txt, foreground=fg, font=font) if font else ttk.Label(frame, text=txt, foreground=fg)
                lab.grid(row=r, column=c * len(row) + j, sticky="w", padx=(0, 12), pady=1)

    def render(self):
        T = self.T
        if not self.items:
            self.big.config(text="—")
            self.sub.config(text=T["none"])
            self.cur_item, self.result = None, None
            for sec in (self.sec_depth, self.sec_params, self.sec_more, self.sec_layers):
                sec.set_summary("")
            self.update_prediction(None)
            self.update_strip()
            return
        names = list(self.combo["values"])
        it = self.items[names.index(self.sel.get())] if self.sel.get() in names else self.items[0]
        self.cur_item = it
        wrapped = wrapper_installed(self.studio_exe)
        self.sw_state.config(text=self.T["sw_on"] if wrapped else self.T["sw_none"])
        r = self.result = estimate(it, it.get("img"), (self.keep_top or 5.0) if wrapped and self.sw_keep.get() else 0.0,
                                   wrapped and self.sw_transp.get())
        v = it["values"]

        if r["deepest"] is not None:
            self.big.config(text=f"{T['deepest']} {r['deepest']:.3f} mm")
            sub = []
            if r.get("mean_layers") is not None:
                sub.append(f"{T['mean']} {r['mean_layers'] * r['per_layer']:.3f} mm")
                sub.append(T["deep_share"].format(p=r["deepest_share"] * 100))
            self.sub.config(text="    ".join(sub))
            rule = T["depth_rule"].format(N=r.get("kept", r["N"]), d=r["per_layer"])
            if r.get("k"):
                rule += "\n" + T["keep_rule"].format(k=r["k"], t=100 - r["keep_top_pct"], u=r["untouched"] * 100)
            elif not wrapped:
                rule += "\n" + T["keep_off"]
            elif not self.sw_keep.get():
                rule += "\n" + T["keep_switch_off"]
            self.rule.config(text=rule)
        else:
            self.big.config(text="—")
            self.sub.config(text="")
            self.rule.config(text=T["no_v"])
        self.draw_map(r)
        self.show_measured()

        head = f"{HEADS.get(r['head'], r['head'])} {r['rated']:g} W" if r["rated"] else str(v.get("laser", ""))
        on_off = lambda b: T["on"] if b else T["off"]
        self.grid_rows(self.pf, [
            (T["head"], head), (T["size"], f"{r['W']:.2f} × {r['H']:.2f} mm"), (T["N"], f"{r['N']}"),
            (T["vmove"], on_off(r["vmove"])), (T["every"], f"{r['every']}"), (T["step"], f"{r['step']:g} mm"),
            (T["power"], f"{r['power']:g}%"), (T["speed"], f"{r['speed']:g} mm/s"), (T["density"], f"{r['density']:g} /cm"),
            (T["freq"], f"{r['freq']:g} kHz"), (T["pulse"], f"{v.get('pulseWidth', '')}"), (T["angle"], f"{v.get('processAngle', '')}°"),
            (T["repeat"], f"{r['passes']}"),
        ], cols=3)

        rows = []
        if r["vmove"]:
            rows.append((T["v_travel"], f"{r['v_travel']:.3f} mm", T["v_travel_f"]))
            rows.append((T["res"], f"{r['deepest'] / 256 * 1000:.2f} µm", T["res_f"]))
        if r["line"]:
            rows.append((T["pitch_line"], f"{r['line']:.4f} mm", T["pitch_line_f"]))
        if r.get("pitch"):
            rows.append((T["pitch_pulse"], f"{r['pitch']:.4f} mm", T["pitch_pulse_f"]))
        if r.get("p_out"):
            rows.append((T["p_out"], f"{r['p_out']:.2f} W", T["p_out_f"]))
        if r.get("e_pulse"):
            rows.append((T["e_pulse"], f"{r['e_pulse']:.1f} µJ", T["e_pulse_f"]))
        if r.get("flu"):
            rows.append((T["flu"], f"{r['flu']:.3f} J/cm²", T["flu_f"]))
            rows.append((T["flu_t"], f"{r['flu_t']:.2f} J/cm²", T["flu_t_f"]))
        if r.get("path") is not None:
            rows.append((T["path"], f"{r['path']:,.0f} m", T["path_f"]))
            if r.get("t_on") is not None:
                rows.append((T["t_on"], hms(r["t_on"]), T["t_on_f"]))
        self.grid_rows(self.ef, rows, cols=1)

        self.tree.delete(*self.tree.get_children())
        for n, after, gray, cov, meters, secs, skipped in r["rows"]:
            depth_txt = T["skipped"] if skipped else (f"{after:.3f}" if after is not None else "")
            self.tree.insert("", "end", values=(n, depth_txt, f"≤{gray}" if gray is not None else "",
                                                f"{cov * 100:.1f}", f"{meters:.1f}", f"{secs:.1f}"))
        self.status.config(text=T["status"].format(t=getattr(self, "saved", "?"), id=it["id"]) +
                           ("" if it.get("img") else "　" + T["noimg"]))
        # one line per folded box
        self.sec_depth.set_summary(f"{T['deepest']} {r['deepest']:.3f} mm" if r["deepest"] is not None else "—")
        self.sec_params.set_summary(f"{head} · {r['W']:.1f} × {r['H']:.1f} mm · {T['N']} {r['N']} · {T['power']} {r['power']:g}% · "
                                    f"{T['speed']} {r['speed']:g} mm/s")
        self.sec_more.set_summary(f"{T['t_on']} {hms(r['t_on'])}" if r.get("t_on") is not None else "")
        self.sec_layers.set_summary(T["hint_layers"].format(n=len(r["rows"])) if r["rows"] else "")
        self.update_prediction(r)
        self.update_strip()

    # ------------------------------------------------------------ preset library
    def switch_values(self):
        return {"wrapper": wrapper_installed(self.studio_exe),
                "keep_top": (self.keep_top or 5.0) if self.sw_keep.get() else 0.0,
                "feather_passes": int(self.sw_feather.get()), "transparent_as_color": bool(self.sw_transp.get())}

    def current(self):
        """(item, estimate) of the relief object shown now, or (None, None)."""
        return (self.cur_item, self.result) if self.cur_item is not None and self.result else (None, None)

    def update_prediction(self, r):
        T = self.T
        mats = preset_materials(self.presets, measured_only=True)
        self.pred_box["values"] = mats
        if not mats:
            self.pred_mat.set("")
            self.pred_out.config(text=T["lib_empty"])
            return
        if self.pred_mat.get() not in mats:
            self.pred_mat.set(mats[0])
        if not (r and r.get("flu")):
            self.pred_out.config(text="")
            return
        m = self.pred_mat.get()
        fit = fit_depth(depth_points(self.presets, m, r["head"]))
        if not fit:
            self.pred_out.config(text=T["lib_pred_none"].format(m=m, h=HEADS.get(r["head"], r["head"])))
            return
        e, L = r["flu"] * r["passes"], r.get("kept") or r["N"]
        z = fit_um(fit, e)
        vtxt = T["lib_pred_v"].format(v=r["per_layer"] * 1000) if r.get("per_layer") else ""   # V rise off: no V per layer
        self.pred_out.config(text=T["lib_pred"].format(z=z, vtxt=vtxt, d=z * L / 1000, n=fit["n"]))

    def open_record(self):
        item, r = self.current()
        if item is None:
            self.status.config(text=self.T["no_job"])
            return
        RecordDialog(self, item, r)

    def open_library(self):
        if self.lib_win is not None and self.lib_win.win.winfo_exists():
            self.lib_win.win.deiconify()
            self.lib_win.win.lift()
            return
        self.lib_win = LibraryWindow(self)

    def presets_changed(self):
        self.presets = load_presets()
        self.render()
        if self.lib_win is not None and self.lib_win.win.winfo_exists():
            self.lib_win.refresh()

    def preset_saved(self, rec):
        if isinstance(rec.get("um_per_layer"), (int, float)):
            self.pred_mat.set(rec["material"])
        self.presets_changed()
        self.status.config(text=self.T["saved_ok"].format(m=rec["material"], t=rec["thickness_mm"], n=len(self.presets)))

    # ------------------------------------------------------------ live job
    def live_tick(self):
        try:
            self.live_update()
        except Exception as e:                                   # the live view must never stop the window
            self.live_note.config(text="%s: %s" % (type(e).__name__, e))
        self.root.after(2000, self.live_tick)

    def live_load(self, key, gcode):
        """Worker thread, once per G-code file: its layers / feather passes and the slicer's tiles."""
        try:
            task = os.path.dirname(os.path.dirname(gcode))
            man = load_json(os.path.join(task, "manifest.json"))
            el = [e for e in man.get("elements", []) if e.get("processingType") == "RELIEF"][0]
            tiles = slice_tiles(task)
            n_layers, every = len(tiles[0]), max(1, int(el.get("vLayers") or 1))
            size = (int(el["width"]), int(el["height"]))
            # layers whose V line the slicer drops: the wrapper's record of this job, else the same rule here
            try:
                rec = load_json(os.path.join(task, "relief_fix.json"))
                lost = rec["v_lost_layers"] if rec.get("N_new") == n_layers and isinstance(rec.get("v_lost_layers"), list) else None
            except (OSError, ValueError, KeyError):
                lost = None
            if lost is None:
                dpi = float((el.get("dpi") or [254])[0])
                bpx = int(np.float32((30 if "uv" in str(el.get("laser", "")).lower() else 50) * dpi) / np.float32(25.4))
                lost = v_lost_layers(n_layers, size[0], size[1], bpx, bpx, every)
            with open(gcode, "rb") as f:
                data = f.read()
            stretches = gcode_stretches(data, n_layers, lost, every, len(tiles[1]))
            timer, rough = None, None
            try:                                                 # each layer's time by Studio's own estimator
                timer = VendorTimer(self.studio_exe, os.path.join(task, "manifest.json"))
                spans = layer_seconds(data, stretches, timer)
            except Exception as e:
                spans, rough = layer_seconds(data, stretches), "%s: %s" % (type(e).__name__, e)
            finally:
                if timer is not None:
                    timer.close()
            del data
            scale = min(190 / size[0], 200 / size[1])
            try:
                with Image.open(os.path.join(task, el["filename"])) as im:
                    bg = im.convert("L").resize((max(1, int(size[0] * scale)), max(1, int(size[1] * scale))), Image.BOX)
            except (OSError, ValueError, KeyError):
                bg = None
            self.live_data = {"key": key, "task": task, "spans": spans, "tiles": tiles, "size": size, "scale": scale, "bg": bg,
                              "rough": rough}
        except Exception as e:
            self.live_data = {"key": key, "error": "%s: %s" % (type(e).__name__, e)}
        self.live_busy = False

    def live_image(self, d, kind, n):
        """Worker thread: the picture, dimmed, with what this layer (or feather pass) burns in colour."""
        want = (kind, n, d["key"])
        try:
            layers, feather, origin, ppm = d["tiles"]
            m = layer_mask(d["task"], feather if kind == "feather" else layers.get(n, []), origin, ppm, d["size"], d["scale"])
            base = d["bg"].convert("RGB") if d["bg"] is not None and d["bg"].size == m.size else Image.new("RGB", m.size, (128, 128, 128))
            base = Image.blend(base, Image.new("RGB", m.size, (240, 240, 240)), 0.55)
            col = Image.new("RGB", m.size, (40, 110, 230) if kind == "feather" else (255, 110, 0))
            self.live_img_ready = (want, Image.composite(col, base, m.point(lambda v: 255 if v > 64 else 0)))
        except Exception:
            self.live_img_ready = (want, None)
        self.live_img_busy = False

    def live_update(self, poll=True):
        T = self.T
        j = self.watch.poll() if poll else self.watch.job
        now = time.time()
        if not j or now - (j["end"] or j["start"] or j["seen"]) > 7200:
            if self.live_shown:
                self.sec_live.box.pack_forget()
                self.live_shown = False
                self.fit_window()
                self.update_strip()
            return
        if not self.live_shown:
            self.sec_live.box.pack(fill="x", padx=8, pady=(6, 0), before=self.sec_params.box)
            self.live_shown = True
            self.fit_window()
        el = JobWatch.working(j, now)
        ratio = time_ratio(j["head"]) if j["est"] else 1.0
        total = j["est"] * ratio if j["est"] else None
        notes = [T["live_note"]]
        # the G-code the machine got: layers and passes worked out once, in the background
        d, g, ok_file = None, j["gcode"], False
        if g and os.path.isfile(g):
            st = os.stat(g)
            ok_file = not j["size"] or j["size"] == st.st_size
            key = (g, st.st_size, st.st_mtime_ns)
            if ok_file and self.live_data is not None and self.live_data.get("key") == key:
                d = self.live_data
            elif ok_file and not self.live_busy:
                self.live_busy = True
                threading.Thread(target=self.live_load, args=(key, g), daemon=True).start()
        if j["start"] and not j["est"]:
            notes.insert(0, T["live_noest"])
        elif j["start"] and not ok_file:
            notes.insert(0, T["live_nofile"])
        elif j["start"] and d is None:
            notes.insert(0, T["live_reading"])
        if d and d.get("error"):
            notes.insert(0, d["error"])
        if d and d.get("rough"):
            notes.insert(0, T["live_rough"].format(e=d["rough"][:120]))
        spans = d.get("spans") if d else None
        pos = None
        if spans and total and j["start"] and j["end_action"] != "finish":
            w, acc, pos = sum(s[2] for s in spans), 0.0, len(spans) - 1
            for i, s in enumerate(spans):
                acc += total * s[2] / w
                if el < acc:
                    pos = i
                    break
        state = (T["live_cancel"] if j["end_action"] == "cancel" else T["live_done"]) if j["end"] else \
            T["live_pause"] if j["paused_at"] else T["live_run"] if j["start"] else T["live_ready"]
        if pos is not None:
            kind, n, _ = spans[pos]
            n_l = max((s[1] for s in spans if s[0] == "layer"), default=0)
            n_f = sum(1 for s in spans if s[0] == "feather")
            state += " · " + (T["live_feather"].format(i=n, n=n_f) if kind == "feather" else T["live_layer"].format(i=n, n=n_l)) + T["live_approx"]
        self.live_big.config(text=state)
        self.sec_live.set_summary(state)
        corr = T["live_corr"].format(f=ratio) if abs(ratio - 1) > 0.005 else ""
        if not j["start"]:
            tl = ""
        elif j["end"]:
            tl = T["live_took"].format(el=hms(el), est=hms(j["est"]) if j["est"] else "—")
        elif total and el <= total:
            tl = T["live_time"].format(el=hms(el), rem=hms(total - el), est=hms(j["est"]), corr=corr)
        elif total:
            tl = T["live_over"].format(el=hms(el), over=hms(el - total), est=hms(j["est"]), corr=corr)
        else:
            tl = T["live_time_only"].format(el=hms(el))
        self.live_time.config(text=tl)
        self.live_short = state + (" · " + T["strip_left"].format(rem=hms(total - el))
                                   if j["start"] and not j["end"] and total and el <= total else "")
        self.update_strip()
        self.live_note.config(text="\n".join(notes))
        self.draw_live_bar(spans, pos, j["end_action"] == "finish")
        # this layer's picture, built in the background when the layer changes
        if pos is not None:
            want = (spans[pos][0], spans[pos][1], d["key"])
            if self.live_img_key != want:
                if self.live_img_ready and self.live_img_ready[0] == want:
                    self.live_img_key = want
                    self.live_photo = ImageTk.PhotoImage(self.live_img_ready[1]) if self.live_img_ready[1] is not None else None
                    self.live_img.config(image=self.live_photo if self.live_photo is not None else "")
                elif not self.live_img_busy:
                    self.live_img_busy = True
                    threading.Thread(target=self.live_image, args=(d, want[0], want[1]), daemon=True).start()
        # a finished job tells how Studio's estimate compares with the machine (once per job)
        if j["end_action"] == "finish" and j["est"] and ok_file and el > 60:
            learn_time_ratio("%s@%d" % (j["task"], int(j["start"])), j["head"], el / j["est"])

    def draw_live_bar(self, spans, pos, finished):
        """One cell per layer, then per feather pass: done dark, now orange (blue for feather), to come light."""
        c = self.live_bar
        c.delete("all")
        if not spans:
            return
        width, n = max(100, c.winfo_width() if c.winfo_width() > 1 else 540), len(spans)
        gap = 1 if width / n >= 3 else 0
        for i, (kind, _, _) in enumerate(spans):
            if finished or (pos is not None and i < pos):
                col = "#555555" if kind == "layer" else "#2a4f8f"
            elif pos is not None and i == pos:
                col = "#ff6e00" if kind == "layer" else "#286ee6"
            else:
                col = "#dcdcdc" if kind == "layer" else "#b9cbe8"
            x0 = width * i / n
            c.create_rectangle(x0, 2, max(x0 + 1, width * (i + 1) / n - gap), 14, fill=col, width=0)

    def draw_map(self, r):
        self.bar.delete("all")
        if r.get("hit") is None or not r["N"]:
            self.map_label.config(image="")
            return
        hit = r["hit"]
        hgt = 200
        scale = hgt / hit.shape[0]
        small = np.asarray(Image.fromarray((hit * (255.0 / max(1, r.get("kept", r["N"])))).astype(np.uint8)).resize(
            (max(1, int(hit.shape[1] * scale)), hgt), Image.NEAREST), dtype=np.float32) / 255.0
        self.photo = ImageTk.PhotoImage(Image.fromarray(depth_colors(small)))
        self.map_label.config(image=self.photo)
        for y in range(hgt - 20):
            c = depth_colors(np.array([y / (hgt - 21)]))[0]
            self.bar.create_line(0, y + 10, 14, y + 10, fill="#%02x%02x%02x" % tuple(c))
        if r["deepest"] is not None:
            for f in (0, 0.5, 1):
                y = 10 + f * (hgt - 21)
                self.bar.create_text(18, y, text=f"{r['deepest'] * f:.2f} mm", anchor="w", font=("Microsoft YaHei UI", 8))

    def show_measured(self):
        r = self.result
        txt = ""
        try:
            md = float(self.meas.get()) if self.meas.get().strip() else None
        except ValueError:
            md = None
        if md and r and r["N"] and r.get("per_layer"):
            a = md / (r.get("kept") or r["N"]) * 1000          # the deepest point is engraved by the kept layers only
            b = r["per_layer"] * 1000
            txt = self.T["measured_out"].format(a=a, b=b, c=(a / b - 1) * 100)
        self.mout.config(text=txt)


def main():
    ap = argparse.ArgumentParser(description="Relief depth estimator for xTool Studio")
    ap.add_argument("--data-dir", default=os.path.join(os.environ.get("APPDATA", ""), "xTool Studio"))
    ap.add_argument("--studio-exe", default=None)
    ap.add_argument("--lang", choices=("zh", "en"), default=None)
    ap.add_argument("--keep-top", type=float, default=float(os.environ.get("XTOOL_RELIEF_KEEP_TOP") or 0),
                    help="percent of area kept at the surface: leading layers that each burn >= (100 - P)%% "
                         "of layer 1 are treated as not engraved (default: env XTOOL_RELIEF_KEEP_TOP or 0)")
    args = ap.parse_args()
    kernel32.CreateMutexW(None, False, "Local\\xtool_relief_depth_estimator")
    if kernel32.GetLastError() == 183:
        return
    App(args).root.mainloop()


if __name__ == "__main__":
    main()
