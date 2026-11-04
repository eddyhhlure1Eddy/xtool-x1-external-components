/*
 * relief_fix.h - hooks called by the lib_path_planning.dll proxy around the original
 * GenJs001ReliefGcodeSync. The real implementation is written separately; the proxy
 * links relief_fix_stub.c until then (build.cmd <hooks.c> links another file).
 *
 * Call order inside the proxy's GenJs001ReliefGcodeSync(in, out):
 *   1. task folder found from the input struct (fallback: newest manifest.json)
 *   2. rf_prepare_task(task_dir, &opts, &res)   rewrite Studio's inputs for this job
 *   3. ret = original GenJs001ReliefGcodeSync(in, out)
 *   4. rf_restore_task(task_dir)                only if rf_prepare_task returned 0
 *   5. return ret unchanged (the generated G-code is only touched when feather_passes is 1..7:
 *      rf_restore_task then cuts the feather passes after the first n before Studio reads the file)
 * If no task folder is found, or in/out is NULL, no hook is called.
 * If rf_prepare_task returns != 0 the original still runs and rf_restore_task is skipped.
 * Both hooks run under an SEH guard; if rf_prepare_task raises an exception the proxy
 * calls rf_restore_task before running the original.
 *
 * task_dir_utf8: absolute path, backslashes, no trailing separator, UTF-8.
 * opts: keep_top_pct from env XTOOL_RELIEF_KEEP_TOP (default 5, 0 = off),
 *       strip_cleaning from env XTOOL_STRIP_CLEANING ("0" = 0, anything else or unset = 1), dry_run = 0,
 *       transparent_as_color = 1, feather_passes = 0; relief_wrapper.ini overrides them (see read_switches).
 *       feather_passes sits in what was the struct's tail padding: sizeof(rf_opts) is 24 either way.
 * res: zero-filled by the proxy before the call and logged after it.
 */
#ifndef RELIEF_FIX_H
#define RELIEF_FIX_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct { double keep_top_pct; int strip_cleaning; int dry_run; int transparent_as_color; int feather_passes; } rf_opts;
typedef struct { int k; int n_layers; int cleaning_cut; char shift[32]; long long size_in, size_out;
                 double untouched; double deepest_mm; char note[512]; } rf_result;

int rf_prepare_task(const char *task_dir_utf8, const rf_opts *opts, rf_result *res);
int rf_restore_task(const char *task_dir_utf8);

#ifdef __cplusplus
}
#endif

#endif /* RELIEF_FIX_H */
