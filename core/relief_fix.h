/* relief_fix.h - xTool Studio 1.10.2 beta (X1) relief keep-top helpers, C port (Windows x64).
 *
 * rf_prepare_task / rf_restore_task (primary): change the generator INPUT before GenJs001ReliefGcodeSync
 *   runs (slicer image remapped through a 256-entry LUT, sliceNumber N -> N-k), and put Studio's original
 *   inputs back afterwards.  Slicing model (verified by the coordinator against the real generator):
 *     layers(g, N) = 0 if g == 255, else 1 + #{ i in [1, N-1] : g <= (256*(N-i))/N }   (g = 8-bit gray)
 * rf_fix_task (secondary / verification): port of strip_cleaning.py plan() + keep_top.py: rewrites
 *   gcode\galvo.gc in place (cleaning passes cut, leading layers dropped, V shifted).
 *
 * All paths are UTF-8.  Return codes: 0 = done, 1 = nothing to change / skipped, < 0 = error (RF_E_*).
 * On 1 or < 0 the task files are left as they were (prepare always rewrites relief_fix.json, see below).
 */
#ifndef RELIEF_FIX_H
#define RELIEF_FIX_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct { double keep_top_pct; int strip_cleaning; int dry_run; int transparent_as_color; int feather_passes; } rf_opts;
/* defaults 5.0, 1, 0, 1, 0.  transparent_as_color (prepare only): pixels that are transparent in Studio's
 * pre-conversion copy of the slicer image get that copy's colour (luma ignoring alpha) instead of Studio's white.
 * feather_passes: 1..7 keeps only the first n of the generator's 8 trailing feather passes (the count is fixed in the
 * generator: 2 rounds x 4 directions); rf_restore_task cuts the others right after the generator returns, and
 * strip_cleaning is ignored.  Any other value: strip_cleaning alone decides (all 8 kept, or none via featherPower = 0).
 * The field sits in what was the struct's tail padding (sizeof stays 24), so a proxy built before it passes 0. */
typedef struct { int k; int n_layers; int cleaning_cut; char shift[32]; long long size_in, size_out;
                 double untouched; double deepest_mm; char note[512]; } rf_result;
/* untouched / deepest_mm are NAN when not applicable (Python None). */

/* 0 = galvo.gc rewritten, 1 = nothing to change, < 0 = error; on 1 or < 0 the file is untouched.
 * Same decisions as `strip_cleaning.py --test` (11-03 08:12 version: keep_top_k with layer_thresholds and the
 * V-line choice avoid_v_loss, plan(task, declared(task))).  Report (only when rewritten): <task>\relief_fix.json,
 * mode "fix" -- or relief_fix_gcode.json
 * when relief_fix.json already holds a prepare record (never clobbered).  dry_run: everything computed, nothing
 * written, returns 0 when a rewrite would happen.  strip_cleaning = 0: cleaning passes are kept and, like Studio's
 * Cleanup switch, keep-top is then not applied while cleaning passes are present. */
int rf_fix_task(const char *task_dir_utf8, const rf_opts *opts, rf_result *res);

/* Called right BEFORE GenJs001ReliefGcodeSync runs on this task folder.  Two independent input changes:
 *   keep-top: slicer image remapped (8-bit gray) and sliceNumber N -> N-k (when choose_k gives k > 0);
 *   cleaning passes off: featherPower of the single RELIEF element -> 0 (strip_cleaning != 0, reliefCleanUp false or
 *   absent, featherPower a number != 0).
 * feather_passes 1..7 changes no input: relief_fix.json gets "feather_keep" (reliefCleanUp false or absent, featherPower
 *   a number != 0), which rf_restore_task acts on.
 * 0 = something changed or a cut after generation is pending, 1 = nothing to do.  Only the two numbers change in
 * manifest.json (verified by re-parsing).
 * res: k (0 if keep-top not applied), n_layers = N (original sliceNumber), size_in / size_out = image bytes before /
 * after, untouched = share of pixels that stay white (NAN without keep-top), deepest_mm, note.  shift is "" (unused).
 * Writes <task>\relief_fix.json (mode "prepare") in every case except dry_run. */
int rf_prepare_task(const char *task_dir_utf8, const rf_opts *opts, rf_result *res);

/* Called right AFTER the generator returns: moves <image>.rf_orig / manifest.json.rf_orig back, then, if this job's
 * prepare record (state "done") holds feather_keep n, keeps only the first n feather passes: gcode\galvo.gc loses the
 * blocks of passes n+1..8 (up to the "# END" line), manifest_sliced.json their isCleaningLayer elements.  The cut only
 * happens when the files are newer than the prepare and every structural check holds (exactly 8 passes, the 8 cleaning
 * elements are the last ones and pass i starts with element i's "head", one galvo file); otherwise both stay as
 * generated and relief_fix.json "feather_trim_note" says why.
 * 0 = restored and / or cut, 1 = nothing to do, < 0 = error (see relief_fix.json "restore_note" / "feather_trim_note"). */
int rf_restore_task(const char *task_dir_utf8);

void rf_opts_init(rf_opts *o);                 /* 5.0, 1, 0, 1, 0 */
const char *rf_strerror(int rc);

#define RF_E_ARG      (-1)    /* bad argument */
#define RF_E_IO       (-2)    /* file system error */
#define RF_E_JSON     (-3)    /* manifest / json unreadable or a value Python could not convert */
#define RF_E_GCODE    (-4)    /* G-code structure the Python reference rejects (ValueError) */
#define RF_E_IMAGE    (-5)    /* slicer image unreadable (rf_fix_task only; prepare skips instead) */
#define RF_E_NOMEM    (-6)
#define RF_E_VERIFY   (-7)    /* internal self-check failed: nothing written */
#define RF_E_STATE    (-8)    /* stale backups that cannot be attributed */
#define RF_E_LUT      (-9)    /* a gray would burn more than one layer off its keep-top count with N-k layers */
#define RF_E_INTERNAL (-10)

#ifdef __cplusplus
}
#endif
#endif
