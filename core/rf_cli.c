/* rf_cli.c - command line front end for relief_fix (pure ASCII).
 *
 *   rf_cli.exe prepare <task_dir> [--pct P] [--dry-run] [--no-strip] [--no-transparent-fix] [--feather-passes N]
 *                                                           before GenJs001ReliefGcodeSync
 *   rf_cli.exe restore <task_dir>                           after it (also cuts the feather passes prepare asked for)
 *   rf_cli.exe <task_dir> [--pct P] [--dry-run] [--no-strip]  rewrite gcode\galvo.gc (strip_cleaning.py port)
 *
 * Exit code: 0 done, 1 nothing to change, 2 + |rc| on error.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "relief_fix.h"

static char *w2u(const wchar_t *w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *s;
    if (n <= 0) return NULL;
    s = malloc((size_t)n);
    if (!s) return NULL;
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
    return s;
}
static double now_s(void)
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}
static void pd(const char *name, double v)
{
    if (isnan(v)) printf("%s=None\n", name);
    else printf("%s=%.6f\n", name, v);
}
static void print_res(int rc, const rf_result *r, double sec)
{
    printf("rc=%d (%s)\n", rc, rf_strerror(rc));
    printf("k=%d\nn_layers=%d\ncleaning_cut=%d\nshift=%s\n", r->k, r->n_layers, r->cleaning_cut, r->shift);
    printf("size_in=%lld\nsize_out=%lld\n", r->size_in, r->size_out);
    pd("untouched", r->untouched);
    pd("deepest_mm", r->deepest_mm);
    printf("note=%s\n", r->note);
    printf("time_s=%.3f\n", sec);
}
static int usage(void)
{
    fprintf(stderr,
            "usage:\n"
            "  rf_cli.exe prepare <task_dir> [--pct P] [--dry-run] [--no-strip] [--no-transparent-fix] [--feather-passes N]\n"
            "  rf_cli.exe restore <task_dir>\n"
            "  rf_cli.exe <task_dir> [--pct P] [--dry-run] [--no-strip]\n");
    return 64;
}
int main(void)
{
    int argc = 0, i, rc, mode = 0, ai;
    wchar_t **wv = CommandLineToArgvW(GetCommandLineW(), &argc);
    char **av;
    const char *task = NULL;
    rf_opts o;
    rf_result r;
    double t0;
    SetConsoleOutputCP(CP_UTF8);
    if (!wv || argc < 2) return usage();
    av = calloc((size_t)argc, sizeof(char *));
    if (!av) return 70;
    for (i = 0; i < argc; i++) av[i] = w2u(wv[i]);
    rf_opts_init(&o);
    ai = 1;
    if (!strcmp(av[1], "prepare")) { mode = 1; ai = 2; }
    else if (!strcmp(av[1], "restore")) { mode = 2; ai = 2; }
    for (i = ai; i < argc; i++) {
        if (!strcmp(av[i], "--pct") && i + 1 < argc) {
            char *end;
            o.keep_top_pct = strtod(av[++i], &end);
            if (*end) return usage();
        } else if (!strcmp(av[i], "--dry-run")) {
            o.dry_run = 1;
        } else if (!strcmp(av[i], "--no-strip")) {
            o.strip_cleaning = 0;
        } else if (!strcmp(av[i], "--no-transparent-fix")) {
            o.transparent_as_color = 0;
        } else if (!strcmp(av[i], "--feather-passes") && i + 1 < argc) {
            char *end;
            long v = strtol(av[++i], &end, 10);
            if (*end || v < 0 || v > 8) return usage();
            /* same mapping as the proxy's relief_wrapper.ini: 0 = none (featherPower 0), 8 = all, 1..7 = cut after */
            o.strip_cleaning = v == 0;
            o.feather_passes = v >= 1 && v <= 7 ? (int)v : 0;
        } else if (av[i][0] == '-' && av[i][1] == '-') {
            return usage();
        } else if (!task) {
            task = av[i];
        } else {
            return usage();
        }
    }
    if (!task) return usage();
    t0 = now_s();
    if (mode == 2) {
        rc = rf_restore_task(task);
        printf("rc=%d (%s)\ntime_s=%.3f\n", rc, rf_strerror(rc), now_s() - t0);
    } else {
        rc = mode == 1 ? rf_prepare_task(task, &o, &r) : rf_fix_task(task, &o, &r);
        print_res(rc, &r, now_s() - t0);
    }
    fflush(stdout);
    return rc >= 0 ? rc : 2 - rc;
}
