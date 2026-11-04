/*
 * rf_dynamic.c - hooks for the lib_path_planning.dll proxy that load the relief core
 * (relief_fix_core.dll, next to the proxy) for the duration of one call only.
 *
 * The core is loaded in rf_prepare_task and released after rf_restore_task (or right away
 * when rf_prepare_task reports that nothing was changed), so between two relief jobs no
 * process holds the file: a new core can be dropped in while Studio keeps running.
 * If the core is missing or lacks the two exports, nothing is changed (returns 1).
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string.h>
#include "relief_fix.h"
#include "rf_log.h"

typedef int (*prepare_fn)(const char *, const rf_opts *, rf_result *);
typedef int (*restore_fn)(const char *);

#define RF_CORE_FILE L"relief_fix_core.dll"

static HMODULE g_core;
static restore_fn g_restore;

static int core_path(WCHAR *out, DWORD cap)
{
    HMODULE self = NULL;
    DWORD n, i, cut = 0;

    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)(void *)&core_path, &self))
        return 0;
    n = GetModuleFileNameW(self, out, cap);
    if (n == 0 || n >= cap)
        return 0;
    for (i = 0; i < n; i++)
        if (out[i] == L'\\' || out[i] == L'/')
            cut = i + 1;
    if (cut == 0 || cut + wcslen(RF_CORE_FILE) + 1 > cap)
        return 0;
    wcscpy(out + cut, RF_CORE_FILE);
    return 1;
}

static void release_core(void)
{
    if (g_core)
        FreeLibrary(g_core);
    g_core = NULL;
    g_restore = NULL;
}

int rf_prepare_task(const char *task_dir_utf8, const rf_opts *opts, rf_result *res)
{
    WCHAR path[MAX_PATH * 2];
    prepare_fn prepare;
    int r;

    release_core();                                  /* a previous call that never reached restore */
    if (!core_path(path, (DWORD)(sizeof path / sizeof path[0]))) {
        rf_log("core: own path not available, nothing changed");
        return 1;
    }
    g_core = LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!g_core) {
        rf_log("core: relief_fix_core.dll not loaded (error %lu), nothing changed", (unsigned long)GetLastError());
        return 1;
    }
    prepare = (prepare_fn)(void *)GetProcAddress(g_core, "rf_prepare_task");
    g_restore = (restore_fn)(void *)GetProcAddress(g_core, "rf_restore_task");
    if (!prepare || !g_restore) {
        rf_log("core: rf_prepare_task / rf_restore_task not exported, nothing changed");
        release_core();
        return 1;
    }
    r = prepare(task_dir_utf8, opts, res);
    if (r != 0)
        release_core();                              /* the proxy skips restore: nothing to put back */
    return r;
}

int rf_restore_task(const char *task_dir_utf8)
{
    int r = 1;

    if (g_restore)
        r = g_restore(task_dir_utf8);
    else
        rf_log("core: restore called without a loaded core");
    release_core();
    return r;
}
