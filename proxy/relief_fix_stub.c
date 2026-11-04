/*
 * relief_fix_stub.c - placeholder for the real rf_prepare_task / rf_restore_task.
 * It only logs and changes nothing on disk. To link the real implementation:
 *     build.cmd path\to\relief_fix.c
 */
#include <string.h>

#include "relief_fix.h"
#include "rf_log.h"

int rf_prepare_task(const char *task_dir_utf8, const rf_opts *opts, rf_result *res)
{
    rf_log("stub rf_prepare_task(task_dir=\"%s\", keep_top_pct=%g, strip_cleaning=%d, dry_run=%d): "
           "no changes made",
           task_dir_utf8 ? task_dir_utf8 : "(null)",
           opts ? opts->keep_top_pct : -1.0,
           opts ? opts->strip_cleaning : -1,
           opts ? opts->dry_run : -1);
    if (res) {
        memset(res, 0, sizeof *res);
        strncpy(res->note, "stub: inputs not modified", sizeof res->note - 1);
    }
    return 0;
}

int rf_restore_task(const char *task_dir_utf8)
{
    rf_log("stub rf_restore_task(task_dir=\"%s\"): nothing to restore",
           task_dir_utf8 ? task_dir_utf8 : "(null)");
    return 0;
}
