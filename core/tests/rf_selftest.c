/* rf_selftest.c - exercises relief_fix.c internals for differential tests against Python (pure ASCII).
 *   md5 <file> | hist <png> | json <file> | pyconv <json file> | deflate <in> <out> | layers | repr <x>... | join <a> <b>
 */
#include "../relief_fix.c"
#include <shellapi.h>

static char *w2u8(const wchar_t *w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *s = malloc((size_t)(n > 0 ? n : 1));
    if (s) WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
    return s;
}
static int load(const char *p, buf_t *b)
{
    DWORD we;
    if (read_all(p, b, &we) != 0) {
        printf("ERR read %s %lu\n", p, we);
        return 0;
    }
    return 1;
}
typedef struct { buf_t *b; int ok; char err[200]; } deep_arg;
static DWORD WINAPI deep_thread(LPVOID p)
{
    deep_arg *a = p;
    arena_t ar = {0};
    a->ok = json_parse(a->b->p, a->b->n, &ar, a->err, sizeof a->err) != NULL;
    arena_free(&ar);
    return 0;
}
static void put_str(const char *s, size_t n)
{
    buf_t o = {0};
    jw_str(&o, s, n);
    fwrite(o.p, 1, o.n, stdout);
    buf_free(&o);
}
int main(void)
{
    int argc = 0, i;
    wchar_t **wv = CommandLineToArgvW(GetCommandLineW(), &argc);
    char **av = calloc((size_t)argc + 1, sizeof(char *));
    buf_t b = {0};
    for (i = 0; i < argc; i++) av[i] = w2u8(wv[i]);
    SetConsoleOutputCP(CP_UTF8);
    if (argc < 2) return 2;
    if (!strcmp(av[1], "md5") && argc == 3) {
        char h[33];
        if (!load(av[2], &b)) return 1;
        md5_hex(b.p, b.n, h);
        printf("%s\n", h);
        return 0;
    }
    if (!strcmp(av[1], "hist") && argc == 3) {
        pngimg p;
        memset(&p, 0, sizeof p);
        if (!load(av[2], &b)) return 1;
        if (png_decode(b.p, b.n, &p, 0) != 0) {
            printf("ERR %s\n", p.err);
            return 0;
        }
        printf("OK %u %u %d %d", p.w, p.h, p.bd, p.ct);
        for (i = 0; i < 256; i++) printf(" %llu", (unsigned long long)p.hist[i]);
        printf("\n");
        return 0;
    }
    if (!strcmp(av[1], "json") && argc == 3) {
        arena_t ar = {0};
        char err[200];
        if (!load(av[2], &b)) return 1;
        printf(json_parse(b.p, b.n, &ar, err, sizeof err) ? "OK\n" : "ERR %s\n", err);
        return 0;
    }
    if (!strcmp(av[1], "pyconv") && argc == 3) {
        /* the file holds a JSON array; for each element print truthy | int() | float() | str() */
        arena_t ar = {0};
        char err[200], t[64];
        jval *v;
        size_t k;
        if (!load(av[2], &b)) return 1;
        v = json_parse(b.p, b.n, &ar, err, sizeof err);
        if (!v || v->t != J_ARR) {
            printf("ERR %s\n", err);
            return 0;
        }
        for (k = 0; k < v->u.arr.n; k++) {
            const jval *x = v->u.arr.v[k];
            pyint pi;
            double f;
            buf_t s = {0};
            printf("%d\t", py_truthy(x));
            if (py_int(&ar, x, &pi, err, sizeof err) == 0) printf("%s%.*s\t", pi.neg ? "-" : "", (int)pi.n, pi.d);
            else printf("E\t");
            if (py_float(x, &f, err, sizeof err) == 0) {
                py_float_repr(f, t, sizeof t);
                printf("%s\t", t);
            } else printf("E\t");
            if (py_str(x, &s)) put_str((const char *)s.p, s.n);
            else printf("U");
            printf("\n");
            buf_free(&s);
        }
        return 0;
    }
    if (!strcmp(av[1], "deflate") && argc == 4) {
        buf_t z = {0};
        DWORD we;
        if (!load(av[2], &b)) return 1;
        if (zlib_compress_fixed(b.p, b.n, &z) != 0) return 1;
        if (write_replace(av[3], z.p, z.n, (char[200]){0}, 200) != 0) return 1;
        (void)we;
        printf("OK %zu -> %zu\n", b.n, z.n);
        return 0;
    }
    if (!strcmp(av[1], "layers")) {
        long long n, bad = 0;
        int g;
        for (n = 1; n <= 700; n++)
            for (g = 0; g < 256; g++) {
                long long c = 0, i2;
                if (g != 255)
                    for (i2 = 1; i2 < n; i2++)
                        if (g <= (256 * (n - i2)) / n) c++;
                if ((g == 255 ? 0 : 1 + c) != gen_layers(g, n)) bad++;
            }
        printf("layers closed form vs loop, N=1..700: %lld mismatches\n", bad);
        return 0;
    }
    if (!strcmp(av[1], "repr")) {
        char t[64];
        for (i = 2; i < argc; i++) {
            double x = __mingw_strtod(av[i], NULL);
            py_float_repr(x, t, sizeof t);
            printf("%s\n", t);
        }
        return 0;
    }
    if (!strcmp(av[1], "deepstack") && argc == 4) {
        /* parse a JSON file on a thread whose stack RESERVE is argv[3] KB */
        deep_arg a;
        HANDLE th;
        SIZE_T kb = (SIZE_T)strtoul(av[3], NULL, 10);
        memset(&a, 0, sizeof a);
        if (!load(av[2], &b)) return 1;
        a.b = &b;
        th = CreateThread(NULL, kb * 1024, deep_thread, &a, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
        if (!th) return 1;
        WaitForSingleObject(th, INFINITE);
        CloseHandle(th);
        printf(a.ok ? "OK (stack %s KB)\n" : "ERR %s (stack %s KB)\n", a.ok ? av[3] : a.err, av[3]);
        return 0;
    }
    if (!strcmp(av[1], "join") && argc == 4) {
        char *j = py_join(av[2], av[3]);
        printf("%s\n", j ? j : "(null)");
        return 0;
    }
    return 2;
}
