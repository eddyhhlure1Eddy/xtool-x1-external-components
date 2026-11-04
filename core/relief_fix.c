/* relief_fix.c - xTool Studio relief keep-top helpers (see relief_fix.h).  Windows x64, MinGW-w64 gcc.
 *
 * Sections: text/buffer helpers, MD5/CRC/Adler, UTF-8 + Python ntpath.join, file helpers,
 * JSON parser with Python json semantics (+ Python int()/float()/str()/truth of the values),
 * PNG decoder emulating Pillow 12 + zlib, PNG encoder (8-bit gray), keep-top model, prepare/restore,
 * G-code port of strip_cleaning.py / keep_top.py (rf_fix_task).
 * This file is pure ASCII.
 */
#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <float.h>
#include <errno.h>
#include <limits.h>
#include "relief_fix.h"

/* feather_passes took the old tail padding: proxies built before it still hand over 24 zero-filled bytes */
_Static_assert(sizeof(rf_opts) == 24, "rf_opts must stay 24 bytes");

#define ARRLEN(a) (sizeof(a) / sizeof((a)[0]))

/* ================================================================== text helpers */
static void vappend(char *dst, size_t cap, const char *fmt, va_list ap)
{
    size_t n;
    if (!cap) return;
    n = strnlen(dst, cap);
    if (n + 1 >= cap) return;
    vsnprintf(dst + n, cap - n, fmt, ap);
}
static void sappend(char *dst, size_t cap, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vappend(dst, cap, fmt, ap);
    va_end(ap);
}
static void sset(char *dst, size_t cap, const char *fmt, ...)
{
    va_list ap;
    if (!cap) return;
    dst[0] = 0;
    va_start(ap, fmt);
    vappend(dst, cap, fmt, ap);
    va_end(ap);
}
/* append "; " + text when dst is not empty */
static void note_add(char *dst, size_t cap, const char *fmt, ...)
{
    va_list ap;
    if (!cap) return;
    if (dst[0]) sappend(dst, cap, "; ");
    va_start(ap, fmt);
    vappend(dst, cap, fmt, ap);
    va_end(ap);
}

/* ================================================================== growable byte buffer */
typedef struct { unsigned char *p; size_t n, cap; int oom; } buf_t;

static int buf_grow(buf_t *b, size_t need)
{
    size_t nc;
    void *q;
    if (b->oom) return 0;
    if (need <= b->cap) return 1;
    nc = b->cap ? b->cap : 256;
    while (nc < need) {
        if (nc > SIZE_MAX / 2) { b->oom = 1; return 0; }
        nc *= 2;
    }
    q = realloc(b->p, nc);
    if (!q) { b->oom = 1; return 0; }
    b->p = q;
    b->cap = nc;
    return 1;
}
static int buf_put(buf_t *b, const void *s, size_t n)
{
    if (n > SIZE_MAX - b->n - 1 || !buf_grow(b, b->n + n + 1)) { b->oom = 1; return 0; }
    if (n) memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
    return 1;
}
static int buf_putc(buf_t *b, int c)
{
    unsigned char ch = (unsigned char)c;
    return buf_put(b, &ch, 1);
}
static int buf_puts(buf_t *b, const char *s) { return buf_put(b, s, strlen(s)); }
static int buf_printf(buf_t *b, const char *fmt, ...)
{
    char tmp[512];
    char *big;
    int n, r;
    va_list ap;
    va_start(ap, fmt);
    n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0) { b->oom = 1; return 0; }
    if ((size_t)n < sizeof tmp) return buf_put(b, tmp, (size_t)n);
    big = malloc((size_t)n + 1);
    if (!big) { b->oom = 1; return 0; }
    va_start(ap, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    va_end(ap);
    r = buf_put(b, big, (size_t)n);
    free(big);
    return r;
}
static void buf_free(buf_t *b)
{
    free(b->p);
    memset(b, 0, sizeof *b);
}

/* ================================================================== arena (JSON trees) */
typedef struct ablk { struct ablk *next; size_t used, cap; } ablk;
typedef struct { ablk *head; int oom; } arena_t;
#define AHDR ((sizeof(ablk) + 15) & ~(size_t)15)

static void *arena_alloc(arena_t *a, size_t n)
{
    ablk *b;
    n = (n + 15) & ~(size_t)15;
    if (n == 0) n = 16;
    if (!a->head || a->head->cap - a->head->used < n) {
        size_t cap = n > 65536 ? n : 65536;
        if (cap > SIZE_MAX - AHDR) { a->oom = 1; return NULL; }
        b = malloc(AHDR + cap);
        if (!b) { a->oom = 1; return NULL; }
        b->next = a->head;
        b->used = 0;
        b->cap = cap;
        a->head = b;
    }
    b = a->head;
    b->used += n;
    return (unsigned char *)b + AHDR + b->used - n;
}
static void arena_free(arena_t *a)
{
    while (a->head) {
        ablk *n = a->head->next;
        free(a->head);
        a->head = n;
    }
    a->oom = 0;
}

/* ================================================================== MD5 / CRC32 / Adler-32 */
typedef struct { uint32_t s[4]; uint64_t len; unsigned char buf[64]; size_t bn; } md5_ctx;
static const uint32_t MD5K[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
static const unsigned char MD5R[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                                       5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                                       4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                                       6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};

static void md5_block(uint32_t st[4], const unsigned char *p)
{
    uint32_t M[16], a = st[0], b = st[1], c = st[2], d = st[3];
    int i;
    for (i = 0; i < 16; i++)
        M[i] = (uint32_t)p[4 * i] | ((uint32_t)p[4 * i + 1] << 8) | ((uint32_t)p[4 * i + 2] << 16) |
               ((uint32_t)p[4 * i + 3] << 24);
    for (i = 0; i < 64; i++) {
        uint32_t f, t, x;
        int g;
        if (i < 16) { f = (b & c) | (~b & d); g = i; }
        else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) & 15; }
        else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) & 15; }
        else { f = c ^ (b | ~d); g = (7 * i) & 15; }
        t = d;
        d = c;
        c = b;
        x = a + f + MD5K[i] + M[g];
        b = b + ((x << MD5R[i]) | (x >> (32 - MD5R[i])));
        a = t;
    }
    st[0] += a;
    st[1] += b;
    st[2] += c;
    st[3] += d;
}
static void md5_init(md5_ctx *m)
{
    m->s[0] = 0x67452301;
    m->s[1] = 0xefcdab89;
    m->s[2] = 0x98badcfe;
    m->s[3] = 0x10325476;
    m->len = 0;
    m->bn = 0;
}
static void md5_update(md5_ctx *m, const void *data, size_t n)
{
    const unsigned char *p = data;
    m->len += n;
    if (m->bn) {
        size_t t = 64 - m->bn;
        if (t > n) t = n;
        memcpy(m->buf + m->bn, p, t);
        m->bn += t;
        p += t;
        n -= t;
        if (m->bn == 64) {
            md5_block(m->s, m->buf);
            m->bn = 0;
        }
    }
    while (n >= 64) {
        md5_block(m->s, p);
        p += 64;
        n -= 64;
    }
    if (n) {
        memcpy(m->buf, p, n);
        m->bn = n;
    }
}
static void md5_final(md5_ctx *m, unsigned char out[16])
{
    uint64_t bits = m->len * 8;
    unsigned char pad = 0x80, z = 0, L[8];
    int i, j;
    md5_update(m, &pad, 1);
    while (m->bn != 56) md5_update(m, &z, 1);
    for (i = 0; i < 8; i++) L[i] = (unsigned char)(bits >> (8 * i));
    md5_update(m, L, 8);
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++) out[4 * i + j] = (unsigned char)(m->s[i] >> (8 * j));
}
static void md5_hex(const void *data, size_t n, char hex[33])
{
    static const char H[] = "0123456789abcdef";
    md5_ctx m;
    unsigned char d[16];
    int i;
    md5_init(&m);
    md5_update(&m, data, n);
    md5_final(&m, d);
    for (i = 0; i < 16; i++) {
        hex[2 * i] = H[d[i] >> 4];
        hex[2 * i + 1] = H[d[i] & 15];
    }
    hex[32] = 0;
}

static uint32_t CRCT[256];
static INIT_ONCE crc_once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK crc_build(PINIT_ONCE o, PVOID p, PVOID *c)
{
    uint32_t i, k, v;
    (void)o;
    (void)p;
    (void)c;
    for (i = 0; i < 256; i++) {
        v = i;
        for (k = 0; k < 8; k++) v = (v & 1) ? 0xEDB88320u ^ (v >> 1) : v >> 1;
        CRCT[i] = v;
    }
    return TRUE;
}
static uint32_t crc32_update(uint32_t crc, const unsigned char *p, size_t n)
{
    InitOnceExecuteOnce(&crc_once, crc_build, NULL, NULL);
    crc = ~crc;
    while (n--) crc = CRCT[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}
static uint32_t adler32(const unsigned char *p, size_t n)
{
    uint32_t a = 1, b = 0;
    while (n) {
        size_t k = n < 5552 ? n : 5552;
        n -= k;
        while (k--) {
            a += *p++;
            b += a;
        }
        a %= 65521;
        b %= 65521;
    }
    return (b << 16) | a;
}
static uint32_t be32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static void put_be32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

/* ================================================================== UTF-8 / WTF-8 */
/* decode one code point; allow_sur: accept encoded surrogates (WTF-8).  Returns length, 0 if invalid. */
static int u8_decode(const unsigned char *s, size_t n, uint32_t *cp, int allow_sur)
{
    unsigned c;
    uint32_t v;
    if (n == 0) return 0;
    c = s[0];
    if (c < 0x80) { *cp = c; return 1; }
    if (c < 0xC2) return 0;
    if (c < 0xE0) {
        if (n < 2 || (s[1] & 0xC0) != 0x80) return 0;
        *cp = ((uint32_t)(c & 0x1F) << 6) | (s[1] & 0x3F);
        return 2;
    }
    if (c < 0xF0) {
        if (n < 3 || (s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80) return 0;
        v = ((uint32_t)(c & 0x0F) << 12) | ((uint32_t)(s[1] & 0x3F) << 6) | (s[2] & 0x3F);
        if (v < 0x800) return 0;
        if (!allow_sur && v >= 0xD800 && v <= 0xDFFF) return 0;
        *cp = v;
        return 3;
    }
    if (c < 0xF5) {
        if (n < 4 || (s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80 || (s[3] & 0xC0) != 0x80) return 0;
        v = ((uint32_t)(c & 0x07) << 18) | ((uint32_t)(s[1] & 0x3F) << 12) | ((uint32_t)(s[2] & 0x3F) << 6) |
            (s[3] & 0x3F);
        if (v < 0x10000 || v > 0x10FFFF) return 0;
        *cp = v;
        return 4;
    }
    return 0;
}
static int u8_encode(uint32_t cp, unsigned char *o)
{
    if (cp < 0x80) { o[0] = (unsigned char)cp; return 1; }
    if (cp < 0x800) {
        o[0] = (unsigned char)(0xC0 | (cp >> 6));
        o[1] = (unsigned char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        o[0] = (unsigned char)(0xE0 | (cp >> 12));
        o[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
        o[2] = (unsigned char)(0x80 | (cp & 0x3F));
        return 3;
    }
    o[0] = (unsigned char)(0xF0 | (cp >> 18));
    o[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
    o[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
    o[3] = (unsigned char)(0x80 | (cp & 0x3F));
    return 4;
}
static int utf8_strict_ok(const unsigned char *s, size_t n)
{
    size_t i = 0;
    uint32_t cp;
    while (i < n) {
        int k;
        if (s[i] < 0x80) { i++; continue; }
        k = u8_decode(s + i, n - i, &cp, 0);
        if (!k) return 0;
        i += (size_t)k;
    }
    return 1;
}
/* WTF-8 -> NUL-terminated UTF-16 (malloc).  NULL if invalid or containing U+0000. */
static wchar_t *wtf8_to_wide(const char *s, size_t n)
{
    wchar_t *w = malloc((n + 1) * sizeof(wchar_t));
    size_t i = 0, k = 0;
    if (!w) return NULL;
    while (i < n) {
        uint32_t cp;
        int l = u8_decode((const unsigned char *)s + i, n - i, &cp, 1);
        if (!l || cp == 0) { free(w); return NULL; }
        i += (size_t)l;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            w[k++] = (wchar_t)(0xD800 + (cp >> 10));
            w[k++] = (wchar_t)(0xDC00 + (cp & 0x3FF));
        } else {
            w[k++] = (wchar_t)cp;
        }
    }
    w[k] = 0;
    return w;
}

/* ================================================================== paths (Python 3.13 ntpath.join) */
static char *str_dup_n(const char *s, size_t n)
{
    char *r = malloc(n + 1);
    if (!r) return NULL;
    memcpy(r, s, n);
    r[n] = 0;
    return r;
}
static char *str_cat(const char *a, const char *b)
{
    size_t na = strlen(a), nb = strlen(b);
    char *r = malloc(na + nb + 1);
    if (!r) return NULL;
    memcpy(r, a, na);
    memcpy(r + na, b, nb + 1);
    return r;
}
static int is_sep(int c) { return c == '\\' || c == '/'; }

/* ntpath.splitroot -> drive length, root length (bytes) */
static void py_splitroot(const char *p, size_t n, size_t *dl, size_t *rl)
{
    *dl = 0;
    *rl = 0;
    if (n >= 1 && is_sep((unsigned char)p[0])) {
        if (n >= 2 && is_sep((unsigned char)p[1])) {
            size_t start = 2, i, idx, idx2;
            if (n >= 8 && is_sep((unsigned char)p[3]) && p[2] == '?' && (p[4] == 'U' || p[4] == 'u') &&
                (p[5] == 'N' || p[5] == 'n') && (p[6] == 'C' || p[6] == 'c') && is_sep((unsigned char)p[7]))
                start = 8;
            idx = n;
            for (i = start; i < n; i++)
                if (is_sep((unsigned char)p[i])) { idx = i; break; }
            if (idx == n) { *dl = n; return; }
            idx2 = n;
            for (i = idx + 1; i < n; i++)
                if (is_sep((unsigned char)p[i])) { idx2 = i; break; }
            if (idx2 == n) { *dl = n; return; }
            *dl = idx2;
            *rl = 1;
            return;
        }
        *rl = 1;
        return;
    }
    if (n >= 1) {
        uint32_t cp;
        int l = u8_decode((const unsigned char *)p, n, &cp, 1);
        if (l < 1) l = 1;
        if ((size_t)l < n && p[l] == ':') {
            *dl = (size_t)l + 1;
            if ((size_t)l + 1 < n && is_sep((unsigned char)p[l + 1])) *rl = 1;
        }
    }
}
static int ascii_ieq(const char *a, size_t na, const char *b, size_t nb)
{
    size_t i;
    if (na != nb) return 0;
    for (i = 0; i < na; i++) {
        int x = (unsigned char)a[i], y = (unsigned char)b[i];
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
    }
    return 1;
}
/* os.path.join(a, b) on Windows (Python 3.13); malloc'd */
static char *py_join(const char *a, const char *b)
{
    size_t na = strlen(a), nb = strlen(b), ad, ar, bd, br;
    const char *rd, *rr, *rp;
    size_t nrd, nrr, nrp;
    buf_t o = {0};
    int append = 1;
    char *res;
    py_splitroot(a, na, &ad, &ar);
    py_splitroot(b, nb, &bd, &br);
    rd = a; nrd = ad;
    rr = a + ad; nrr = ar;
    rp = a + ad + ar; nrp = na - ad - ar;
    if (br) {
        if (bd || !nrd) { rd = b; nrd = bd; }
        rr = b + bd; nrr = br;
        rp = b + bd + br; nrp = nb - bd - br;
        append = 0;
    } else if (bd && !(bd == nrd && memcmp(b, rd, bd) == 0)) {
        if (!ascii_ieq(b, bd, rd, nrd)) {
            rd = b; nrd = bd;
            rr = b + bd; nrr = 0;
            rp = b + bd; nrp = nb - bd;
            append = 0;
        } else {
            rd = b; nrd = bd;
        }
    }
    if (append) {
        buf_put(&o, rp, nrp);
        if (o.n && !is_sep(o.p[o.n - 1])) buf_putc(&o, '\\');
        buf_put(&o, b + bd + br, nb - bd - br);
        {
            buf_t f = {0};
            if (o.n && !nrr && nrd && !(rd[nrd - 1] == ':' || is_sep((unsigned char)rd[nrd - 1]))) {
                buf_put(&f, rd, nrd);
                buf_putc(&f, '\\');
                buf_put(&f, o.p, o.n);
            } else {
                buf_put(&f, rd, nrd);
                buf_put(&f, rr, nrr);
                buf_put(&f, o.p, o.n);
            }
            buf_free(&o);
            if (f.oom) { buf_free(&f); return NULL; }
            res = str_dup_n((const char *)f.p, f.n);
            buf_free(&f);
            return res;
        }
    }
    if (nrp && !nrr && nrd && !(rd[nrd - 1] == ':' || is_sep((unsigned char)rd[nrd - 1]))) {
        buf_put(&o, rd, nrd);
        buf_putc(&o, '\\');
        buf_put(&o, rp, nrp);
    } else {
        buf_put(&o, rd, nrd);
        buf_put(&o, rr, nrr);
        buf_put(&o, rp, nrp);
    }
    if (o.oom) { buf_free(&o); return NULL; }
    res = str_dup_n((const char *)o.p, o.n);
    buf_free(&o);
    return res;
}
/* WTF-8 path -> wide path for the Win32 API; long paths get the \\?\ prefix. */
static wchar_t *api_path(const char *p)
{
    wchar_t *w = wtf8_to_wide(p, strlen(p)), *full, *r;
    size_t len;
    DWORD need;
    if (!w) return NULL;
    len = wcslen(w);
    if (len < 240 || wcsncmp(w, L"\\\\?\\", 4) == 0 || wcsncmp(w, L"\\\\.\\", 4) == 0) return w;
    need = GetFullPathNameW(w, 0, NULL, NULL);
    if (!need) return w;
    full = malloc(((size_t)need + 2) * sizeof(wchar_t));
    if (!full) return w;
    if (!GetFullPathNameW(w, need + 1, full, NULL)) { free(full); return w; }
    len = wcslen(full);
    r = malloc((len + 10) * sizeof(wchar_t));
    if (!r) { free(full); return w; }
    if (full[0] == L'\\' && full[1] == L'\\') {
        wcscpy(r, L"\\\\?\\UNC\\");
        wcscat(r, full + 2);
    } else {
        wcscpy(r, L"\\\\?\\");
        wcscat(r, full);
    }
    free(full);
    free(w);
    return r;
}

/* ================================================================== file helpers */
#define RD_MAX ((size_t)1 << 30)
/* 0 ok, -1 not found, -2 other error */
static int read_all(const char *path, buf_t *out, DWORD *werr)
{
    wchar_t *w = api_path(path);
    HANDLE h;
    LARGE_INTEGER sz;
    size_t want, got = 0;
    DWORD e;
    *werr = 0;
    out->n = 0;
    if (!w) { *werr = ERROR_INVALID_NAME; return -2; }
    h = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    free(w);
    if (h == INVALID_HANDLE_VALUE) {
        e = GetLastError();
        *werr = e;
        return (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) ? -1 : -2;
    }
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart < 0 || (unsigned long long)sz.QuadPart > RD_MAX) {
        CloseHandle(h);
        *werr = ERROR_FILE_TOO_LARGE;
        return -2;
    }
    want = (size_t)sz.QuadPart;
    if (!buf_grow(out, want + 1)) {
        CloseHandle(h);
        *werr = ERROR_NOT_ENOUGH_MEMORY;
        return -2;
    }
    while (got < want) {
        DWORD rd = 0, chunk = (DWORD)((want - got) > ((size_t)1 << 24) ? ((size_t)1 << 24) : (want - got));
        if (!ReadFile(h, out->p + got, chunk, &rd, NULL)) {
            *werr = GetLastError();
            CloseHandle(h);
            return -2;
        }
        if (rd == 0) break;
        got += rd;
    }
    CloseHandle(h);
    out->n = got;
    out->p[got] = 0;
    return 0;
}
/* os.path.isfile (follows links; a locked file still counts) */
static int path_isfile(const char *path)
{
    wchar_t *w = api_path(path);
    HANDLE h;
    int r = 0;
    if (!w) return 0;
    h = CreateFileW(w, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                    OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        BY_HANDLE_FILE_INFORMATION fi;
        if (GetFileInformationByHandle(h, &fi)) r = !(fi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
        CloseHandle(h);
    } else {
        DWORD e = GetLastError();
        if (e == ERROR_ACCESS_DENIED || e == ERROR_SHARING_VIOLATION) {
            DWORD a = GetFileAttributesW(w);
            if (a != INVALID_FILE_ATTRIBUTES) r = !(a & FILE_ATTRIBUTE_DIRECTORY);
        }
    }
    free(w);
    return r;
}
static int path_exists(const char *path)
{
    wchar_t *w = api_path(path);
    DWORD a;
    if (!w) return 0;
    a = GetFileAttributesW(w);
    free(w);
    return a != INVALID_FILE_ATTRIBUTES;
}
/* size and last-write FILETIME; 0 ok */
static int path_stat(const char *path, long long *size, unsigned long long *mtime)
{
    wchar_t *w = api_path(path);
    WIN32_FILE_ATTRIBUTE_DATA d;
    BOOL ok;
    if (!w) return -1;
    ok = GetFileAttributesExW(w, GetFileExInfoStandard, &d);
    free(w);
    if (!ok || (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return -1;
    *size = (long long)(((unsigned long long)d.nFileSizeHigh << 32) | d.nFileSizeLow);
    *mtime = ((unsigned long long)d.ftLastWriteTime.dwHighDateTime << 32) | d.ftLastWriteTime.dwLowDateTime;
    return 0;
}
/* delete one of our own files (backups); a read-only attribute copied from the original is cleared first */
static int path_delete(const char *path)
{
    wchar_t *w = api_path(path);
    BOOL ok;
    if (!w) return 0;
    ok = DeleteFileW(w);
    if (!ok && GetLastError() == ERROR_ACCESS_DENIED) {
        DWORD a = GetFileAttributesW(w);
        if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_READONLY) && !(a & FILE_ATTRIBUTE_DIRECTORY) &&
            SetFileAttributesW(w, a & ~(DWORD)FILE_ATTRIBUTE_READONLY))
            ok = DeleteFileW(w);
    }
    free(w);
    return ok ? 1 : 0;
}
static int path_move_replace(const char *from, const char *to)
{
    wchar_t *a = api_path(from), *b = api_path(to);
    BOOL ok = FALSE;
    if (a && b) ok = MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    free(a);
    free(b);
    return ok ? 1 : 0;
}
static int path_copy_new(const char *from, const char *to)
{
    wchar_t *a = api_path(from), *b = api_path(to);
    BOOL ok = FALSE;
    if (a && b) ok = CopyFileW(a, b, TRUE);
    free(a);
    free(b);
    return ok ? 1 : 0;
}
/* MoveFileExW(REPLACE_EXISTING): a file just written by the generator may still be open in a scanner for a moment,
 * so sharing / access errors are retried for up to about a second; GetLastError() holds the last error on failure */
static BOOL move_replace_retry(const wchar_t *from, const wchar_t *to)
{
    int i;
    for (i = 0;; i++) {
        DWORD e;
        if (MoveFileExW(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return TRUE;
        e = GetLastError();
        if (i >= 19 || (e != ERROR_ACCESS_DENIED && e != ERROR_SHARING_VIOLATION && e != ERROR_LOCK_VIOLATION)) {
            SetLastError(e);
            return FALSE;
        }
        Sleep(50);
    }
}
/* write path via path.rf_tmp + MoveFileExW(REPLACE_EXISTING); 0 ok */
static int write_replace(const char *path, const void *data, size_t n, char *err, size_t ec)
{
    char *tmp = str_cat(path, ".rf_tmp");
    wchar_t *wt = tmp ? api_path(tmp) : NULL, *wp = api_path(path);
    HANDLE h;
    size_t done = 0;
    int rc = -1;
    if (!wt || !wp) { sset(err, ec, "bad path %s", path); goto out; }
    DeleteFileW(wt);
    h = CreateFileW(wt, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { sset(err, ec, "cannot create %s (Win32 error %lu)", tmp, GetLastError()); goto out; }
    while (done < n) {
        DWORD wr = 0, chunk = (DWORD)((n - done) > ((size_t)1 << 24) ? ((size_t)1 << 24) : (n - done));
        if (!WriteFile(h, (const unsigned char *)data + done, chunk, &wr, NULL) || wr == 0) {
            sset(err, ec, "write %s failed (Win32 error %lu)", tmp, GetLastError());
            CloseHandle(h);
            DeleteFileW(wt);
            goto out;
        }
        done += wr;
    }
    FlushFileBuffers(h);
    CloseHandle(h);
    if (!move_replace_retry(wt, wp)) {
        sset(err, ec, "cannot replace %s (Win32 error %lu)", path, GetLastError());
        DeleteFileW(wt);
        goto out;
    }
    rc = 0;
out:
    free(tmp);
    free(wt);
    free(wp);
    return rc;
}

/* ================================================================== JSON with Python json semantics */
enum { J_NULL = 0, J_FALSE, J_TRUE, J_INT, J_FLOAT, J_STR, J_ARR, J_OBJ };
typedef struct jval jval;
typedef struct { const char *k; size_t kn; jval *v; } jmem;
struct jval {
    int t;
    size_t a, b; /* byte span of the value in the source text */
    union {
        struct { const char *d; size_t n; int neg; } i; /* canonical digits (no sign, no leading zeros) */
        double f;
        struct { const char *p; size_t n; } s; /* WTF-8 */
        struct { jval **v; size_t n; } arr;
        struct { jmem *m; size_t n; } obj;
    } u;
};
#define J_MAXDEPTH 2998   /* Python 3.13: json.loads fails (RecursionError) beyond 2998 nested containers */
#define J_MAXDIGITS 4300  /* sys.get_int_max_str_digits() */

typedef struct {
    const unsigned char *s;
    size_t n, i;
    arena_t *ar;
    int depth;
    uintptr_t stack_floor; /* fail before the thread stack runs out (host threads may have 1 MB or less) */
    char err[160];
} jparse;

/* lowest usable stack address of this thread plus a safety margin; 0 if unknown */
static uintptr_t stack_floor(void)
{
    typedef VOID(WINAPI * limits_fn)(PULONG_PTR, PULONG_PTR);
    limits_fn f = (limits_fn)(void (*)(void))GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetCurrentThreadStackLimits");
    ULONG_PTR lo = 0, hi = 0;
    if (!f) return 0;
    f(&lo, &hi);
    return lo ? (uintptr_t)lo + 128 * 1024 : 0;
}

static jval *jp_fail(jparse *p, const char *msg)
{
    if (!p->err[0]) sset(p->err, sizeof p->err, "%s (byte %zu)", msg, p->i);
    return NULL;
}
static void jp_ws(jparse *p)
{
    while (p->i < p->n) {
        unsigned char c = p->s[p->i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') p->i++;
        else break;
    }
}
static jval *jp_new(jparse *p, int t, size_t a)
{
    jval *v = arena_alloc(p->ar, sizeof(jval));
    if (!v) return jp_fail(p, "out of memory");
    memset(v, 0, sizeof *v);
    v->t = t;
    v->a = a;
    return v;
}
static int jp_hex4(const unsigned char *s, uint32_t *out)
{
    uint32_t v = 0;
    int k;
    for (k = 0; k < 4; k++) {
        unsigned c = s[k];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= c - '0';
        else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
        else return 0;
    }
    *out = v;
    return 1;
}
/* p->s[p->i] == '"'; decodes into the arena (WTF-8) */
static int jp_string(jparse *p, const char **out, size_t *outn)
{
    size_t start = p->i + 1, j = start, end, i, k = 0;
    unsigned char *o;
    for (;;) {
        unsigned char c;
        if (j >= p->n) { jp_fail(p, "Unterminated string"); return 0; }
        c = p->s[j];
        if (c == '"') break;
        if (c == '\\') { j += 2; continue; }
        if (c < 0x20) { p->i = j; jp_fail(p, "Invalid control character"); return 0; }
        j++;
    }
    end = j;
    o = arena_alloc(p->ar, end - start + 1);
    if (!o) { jp_fail(p, "out of memory"); return 0; }
    i = start;
    while (i < end) {
        unsigned char c = p->s[i], e;
        if (c != '\\') { o[k++] = c; i++; continue; }
        e = p->s[i + 1];
        i += 2;
        switch (e) {
        case '"': o[k++] = '"'; break;
        case '\\': o[k++] = '\\'; break;
        case '/': o[k++] = '/'; break;
        case 'b': o[k++] = 8; break;
        case 'f': o[k++] = 12; break;
        case 'n': o[k++] = 10; break;
        case 'r': o[k++] = 13; break;
        case 't': o[k++] = 9; break;
        case 'u': {
            uint32_t u, u2;
            if (i + 4 > end || !jp_hex4(p->s + i, &u)) { p->i = i; jp_fail(p, "Invalid \\uXXXX escape"); return 0; }
            i += 4;
            if (u >= 0xD800 && u <= 0xDBFF && i + 6 <= end && p->s[i] == '\\' && p->s[i + 1] == 'u') {
                if (!jp_hex4(p->s + i + 2, &u2)) { p->i = i; jp_fail(p, "Invalid \\uXXXX escape"); return 0; }
                if (u2 >= 0xDC00 && u2 <= 0xDFFF) {
                    u = 0x10000 + ((u - 0xD800) << 10) + (u2 - 0xDC00);
                    i += 6;
                }
            }
            k += (size_t)u8_encode(u, o + k);
            break;
        }
        default:
            p->i = i - 2;
            jp_fail(p, "Invalid \\escape");
            return 0;
        }
    }
    o[k] = 0;
    p->i = end + 1;
    *out = (const char *)o;
    *outn = k;
    return 1;
}
static int is_dig(unsigned c) { return c >= '0' && c <= '9'; }
static jval *jp_number(jparse *p)
{
    const unsigned char *s = p->s;
    size_t n = p->n, st = p->i, i = st, ds;
    int neg = 0, isf = 0;
    jval *v;
    if (s[i] == '-') {
        neg = 1;
        i++;
        if (i >= n) return jp_fail(p, "Expecting value");
    }
    ds = i;
    if (s[i] >= '1' && s[i] <= '9') {
        i++;
        while (i < n && is_dig(s[i])) i++;
    } else if (s[i] == '0') {
        i++;
    } else {
        return jp_fail(p, "Expecting value");
    }
    if (i + 1 < n && s[i] == '.' && is_dig(s[i + 1])) {
        isf = 1;
        i += 2;
        while (i < n && is_dig(s[i])) i++;
    }
    if (i + 1 < n && (s[i] == 'e' || s[i] == 'E')) {
        size_t es = i;
        i++;
        if (i + 1 < n && (s[i] == '-' || s[i] == '+')) i++;
        while (i < n && is_dig(s[i])) i++;
        if (is_dig(s[i - 1])) isf = 1;
        else i = es;
    }
    if (isf) {
        char stackbuf[64], *tmp = stackbuf;
        size_t len = i - st;
        if (len >= sizeof stackbuf) {
            tmp = malloc(len + 1);
            if (!tmp) return jp_fail(p, "out of memory");
        }
        memcpy(tmp, s + st, len);
        tmp[len] = 0;
        v = jp_new(p, J_FLOAT, st);
        if (v) v->u.f = __mingw_strtod(tmp, NULL);
        if (tmp != stackbuf) free(tmp);
        if (!v) return NULL;
    } else {
        if (i - ds > J_MAXDIGITS) return jp_fail(p, "Exceeds the limit (4300 digits) for integer string conversion");
        v = jp_new(p, J_INT, st);
        if (!v) return NULL;
        v->u.i.d = (const char *)s + ds;
        v->u.i.n = i - ds;
        v->u.i.neg = neg && !(i - ds == 1 && s[ds] == '0');
    }
    p->i = i;
    v->b = i;
    return v;
}
static jval *jp_value(jparse *p);
static jval *jp_container(jparse *p, int obj)
{
    size_t a = p->i, cap = 0, cnt = 0;
    jmem *tmp = NULL;
    jval *v;
    char close = obj ? '}' : ']';
    volatile char here = 0;
    if (++p->depth > J_MAXDEPTH) return jp_fail(p, "maximum recursion depth exceeded");
    if (p->stack_floor && (uintptr_t)&here < p->stack_floor) return jp_fail(p, "JSON nested too deeply for this thread's stack");
    p->i++;
    jp_ws(p);
    if (p->i < p->n && p->s[p->i] == close) {
        p->i++;
    } else {
        for (;;) {
            jmem m = {0};
            if (obj) {
                if (p->i >= p->n || p->s[p->i] != '"') {
                    jp_fail(p, "Expecting property name enclosed in double quotes");
                    goto fail;
                }
                if (!jp_string(p, &m.k, &m.kn)) goto fail;
                jp_ws(p);
                if (p->i >= p->n || p->s[p->i] != ':') { jp_fail(p, "Expecting ':' delimiter"); goto fail; }
                p->i++;
                jp_ws(p);
            }
            m.v = jp_value(p);
            if (!m.v) goto fail;
            if (cnt == cap) {
                size_t nc = cap ? cap * 2 : 8;
                jmem *q = realloc(tmp, nc * sizeof(jmem));
                if (!q) { jp_fail(p, "out of memory"); goto fail; }
                tmp = q;
                cap = nc;
            }
            tmp[cnt++] = m;
            jp_ws(p);
            if (p->i < p->n && p->s[p->i] == close) { p->i++; break; }
            if (p->i >= p->n || p->s[p->i] != ',') { jp_fail(p, "Expecting ',' delimiter"); goto fail; }
            p->i++;
            jp_ws(p);
            if (p->i < p->n && p->s[p->i] == close) {
                jp_fail(p, obj ? "Illegal trailing comma before end of object" : "Illegal trailing comma before end of array");
                goto fail;
            }
        }
    }
    v = jp_new(p, obj ? J_OBJ : J_ARR, a);
    if (!v) goto fail;
    if (obj) {
        v->u.obj.m = arena_alloc(p->ar, (cnt ? cnt : 1) * sizeof(jmem));
        if (!v->u.obj.m) { jp_fail(p, "out of memory"); goto fail; }
        if (cnt) memcpy(v->u.obj.m, tmp, cnt * sizeof(jmem));
        v->u.obj.n = cnt;
    } else {
        size_t k;
        v->u.arr.v = arena_alloc(p->ar, (cnt ? cnt : 1) * sizeof(jval *));
        if (!v->u.arr.v) { jp_fail(p, "out of memory"); goto fail; }
        for (k = 0; k < cnt; k++) v->u.arr.v[k] = tmp[k].v;
        v->u.arr.n = cnt;
    }
    free(tmp);
    v->b = p->i;
    p->depth--;
    return v;
fail:
    free(tmp);
    return NULL;
}
static jval *jp_lit(jparse *p, const char *lit, int t, double f)
{
    size_t L = strlen(lit);
    jval *v;
    if (p->n - p->i < L || memcmp(p->s + p->i, lit, L) != 0) return NULL;
    v = jp_new(p, t, p->i);
    if (!v) return NULL;
    v->u.f = f;
    p->i += L;
    v->b = p->i;
    return v;
}
static jval *jp_value(jparse *p)
{
    unsigned char c;
    jval *v;
    if (p->i >= p->n) return jp_fail(p, "Expecting value");
    c = p->s[p->i];
    switch (c) {
    case '"': {
        size_t a = p->i;
        const char *s;
        size_t sn;
        if (!jp_string(p, &s, &sn)) return NULL;
        v = jp_new(p, J_STR, a);
        if (!v) return NULL;
        v->u.s.p = s;
        v->u.s.n = sn;
        v->b = p->i;
        return v;
    }
    case '{': return jp_container(p, 1);
    case '[': return jp_container(p, 0);
    case 'n': if ((v = jp_lit(p, "null", J_NULL, 0))) return v; break;
    case 't': if ((v = jp_lit(p, "true", J_TRUE, 0))) return v; break;
    case 'f': if ((v = jp_lit(p, "false", J_FALSE, 0))) return v; break;
    case 'N': if ((v = jp_lit(p, "NaN", J_FLOAT, NAN))) return v; break;
    case 'I': if ((v = jp_lit(p, "Infinity", J_FLOAT, INFINITY))) return v; break;
    case '-': if ((v = jp_lit(p, "-Infinity", J_FLOAT, -INFINITY))) return v; break;
    default: break;
    }
    if (p->err[0]) return NULL;
    return jp_number(p);
}
/* json.load(open(path, encoding="utf-8")) */
static jval *json_parse(const unsigned char *s, size_t n, arena_t *ar, char *err, size_t ec)
{
    jparse p;
    jval *v;
    memset(&p, 0, sizeof p);
    p.s = s;
    p.n = n;
    p.ar = ar;
    p.stack_floor = stack_floor();
    if (n >= 3 && s[0] == 0xEF && s[1] == 0xBB && s[2] == 0xBF) { sset(err, ec, "Unexpected UTF-8 BOM"); return NULL; }
    if (!utf8_strict_ok(s, n)) { sset(err, ec, "not valid UTF-8"); return NULL; }
    jp_ws(&p);
    v = jp_value(&p);
    if (v) {
        jp_ws(&p);
        if (p.i != p.n) {
            jp_fail(&p, "Extra data");
            v = NULL;
        }
    }
    if (!v) sset(err, ec, "%s", p.err[0] ? p.err : "json error");
    return v;
}
/* dict.get(key): last occurrence wins (Python dict) */
static const jval *jget(const jval *o, const char *key)
{
    size_t kn = strlen(key), i;
    if (!o || o->t != J_OBJ) return NULL;
    for (i = o->u.obj.n; i-- > 0;)
        if (o->u.obj.m[i].kn == kn && memcmp(o->u.obj.m[i].k, key, kn) == 0) return o->u.obj.m[i].v;
    return NULL;
}
static int jstr_is(const jval *v, const char *s)
{
    size_t n = strlen(s);
    return v && v->t == J_STR && v->u.s.n == n && memcmp(v->u.s.p, s, n) == 0;
}
/* how often key occurs in object o (a JSON object may repeat a key; Python keeps the last) */
static size_t jcount(const jval *o, const char *key)
{
    size_t kn = strlen(key), i, c = 0;
    if (!o || o->t != J_OBJ) return 0;
    for (i = 0; i < o->u.obj.n; i++)
        if (o->u.obj.m[i].kn == kn && memcmp(o->u.obj.m[i].k, key, kn) == 0) c++;
    return c;
}
/* structural equality of two parsed documents; the values s1 / s2 (nodes of a) may differ */
static int jv_same(const jval *a, const jval *b, const jval *s1, const jval *s2)
{
    size_t i;
    if (a == s1 || a == s2) return 1;
    if (a->t != b->t) return 0;
    switch (a->t) {
    case J_NULL: case J_TRUE: case J_FALSE: return 1;
    case J_INT: return a->u.i.neg == b->u.i.neg && a->u.i.n == b->u.i.n && memcmp(a->u.i.d, b->u.i.d, a->u.i.n) == 0;
    case J_FLOAT: return memcmp(&a->u.f, &b->u.f, sizeof(double)) == 0;
    case J_STR: return a->u.s.n == b->u.s.n && memcmp(a->u.s.p, b->u.s.p, a->u.s.n) == 0;
    case J_ARR:
        if (a->u.arr.n != b->u.arr.n) return 0;
        for (i = 0; i < a->u.arr.n; i++)
            if (!jv_same(a->u.arr.v[i], b->u.arr.v[i], s1, s2)) return 0;
        return 1;
    default:
        if (a->u.obj.n != b->u.obj.n) return 0;
        for (i = 0; i < a->u.obj.n; i++) {
            const jmem *x = &a->u.obj.m[i], *y = &b->u.obj.m[i];
            if (x->kn != y->kn || memcmp(x->k, y->k, x->kn) != 0 || !jv_same(x->v, y->v, s1, s2)) return 0;
        }
        return 1;
    }
}

/* ================================================================== Python value semantics */
/* truth value of a JSON value; missing (NULL) = None */
static int py_truthy(const jval *x)
{
    if (!x) return 0;
    switch (x->t) {
    case J_NULL: case J_FALSE: return 0;
    case J_TRUE: return 1;
    case J_INT: return !(x->u.i.n == 1 && x->u.i.d[0] == '0');
    case J_FLOAT: return x->u.f != 0.0 || isnan(x->u.f);
    case J_STR: return x->u.s.n > 0;
    case J_ARR: return x->u.arr.n > 0;
    case J_OBJ: return x->u.obj.n > 0;
    }
    return 0;
}

/* Python int: sign + canonical decimal digits */
typedef struct { int neg; const char *d; size_t n; } pyint;

static int pyint_to_ll(const pyint *x, long long *v)
{
    unsigned long long acc = 0;
    size_t i;
    if (x->n > 19) return 0;
    for (i = 0; i < x->n; i++) {
        unsigned d = (unsigned)(x->d[i] - '0');
        if (acc > (ULLONG_MAX - d) / 10) return 0;
        acc = acc * 10 + d;
    }
    if (!x->neg && acc > (unsigned long long)LLONG_MAX) return 0;
    if (x->neg && acc > (unsigned long long)LLONG_MAX + 1ull) return 0;
    *v = x->neg ? (long long)(0ull - acc) : (long long)acc;
    return 1;
}
static void pyint_set_ll(arena_t *ar, long long v, pyint *o)
{
    char *b = arena_alloc(ar, 24);
    unsigned long long u = v < 0 ? 0ull - (unsigned long long)v : (unsigned long long)v;
    if (!b) { o->neg = 0; o->d = "0"; o->n = 1; return; }
    snprintf(b, 24, "%llu", u);
    o->neg = v < 0;
    o->d = b;
    o->n = strlen(b);
}
static int pyint_eq(const pyint *a, const pyint *b)
{
    return a->neg == b->neg && a->n == b->n && memcmp(a->d, b->d, a->n) == 0;
}
static void pyint_str(const pyint *x, char *out, size_t cap)
{
    if (cap == 0) return;
    if (x->n + 2 > cap) {
        sset(out, cap, "%s%.*s...(%zu digits)", x->neg ? "-" : "", (int)(cap > 40 ? 20 : 4), x->d, x->n);
        return;
    }
    sset(out, cap, "%s%.*s", x->neg ? "-" : "", (int)x->n, x->d);
}

/* Unicode 15.1 decimal digit blocks (start code points of the 68 runs 0..9) and non-ASCII whitespace */
static const uint32_t U_DEC[68] = {
    0x30, 0x660, 0x6F0, 0x7C0, 0x966, 0x9E6, 0xA66, 0xAE6, 0xB66, 0xBE6, 0xC66, 0xCE6, 0xD66, 0xDE6,
    0xE50, 0xED0, 0xF20, 0x1040, 0x1090, 0x17E0, 0x1810, 0x1946, 0x19D0, 0x1A80, 0x1A90, 0x1B50, 0x1BB0,
    0x1C40, 0x1C50, 0xA620, 0xA8D0, 0xA900, 0xA9D0, 0xA9F0, 0xAA50, 0xABF0, 0xFF10, 0x104A0, 0x10D30,
    0x11066, 0x110F0, 0x11136, 0x111D0, 0x112F0, 0x11450, 0x114D0, 0x11650, 0x116C0, 0x11730, 0x118E0,
    0x11950, 0x11C50, 0x11D50, 0x11DA0, 0x11F50, 0x16A60, 0x16AC0, 0x16B50, 0x1D7CE, 0x1D7D8, 0x1D7E2,
    0x1D7EC, 0x1D7F6, 0x1E140, 0x1E2F0, 0x1E4F0, 0x1E950, 0x1FBF0};
static int u_decimal(uint32_t cp)
{
    size_t i;
    for (i = 0; i < ARRLEN(U_DEC); i++)
        if (cp >= U_DEC[i] && cp < U_DEC[i] + 10) return (int)(cp - U_DEC[i]);
    return -1;
}
static int u_space_nonascii(uint32_t cp)
{
    return cp == 0x85 || cp == 0xA0 || cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 ||
           cp == 0x2029 || cp == 0x202F || cp == 0x205F || cp == 0x3000;
}
static int py_isspace(int c) { return c == ' ' || (c >= 9 && c <= 13); }
/* _PyUnicode_TransformDecimalAndSpaceToASCII; returns malloc'd buffer (length *on, may contain NUL) */
static char *py_to_ascii(const char *s, size_t n, size_t *on)
{
    char *o = malloc(n + 1);
    size_t i = 0, k = 0, j;
    int ascii = 1;
    if (!o) return NULL;
    for (j = 0; j < n; j++)
        if ((unsigned char)s[j] >= 0x80) { ascii = 0; break; }
    if (ascii) {
        memcpy(o, s, n);
        o[n] = 0;
        *on = n;
        return o;
    }
    while (i < n) {
        uint32_t cp;
        int l = u8_decode((const unsigned char *)s + i, n - i, &cp, 1), d;
        if (!l) { o[k++] = '?'; break; }
        i += (size_t)l;
        if (cp < 127) { o[k++] = (char)cp; continue; }
        if (u_space_nonascii(cp)) { o[k++] = ' '; continue; }
        d = u_decimal(cp);
        if (d < 0) { o[k++] = '?'; break; }
        o[k++] = (char)('0' + d);
    }
    o[k] = 0;
    *on = k;
    return o;
}
/* int(str) base 10 (Python 3.13) */
static int py_int_from_str(arena_t *ar, const char *s0, size_t n0, pyint *o, char *err, size_t ec)
{
    size_t n, i = 0, ds, digits = 0, k;
    char *s = py_to_ascii(s0, n0, &n), *dig;
    int neg = 0, prev = 0;
    if (!s) { sset(err, ec, "out of memory"); return -1; }
    while (i < n && py_isspace((unsigned char)s[i])) i++;
    if (i < n && (s[i] == '+' || s[i] == '-')) { neg = s[i] == '-'; i++; }
    if (i < n && s[i] == '_') goto bad;
    ds = i;
    while (i < n && (is_dig((unsigned char)s[i]) || s[i] == '_')) {
        if (s[i] == '_') {
            if (prev == '_') goto bad;
        } else {
            digits++;
        }
        prev = s[i];
        i++;
    }
    if (prev == '_' || digits == 0) goto bad;
    {
        size_t de = i;
        while (i < n && py_isspace((unsigned char)s[i])) i++;
        if (i != n) goto bad;
        if (digits > J_MAXDIGITS) {
            sset(err, ec, "Exceeds the limit (4300 digits) for integer string conversion");
            free(s);
            return -1;
        }
        dig = arena_alloc(ar, digits + 1);
        if (!dig) { free(s); sset(err, ec, "out of memory"); return -1; }
        k = 0;
        for (i = ds; i < de; i++)
            if (s[i] != '_') dig[k++] = s[i];
        dig[k] = 0;
        i = 0;
        while (i + 1 < k && dig[i] == '0') i++;
        o->d = dig + i;
        o->n = k - i;
        o->neg = neg && !(o->n == 1 && o->d[0] == '0');
    }
    free(s);
    return 0;
bad:
    sset(err, ec, "invalid literal for int() with base 10");
    free(s);
    return -1;
}
/* int(float) */
static int py_int_from_double(arena_t *ar, double x, pyint *o, char *err, size_t ec)
{
    char *b;
    double t;
    if (isnan(x)) { sset(err, ec, "cannot convert float NaN to integer"); return -1; }
    if (isinf(x)) { sset(err, ec, "cannot convert float infinity to integer"); return -1; }
    t = trunc(x);
    b = arena_alloc(ar, 400);
    if (!b) { sset(err, ec, "out of memory"); return -1; }
    snprintf(b, 400, "%.0f", fabs(t));
    o->d = b;
    o->n = strlen(b);
    o->neg = t < 0 && !(o->n == 1 && b[0] == '0');
    return 0;
}
/* Python int(x) of a (truthy) JSON value */
static int py_int(arena_t *ar, const jval *x, pyint *o, char *err, size_t ec)
{
    switch (x ? x->t : J_NULL) {
    case J_TRUE: o->neg = 0; o->d = "1"; o->n = 1; return 0;
    case J_FALSE: o->neg = 0; o->d = "0"; o->n = 1; return 0;
    case J_INT: o->neg = x->u.i.neg; o->d = x->u.i.d; o->n = x->u.i.n; return 0;
    case J_FLOAT: return py_int_from_double(ar, x->u.f, o, err, ec);
    case J_STR: return py_int_from_str(ar, x->u.s.p, x->u.s.n, o, err, ec);
    case J_NULL: sset(err, ec, "int() argument must be a string or a real number, not 'NoneType'"); return -1;
    case J_ARR: sset(err, ec, "int() argument must be a string or a real number, not 'list'"); return -1;
    default: sset(err, ec, "int() argument must be a string or a real number, not 'dict'"); return -1;
    }
}
/* int(x or dflt) */
static int py_int_or(arena_t *ar, const jval *x, long long dflt, pyint *o, char *err, size_t ec)
{
    if (!py_truthy(x)) { pyint_set_ll(ar, dflt, o); return 0; }
    return py_int(ar, x, o, err, ec);
}
/* float(str) */
static int py_float_from_str(const char *s0, size_t n0, double *out, char *err, size_t ec)
{
    size_t n, i, j, k, b, e;
    char *s = py_to_ascii(s0, n0, &n), *t = NULL;
    int ok = 0;
    if (!s) { sset(err, ec, "out of memory"); return -1; }
    if (memchr(s, '_', n)) {
        int prev = 0;
        t = malloc(n + 1);
        if (!t) goto bad;
        k = 0;
        for (i = 0; i < n; i++) {
            if (s[i] == '_') {
                if (!is_dig((unsigned char)prev)) goto bad;
            } else {
                if (prev == '_' && !is_dig((unsigned char)s[i])) goto bad;
                t[k++] = s[i];
            }
            prev = (unsigned char)s[i];
        }
        if (prev == '_') goto bad;
        t[k] = 0;
        free(s);
        s = t;
        t = NULL;
        n = k;
    }
    if (memchr(s, 0, n)) goto bad;
    b = 0;
    e = n;
    while (b < e && py_isspace((unsigned char)s[b])) b++;
    if (b == e) goto bad;
    while (e - 1 > b && py_isspace((unsigned char)s[e - 1])) e--;
    s[e] = 0;
    {
        const char *p = s + b;
        size_t m = e - b, q = 0, nd = 0;
        int neg = 0;
        if (q < m && (p[q] == '+' || p[q] == '-')) { neg = p[q] == '-'; q++; }
        j = q;
        while (q < m && is_dig((unsigned char)p[q])) { q++; nd++; }
        if (q < m && p[q] == '.') {
            q++;
            while (q < m && is_dig((unsigned char)p[q])) { q++; nd++; }
        }
        if (nd > 0) {
            if (q < m && (p[q] == 'e' || p[q] == 'E')) {
                size_t es = q, ne = 0;
                q++;
                if (q < m && (p[q] == '+' || p[q] == '-')) q++;
                while (q < m && is_dig((unsigned char)p[q])) { q++; ne++; }
                if (!ne) q = es;
            }
            if (q == m) {
                *out = __mingw_strtod(p, NULL);
                ok = 1;
            }
        } else {
            const char *r = p + j;
            size_t rl = m - j;
            if ((rl == 3 && _strnicmp(r, "inf", 3) == 0) || (rl == 8 && _strnicmp(r, "infinity", 8) == 0)) {
                *out = neg ? -INFINITY : INFINITY;
                ok = 1;
            } else if (rl == 3 && _strnicmp(r, "nan", 3) == 0) {
                *out = NAN;
                ok = 1;
            }
        }
    }
    if (ok) {
        free(s);
        return 0;
    }
bad:
    free(s);
    free(t);
    sset(err, ec, "could not convert string to float");
    return -1;
}
/* float(int) (PyLong_AsDouble: correctly rounded, OverflowError when too large) */
static int py_float_from_pyint(const pyint *x, double *out, char *err, size_t ec)
{
    char *t = malloc(x->n + 2);
    double v;
    if (!t) { sset(err, ec, "out of memory"); return -1; }
    t[0] = '-';
    memcpy(t + 1, x->d, x->n);
    t[x->n + 1] = 0;
    v = __mingw_strtod(x->neg ? t : t + 1, NULL);
    free(t);
    if (isinf(v)) { sset(err, ec, "int too large to convert to float"); return -1; }
    *out = v;
    return 0;
}
/* Python float(x) of a (truthy) JSON value */
static int py_float(const jval *x, double *out, char *err, size_t ec)
{
    switch (x ? x->t : J_NULL) {
    case J_TRUE: *out = 1.0; return 0;
    case J_FALSE: *out = 0.0; return 0;
    case J_FLOAT: *out = x->u.f; return 0;
    case J_INT: {
        pyint pi;
        pi.neg = x->u.i.neg;
        pi.d = x->u.i.d;
        pi.n = x->u.i.n;
        return py_float_from_pyint(&pi, out, err, ec);
    }
    case J_STR: return py_float_from_str(x->u.s.p, x->u.s.n, out, err, ec);
    default: sset(err, ec, "float() argument must be a string or a real number"); return -1;
    }
}
/* repr(float), Python 3 'short' style */
static void py_float_repr(double x, char *out, size_t cap)
{
    char tmp[64], dig[32];
    int p, nd = 0, e10 = 0, decpt, i;
    const char *q;
    buf_t b = {0};
    if (isnan(x)) { sset(out, cap, "nan"); return; }
    if (isinf(x)) { sset(out, cap, x > 0 ? "inf" : "-inf"); return; }
    if (x == 0) { sset(out, cap, signbit(x) ? "-0.0" : "0.0"); return; }
    for (p = 1; p <= 17; p++) {
        snprintf(tmp, sizeof tmp, "%.*e", p - 1, x);
        if (__mingw_strtod(tmp, NULL) == x) break;
    }
    q = tmp;
    if (*q == '-') q++;
    for (; *q && *q != 'e'; q++)
        if (*q >= '0' && *q <= '9') dig[nd++] = *q;
    if (*q == 'e') e10 = atoi(q + 1);
    while (nd > 1 && dig[nd - 1] == '0') nd--;
    dig[nd] = 0;
    decpt = e10 + 1;
    if (x < 0) buf_putc(&b, '-');
    if (decpt <= -4 || decpt > 16) {
        buf_putc(&b, dig[0]);
        if (nd > 1) {
            buf_putc(&b, '.');
            buf_put(&b, dig + 1, (size_t)nd - 1);
        }
        buf_printf(&b, "e%c%02d", e10 < 0 ? '-' : '+', e10 < 0 ? -e10 : e10);
    } else if (decpt <= 0) {
        buf_puts(&b, "0.");
        for (i = 0; i < -decpt; i++) buf_putc(&b, '0');
        buf_put(&b, dig, (size_t)nd);
    } else if (decpt >= nd) {
        buf_put(&b, dig, (size_t)nd);
        for (i = nd; i < decpt; i++) buf_putc(&b, '0');
        buf_puts(&b, ".0");
    } else {
        buf_put(&b, dig, (size_t)decpt);
        buf_putc(&b, '.');
        buf_put(&b, dig + decpt, (size_t)(nd - decpt));
    }
    sset(out, cap, "%s", b.p ? (const char *)b.p : "?");
    buf_free(&b);
}
/* str(x) of a scalar or empty container; 0 = not representable here (non-empty list/dict) */
static int py_str(const jval *x, buf_t *o)
{
    char t[64];
    if (!x) return buf_puts(o, "None");
    switch (x->t) {
    case J_NULL: return buf_puts(o, "None");
    case J_TRUE: return buf_puts(o, "True");
    case J_FALSE: return buf_puts(o, "False");
    case J_INT:
        if (x->u.i.neg) buf_putc(o, '-');
        return buf_put(o, x->u.i.d, x->u.i.n);
    case J_FLOAT: py_float_repr(x->u.f, t, sizeof t); return buf_puts(o, t);
    case J_STR: return buf_put(o, x->u.s.p, x->u.s.n);
    case J_ARR: return x->u.arr.n ? 0 : buf_puts(o, "[]");
    default: return x->u.obj.n ? 0 : buf_puts(o, "{}");
    }
}

/* ================================================================== JSON writer */
static void jw_str(buf_t *b, const char *s, size_t n)
{
    size_t i = 0;
    buf_putc(b, '"');
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 0x20 && c < 0x7F && c != '"' && c != '\\') { buf_putc(b, c); i++; continue; }
        if (c == '"' || c == '\\') { buf_putc(b, '\\'); buf_putc(b, c); i++; continue; }
        if (c < 0x80) { buf_printf(b, "\\u%04x", c); i++; continue; }
        {
            uint32_t cp;
            int l = u8_decode((const unsigned char *)s + i, n - i, &cp, 1);
            if (!l) { buf_puts(b, "\\ufffd"); i++; continue; }
            i += (size_t)l;
            if (cp >= 0x10000) {
                cp -= 0x10000;
                buf_printf(b, "\\u%04x\\u%04x", 0xD800 + (cp >> 10), 0xDC00 + (cp & 0x3FF));
            } else {
                buf_printf(b, "\\u%04x", cp);
            }
        }
    }
    buf_putc(b, '"');
}
static void jw_cstr(buf_t *b, const char *s) { jw_str(b, s, strlen(s)); }
static void jw_double(buf_t *b, double v)
{
    char t[64];
    if (!isfinite(v)) { buf_puts(b, "null"); return; }
    py_float_repr(v, t, sizeof t);
    buf_puts(b, t);
}
static void jw_val(buf_t *b, const jval *v)
{
    size_t i;
    switch (v->t) {
    case J_NULL: buf_puts(b, "null"); break;
    case J_TRUE: buf_puts(b, "true"); break;
    case J_FALSE: buf_puts(b, "false"); break;
    case J_INT:
        if (v->u.i.neg) buf_putc(b, '-');
        buf_put(b, v->u.i.d, v->u.i.n);
        break;
    case J_FLOAT: jw_double(b, v->u.f); break;
    case J_STR: jw_str(b, v->u.s.p, v->u.s.n); break;
    case J_ARR:
        buf_putc(b, '[');
        for (i = 0; i < v->u.arr.n; i++) {
            if (i) buf_puts(b, ", ");
            jw_val(b, v->u.arr.v[i]);
        }
        buf_putc(b, ']');
        break;
    default:
        buf_putc(b, '{');
        for (i = 0; i < v->u.obj.n; i++) {
            if (i) buf_puts(b, ", ");
            jw_str(b, v->u.obj.m[i].k, v->u.obj.m[i].kn);
            buf_puts(b, ": ");
            jw_val(b, v->u.obj.m[i].v);
        }
        buf_putc(b, '}');
        break;
    }
}

/* ================================================================== inflate (zlib semantics) */
typedef struct { const unsigned char *in; size_t in_len; uint64_t pos, lim; } bitrd; /* lim in bits */
static int br_need(const bitrd *b, unsigned n) { return b->pos + n <= b->lim; }
static uint32_t br_peek(const bitrd *b, unsigned n)
{
    uint64_t byte = b->pos >> 3, v = 0;
    unsigned sh = (unsigned)(b->pos & 7), k;
    if (byte + 8 <= b->in_len) {
        memcpy(&v, b->in + byte, 8);
    } else {
        for (k = 0; k < 8 && byte + k < b->in_len; k++) v |= (uint64_t)b->in[byte + k] << (8 * k);
    }
    return (uint32_t)((v >> sh) & ((1ull << n) - 1));
}
static int br_get(bitrd *b, unsigned n, uint32_t *v)
{
    if (!br_need(b, n)) return 0;
    *v = n ? br_peek(b, n) : 0;
    b->pos += n;
    return 1;
}
typedef struct { uint16_t count[16]; uint16_t sym[288]; uint16_t fast[512]; int maxlen; } huff_t;
static uint32_t rev_bits(uint32_t c, int n)
{
    uint32_t r = 0;
    while (n--) {
        r = (r << 1) | (c & 1);
        c >>= 1;
    }
    return r;
}
/* kind 0 = code-length code, 1 = literal/length, 2 = distance (zlib inftrees.c rules) */
static int huff_build(huff_t *h, const uint8_t *lens, int n, int kind)
{
    int len, i, left;
    uint16_t offs[17];
    uint32_t next[16], code = 0;
    memset(h, 0, sizeof *h);
    for (i = 0; i < n; i++) h->count[lens[i]]++;
    h->count[0] = 0;
    for (len = 1; len <= 15; len++)
        if (h->count[len]) h->maxlen = len;
    left = 1;
    for (len = 1; len <= 15; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return 0;
    }
    if (h->maxlen == 0) return kind != 0;
    if (left > 0 && (kind == 0 || h->maxlen != 1)) return 0;
    offs[1] = 0;
    for (len = 1; len < 15; len++) offs[len + 1] = (uint16_t)(offs[len] + h->count[len]);
    for (i = 0; i < n; i++)
        if (lens[i]) h->sym[offs[lens[i]]++] = (uint16_t)i;
    for (len = 1; len <= 15; len++) {
        code = (code + h->count[len - 1]) << 1;
        next[len] = code;
    }
    for (i = 0; i < n; i++) {
        uint32_t c, r, f;
        len = lens[i];
        if (!len) continue;
        c = next[len]++;
        if (len > 9) continue;
        r = rev_bits(c, len);
        for (f = r; f < 512; f += 1u << len) h->fast[f] = (uint16_t)((len << 9) | i);
    }
    return 1;
}
/* >= 0 symbol, -1 need input, -2 invalid code */
static int huff_decode(bitrd *b, const huff_t *h)
{
    int len, code = 0, first = 0, index = 0;
    uint64_t p;
    if (b->pos + 9 <= b->lim) {
        uint32_t e = h->fast[br_peek(b, 9)];
        if (e) {
            b->pos += e >> 9;
            return (int)(e & 511);
        }
    }
    p = b->pos;
    for (len = 1; len <= 15; len++) {
        int count;
        if (p >= b->lim) return -1;
        code |= (b->in[p >> 3] >> (p & 7)) & 1;
        p++;
        count = h->count[len];
        if (code - count < first) {
            b->pos = p;
            return h->sym[index + (code - first)];
        }
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -2;
}
static const uint16_t LBASE[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                   31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t LEXT[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t DBASE[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                   193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const uint8_t DEXT[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

enum { IM_ZHDR, IM_BLOCK, IM_STORED, IM_HUFF, IM_CHECK, IM_DONE };
typedef struct {
    bitrd br;
    int mode, final;
    uint32_t stored_left, mlen, mdist;
    const huff_t *L, *D;
    huff_t dl, dd, fl, fd;
    unsigned char *out;
    size_t out_pos, out_cap;
    char err[96];
} infl;

static void infl_init(infl *s, const unsigned char *in, size_t n, unsigned char *out, size_t cap)
{
    uint8_t l[288];
    int i;
    memset(s, 0, sizeof *s);
    s->br.in = in;
    s->br.in_len = n;
    s->br.lim = (uint64_t)n * 8;
    s->out = out;
    s->out_cap = cap;
    s->mode = IM_ZHDR;
    for (i = 0; i < 144; i++) l[i] = 8;
    for (; i < 256; i++) l[i] = 9;
    for (; i < 280; i++) l[i] = 7;
    for (; i < 288; i++) l[i] = 8;
    huff_build(&s->fl, l, 288, 1);
    for (i = 0; i < 32; i++) l[i] = 5;
    huff_build(&s->fd, l, 32, 2);
}
/* zlib header: 1 ok, 0 need input, -1 error */
static int infl_zhdr(bitrd *br, char *err, size_t ec)
{
    uint32_t cmf = 0, flg = 0;
    if (!br_need(br, 16)) return 0;
    br_get(br, 8, &cmf);
    br_get(br, 8, &flg);
    if (((cmf << 8) | flg) % 31) { sset(err, ec, "incorrect header check"); return -1; }
    if ((cmf & 15) != 8) { sset(err, ec, "unknown compression method"); return -1; }
    if ((cmf >> 4) + 8 > 15) { sset(err, ec, "invalid window size"); return -1; }
    if (flg & 0x20) { sset(err, ec, "preset dictionary required"); return -1; }
    return 1;
}
/* block header: 1 ok, 0 need input (position restored), -1 error */
static int infl_block(bitrd *br, int *final, int *mode, uint32_t *stored_left, huff_t *dl, huff_t *dd,
                      const huff_t *fl, const huff_t *fd, const huff_t **L, const huff_t **D, char *err, size_t ec)
{
    static const uint8_t ord[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    uint64_t save = br->pos;
    uint32_t v, hlit, hdist, hclen, len, nlen;
    int type, i, have, total;
    uint8_t cl[19], lens[320];
    huff_t ch;
    if (!br_get(br, 3, &v)) goto need;
    *final = (int)(v & 1);
    type = (int)(v >> 1);
    if (type == 0) {
        br->pos = (br->pos + 7) & ~(uint64_t)7;
        if (!br_get(br, 16, &len) || !br_get(br, 16, &nlen)) goto need;
        if ((len ^ 0xFFFFu) != nlen) { sset(err, ec, "invalid stored block lengths"); return -1; }
        *stored_left = len;
        *mode = IM_STORED;
        return 1;
    }
    if (type == 1) {
        *L = fl;
        *D = fd;
        *mode = IM_HUFF;
        return 1;
    }
    if (type == 3) { sset(err, ec, "invalid block type"); return -1; }
    if (!br_get(br, 5, &hlit) || !br_get(br, 5, &hdist) || !br_get(br, 4, &hclen)) goto need;
    hlit += 257;
    hdist += 1;
    hclen += 4;
    if (hlit > 286 || hdist > 30) { sset(err, ec, "too many length or distance symbols"); return -1; }
    memset(cl, 0, sizeof cl);
    for (i = 0; i < (int)hclen; i++) {
        if (!br_get(br, 3, &v)) goto need;
        cl[ord[i]] = (uint8_t)v;
    }
    if (!huff_build(&ch, cl, 19, 0)) { sset(err, ec, "invalid code lengths set"); return -1; }
    have = 0;
    total = (int)(hlit + hdist);
    while (have < total) {
        int sym = huff_decode(br, &ch), rep, val;
        if (sym == -1) goto need;
        if (sym < 0) { sset(err, ec, "invalid code lengths set"); return -1; }
        if (sym < 16) {
            lens[have++] = (uint8_t)sym;
            continue;
        }
        if (sym == 16) {
            if (!br_get(br, 2, &v)) goto need;
            if (have == 0) { sset(err, ec, "invalid bit length repeat"); return -1; }
            rep = 3 + (int)v;
            val = lens[have - 1];
        } else if (sym == 17) {
            if (!br_get(br, 3, &v)) goto need;
            rep = 3 + (int)v;
            val = 0;
        } else {
            if (!br_get(br, 7, &v)) goto need;
            rep = 11 + (int)v;
            val = 0;
        }
        if (have + rep > total) { sset(err, ec, "invalid bit length repeat"); return -1; }
        while (rep--) lens[have++] = (uint8_t)val;
    }
    if (lens[256] == 0) { sset(err, ec, "invalid code -- missing end-of-block"); return -1; }
    if (!huff_build(dl, lens, (int)hlit, 1)) { sset(err, ec, "invalid literal/lengths set"); return -1; }
    if (!huff_build(dd, lens + hlit, (int)hdist, 2)) { sset(err, ec, "invalid distances set"); return -1; }
    *L = dl;
    *D = dd;
    *mode = IM_HUFF;
    return 1;
need:
    br->pos = save;
    return 0;
}
/* produce exactly need bytes: 1 done, 0 input exhausted, 2 stream ended first, -1 error */
static int infl_produce(infl *s, size_t need)
{
    size_t end = s->out_pos + need;
    if (end > s->out_cap) { sset(s->err, sizeof s->err, "output overflow"); return -1; }
    while (s->out_pos < end) {
        if (s->mlen) {
            size_t n = s->mlen, i;
            unsigned char *d = s->out + s->out_pos;
            const unsigned char *src = d - s->mdist;
            if (n > end - s->out_pos) n = end - s->out_pos;
            for (i = 0; i < n; i++) d[i] = src[i];
            s->out_pos += n;
            s->mlen -= (uint32_t)n;
            continue;
        }
        switch (s->mode) {
        case IM_ZHDR: {
            int r = infl_zhdr(&s->br, s->err, sizeof s->err);
            if (r <= 0) return r;
            s->mode = IM_BLOCK;
            break;
        }
        case IM_BLOCK: {
            int r = infl_block(&s->br, &s->final, &s->mode, &s->stored_left, &s->dl, &s->dd, &s->fl, &s->fd, &s->L,
                               &s->D, s->err, sizeof s->err);
            if (r <= 0) return r;
            break;
        }
        case IM_STORED: {
            size_t avail, n;
            if (s->stored_left == 0) {
                s->mode = s->final ? IM_CHECK : IM_BLOCK;
                break;
            }
            avail = (size_t)((s->br.lim - s->br.pos) >> 3);
            n = s->stored_left;
            if (n > end - s->out_pos) n = end - s->out_pos;
            if (n > avail) n = avail;
            if (n == 0) return 0;
            memcpy(s->out + s->out_pos, s->br.in + (s->br.pos >> 3), n);
            s->br.pos += (uint64_t)n * 8;
            s->out_pos += n;
            s->stored_left -= (uint32_t)n;
            break;
        }
        case IM_HUFF: {
            uint64_t save = s->br.pos;
            uint32_t v;
            int sym = huff_decode(&s->br, s->L), ds;
            uint32_t len, dist;
            if (sym == -1) return 0;
            if (sym < 0) { sset(s->err, sizeof s->err, "invalid literal/length code"); return -1; }
            if (sym < 256) {
                s->out[s->out_pos++] = (unsigned char)sym;
                break;
            }
            if (sym == 256) {
                s->mode = s->final ? IM_CHECK : IM_BLOCK;
                break;
            }
            sym -= 257;
            if (sym >= 29) { sset(s->err, sizeof s->err, "invalid literal/length code"); return -1; }
            if (!br_get(&s->br, LEXT[sym], &v)) { s->br.pos = save; return 0; }
            len = LBASE[sym] + v;
            ds = huff_decode(&s->br, s->D);
            if (ds == -1) { s->br.pos = save; return 0; }
            if (ds < 0 || ds >= 30) { sset(s->err, sizeof s->err, "invalid distance code"); return -1; }
            if (!br_get(&s->br, DEXT[ds], &v)) { s->br.pos = save; return 0; }
            dist = DBASE[ds] + v;
            if (dist > s->out_pos) { sset(s->err, sizeof s->err, "invalid distance too far back"); return -1; }
            s->mlen = len;
            s->mdist = dist;
            break;
        }
        case IM_CHECK: {
            uint32_t a, want;
            s->br.pos = (s->br.pos + 7) & ~(uint64_t)7;
            if (!br_need(&s->br, 32)) return 0;
            want = be32(s->br.in + (s->br.pos >> 3));
            a = adler32(s->out, s->out_pos);
            if (a != want) { sset(s->err, sizeof s->err, "incorrect data check"); return -1; }
            s->br.pos += 32;
            s->mode = IM_DONE;
            return 2;
        }
        default:
            return 2;
        }
    }
    return 1;
}
/* What zlib does after the output buffer is full: keep decoding without output until it needs output or
 * input (lim).  1 = stream end reached with a good check value, 0 = stopped, -1 = data error. */
static int infl_lookahead(const infl *s0, uint64_t lim, char *err, size_t ec)
{
    bitrd br = s0->br;
    int mode = s0->mode, final = s0->final;
    uint32_t stored_left = s0->stored_left, v;
    const huff_t *L = s0->L, *D = s0->D;
    huff_t *tabs = NULL;
    int rc = 0;
    if (lim < br.lim) br.lim = lim;
    if (s0->mlen) return 0;
    for (;;) {
        if (mode == IM_ZHDR) {
            int r = infl_zhdr(&br, err, ec);
            if (r <= 0) { rc = r; break; }
            mode = IM_BLOCK;
        } else if (mode == IM_BLOCK) {
            int r;
            if (!tabs) {
                tabs = malloc(2 * sizeof(huff_t));
                if (!tabs) { rc = 0; break; }
            }
            r = infl_block(&br, &final, &mode, &stored_left, &tabs[0], &tabs[1], &s0->fl, &s0->fd, &L, &D, err, ec);
            if (r <= 0) { rc = r; break; }
        } else if (mode == IM_STORED) {
            if (stored_left) { rc = 0; break; }
            mode = final ? IM_CHECK : IM_BLOCK;
        } else if (mode == IM_HUFF) {
            int sym = huff_decode(&br, L), ds;
            if (sym == -1) { rc = 0; break; }
            if (sym == -2 || sym > 285) { sset(err, ec, "invalid literal/length code"); rc = -1; break; }
            if (sym < 256) { rc = 0; break; }
            if (sym == 256) {
                mode = final ? IM_CHECK : IM_BLOCK;
                continue;
            }
            if (!br_get(&br, LEXT[sym - 257], &v)) { rc = 0; break; }
            ds = huff_decode(&br, D);
            if (ds == -1) { rc = 0; break; }
            if (ds < 0 || ds >= 30) { sset(err, ec, "invalid distance code"); rc = -1; break; }
            rc = 0;
            break;
        } else if (mode == IM_CHECK) {
            br.pos = (br.pos + 7) & ~(uint64_t)7;
            if (!br_need(&br, 32)) { rc = 0; break; }
            if (be32(br.in + (br.pos >> 3)) != adler32(s0->out, s0->out_pos)) {
                sset(err, ec, "incorrect data check");
                rc = -1;
            } else {
                rc = 1;
            }
            break;
        } else {
            rc = 1;
            break;
        }
    }
    free(tabs);
    return rc;
}

/* ================================================================== PNG decoder (Pillow 12 + zlib behaviour) */
typedef struct {
    uint32_t w, h;
    int bd, ct, ilace, ch;
    unsigned char pal[256][4]; /* RGBA as Pillow sees it: missing entries black, alpha 255 */
    int apng;
    int idat_count;            /* IDAT chunks in the run used for the image */
    int has_phys;
    unsigned char phys[9];
    unsigned char *gray;       /* w*h, g = 255 - darkness (only when requested) */
    unsigned char *luma_na;    /* w*h luma of the colour IGNORING alpha (want_src) */
    unsigned char *alpha;      /* w*h alpha: channel, palette tRNS or colour-key tRNS; 255 without (want_src) */
    int has_alpha;             /* alpha channel, palette tRNS or colour-key tRNS present */
    uint64_t hist[256];
    char err[200];
} pngimg;

static int is_cid(const unsigned char *c)
{
    int i;
    for (i = 0; i < 4; i++) {
        unsigned x = c[i];
        if (!((x >= 'a' && x <= 'z') || (x >= 'A' && x <= 'Z') || (x >= '0' && x <= '9') || x == '_')) return 0;
    }
    return 1;
}
static int png_mode_ok(int bd, int ct)
{
    switch (ct) {
    case 0: return bd == 1 || bd == 2 || bd == 4 || bd == 8 || bd == 16;
    case 2: case 4: case 6: return bd == 8 || bd == 16;
    case 3: return bd == 1 || bd == 2 || bd == 4 || bd == 8;
    }
    return 0;
}
typedef struct {
    int have_mode, bd, ct, ilace;
    uint32_t w, h;
    int have_size;
    int plte_set;
    unsigned char plte[768];
    int plte_n;
    int trns_set;          /* palette transparency set */
    unsigned char trns[300];
    int trns_n;            /* may exceed 256: error at conversion time */
    int apng_frames;
    int has_phys;
    unsigned char phys[9];
    int key_set;           /* gray / RGB colour-key tRNS (before IDAT) */
    unsigned key[3];
} pngstate;

/* Pillow chunk handlers that can fail or change what the image looks like (pre: before IDAT). */
static int png_chunk(pngstate *st, const unsigned char *cid, const unsigned char *d, uint32_t len, int pre, char *err,
                     size_t ec)
{
    if (!memcmp(cid, "IHDR", 4)) {
        if (len < 13) { sset(err, ec, "Truncated IHDR chunk"); return -1; }
        st->w = be32(d);
        st->h = be32(d + 4);
        st->have_size = 1;
        if (png_mode_ok(d[8], d[9])) {
            st->have_mode = 1;
            st->bd = d[8];
            st->ct = d[9];
        }
        if (d[12]) st->ilace = 1;
        if (d[11]) { sset(err, ec, "unknown filter category"); return -1; }
    } else if (!memcmp(cid, "PLTE", 4)) {
        if (pre && st->have_mode && st->ct == 3) {
            uint32_t n = len / 3;
            st->plte_set = 1;
            st->plte_n = (int)(n > 256 ? 256 : n);
            if (n > 256) { sset(err, ec, "palette with more than 256 entries"); return -1; }
            memcpy(st->plte, d, (size_t)st->plte_n * 3);
        }
    } else if (!memcmp(cid, "tRNS", 4)) {
        if (st->have_mode && st->ct == 3) {
            st->trns_set = 1;
            st->trns_n = len > 256 ? 257 : (int)len; /* 257: more alphas than palette entries -> error later */
            memcpy(st->trns, d, len > 256 ? 256 : len);
        } else if (st->have_mode && st->ct == 0) {
            if (len < 2) { sset(err, ec, "tRNS chunk too short"); return -1; }
            if (pre) {
                st->key_set = 1;
                st->key[0] = ((unsigned)d[0] << 8) | d[1];
            }
        } else if (st->have_mode && st->ct == 2) {
            if (len < 6) { sset(err, ec, "tRNS chunk too short"); return -1; }
            if (pre) {
                st->key_set = 1;
                st->key[0] = ((unsigned)d[0] << 8) | d[1];
                st->key[1] = ((unsigned)d[2] << 8) | d[3];
                st->key[2] = ((unsigned)d[4] << 8) | d[5];
            }
        }
    } else if (!memcmp(cid, "gAMA", 4)) {
        if (len < 4) { sset(err, ec, "gAMA chunk too short"); return -1; }
    } else if (!memcmp(cid, "cHRM", 4)) {
        if (len % 4) { sset(err, ec, "cHRM chunk length not a multiple of 4"); return -1; }
    } else if (!memcmp(cid, "sRGB", 4)) {
        if (len < 1) { sset(err, ec, "Truncated sRGB chunk"); return -1; }
    } else if (!memcmp(cid, "pHYs", 4)) {
        if (len < 9) { sset(err, ec, "Truncated pHYs chunk"); return -1; }
        if (pre) {
            st->has_phys = 1;
            memcpy(st->phys, d, 9);
        }
    } else if (!memcmp(cid, "acTL", 4)) {
        if (len < 8) { sset(err, ec, "APNG contains truncated acTL chunk"); return -1; }
        if (pre) {
            uint32_t nf = be32(d);
            if (nf != 0 && nf <= 0x80000000u) st->apng_frames = (int)(nf > 1000000 ? 1000000 : nf);
        }
    } else if (!memcmp(cid, "fcTL", 4)) {
        if (len < 26) { sset(err, ec, "APNG contains truncated fcTL chunk"); return -1; }
    } else if (!memcmp(cid, "zTXt", 4)) {
        const unsigned char *z = memchr(d, 0, len);
        if (z && (size_t)(z - d) + 1 < len && z[1] != 0) { sset(err, ec, "Unknown compression method in zTXt chunk"); return -1; }
    }
    return 0;
}

typedef struct { size_t cstart, clen, fstart; } idat_piece_src;

/* piece end (concatenated offset) of the 64 KiB Pillow read that contains byte q */
static size_t png_piece_end(const idat_piece_src *ch, int nch, size_t q, size_t *file_end)
{
    int i;
    for (i = 0; i < nch; i++) {
        if (ch[i].clen && q >= ch[i].cstart && q < ch[i].cstart + ch[i].clen) {
            size_t rel = q - ch[i].cstart, pe = (rel / 65536 + 1) * 65536;
            if (pe > ch[i].clen) pe = ch[i].clen;
            *file_end = ch[i].fstart + pe;
            return ch[i].cstart + pe;
        }
    }
    *file_end = nch ? ch[nch - 1].fstart + ch[nch - 1].clen : 0;
    return q + 1;
}
static void png_unfilter(unsigned char *cur, const unsigned char *prev, size_t rb, size_t bpp, int ft)
{
    size_t i;
    switch (ft) {
    case 1:
        for (i = bpp; i < rb; i++) cur[i] = (unsigned char)(cur[i] + cur[i - bpp]);
        break;
    case 2:
        if (prev)
            for (i = 0; i < rb; i++) cur[i] = (unsigned char)(cur[i] + prev[i]);
        break;
    case 3:
        for (i = 0; i < rb; i++) {
            unsigned a = i >= bpp ? cur[i - bpp] : 0, b = prev ? prev[i] : 0;
            cur[i] = (unsigned char)(cur[i] + ((a + b) >> 1));
        }
        break;
    case 4:
        for (i = 0; i < rb; i++) {
            int a = i >= bpp ? cur[i - bpp] : 0, b = prev ? prev[i] : 0, c = (prev && i >= bpp) ? prev[i - bpp] : 0;
            int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
            int pr = (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
            cur[i] = (unsigned char)(cur[i] + pr);
        }
        break;
    default:
        break;
    }
}
static unsigned l24(unsigned r, unsigned g, unsigned b) { return (r * 19595u + g * 38470u + b * 7471u + 0x8000u) >> 16; }

#define PIL_MAX_PIXELS 89478485ull

/* Decode like Image.open() + strip_cleaning.darkness(): fills hist (of gray g) and optionally gray[].
 * 0 ok, -1 error (p->err) */
static int png_decode2(const unsigned char *d, size_t n, pngimg *P, int want_gray, int want_src);
static int png_decode(const unsigned char *d, size_t n, pngimg *P, int want_gray) { return png_decode2(d, n, P, want_gray, 0); }
/* want_src: also luma_na (colour luma ignoring alpha) and alpha planes */
static int png_decode2(const unsigned char *d, size_t n, pngimg *P, int want_gray, int want_src)
{
    static const unsigned char SIG[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    static const int A7[7][4] = {{0, 0, 8, 8}, {4, 0, 8, 8}, {0, 4, 4, 8}, {2, 0, 4, 4}, {0, 2, 2, 4}, {1, 0, 2, 2}, {0, 1, 1, 2}};
    pngstate st;
    size_t pos = 8, q, clen = 0, rawtot = 0, sampsz, npix, fpos = 0;
    idat_piece_src *chs = NULL;
    int nch = 0, cap_ch = 0, rc = -1, npass, pi, done = 0, bps;
    unsigned char *cat = NULL, *raw = NULL, *samp = NULL, *ubuf = NULL;
    size_t maxrb = 0;
    infl *zs = NULL;
    uint32_t w, h;
    int bd, ct, ch;
    memset(P->hist, 0, sizeof P->hist);
    P->gray = NULL;
    P->luma_na = NULL;
    P->alpha = NULL;
    P->has_alpha = 0;
    P->err[0] = 0;
    memset(&st, 0, sizeof st);
    if (n < 8 || memcmp(d, SIG, 8) != 0) { sset(P->err, sizeof P->err, "not a PNG file"); return -1; }
    /* ---- chunks before the first IDAT (PngImageFile._open: handler + CRC check) */
    for (;;) {
        uint32_t len;
        const unsigned char *cid;
        if (pos + 8 > n) { sset(P->err, sizeof P->err, "truncated PNG (chunk header)"); goto out; }
        len = be32(d + pos);
        cid = d + pos + 4;
        if (!is_cid(cid)) { sset(P->err, sizeof P->err, "broken PNG file (chunk type)"); goto out; }
        if (!memcmp(cid, "IDAT", 4)) break;
        if (!memcmp(cid, "IEND", 4)) { sset(P->err, sizeof P->err, "no image data"); goto out; }
        if ((uint64_t)pos + 8 + len > n) { sset(P->err, sizeof P->err, "Truncated File Read"); goto out; }
        if (png_chunk(&st, cid, d + pos + 8, len, 1, P->err, sizeof P->err) < 0) goto out;
        if ((uint64_t)pos + 12 + len > n) { sset(P->err, sizeof P->err, "broken PNG file (incomplete checksum)"); goto out; }
        if (crc32_update(crc32_update(0, cid, 4), d + pos + 8, len) != be32(d + pos + 8 + len)) {
            sset(P->err, sizeof P->err, "broken PNG file (bad header checksum in %.4s)", (const char *)cid);
            goto out;
        }
        pos += 12 + (size_t)len;
    }
    if (!st.have_mode || !st.have_size || st.w == 0 || st.h == 0) { sset(P->err, sizeof P->err, "cannot identify image file"); goto out; }
    if ((uint64_t)st.w * st.h > 2 * PIL_MAX_PIXELS) { sset(P->err, sizeof P->err, "DecompressionBombError (%u x %u)", st.w, st.h); goto out; }
    w = st.w;
    h = st.h;
    bd = st.bd;
    ct = st.ct;
    ch = ct == 0 ? 1 : ct == 2 ? 3 : ct == 3 ? 1 : ct == 4 ? 2 : 4;
    P->w = w;
    P->h = h;
    P->bd = bd;
    P->ct = ct;
    P->ilace = st.ilace;
    P->ch = ch;
    P->apng = st.apng_frames > 0;
    P->has_phys = st.has_phys;
    memcpy(P->phys, st.phys, 9);
    /* ---- the run of IDAT chunks Pillow reads (load_read) */
    q = pos;
    for (;;) {
        uint32_t len;
        size_t ds, avail, take;
        if (q + 8 > n) break;
        len = be32(d + q);
        if (!is_cid(d + q + 4)) break;
        if (memcmp(d + q + 4, "IDAT", 4) && memcmp(d + q + 4, "DDAT", 4)) break;
        ds = q + 8;
        avail = n - ds;
        take = len < avail ? len : avail;
        if (nch == cap_ch) {
            int nc = cap_ch ? cap_ch * 2 : 16;
            idat_piece_src *t = realloc(chs, (size_t)nc * sizeof *chs);
            if (!t) { sset(P->err, sizeof P->err, "out of memory"); goto out; }
            chs = t;
            cap_ch = nc;
        }
        chs[nch].cstart = clen;
        chs[nch].clen = take;
        chs[nch].fstart = ds;
        nch++;
        clen += take;
        if (take < len) break;
        q = ds + (size_t)len + 4;
    }
    P->idat_count = nch;
    cat = malloc(clen ? clen : 1);
    if (!cat) { sset(P->err, sizeof P->err, "out of memory"); goto out; }
    for (pi = 0; pi < nch; pi++) memcpy(cat + chs[pi].cstart, d + chs[pi].fstart, chs[pi].clen);
    /* ---- passes and buffers */
    npass = st.ilace ? 7 : 1;
    bps = bd == 16 ? 2 : 1;
    for (pi = 0; pi < npass; pi++) {
        uint64_t pw, ph, rb;
        if (st.ilace) {
            pw = w > (uint32_t)A7[pi][0] ? (w - A7[pi][0] + A7[pi][2] - 1) / A7[pi][2] : 0;
            ph = h > (uint32_t)A7[pi][1] ? (h - A7[pi][1] + A7[pi][3] - 1) / A7[pi][3] : 0;
        } else {
            pw = w;
            ph = h;
        }
        if (!pw || !ph) continue;
        rb = (pw * (uint64_t)ch * (uint64_t)bd + 7) / 8;
        if (rb + 1 > ((uint64_t)SIZE_MAX / 4) / ph) { sset(P->err, sizeof P->err, "image too large"); goto out; }
        rawtot += (size_t)(ph * (rb + 1));
        if (rb > maxrb) maxrb = (size_t)rb;
    }
    npix = (size_t)w * h;
    sampsz = npix * (size_t)ch * (size_t)bps;
    raw = malloc(rawtot ? rawtot : 1);
    samp = calloc(sampsz ? sampsz : 1, 1);
    zs = malloc(sizeof *zs);
    if (!raw || !samp || !zs) { sset(P->err, sizeof P->err, "out of memory"); goto out; }
    infl_init(zs, cat, clen, raw, rawtot);
    /* ---- rows (ZipDecode: one row per inflate output buffer) */
    {
        int last_pass = -1;
        for (pi = 0; pi < npass; pi++) {
            uint64_t pw = st.ilace ? (w > (uint32_t)A7[pi][0] ? (w - A7[pi][0] + A7[pi][2] - 1) / A7[pi][2] : 0) : w;
            uint64_t ph = st.ilace ? (h > (uint32_t)A7[pi][1] ? (h - A7[pi][1] + A7[pi][3] - 1) / A7[pi][3] : 0) : h;
            if (pw && ph) last_pass = pi;
        }
        for (pi = 0; pi < npass && !done; pi++) {
            int x0 = st.ilace ? A7[pi][0] : 0, y0 = st.ilace ? A7[pi][1] : 0;
            int dx = st.ilace ? A7[pi][2] : 1, dy = st.ilace ? A7[pi][3] : 1;
            uint64_t pw = w > (uint32_t)x0 ? (w - x0 + dx - 1) / dx : 0, ph = h > (uint32_t)y0 ? (h - y0 + dy - 1) / dy : 0;
            size_t rb, bpp, r;
            int have_prev = 0;
            if (!pw || !ph) continue;
            rb = (size_t)((pw * (uint64_t)ch * (uint64_t)bd + 7) / 8);
            bpp = (size_t)(ch * bd) / 8;
            if (bpp < 1) bpp = 1;
            if (!ubuf) {
                ubuf = malloc(2 * (maxrb + 1));
                if (!ubuf) { sset(P->err, sizeof P->err, "out of memory"); goto out; }
            }
            for (r = 0; r < ph; r++) {
                unsigned char *rawrow = raw + zs->out_pos, *row = ubuf + (r & 1) * (maxrb + 1),
                              *prev = ubuf + ((r + 1) & 1) * (maxrb + 1);
                int zr = infl_produce(zs, rb + 1), la;
                size_t c, pe, lastbyte;
                uint64_t yy = y0 + (uint64_t)r * dy;
                if (zr < 0) { sset(P->err, sizeof P->err, "broken data stream (%s)", zs->err); goto out; }
                if (zr != 1) { sset(P->err, sizeof P->err, "image file is truncated"); goto out; }
                if (rawrow[0] > 4) { sset(P->err, sizeof P->err, "unrecognized data stream contents (filter %d)", rawrow[0]); goto out; }
                /* unfilter into a separate buffer: the inflate output must stay intact for back-references */
                memcpy(row, rawrow, rb + 1);
                png_unfilter(row + 1, have_prev ? prev + 1 : NULL, rb, bpp, row[0]);
                have_prev = 1;
                /* scatter samples */
                for (c = 0; c < pw; c++) {
                    size_t xx = (size_t)(x0 + (uint64_t)c * dx), o = ((size_t)yy * w + xx) * (size_t)ch;
                    int k;
                    for (k = 0; k < ch; k++) {
                        size_t si = c * (size_t)ch + (size_t)k;
                        if (bd == 16) {
                            uint16_t v = (uint16_t)((row[1 + 2 * si] << 8) | row[2 + 2 * si]);
                            memcpy(samp + 2 * (o + (size_t)k), &v, 2);
                        } else if (bd == 8) {
                            samp[o + (size_t)k] = row[1 + si];
                        } else {
                            size_t bit = si * (size_t)bd;
                            samp[o + (size_t)k] = (unsigned char)((row[1 + (bit >> 3)] >> (8 - bd - (int)(bit & 7))) & ((1 << bd) - 1));
                        }
                    }
                }
                lastbyte = (size_t)((zs->br.pos + 7) / 8) - 1;
                pe = png_piece_end(chs, nch, lastbyte, &fpos);
                la = infl_lookahead(zs, (uint64_t)pe * 8, zs->err, sizeof zs->err);
                if (la < 0) { sset(P->err, sizeof P->err, "broken data stream (%s)", zs->err); goto out; }
                if (pi == last_pass && r + 1 == ph) { done = 1; break; }
                if (la == 1) { done = 1; break; } /* Z_STREAM_END at a row boundary: Pillow stops, rest stays 0 */
            }
        }
    }
    /* ---- ImageFile.load_end(): walk the chunks after the image data from the current file position */
    {
        size_t fp = fpos;
        for (;;) {
            uint32_t len;
            const unsigned char *cid;
            fp += 4;
            if (fp + 8 > n || fp + 8 < fp) break;
            len = be32(d + fp);
            cid = d + fp + 4;
            if (!is_cid(cid)) break;
            fp += 8;
            if (!memcmp(cid, "IEND", 4)) break;
            if (!memcmp(cid, "fcTL", 4) && st.apng_frames > 1) break;
            if ((uint64_t)fp + len > n) { sset(P->err, sizeof P->err, "Truncated File Read (chunk after image data)"); goto out; }
            if (png_chunk(&st, cid, d + fp, len, 0, P->err, sizeof P->err) < 0) goto out;
            fp += len;
        }
    }
    /* ---- Pillow palette: entries beyond PLTE are black, alpha 255; tRNS alphas for the first entries */
    {
        int i;
        for (i = 0; i < 256; i++) {
            P->pal[i][0] = P->pal[i][1] = P->pal[i][2] = 0;
            P->pal[i][3] = 255;
        }
        if (ct == 3) {
            for (i = 0; i < st.plte_n; i++) memcpy(P->pal[i], st.plte + 3 * i, 3);
            if (st.trns_set) {
                if (st.trns_n > 256) { sset(P->err, sizeof P->err, "palette index out of range (tRNS)"); goto out; }
                for (i = 0; i < st.trns_n; i++) P->pal[i][3] = st.trns[i];
            }
        }
    }
    /* ---- gray g = 255 - darkness (strip_cleaning.darkness) */
    {
        size_t i;
        unsigned top = 255;
        if (want_gray) {
            P->gray = malloc(npix ? npix : 1);
            if (!P->gray) { sset(P->err, sizeof P->err, "out of memory"); goto out; }
        }
        if (ct == 0 && bd == 16) {
            for (i = 0; i < npix; i++) {
                uint16_t v;
                memcpy(&v, samp + 2 * i, 2);
                if (v > 255) { top = 65535; break; }
            }
        }
        if (want_src) {
            P->luma_na = malloc(npix ? npix : 1);
            P->alpha = malloc(npix ? npix : 1);
            if (!P->luma_na || !P->alpha) { sset(P->err, sizeof P->err, "out of memory"); goto out; }
        }
        P->has_alpha = ct == 4 || ct == 6 || (ct == 3 && st.trns_set) || ((ct == 0 || ct == 2) && st.key_set);
        for (i = 0; i < npix; i++) {
            unsigned g, lna, a = 255;
            const unsigned char *s8 = samp + i * (size_t)ch;
            uint16_t s16[4] = {0, 0, 0, 0};
            if (bd == 16) memcpy(s16, samp + 2 * i * (size_t)ch, 2 * (size_t)ch);
            switch (ct) {
            case 0:
                if (bd == 16) {
                    double t = nearbyint((double)s16[0] * 255.0 / (double)top);
                    g = t < 0 ? 0 : t > 255 ? 255 : (unsigned)t;
                } else if (bd == 8) g = s8[0];
                else if (bd == 4) g = s8[0] * 17u;
                else if (bd == 2) g = s8[0] * 85u;
                else g = s8[0] ? 255u : 0u;
                lna = g;
                if (st.key_set && (bd == 16 ? (unsigned)s16[0] : (unsigned)s8[0]) == st.key[0]) a = 0;
                break;
            case 2:
                g = bd == 16 ? l24(s16[0] >> 8, s16[1] >> 8, s16[2] >> 8) : l24(s8[0], s8[1], s8[2]);
                lna = g;
                if (st.key_set && (bd == 16 ? ((unsigned)s16[0] == st.key[0] && (unsigned)s16[1] == st.key[1] && (unsigned)s16[2] == st.key[2])
                                            : ((unsigned)s8[0] == st.key[0] && (unsigned)s8[1] == st.key[1] && (unsigned)s8[2] == st.key[2])))
                    a = 0;
                break;
            case 3: {
                const unsigned char *e = P->pal[s8[0]];
                lna = l24(e[0], e[1], e[2]);
                a = e[3];
                g = e[3] < 128 ? 255u : lna;
                break;
            }
            case 4: {
                unsigned L = bd == 16 ? (unsigned)(s16[0] >> 8) : s8[0], A = bd == 16 ? (unsigned)(s16[1] >> 8) : s8[1];
                lna = L;
                a = A;
                g = A < 128 ? 255u : L;
                break;
            }
            default: {
                unsigned A = bd == 16 ? (unsigned)(s16[3] >> 8) : s8[3];
                lna = bd == 16 ? l24(s16[0] >> 8, s16[1] >> 8, s16[2] >> 8) : l24(s8[0], s8[1], s8[2]);
                a = A;
                g = A < 128 ? 255u : lna;
                break;
            }
            }
            P->hist[g]++;
            if (P->gray) P->gray[i] = (unsigned char)g;
            if (want_src) {
                P->luma_na[i] = (unsigned char)lna;
                P->alpha[i] = (unsigned char)a;
            }
        }
    }
    rc = 0;
out:
    free(chs);
    free(cat);
    free(raw);
    free(samp);
    free(ubuf);
    free(zs);
    if (rc != 0) {
        free(P->gray);
        free(P->luma_na);
        free(P->alpha);
        P->gray = P->luma_na = P->alpha = NULL;
    }
    return rc;
}

/* ================================================================== deflate (fixed Huffman, LZ77) + PNG writer */
typedef struct { buf_t *o; uint64_t acc; int n; } bitw;
static void bw_put(bitw *w, uint32_t v, int n)
{
    w->acc |= (uint64_t)v << w->n;
    w->n += n;
    while (w->n >= 8) {
        buf_putc(w->o, (int)(w->acc & 0xFF));
        w->acc >>= 8;
        w->n -= 8;
    }
}
static void bw_flush(bitw *w)
{
    if (w->n > 0) buf_putc(w->o, (int)(w->acc & 0xFF));
    w->acc = 0;
    w->n = 0;
}
static void put_litlen(bitw *w, int s)
{
    if (s <= 143) bw_put(w, rev_bits(0x30 + (uint32_t)s, 8), 8);
    else if (s <= 255) bw_put(w, rev_bits(0x190 + (uint32_t)(s - 144), 9), 9);
    else if (s <= 279) bw_put(w, rev_bits((uint32_t)(s - 256), 7), 7);
    else bw_put(w, rev_bits(0xC0 + (uint32_t)(s - 280), 8), 8);
}
#define LZ_WBITS 15
#define LZ_WSIZE (1u << LZ_WBITS)
#define LZ_HBITS 15
static int zlib_compress_fixed(const unsigned char *src, size_t n, buf_t *out)
{
    int32_t *head = malloc(sizeof(int32_t) << LZ_HBITS), *prev = malloc(sizeof(int32_t) * LZ_WSIZE);
    bitw bw;
    size_t i = 0;
    uint32_t ad;
    unsigned char tail[4];
    if (!head || !prev || n > 0x7FFFFFF0u) {
        free(head);
        free(prev);
        return -1;
    }
    memset(head, 0xFF, sizeof(int32_t) << LZ_HBITS);
    memset(prev, 0xFF, sizeof(int32_t) * LZ_WSIZE);
    buf_putc(out, 0x78);
    buf_putc(out, 0x9C);
    bw.o = out;
    bw.acc = 0;
    bw.n = 0;
    bw_put(&bw, 1, 1);
    bw_put(&bw, 1, 2);
#define LZ_HASH(p) ((((uint32_t)(p)[0] << 10) ^ ((uint32_t)(p)[1] << 5) ^ (uint32_t)(p)[2]) & ((1u << LZ_HBITS) - 1))
    while (i < n) {
        size_t best = 0, bdist = 0;
        if (i + 3 <= n) {
            uint32_t hh = LZ_HASH(src + i);
            int32_t cand = head[hh];
            int chain = 0;
            size_t maxl = n - i < 258 ? n - i : 258;
            while (cand >= 0 && (size_t)cand < i && i - (size_t)cand <= LZ_WSIZE && chain < 64) {
                const unsigned char *a = src + cand, *b = src + i;
                if (a[best] == b[best]) {
                    size_t l = 0;
                    while (l < maxl && a[l] == b[l]) l++;
                    if (l > best) {
                        best = l;
                        bdist = i - (size_t)cand;
                        if (l == maxl) break;
                    }
                }
                {
                    int32_t nx = prev[(size_t)cand & (LZ_WSIZE - 1)];
                    if (nx >= cand) break;
                    cand = nx;
                }
                chain++;
            }
            prev[i & (LZ_WSIZE - 1)] = head[hh];
            head[hh] = (int32_t)i;
        }
        if (best >= 3) {
            int ls = 28, dsy = 29;
            size_t k;
            while (LBASE[ls] > best) ls--;
            while (DBASE[dsy] > bdist) dsy--;
            put_litlen(&bw, 257 + ls);
            if (LEXT[ls]) bw_put(&bw, (uint32_t)(best - LBASE[ls]), LEXT[ls]);
            bw_put(&bw, rev_bits((uint32_t)dsy, 5), 5);
            if (DEXT[dsy]) bw_put(&bw, (uint32_t)(bdist - DBASE[dsy]), DEXT[dsy]);
            for (k = 1; k < best; k++) {
                size_t j = i + k;
                if (j + 3 <= n) {
                    uint32_t hh = LZ_HASH(src + j);
                    prev[j & (LZ_WSIZE - 1)] = head[hh];
                    head[hh] = (int32_t)j;
                }
            }
            i += best;
        } else {
            put_litlen(&bw, src[i]);
            i++;
        }
    }
#undef LZ_HASH
    put_litlen(&bw, 256);
    bw_flush(&bw);
    ad = adler32(src, n);
    put_be32(tail, ad);
    buf_put(out, tail, 4);
    free(head);
    free(prev);
    return out->oom ? -1 : 0;
}
static void png_put_chunk(buf_t *o, const char *type, const unsigned char *data, size_t n)
{
    unsigned char hdr[8], crc[4];
    put_be32(hdr, (uint32_t)n);
    memcpy(hdr + 4, type, 4);
    buf_put(o, hdr, 8);
    if (n) buf_put(o, data, n);
    put_be32(crc, crc32_update(crc32_update(0, (const unsigned char *)type, 4), data, n));
    buf_put(o, crc, 4);
}
/* 8-bit grayscale, non-interlaced; phys9 copied when given.  0 ok */
static int png_encode_gray8(uint32_t w, uint32_t h, const unsigned char *g, const unsigned char *phys9, buf_t *out)
{
    static const unsigned char SIG[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    size_t rb = w, total, y, i;
    unsigned char *raw, *cand, ihdr[13];
    buf_t z = {0};
    int rc = -1;
    if (!w || !h || (uint64_t)(rb + 1) * h > 0x7FFFFFF0u) return -1;
    total = (rb + 1) * (size_t)h;
    raw = malloc(total);
    cand = malloc(5 * rb);
    if (!raw || !cand) goto out;
    for (y = 0; y < h; y++) {
        const unsigned char *cur = g + y * rb, *prv = y ? g + (y - 1) * rb : NULL;
        unsigned long long best = ~0ull;
        int bf = 0, f;
        for (f = 0; f < 5; f++) {
            unsigned char *o = cand + (size_t)f * rb;
            unsigned long long sum = 0;
            for (i = 0; i < rb; i++) {
                int a = i ? cur[i - 1] : 0, b = prv ? prv[i] : 0, c = (prv && i) ? prv[i - 1] : 0, x = cur[i], pr;
                switch (f) {
                case 0: pr = 0; break;
                case 1: pr = a; break;
                case 2: pr = b; break;
                case 3: pr = (a + b) >> 1; break;
                default: {
                    int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
                    pr = (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
                }
                }
                o[i] = (unsigned char)(x - pr);
                sum += (unsigned)abs((signed char)o[i]);
            }
            if (sum < best) {
                best = sum;
                bf = f;
            }
        }
        raw[y * (rb + 1)] = (unsigned char)bf;
        memcpy(raw + y * (rb + 1) + 1, cand + (size_t)bf * rb, rb);
    }
    if (zlib_compress_fixed(raw, total, &z) != 0) goto out;
    buf_put(out, SIG, 8);
    put_be32(ihdr, w);
    put_be32(ihdr + 4, h);
    ihdr[8] = 8;
    ihdr[9] = 0;
    ihdr[10] = 0;
    ihdr[11] = 0;
    ihdr[12] = 0;
    png_put_chunk(out, "IHDR", ihdr, 13);
    if (phys9) png_put_chunk(out, "pHYs", phys9, 9);
    for (i = 0; i < z.n; i += (size_t)1 << 20) {
        size_t m = z.n - i < ((size_t)1 << 20) ? z.n - i : ((size_t)1 << 20);
        png_put_chunk(out, "IDAT", z.p + i, m);
    }
    png_put_chunk(out, "IEND", NULL, 0);
    rc = out->oom ? -1 : 0;
out:
    free(raw);
    free(cand);
    buf_free(&z);
    return rc;
}

/* ================================================================== keep-top models */
/* strip_cleaning.choose_k (verbatim logic, double arithmetic) */
static int choose_k(const double *covs, long long n, double pct, char *why, size_t wc)
{
    double c0 = n > 0 ? covs[0] : 0.0, kept;
    long long k = 0;
    why[0] = 0;
    if (pct <= 0 || c0 <= 0) return 0;
    while (k < n - 1 && covs[k] >= (1 - pct / 100.0) * c0) k++;
    kept = 1 - covs[k] / c0;
    if (kept < pct / 100.0) {
        sset(why, wc, "flat-tone image: no layer split keeps the brightest part");
        return 0;
    }
    if (kept > 0.5) {
        double v = kept * 100, f = floor(v), fr = v - f;
        long long r = (long long)f;
        if (fr > 0.5 || (fr == 0.5 && (r & 1))) r++;
        sset(why, wc, "flat top: %lld%% of the engraved area would stay", r);
        return 0;
    }
    return (int)k;
}
/* verified generator model (coordinator 2026-11-03): number of layers that burn gray g out of n */
static long long gen_layers(int g, long long n)
{
    long long c;
    if (g >= 255 || n <= 0) return 0;
    /* #{ i in [1, n-1] : g <= floor(256*(n-i)/n) }  <=>  i <= n - ceil(g*n/256) */
    c = n - ((long long)g * n + 255) / 256;
    if (c > n - 1) c = n - 1;
    if (c < 0) c = 0;
    return 1 + c;
}

#define RF_MAX_N 1000000LL

/* Vendor bug in lib_path_planning (coordinator, disassembly + real-generator runs): the per-layer "G200 V..F600"
 * line is only written in a layer's block at grid row 0 / col 0.  Layer j (1 <= j <= N-1) gets no V line iff
 * (j+1) % L == 0 and ((W > BW && (j*s) % BW == 0) || (H > BH && (j*s) % BH == 0)), s = max(W / N, 5) + 1. */
typedef struct { long long W, H, BW, BH, L; char why[160]; } vline_geo;

#define V_LIM (1LL << 62) /* values beyond this are treated as "not convertible" (Python would carry on with big ints) */
/* Python int(v) inside strip_cleaning.element_v_loss's try block: 1 ok, 0 = exception it catches
 * (KeyError for a missing key, TypeError, ValueError), -2 = OverflowError (escapes the try) */
static int v_int(arena_t *ar, const jval *v, long long *out)
{
    pyint p;
    char err[80];
    if (!v) return 0;
    if (v->t == J_FLOAT && isinf(v->u.f)) return -2;
    if (py_int(ar, v, &p, err, sizeof err) != 0) return 0;
    if (!pyint_to_ll(&p, out) || *out > V_LIM || *out < -V_LIM) return 0;
    return 1;
}
/* x[i] of a JSON value as Python indexes it (list item, or the i-th character of a str as a 1-char str) */
static const jval *v_index(arena_t *ar, const jval *x, size_t i, int *caught)
{
    *caught = 0;
    if (x && x->t == J_ARR) {
        if (i < x->u.arr.n) return x->u.arr.v[i];
        *caught = 1; /* IndexError */
        return NULL;
    }
    if (x && x->t == J_STR) {
        size_t pos = 0, k = 0;
        while (pos < x->u.s.n) {
            uint32_t cp;
            int l = u8_decode((const unsigned char *)x->u.s.p + pos, x->u.s.n - pos, &cp, 1);
            if (l < 1) l = 1;
            if (k == i) {
                jval *c = arena_alloc(ar, sizeof(jval));
                if (!c) break;
                memset(c, 0, sizeof *c);
                c->t = J_STR;
                c->u.s.p = x->u.s.p + pos;
                c->u.s.n = (size_t)l;
                return c;
            }
            k++;
            pos += (size_t)l;
        }
    }
    *caught = 1; /* IndexError / KeyError / TypeError: not subscriptable */
    return NULL;
}
/* bw = int(np.float32(bw_mm * d) / np.float32(25.4)): 1 ok, 0 too large for us, -2 Python raises (inf) */
static int v_block_px(long long mm, long long d, long long *out)
{
    __int128 p = (__int128)mm * d;
    double pd = (double)p;
    float pf, q;
    if (fabs(pd) > (double)FLT_MAX) return -2; /* float32 inf -> int(inf) raises OverflowError */
    pf = (float)pd;
    q = pf / 25.4f;                           /* float32 division as numpy does */
    if (!isfinite(q)) return -2;
    if (fabsf(q) > 4.0e18f) return 0;
    *out = (long long)q;
    return 1;
}
/* strip_cleaning.element_v_loss (11-03 08:12) / the DLL rule.  1 = lost(n) is defined (g filled),
 * 0 = no V lines in this job (zAxisMove off or vDecline 0), -1 = Python returns None (a value is missing or not
 * convertible), -2 = Python raises (OverflowError). */
static int vline_geo_of(arena_t *ar, const jval *root, const jval *rel, vline_geo *g)
{
    const jval *dpi, *params, *bs, *x;
    long long bwmm, bhmm, d0, d1;
    double vd = 0.0;
    int r, caught;
    char err[120];
    memset(g, 0, sizeof *g);
    if (!py_truthy(jget(rel, "zAxisMove"))) { sset(g->why, sizeof g->why, "zAxisMove off: no V lines"); return 0; }
    x = jget(rel, "vDecline");
    if (py_truthy(x) && py_float(x, &vd, err, sizeof err) != 0) {
        if (x->t == J_INT) { sset(g->why, sizeof g->why, "vDecline too large for a float"); return -2; }
        sset(g->why, sizeof g->why, "vDecline not a number");
        return -1;
    }
    if (vd == 0.0) { sset(g->why, sizeof g->why, "vDecline 0: no V lines"); return 0; }
    if ((r = v_int(ar, jget(rel, "width"), &g->W)) != 1 || (r = v_int(ar, jget(rel, "height"), &g->H)) != 1) {
        sset(g->why, sizeof g->why, "element width/height missing or not numeric");
        return r == -2 ? -2 : -1;
    }
    dpi = jget(rel, "dpi");
    if (!dpi) { sset(g->why, sizeof g->why, "element dpi missing"); return -1; }
    params = jget(root, "params");
    if (!py_truthy(params) || params->t != J_OBJ || !(bs = jget(params, "blockSize")) || bs->t != J_OBJ) {
        sset(g->why, sizeof g->why, "params.blockSize missing or not numeric");
        return -1;
    }
    if ((r = v_int(ar, jget(bs, "blockWidthmm"), &bwmm)) != 1 || (r = v_int(ar, jget(bs, "blockHeightmm"), &bhmm)) != 1) {
        sset(g->why, sizeof g->why, "params.blockSize missing or not numeric");
        return r == -2 ? -2 : -1;
    }
    if (jstr_is(jget(rel, "processHead"), "355nm-5W")) {
        if (bwmm > 30) bwmm = 30;
        if (bhmm > 30) bhmm = 30;
    }
    x = v_index(ar, dpi, 0, &caught);
    if (caught || (r = v_int(ar, x, &d0)) != 1) { sset(g->why, sizeof g->why, "element dpi missing or not numeric"); return !caught && r == -2 ? -2 : -1; }
    if ((r = v_block_px(bwmm, d0, &g->BW)) != 1) { sset(g->why, sizeof g->why, "block width out of range"); return r; }
    x = v_index(ar, dpi, 1, &caught);
    if (caught || (r = v_int(ar, x, &d1)) != 1) { sset(g->why, sizeof g->why, "element dpi missing or not numeric"); return !caught && r == -2 ? -2 : -1; }
    if ((r = v_block_px(bhmm, d1, &g->BH)) != 1) { sset(g->why, sizeof g->why, "block height out of range"); return r; }
    x = jget(rel, "vLayers");
    if (!py_truthy(x)) g->L = 1;
    else {
        r = v_int(ar, x, &g->L);
        if (r != 1) { sset(g->why, sizeof g->why, "vLayers not a number"); return r == -2 ? -2 : -1; }
        if (g->L < 1) g->L = 1;
    }
    return 1;
}
/* strip_cleaning.v_lost_layers: layers of an n-layer job without V line; fills out[0..cap), returns the count */
static long long vline_lost(long long n, const vline_geo *g, long long *out, long long cap)
{
    long long s, j, c = 0, q;
    if (n < 2 || !g->W || !g->H || !g->BW || !g->BH) return 0;
    q = g->W / n; /* Python floor division */
    if (g->W % n != 0 && g->W < 0) q--;
    s = (q > 5 ? q : 5) + 1;
    for (j = 1; j <= n - 1; j++) {
        __int128 js = (__int128)j * s;
        if ((j + 1) % g->L != 0) continue;
        if ((g->W > g->BW && js % g->BW == 0) || (g->H > g->BH && js % g->BH == 0)) {
            if (c < cap) out[c] = j;
            c++;
        }
    }
    return c;
}
/* strip_cleaning.avoid_v_loss: k, or the first of k+1..k+3, k-1..k-3 whose N-k loses no V line (the first with
 * the fewest if none is clean), keeping at most half of the engraved area at the surface */
static int avoid_v_loss(int k, long long n, const double *covs, const vline_geo *g, long long *m_k, long long *m_best)
{
    static const int off[6] = {1, 2, 3, -1, -2, -3};
    long long best_m, c, m;
    int best = k, q;
    *m_k = *m_best = 0;
    if (k <= 0) return k;
    best_m = *m_k = vline_lost(n - k, g, NULL, 0);
    if (!best_m) return k;
    for (q = 0; q < 6; q++) {
        c = k + off[q];
        if (c < 1 || c > n - 1 || 1 - covs[c] / covs[0] > 0.5) continue;
        m = vline_lost(n - c, g, NULL, 0);
        if (m < best_m) {
            best = (int)c;
            best_m = m;
        }
        if (m == 0) break;
    }
    *m_best = best_m;
    return best;
}

/* ================================================================== prepare / restore */
typedef struct {
    int have;
    long long bytes;
    char md5[33];
} fhash;

static void fhash_of(const buf_t *b, fhash *h)
{
    h->have = 1;
    h->bytes = (long long)b->n;
    md5_hex(b->p ? b->p : (const unsigned char *)"", b->n, h->md5);
}
static void jw_fhash(buf_t *o, const char *key, const fhash *h)
{
    buf_printf(o, ",\n  ");
    jw_cstr(o, key);
    if (!h->have) {
        buf_puts(o, ": null");
        return;
    }
    buf_printf(o, ": {\"bytes\": %lld, \"md5\": \"%s\"}", h->bytes, h->md5);
}
typedef struct {
    const char *state;
    int rc;
    int k, k_model;
    long long N, Nn;
    double pct, untouched, deepest;
    const char *note;
    const char *image; /* manifest filename (WTF-8) or NULL */
    fhash img_orig, man_orig, img_written, man_written;
    int have_lut;
    unsigned char lut[256];
    long long sc_pairs, sc_bad;
    int band_min_used, band_min_all;
    int napprox;              /* targets that no gray reaches with N' layers: (t, used) */
    long long approx[256][2];
    uint64_t approx_px;       /* pixels whose layer count deviates from exact keep-top */
    long long max_err;        /* 0 or 1 */
    const char *keep_skip;    /* why keep-top was not applied (NULL when applied) */
    int k_rule;               /* choose_k result before the V-line rule */
    int vrule;                /* 1 rule inputs known, 0 no V lines in this job, -1 inputs missing */
    long long *vlost, nvlost;     /* layers without V line in the job as generated (N' or N) */
    long long *vlost_v, nvlost_v; /* same for the original N (information) */
    int feather_off;          /* featherPower set to 0: the generator emits no cleaning passes */
    const char *fp_orig;      /* source text of the original numeric featherPower, NULL if absent / not a number */
    size_t fp_orig_n;
    int feather_keep;         /* 1..7: rf_restore_task keeps only that many of the 8 feather passes; 0 = no cut */
    const char *feather_keep_skip; /* why feather_passes 1..7 was not taken on (NULL when taken or not asked) */
    unsigned long long started_ft; /* prepare start, FILETIME (UTC); the cut only touches files newer than this */
    int tfix;                 /* transparent background taken from the pre-conversion copy */
    double tshare;            /* share of slicer pixels replaced */
    const char *tsource, *tskip;
    double t_ms;
} prep_rec;

static void rec_render(const prep_rec *r, buf_t *o)
{
    int i;
    buf_puts(o, "{\n  \"mode\": \"prepare\",\n  \"state\": ");
    jw_cstr(o, r->state);
    buf_printf(o, ",\n  \"rc\": %d,\n  \"k\": %d,\n  \"k_model\": %d,\n  \"N\": %lld,\n  \"N_new\": %lld,\n  \"pct\": ", r->rc,
               r->k, r->k_model, r->N, r->Nn);
    jw_double(o, r->pct);
    buf_puts(o, ",\n  \"untouched\": ");
    jw_double(o, r->untouched);
    buf_puts(o, ",\n  \"deepest_mm\": ");
    jw_double(o, r->deepest);
    buf_puts(o, ",\n  \"note\": ");
    jw_cstr(o, r->note ? r->note : "");
    buf_puts(o, ",\n  \"image\": ");
    if (r->image) jw_cstr(o, r->image);
    else buf_puts(o, "null");
    jw_fhash(o, "image_orig", &r->img_orig);
    jw_fhash(o, "manifest_orig", &r->man_orig);
    jw_fhash(o, "image_written", &r->img_written);
    jw_fhash(o, "manifest_written", &r->man_written);
    buf_puts(o, ",\n  \"keep_top_skip\": ");
    if (r->keep_skip) jw_cstr(o, r->keep_skip);
    else buf_puts(o, "null");
    buf_printf(o, ",\n  \"k_rule\": %d,\n  \"v_lost_layers\": ", r->k_rule);
    if (r->vrule >= 0) {
        long long q;
        buf_putc(o, '[');
        for (q = 0; q < r->nvlost && r->vlost; q++) buf_printf(o, q ? ", %lld" : "%lld", r->vlost[q]);
        buf_puts(o, "],\n  \"v_lost_layers_vendor_N\": [");
        for (q = 0; q < r->nvlost_v && r->vlost_v; q++) buf_printf(o, q ? ", %lld" : "%lld", r->vlost_v[q]);
        buf_putc(o, ']');
    } else {
        buf_puts(o, "null,\n  \"v_lost_layers_vendor_N\": null");
    }
    buf_printf(o, ",\n  \"feather_off\": %s,\n  \"feather_power_orig\": ", r->feather_off ? "true" : "false");
    if (r->fp_orig) buf_put(o, r->fp_orig, r->fp_orig_n);
    else buf_puts(o, "null");
    buf_printf(o, ",\n  \"feather_keep\": %d,\n  \"feather_keep_skip\": ", r->feather_keep);
    if (r->feather_keep_skip) jw_cstr(o, r->feather_keep_skip);
    else buf_puts(o, "null");
    buf_printf(o, ",\n  \"prepared_at_ft\": %llu", r->started_ft);
    buf_printf(o, ",\n  \"transparent_fixed\": %s,\n  \"transparent_share\": ", r->tfix ? "true" : "false");
    if (r->tfix) jw_double(o, r->tshare);
    else buf_puts(o, "null");
    buf_puts(o, ",\n  \"transparent_source\": ");
    if (r->tfix && r->tsource) jw_cstr(o, r->tsource);
    else buf_puts(o, "null");
    buf_puts(o, ",\n  \"transparent_skip\": ");
    if (!r->tfix && r->tskip) jw_cstr(o, r->tskip);
    else buf_puts(o, "null");
    buf_puts(o, ",\n  \"model\": \"layers(g,N) = 0 if g == 255 else 1 + #{i in [1,N-1]: g <= (256*(N-i))//N}\"");
    if (r->have_lut) {
        buf_puts(o, ",\n  \"approx\": [");
        for (i = 0; i < r->napprox; i++) buf_printf(o, i ? ", [%lld, %lld]" : "[%lld, %lld]", r->approx[i][0], r->approx[i][1]);
        buf_printf(o, "],\n  \"approx_pixels\": %llu,\n  \"max_layer_error\": %lld", (unsigned long long)r->approx_px, r->max_err);
        buf_printf(o, ",\n  \"selfcheck\": {\"pairs\": %lld, \"mismatches\": %lld}", r->sc_pairs, r->sc_bad);
        buf_printf(o, ",\n  \"band_min_dist_used\": %d,\n  \"band_min_dist_all\": %d", r->band_min_used, r->band_min_all);
        buf_puts(o, ",\n  \"lut\": [");
        for (i = 0; i < 256; i++) buf_printf(o, i ? ", %d" : "%d", r->lut[i]);
        buf_putc(o, ']');
    }
    buf_printf(o, ",\n  \"time_ms\": %.1f\n}\n", r->t_ms);
}
static int rec_write(const char *path, const prep_rec *r, char *err, size_t ec)
{
    buf_t o = {0};
    int rc;
    rec_render(r, &o);
    if (o.oom) { buf_free(&o); sset(err, ec, "out of memory"); return -1; }
    rc = write_replace(path, o.p, o.n, err, ec);
    buf_free(&o);
    return rc;
}
/* written-file info recorded by an earlier prepare */
typedef struct {
    int ok;               /* relief_fix.json parsed and mode == "prepare" */
    char *image;          /* malloc'd */
    fhash img_w, man_w;
    int done;             /* state == "done": that prepare finished */
    int feather_keep;     /* 1..7 asked for, else 0 */
    unsigned long long started_ft;
} old_rec;
/* feather pass cut after generation (G-code section below) */
typedef struct { int found, kept; long long size_in, size_out; size_t els_in, els_out; } ftrim_t;
static int feather_trim(const char *task, int keep, unsigned long long not_before, ftrim_t *ft, char *note, size_t nc);
static void old_rec_free(old_rec *r) { free(r->image); memset(r, 0, sizeof *r); }
static void fhash_from_json(const jval *v, fhash *h)
{
    const jval *b = jget(v, "bytes"), *m = jget(v, "md5");
    long long x;
    pyint pi;
    memset(h, 0, sizeof *h);
    if (!b || b->t != J_INT || !m || m->t != J_STR || m->u.s.n != 32) return;
    pi.neg = b->u.i.neg;
    pi.d = b->u.i.d;
    pi.n = b->u.i.n;
    if (!pyint_to_ll(&pi, &x)) return;
    h->have = 1;
    h->bytes = x;
    memcpy(h->md5, m->u.s.p, 32);
    h->md5[32] = 0;
}
static void old_rec_read(const char *path, old_rec *r)
{
    buf_t b = {0};
    DWORD we;
    arena_t ar = {0};
    char err[160];
    jval *v;
    memset(r, 0, sizeof *r);
    if (read_all(path, &b, &we) != 0) { buf_free(&b); return; }
    v = json_parse(b.p, b.n, &ar, err, sizeof err);
    if (v && v->t == J_OBJ && jstr_is(jget(v, "mode"), "prepare")) {
        const jval *im = jget(v, "image"), *fk = jget(v, "feather_keep"), *ft = jget(v, "prepared_at_ft");
        pyint pi;
        long long x;
        r->ok = 1;
        if (im && im->t == J_STR) r->image = str_dup_n(im->u.s.p, im->u.s.n);
        if (jget(v, "image_written")) fhash_from_json(jget(v, "image_written"), &r->img_w);
        if (jget(v, "manifest_written")) fhash_from_json(jget(v, "manifest_written"), &r->man_w);
        r->done = jstr_is(jget(v, "state"), "done");
        if (fk && fk->t == J_INT && fk->u.i.n == 1 && !fk->u.i.neg && fk->u.i.d[0] >= '1' && fk->u.i.d[0] <= '7')
            r->feather_keep = fk->u.i.d[0] - '0';
        if (ft && ft->t == J_INT && !ft->u.i.neg) {
            pi.neg = 0;
            pi.d = ft->u.i.d;
            pi.n = ft->u.i.n;
            if (pyint_to_ll(&pi, &x)) r->started_ft = (unsigned long long)x;
        }
    }
    arena_free(&ar);
    buf_free(&b);
}
/* does the file at path have exactly (bytes, md5)?  -1 missing */
static int file_matches(const char *path, const fhash *h)
{
    buf_t b = {0};
    DWORD we;
    char md[33];
    int r;
    r = read_all(path, &b, &we);
    if (r != 0) { buf_free(&b); return -1; }
    md5_hex(b.p ? b.p : (const unsigned char *)"", b.n, md);
    r = h->have && (long long)b.n == h->bytes && strcmp(md, h->md5) == 0;
    buf_free(&b);
    return r;
}
static int files_equal(const char *a, const char *b)
{
    buf_t x = {0}, y = {0};
    DWORD we;
    int r = 0;
    if (read_all(a, &x, &we) == 0 && read_all(b, &y, &we) == 0) r = x.n == y.n && (x.n == 0 || memcmp(x.p, y.p, x.n) == 0);
    buf_free(&x);
    buf_free(&y);
    return r;
}

/* the RELIEF elements of a parsed manifest (Python: [e for e in man.get("elements", []) if ...]) */
static int relief_elements(const jval *man, const jval **first, size_t *count, char *err, size_t ec)
{
    const jval *els;
    size_t i, n = 0;
    *first = NULL;
    *count = 0;
    if (!man || man->t != J_OBJ) { sset(err, ec, "manifest is not a JSON object"); return -1; }
    els = jget(man, "elements");
    if (!els) return 0;
    switch (els->t) {
    case J_ARR: break;
    case J_OBJ: if (els->u.obj.n == 0) return 0; sset(err, ec, "'elements' is a dict"); return -1;
    case J_STR: if (els->u.s.n == 0) return 0; sset(err, ec, "'elements' is a string"); return -1;
    default: sset(err, ec, "'elements' is not iterable"); return -1;
    }
    for (i = 0; i < els->u.arr.n; i++) {
        const jval *e = els->u.arr.v[i];
        if (e->t != J_OBJ) { sset(err, ec, "elements[%zu] is not a dict", i); return -1; }
        if (jstr_is(jget(e, "processingType"), "RELIEF")) {
            if (!n) *first = e;
            n++;
        }
    }
    *count = n;
    return 0;
}
/* image path of a relief element: os.path.join(task, str(e.get("filename") or "")); NULL if not representable */
static char *element_image_path(const char *task, const jval *e, char **fname_out)
{
    buf_t s = {0};
    const jval *f = jget(e, "filename");
    char *r;
    *fname_out = NULL;
    if (py_truthy(f)) {
        if (!py_str(f, &s)) { buf_free(&s); return NULL; }
    }
    if (s.oom) { buf_free(&s); return NULL; }
    *fname_out = str_dup_n(s.p ? (const char *)s.p : "", s.n);
    r = py_join(task, *fname_out ? *fname_out : "");
    buf_free(&s);
    return r;
}
/* deepest = (n-k) * per_layer if per_layer else None,
 * per_layer = float(vDecline or 0) / max(1, int(vLayers or 1)) if zAxisMove else None.  0 ok, -1 Python would raise */
static int deepest_calc(arena_t *ar, const jval *e, long long nk, double *out, char *err, size_t ec)
{
    double vd, per, lay;
    pyint vl;
    long long x;
    *out = NAN;
    if (!py_truthy(jget(e, "zAxisMove"))) return 0;
    if (!py_truthy(jget(e, "vDecline"))) vd = 0.0;
    else if (py_float(jget(e, "vDecline"), &vd, err, ec) != 0) return -1;
    if (py_int_or(ar, jget(e, "vLayers"), 1, &vl, err, ec) != 0) return -1;
    if (vl.neg || (vl.n == 1 && vl.d[0] <= '1')) lay = 1.0;
    else if (pyint_to_ll(&vl, &x)) lay = (double)x;
    else if (py_float_from_pyint(&vl, &lay, err, ec) != 0) return -1;
    per = vd / lay;
    if (per == 0.0) return 0;
    *out = (double)nk * per;
    return 0;
}
static double deepest_mm(arena_t *ar, const jval *e, long long nk, char *note, size_t nc)
{
    double v;
    char err[120];
    if (deepest_calc(ar, e, nk, &v, err, sizeof err) != 0) {
        note_add(note, nc, "deepest_mm not computed: %s", err);
        return NAN;
    }
    return v;
}

typedef struct {
    char *man, *man_bak, *rec, *img, *img_bak, *fname;
} prep_paths;
static void prep_paths_free(prep_paths *p)
{
    free(p->man);
    free(p->man_bak);
    free(p->rec);
    free(p->img);
    free(p->img_bak);
    free(p->fname);
    memset(p, 0, sizeof *p);
}

static double now_ms(void)
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

/* stale backups from a prepare whose restore did not run.  Returns 0 ok (resolved or none), < 0 error.
 * dry: decide only.  *use_bak_man / *use_bak_img: read the source from the backup (dry run only). */
static int resolve_stale(const char *task, const char *man, const char *man_bak, const char *cur_img_fname,
                         int dry, char *note, size_t nc, char **src_man, char **src_img_bak_for)
{
    old_rec orc;
    char *cands[2] = {NULL, NULL};
    int ncand = 0, i, rc = 0, act[3] = {0, 0, 0};
    const char *tgt[3] = {NULL, NULL, NULL}, *bak[3] = {NULL, NULL, NULL};
    char *own[3] = {NULL, NULL, NULL};
    char *recp = py_join(task, "relief_fix.json");
    *src_man = NULL;
    *src_img_bak_for = NULL;
    if (!recp) return RF_E_NOMEM;
    old_rec_read(recp, &orc);
    free(recp);
    /* candidate image file names: current manifest's and the one recorded by the earlier prepare */
    if (cur_img_fname) cands[ncand++] = (char *)cur_img_fname;
    if (orc.ok && orc.image && (!cur_img_fname || strcmp(orc.image, cur_img_fname) != 0)) cands[ncand++] = orc.image;
    /* slot 0: manifest, slots 1..: images */
    tgt[0] = man;
    bak[0] = man_bak;
    for (i = 0; i < ncand; i++) {
        char *ip = py_join(task, cands[i]);
        own[1 + i] = ip;
        tgt[1 + i] = ip;
        bak[1 + i] = NULL;
    }
    for (i = 0; i < 1 + ncand; i++) {
        char *bp;
        const fhash *wh;
        int m;
        if (i > 0) {
            if (!tgt[i]) continue;
            bp = str_cat(tgt[i], ".rf_orig");
            bak[i] = bp;
        }
        if (!bak[i] || !path_exists(bak[i])) { act[i] = 0; continue; }
        wh = NULL;
        if (orc.ok) {
            if (i == 0 && orc.man_w.have) wh = &orc.man_w;
            if (i > 0 && orc.img_w.have && orc.image && strcmp(cands[i - 1], orc.image) == 0) wh = &orc.img_w;
        }
        if (wh) {
            m = file_matches(tgt[i], wh);
            act[i] = m == 1 ? 1 : 2; /* 1 restore over our stale modification, 2 Studio wrote a new file: drop backup */
        } else {
            act[i] = files_equal(tgt[i], bak[i]) ? 2 : 3; /* 3 = cannot attribute */
        }
    }
    for (i = 0; i < 1 + ncand; i++) {
        if (act[i] == 3) {
            note_add(note, nc, "stale backup %s cannot be attributed (no matching relief_fix.json record); nothing changed", bak[i]);
            rc = RF_E_STATE;
        }
    }
    if (rc == 0) {
        for (i = 0; i < 1 + ncand; i++) {
            if (!act[i]) continue;
            if (dry) {
                if (act[i] == 1) {
                    if (i == 0) *src_man = str_dup_n(bak[i], strlen(bak[i]));
                    else *src_img_bak_for = str_dup_n(tgt[i], strlen(tgt[i]));
                }
                note_add(note, nc, "dry run: would %s %s", act[i] == 1 ? "restore stale backup" : "delete stale backup", bak[i]);
                continue;
            }
            if (act[i] == 1) {
                if (!path_move_replace(bak[i], tgt[i])) {
                    note_add(note, nc, "cannot restore stale backup %s (Win32 error %lu)", bak[i], GetLastError());
                    rc = RF_E_IO;
                } else {
                    note_add(note, nc, "restored stale backup %s (our earlier modification was still in place)", bak[i]);
                }
            } else {
                if (!path_delete(bak[i])) {
                    note_add(note, nc, "cannot delete stale backup %s (Win32 error %lu)", bak[i], GetLastError());
                    rc = RF_E_IO;
                } else {
                    note_add(note, nc, "deleted stale backup %s (current file is a new one from Studio)", bak[i]);
                }
            }
        }
    }
    for (i = 1; i < 3; i++) {
        free(own[i]);
        free((char *)bak[i]);
    }
    old_rec_free(&orc);
    return rc;
}

/* ---- transparent background.  Studio's bitmapCv turns transparent pixels of the user's canvas image WHITE in
 * the slicer image; depth maps that store their background as transparent black then come out inverted.  The
 * pre-conversion copy (<task>\..\<filename> or <task>\..\preview\<head>\<filename>) still has the colour. */
typedef struct {
    int fixed;             /* at least one slicer pixel changed */
    double share;          /* replaced / slicer pixels */
    long long replaced, changed;
    double mean_diff;      /* mean |slicer gray - copy luma| over opaque pixels */
    char source[1100];     /* the copy used */
    char skip[600];        /* why not applied */
    int noteworthy;        /* a copy was found but rejected */
} tfix_t;

static int jnum(const jval *v, double *out)
{
    char err[60];
    if (!v) return 0;
    if (v->t == J_FLOAT) { *out = v->u.f; return isfinite(v->u.f); }
    if (v->t == J_INT) return py_float(v, out, err, sizeof err) == 0;
    return 0;
}
/* 0 = done (t->fixed tells), < 0 = error */
static int transparent_fix(const char *task, const char *fname, const jval *rel, pngimg *S, tfix_t *t)
{
    char *base = NULL, *up = NULL, *c[2] = {NULL, NULL}, *pv = NULL, *pb = NULL, tried[500];
    pngimg src;
    buf_t b = {0};
    DWORD we;
    int ci, found = -1, rc = 0;
    uint32_t *sx = NULL, *sy = NULL;
    memset(t, 0, sizeof *t);
    memset(&src, 0, sizeof src);
    tried[0] = 0;
    if (!fname || !*fname) { sset(t->skip, sizeof t->skip, "no filename"); return 0; }
    {
        size_t n = strlen(task), s0;
        while (n > 0 && is_sep((unsigned char)task[n - 1])) n--;
        s0 = n;
        while (s0 > 0 && !is_sep((unsigned char)task[s0 - 1])) s0--;
        base = str_dup_n(task + s0, n - s0);
    }
    up = py_join(task, "..");
    c[0] = up ? py_join(up, fname) : NULL;
    pv = up ? py_join(up, "preview") : NULL;
    pb = pv && base ? py_join(pv, base) : NULL;
    c[1] = pb ? py_join(pb, fname) : NULL;
    if (!base || !c[0] || !c[1]) { rc = RF_E_NOMEM; goto out; }
    for (ci = 0; ci < 2; ci++) {
        size_t i, np;
        int transp = 0;
        const char *tag = ci == 0 ? "..\\<filename>" : "..\\preview\\<head>\\<filename>";
        if (!path_isfile(c[ci])) { note_add(tried, sizeof tried, "%s missing", tag); continue; }
        if (read_all(c[ci], &b, &we) != 0) { note_add(tried, sizeof tried, "%s unreadable", tag); continue; }
        free(src.luma_na);
        free(src.alpha);
        memset(&src, 0, sizeof src);
        if (png_decode2(b.p, b.n, &src, 0, 1) != 0) { note_add(tried, sizeof tried, "%s: %s", tag, src.err); continue; }
        np = (size_t)src.w * src.h;
        for (i = 0; i < np && src.has_alpha; i++)
            if (src.alpha[i] < 255) { transp = 1; break; }
        if (!transp) { note_add(tried, sizeof tried, "%s has no transparent pixel", tag); continue; }
        found = ci;
        break;
    }
    if (found < 0) { sset(t->skip, sizeof t->skip, "no pre-conversion copy with transparency (%s)", tried); goto out; }
    sset(t->source, sizeof t->source, "%s", c[found]);
    t->noteworthy = 1;
    {
        const jval *tr = jget(rel, "transform");
        double a, b1, c1;
        if (!tr || tr->t != J_ARR || tr->u.arr.n < 3 || !jnum(tr->u.arr.v[0], &a) || !jnum(tr->u.arr.v[1], &b1) || !jnum(tr->u.arr.v[2], &c1)) {
            sset(t->skip, sizeof t->skip, "element transform missing or not numeric");
            goto out;
        }
        if (fabs(b1) > 1e-6 * fabs(a) || fabs(c1) > 1e-6 * fabs(a)) {
            sset(t->skip, sizeof t->skip, "element transform has rotation/skew (%g, %g, %g)", a, b1, c1);
            goto out;
        }
    }
    {
        double rs = (double)src.w / (double)src.h, rd = (double)S->w / (double)S->h;
        if (fabs(rs / rd - 1.0) > 0.02) {
            sset(t->skip, sizeof t->skip, "aspect ratio of the copy (%ux%u) differs from the slicer image (%ux%u) by more than 2%%", src.w, src.h, S->w, S->h);
            goto out;
        }
    }
    /* nearest neighbour: sx = floor((x + 0.5) * srcW / dstW) */
    sx = malloc((size_t)S->w * sizeof(uint32_t));
    sy = malloc((size_t)S->h * sizeof(uint32_t));
    if (!sx || !sy) { rc = RF_E_NOMEM; goto out; }
    {
        uint64_t x, y;
        for (x = 0; x < S->w; x++) {
            uint64_t v = ((2 * x + 1) * src.w) / (2 * (uint64_t)S->w);
            sx[x] = (uint32_t)(v < src.w ? v : src.w - 1);
        }
        for (y = 0; y < S->h; y++) {
            uint64_t v = ((2 * y + 1) * src.h) / (2 * (uint64_t)S->h);
            sy[y] = (uint32_t)(v < src.h ? v : src.h - 1);
        }
    }
    {
        uint64_t x, y, cnt = 0;
        double sum = 0;
        for (y = 0; y < S->h; y++)
            for (x = 0; x < S->w; x++) {
                size_t si = (size_t)sy[y] * src.w + sx[x], di = (size_t)y * S->w + x;
                if (src.alpha[si] == 255) {
                    sum += abs((int)S->gray[di] - (int)src.luma_na[si]);
                    cnt++;
                }
            }
        if (!cnt) { sset(t->skip, sizeof t->skip, "the copy has no opaque pixel to compare with"); goto out; }
        t->mean_diff = sum / (double)cnt;
        if (!(t->mean_diff < 12.0)) {
            sset(t->skip, sizeof t->skip, "the copy does not match the slicer image (mean |gray - luma| %.2f over %llu opaque pixels)", t->mean_diff,
                 (unsigned long long)cnt);
            goto out;
        }
        for (y = 0; y < S->h; y++)
            for (x = 0; x < S->w; x++) {
                size_t si = (size_t)sy[y] * src.w + sx[x], di = (size_t)y * S->w + x;
                if (src.alpha[si] < 255) {
                    t->replaced++;
                    if (S->gray[di] != src.luma_na[si]) {
                        t->changed++;
                        S->hist[S->gray[di]]--;
                        S->gray[di] = src.luma_na[si];
                        S->hist[S->gray[di]]++;
                    }
                }
            }
        if (!t->replaced) { sset(t->skip, sizeof t->skip, "no transparent pixel of the copy falls on the slicer grid"); goto out; }
        if (!t->changed) { sset(t->skip, sizeof t->skip, "the transparent pixels already have the copy's colour"); goto out; }
        t->share = (double)t->replaced / ((double)S->w * (double)S->h);
        t->fixed = 1;
        t->noteworthy = 0;
    }
out:
    free(base);
    free(up);
    free(c[0]);
    free(c[1]);
    free(pv);
    free(pb);
    free(sx);
    free(sy);
    free(src.luma_na);
    free(src.alpha);
    buf_free(&b);
    return rc;
}

/* read + decode the slicer image once: 1 ok, -2 not a file, -3 read error (*we), -4 decode error (pi->err), -5 APNG */
static int load_slicer(const char *path, buf_t *img, pngimg *pi, int *state, DWORD *we, fhash *orig, long long *size_in)
{
    if (*state) return *state;
    if (!path || !path_isfile(path)) return *state = -2;
    if (read_all(path, img, we) != 0) return *state = -3;
    fhash_of(img, orig);
    *size_in = (long long)img->n;
    if (png_decode(img->p, img->n, pi, 1) != 0) return *state = -4;
    if (pi->apng) return *state = -5;
    return *state = 1;
}

static void res_init(rf_result *r)
{
    memset(r, 0, sizeof *r);
    r->untouched = NAN;
    r->deepest_mm = NAN;
}

int rf_prepare_task(const char *task_dir_utf8, const rf_opts *opts, rf_result *res)
{
    rf_opts o;
    rf_result dummy;
    prep_paths P = {0};
    prep_rec R;
    buf_t man = {0}, img = {0}, nimg = {0}, nman = {0};
    arena_t ar = {0}, ar2 = {0};
    pngimg pi;
    char err[256], why[160], keep_why[300], fwhy[120], *src_man = NULL, *src_img_for = NULL, *cur_fname = NULL;
    const jval *root, *rel = NULL, *sn = NULL, *fp = NULL;
    size_t nrel = 0;
    pyint Np;
    long long N = 0, Nn = 0, i, j;
    int rc = 1, k = 0, backups_made = 0, dry, use_img_bak = 0, keep = 0, feather = 0, vr = -1;
    int img_state = 0, tfix = 0, chg_img = 0, chg_man = 0;
    const char *img_src = NULL;
    tfix_t tf;
    vline_geo vg;
    double t0 = now_ms(), *covs = NULL;
    long long lay_o[256], lay_n[256], *cnt = NULL;
    unsigned char lut[256];
    DWORD we;
    int r;

    memset(&pi, 0, sizeof pi);
    memset(&R, 0, sizeof R);
    memset(&vg, 0, sizeof vg);
    memset(&tf, 0, sizeof tf);
    R.vrule = -1;
    {
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        R.started_ft = ((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    }
    keep_why[0] = 0;
    fwhy[0] = 0;
    if (!res) res = &dummy;
    res_init(res);
    if (opts) o = *opts;
    else rf_opts_init(&o);
    dry = o.dry_run != 0;
    R.state = "started";
    R.pct = o.keep_top_pct;
    R.untouched = NAN;
    R.deepest = NAN;
    R.note = res->note;
    if (!task_dir_utf8 || !*task_dir_utf8) { sset(res->note, sizeof res->note, "no task folder given"); return RF_E_ARG; }
    P.man = py_join(task_dir_utf8, "manifest.json");
    P.rec = py_join(task_dir_utf8, "relief_fix.json");
    P.man_bak = P.man ? str_cat(P.man, ".rf_orig") : NULL;
    if (!P.man || !P.rec || !P.man_bak) { rc = RF_E_NOMEM; goto done_norec; }

    /* ---- current manifest (only to learn the image name for the stale-backup check) */
    r = read_all(P.man, &man, &we);
    if (r == 0) {
        root = json_parse(man.p, man.n, &ar, err, sizeof err);
        if (root && relief_elements(root, &rel, &nrel, err, sizeof err) == 0 && nrel == 1) {
            char *ip = element_image_path(task_dir_utf8, rel, &cur_fname);
            free(ip);
        }
    }
    arena_free(&ar);
    rel = NULL;
    nrel = 0;

    /* ---- stale backups (a previous restore did not run) */
    rc = resolve_stale(task_dir_utf8, P.man, P.man_bak, cur_fname, dry, res->note, sizeof res->note, &src_man, &src_img_for);
    if (rc < 0) goto done;
    rc = 1;
    /* ---- record for this job, written before anything else is touched */
    if (!dry) {
        R.state = "started";
        if (rec_write(P.rec, &R, err, sizeof err) != 0) {
            note_add(res->note, sizeof res->note, "cannot write relief_fix.json: %s", err);
            rc = RF_E_IO;
            goto done_norec;
        }
    }
    /* ---- manifest */
    r = read_all(src_man ? src_man : P.man, &man, &we);
    if (r != 0) {
        note_add(res->note, sizeof res->note, "cannot read manifest.json (Win32 error %lu)", we);
        rc = RF_E_IO;
        goto done;
    }
    R.man_orig.have = 0;
    fhash_of(&man, &R.man_orig);
    root = json_parse(man.p, man.n, &ar, err, sizeof err);
    if (!root) { note_add(res->note, sizeof res->note, "manifest.json: %s", err); rc = RF_E_JSON; goto done; }
    if (relief_elements(root, &rel, &nrel, err, sizeof err) != 0) { note_add(res->note, sizeof res->note, "manifest.json: %s", err); rc = RF_E_JSON; goto done; }
    if (nrel == 0) { note_add(res->note, sizeof res->note, "not a relief job"); rc = 1; goto done; }
    if (nrel > 1) { note_add(res->note, sizeof res->note, "%zu relief objects in one job", nrel); rc = 1; goto done; }
    /* ---- cleaning passes: featherPower = 0 makes the generator emit none of the trailing isCleaningLayer passes
     * (verified on the real generator; the rest of galvo.gc is identical to the output with those blocks cut).
     * Only a numeric value != 0 is changed, only with strip_cleaning on and reliefCleanUp false or absent.
     * feather_passes 1..7: no input change (the generator still makes its fixed 8 passes); the record asks
     * rf_restore_task to cut the passes after the first n.  With reliefCleanUp on the passes are a different set. */
    {
        const jval *cu = jget(rel, "reliefCleanUp");
        int fkeep = o.feather_passes >= 1 && o.feather_passes <= 7 ? o.feather_passes : 0;
        fp = jget(rel, "featherPower");
        if (fp && (fp->t == J_INT || (fp->t == J_FLOAT && isfinite(fp->u.f)))) {
            R.fp_orig = (const char *)man.p + fp->a; /* the number as written in manifest.json */
            R.fp_orig_n = fp->b - fp->a;
        }
        if (fkeep) {
            sset(fwhy, sizeof fwhy, "feather_passes %d asked for", fkeep);
            if (cu && cu->t != J_FALSE) {
                R.feather_keep_skip = "reliefCleanUp is on";
                note_add(res->note, sizeof res->note, "feather_passes %d not applied: reliefCleanUp is on", fkeep);
            } else {
                R.feather_keep = fkeep;
                note_add(res->note, sizeof res->note, "feather passes: the first %d of 8 kept, the rest cut after generation", fkeep);
            }
        }
        else if (!o.strip_cleaning) sset(fwhy, sizeof fwhy, "strip_cleaning is 0");
        else if (cu && cu->t != J_FALSE) sset(fwhy, sizeof fwhy, "reliefCleanUp is on");
        else if (!fp) sset(fwhy, sizeof fwhy, "featherPower absent");
        else if (fp->t != J_INT && fp->t != J_FLOAT) sset(fwhy, sizeof fwhy, "featherPower is not a number");
        else if ((fp->t == J_INT && fp->u.i.n == 1 && fp->u.i.d[0] == '0') || (fp->t == J_FLOAT && fp->u.f == 0.0))
            sset(fwhy, sizeof fwhy, "featherPower already 0");
        else if (jcount(rel, "featherPower") > 1) sset(fwhy, sizeof fwhy, "featherPower appears more than once");
        else feather = 1;
    }
    /* ---- geometry of the vendor V-line bug (block grid of this element); -2 (absurd values) = rule skipped here */
    vr = vline_geo_of(&ar, root, rel, &vg);
    if (vr < -1) vr = -1;
    R.vrule = vr;
    /* ---- sliceNumber (a value Python cannot convert aborts everything) and the slicer image path */
    sn = jget(rel, "sliceNumber");
    if (py_int_or(&ar, sn, 0, &Np, err, sizeof err) != 0) { note_add(res->note, sizeof res->note, "sliceNumber: %s", err); rc = RF_E_JSON; goto done; }
    free(P.fname);
    P.img = element_image_path(task_dir_utf8, rel, &P.fname);
    R.image = P.fname;
    if (P.img) {
        P.img_bak = str_cat(P.img, ".rf_orig");
        if (!P.img_bak) { rc = RF_E_NOMEM; goto done; }
        use_img_bak = src_img_for && strcmp(src_img_for, P.img) == 0; /* dry run over a stale modification */
        img_src = use_img_bak ? P.img_bak : P.img;
    }
    /* ---- transparent background (before darkness / choose_k; an input change of its own) */
    if (!o.transparent_as_color) {
        sset(tf.skip, sizeof tf.skip, "transparent_as_color is 0");
    } else if (!P.img) {
        sset(tf.skip, sizeof tf.skip, "no slicer image (filename is a list or dict)");
    } else {
        int st = load_slicer(img_src, &img, &pi, &img_state, &we, &R.img_orig, &res->size_in);
        if (st != 1) {
            sset(tf.skip, sizeof tf.skip, "slicer image %s", st == -2 ? "missing" : st == -3 ? "unreadable" : st == -4 ? "not decodable" : "is an animated PNG");
        } else {
            r = transparent_fix(task_dir_utf8, P.fname, rel, &pi, &tf);
            if (r < 0) { rc = r; goto done; }
            tfix = tf.fixed;
        }
    }
    R.tfix = tfix;
    R.tshare = tf.share;
    R.tsource = tf.source;
    R.tskip = tf.skip;
    /* ---- keep-top (a skip here only means "keep-top not applied"; errors still abort everything) */
    if (!pyint_to_ll(&Np, &N) || N > RF_MAX_N) {
        char t[64];
        pyint_str(&Np, t, sizeof t);
        sset(keep_why, sizeof keep_why, "sliceNumber %s not supported (limit %lld)", t, RF_MAX_N);
        N = 0;
        goto keep_end;
    }
    R.N = N;
    res->n_layers = (int)N;
    if (jcount(rel, "sliceNumber") > 1) { sset(keep_why, sizeof keep_why, "sliceNumber appears more than once"); goto keep_end; }
    if (!(o.keep_top_pct > 0)) { sset(keep_why, sizeof keep_why, "keep-top off (pct %g)", o.keep_top_pct); goto keep_end; }
    if (!P.img) { sset(keep_why, sizeof keep_why, "no slicer image (filename is a list or dict)"); goto keep_end; }
    if (N < 2 || !path_isfile(img_src)) {
        sset(keep_why, sizeof keep_why, "no slicer image or N=%lld", N);
        goto keep_end;
    }
    /* ---- image (already loaded, and possibly corrected, by the transparent-background step) */
    r = load_slicer(img_src, &img, &pi, &img_state, &we, &R.img_orig, &res->size_in);
    if (r == -2 || r == -3) { sset(keep_why, sizeof keep_why, "slicer image unreadable (Win32 error %lu)", we); goto keep_end; }
    if (r == -4) { sset(keep_why, sizeof keep_why, "slicer image unreadable: %s", pi.err); goto keep_end; }
    if (r == -5) { sset(keep_why, sizeof keep_why, "animated PNG not supported"); goto keep_end; }
    /* ---- k from the generator model */
    {
        uint64_t total = (uint64_t)pi.w * pi.h;
        covs = malloc((size_t)N * sizeof(double));
        cnt = calloc((size_t)N + 1, sizeof(long long));
        if (!covs || !cnt) { rc = RF_E_NOMEM; goto done; }
        for (i = 0; i < 256; i++) {
            lay_o[i] = gen_layers((int)i, N);
            if (pi.hist[i]) cnt[0] += (long long)pi.hist[i], cnt[lay_o[i]] -= (long long)pi.hist[i];
        }
        /* cnt[i] after prefix sum = pixels burned by layer i (layers(g) > i) */
        for (i = 1; i <= N; i++) cnt[i] += cnt[i - 1];
        for (i = 0; i < N; i++) covs[i] = (double)cnt[i] / (double)total;
        k = choose_k(covs, N, o.keep_top_pct, why, sizeof why);
        R.k = k;
        R.k_model = k;
        R.k_rule = k;
        res->k = k;
        if (k <= 0) {
            sset(keep_why, sizeof keep_why, "keep-top not applied: %s", why[0] ? why : "k = 0");
            goto keep_end;
        }
    }
    /* ---- V-line rule: an N' whose layers lose their V line is moved to the nearest unaffected one.
     * Order k, k+1, k+2, k+3, k-1, k-2, k-3 (1 <= k' <= N-1, kept_share(k') <= 0.5); the first that loses nothing,
     * else the first with the fewest lost layers. */
    if (vr == 1) {
        long long m0, mb;
        int best_k = avoid_v_loss(k, N, covs, &vg, &m0, &mb);
        if (m0 > 0)
            note_add(res->note, sizeof res->note, "V-line bug: N'=%lld would lose %lld V line(s); k %d -> %d (N'=%lld loses %lld)", N - k, m0,
                     k, best_k, N - best_k, mb);
        k = best_k;
        R.k = k;
        res->k = k;
    } else if (vr < 0) {
        note_add(res->note, sizeof res->note, "V-line rule skipped: %s", vg.why);
    }
    Nn = N - k;
    R.Nn = Nn;
    /* ---- LUT.  t(g) = max(0, layers(g,N) - k).  used(g) = t if some gray burns in exactly t of the N' layers,
     * else the nearest reachable count (ties to the smaller one; 0 = white, reached by v = 255).  With N' >= 128
     * layers 0 and 1 both burn every gray <= 254, so t = 1 is never reachable and becomes 0 (stays at the surface).
     * LUT = 255 for used 0, else the centre of { v <= 254 : layers(v,N') == used }.
     * -9 only if a gray would be off by more than one layer. */
    {
        int g, v, lo, hi, bmu = 999, bma = 999, nacnt = 0, a;
        long long acnt[257], used_of[256], t_of[256];
        uint64_t keep_white = 0;
        for (v = 0; v < 256; v++) lay_n[v] = gen_layers(v, Nn);
        for (v = 255; v >= 0; v--) /* lay_n is non-increasing in v: ascending distinct reachable counts */
            if (nacnt == 0 || acnt[nacnt - 1] != lay_n[v]) acnt[nacnt++] = lay_n[v];
        R.napprox = 0;
        R.approx_px = 0;
        R.max_err = 0;
        for (g = 0; g < 256; g++) {
            long long t = lay_o[g] - k, u = -1, bd = 0;
            if (t < 0) t = 0;
            for (a = 0; a < nacnt; a++) { /* ascending, so a tie keeps the smaller count */
                long long d = acnt[a] > t ? acnt[a] - t : t - acnt[a];
                if (u < 0 || d < bd) { u = acnt[a]; bd = d; }
            }
            t_of[g] = t;
            used_of[g] = u;
            if (bd > 1) {
                note_add(res->note, sizeof res->note,
                         "gray %d burns in %lld of %lld layers; with %lld layers the nearest reachable count is %lld (off by %lld > 1 layer, k=%d)",
                         g, lay_o[g], N, Nn, u, bd, k);
                rc = RF_E_LUT;
                goto done;
            }
            if (u != t) {
                int seen = 0, q;
                for (q = 0; q < R.napprox; q++)
                    if (R.approx[q][0] == t) seen = 1;
                if (!seen && R.napprox < 256) {
                    R.approx[R.napprox][0] = t;
                    R.approx[R.napprox][1] = u;
                    R.napprox++;
                }
                R.approx_px += pi.hist[g];
                if (bd > R.max_err) R.max_err = bd;
            }
            if (u == 0) { lut[g] = 255; continue; }
            lo = -1;
            hi = -1;
            for (v = 0; v <= 254; v++)
                if (lay_n[v] == u) {
                    if (lo < 0) lo = v;
                    hi = v;
                }
            if (lo < 0) { note_add(res->note, sizeof res->note, "internal: count %lld has no band", u); rc = RF_E_INTERNAL; goto done; }
            lut[g] = (unsigned char)((lo + hi) / 2);
            {
                int dd = lut[g] - lo < hi - lut[g] ? lut[g] - lo : hi - lut[g];
                if (dd < bma) bma = dd;
                if (pi.hist[g] && dd < bmu) bmu = dd;
            }
        }
        R.band_min_used = bmu == 999 ? -1 : bmu;
        R.band_min_all = bma == 999 ? -1 : bma;
        /* self-check: every g gets the planned count; for exact targets, every kept original layer j in [k, N)
         * burns g exactly when new layer j-k burns LUT[g]; approximated targets are off by at most one layer */
        R.sc_pairs = 0;
        R.sc_bad = 0;
        for (g = 0; g < 256; g++) {
            long long ln = gen_layers(lut[g], Nn), df = used_of[g] - t_of[g];
            if (ln != used_of[g]) R.sc_bad++;
            if (df != 0) {
                if (df > 1 || df < -1) R.sc_bad++;
                continue;
            }
            for (j = k; j < N; j++) {
                R.sc_pairs++;
                if ((lay_o[g] > j) != (ln > j - k)) R.sc_bad++;
            }
        }
        memcpy(R.lut, lut, 256);
        R.have_lut = 1;
        if (R.sc_bad) {
            note_add(res->note, sizeof res->note, "self-check failed: %lld mismatches (%lld (gray, layer) pairs checked)", R.sc_bad, R.sc_pairs);
            rc = RF_E_VERIFY;
            goto done;
        }
        for (g = 0; g < 256; g++)
            if (lut[g] == 255) keep_white += pi.hist[g];
        R.untouched = (double)keep_white / ((double)pi.w * (double)pi.h);
        res->untouched = R.untouched;
    }
    /* ---- new image: 8-bit gray, value LUT[g] */
    {
        size_t np = (size_t)pi.w * pi.h, q;
        pngimg chk;
        for (q = 0; q < np; q++) pi.gray[q] = lut[pi.gray[q]];
        if (png_encode_gray8(pi.w, pi.h, pi.gray, pi.has_phys ? pi.phys : NULL, &nimg) != 0) {
            note_add(res->note, sizeof res->note, "PNG encoding failed");
            rc = RF_E_NOMEM;
            goto done;
        }
        memset(&chk, 0, sizeof chk);
        if (png_decode(nimg.p, nimg.n, &chk, 1) != 0 || chk.w != pi.w || chk.h != pi.h || chk.ct != 0 || chk.bd != 8 ||
            memcmp(chk.gray, pi.gray, np) != 0) {
            note_add(res->note, sizeof res->note, "written PNG does not decode to the intended gray values (%s)", chk.err);
            free(chk.gray);
            rc = RF_E_VERIFY;
            goto done;
        }
        free(chk.gray);
        fhash_of(&nimg, &R.img_written);
        res->size_out = (long long)nimg.n;
    }
    keep = 1;
keep_end:
    if (keep) {
        note_add(res->note, sizeof res->note, "k=%d N=%lld->%lld; self-check %lld/%lld pairs ok; band min distance %d (used) %d (all); untouched %.4f",
                 k, N, Nn, R.sc_pairs - R.sc_bad, R.sc_pairs, R.band_min_used, R.band_min_all, R.untouched);
        if (R.napprox) {
            int q;
            sappend(res->note, sizeof res->note, "; approx");
            for (q = 0; q < R.napprox; q++) sappend(res->note, sizeof res->note, " %lld->%lld", R.approx[q][0], R.approx[q][1]);
            sappend(res->note, sizeof res->note, " (%llu px, max error %lld layer)", (unsigned long long)R.approx_px, R.max_err);
        }
    } else {
        /* keep-top not applied: k = 0, image untouched */
        k = 0;
        R.k = 0;
        Nn = N;
        R.Nn = N;
        R.keep_skip = keep_why;
        R.have_lut = 0;
        R.untouched = NAN;
        res->k = 0;
        res->untouched = NAN;
        res->size_out = res->size_in;
        note_add(res->note, sizeof res->note, "%s", keep_why);
        if (tfix) {
            /* identity LUT: the corrected image itself */
            size_t np = (size_t)pi.w * pi.h;
            pngimg chk;
            if (png_encode_gray8(pi.w, pi.h, pi.gray, pi.has_phys ? pi.phys : NULL, &nimg) != 0) {
                note_add(res->note, sizeof res->note, "PNG encoding failed");
                rc = RF_E_NOMEM;
                goto done;
            }
            memset(&chk, 0, sizeof chk);
            if (png_decode(nimg.p, nimg.n, &chk, 1) != 0 || chk.w != pi.w || chk.h != pi.h || chk.ct != 0 || chk.bd != 8 ||
                memcmp(chk.gray, pi.gray, np) != 0) {
                note_add(res->note, sizeof res->note, "written PNG does not decode to the intended gray values (%s)", chk.err);
                free(chk.gray);
                rc = RF_E_VERIFY;
                goto done;
            }
            free(chk.gray);
            fhash_of(&nimg, &R.img_written);
            res->size_out = (long long)nimg.n;
        }
    }
    if (tfix)
        note_add(res->note, sizeof res->note, "transparent background: %.2f%% of the pixels take the colour of the pre-conversion copy", tf.share * 100.0);
    else if (tf.noteworthy)
        note_add(res->note, sizeof res->note, "transparent background not fixed: %s", tf.skip);
    if (feather) note_add(res->note, sizeof res->note, "featherPower %.*s -> 0 (no cleaning passes)", (int)(fp->b - fp->a > 40 ? 40 : fp->b - fp->a), (const char *)man.p + fp->a);
    else note_add(res->note, sizeof res->note, "featherPower not changed: %s", fwhy);
    /* layers without V line in the job as it will be generated (N' or N), and in Studio's original N */
    if (vr == 1 && N >= 1) {
        long long nj = N - k;
        R.nvlost = vline_lost(nj, &vg, NULL, 0);
        R.nvlost_v = vline_lost(N, &vg, NULL, 0);
        R.vlost = malloc((size_t)(R.nvlost + 1) * sizeof(long long));
        R.vlost_v = malloc((size_t)(R.nvlost_v + 1) * sizeof(long long));
        if (!R.vlost || !R.vlost_v) { rc = RF_E_NOMEM; goto done; }
        vline_lost(nj, &vg, R.vlost, R.nvlost);
        vline_lost(N, &vg, R.vlost_v, R.nvlost_v);
    }
    chg_img = keep || tfix;
    chg_man = keep || feather;
    if (!chg_img && !chg_man && !R.feather_keep) { rc = 1; goto done; }
    /* ---- manifest: minimal edits of the sliceNumber and / or featherPower values, nothing else */
    if (chg_man) {
        size_t ea[2], eb[2], ne = 0, pos = 0, q;
        char et[2][48];
        const jval *root2, *rel2;
        size_t nrel2;
        int ok;
        if (keep) {
            ea[ne] = sn->a;
            eb[ne] = sn->b;
            snprintf(et[ne], sizeof et[ne], sn->t == J_STR ? "\"%lld\"" : "%lld", Nn);
            ne++;
        }
        if (feather) {
            ea[ne] = fp->a;
            eb[ne] = fp->b;
            snprintf(et[ne], sizeof et[ne], "0");
            ne++;
        }
        if (ne == 2 && ea[1] < ea[0]) {
            size_t ta = ea[0], tb = eb[0];
            char tt[48];
            memcpy(tt, et[0], sizeof tt);
            ea[0] = ea[1];
            eb[0] = eb[1];
            memcpy(et[0], et[1], sizeof tt);
            ea[1] = ta;
            eb[1] = tb;
            memcpy(et[1], tt, sizeof tt);
        }
        if (ne == 2 && eb[0] > ea[1]) { note_add(res->note, sizeof res->note, "internal: overlapping edits"); rc = RF_E_INTERNAL; goto done; }
        for (q = 0; q < ne; q++) {
            buf_put(&nman, man.p + pos, ea[q] - pos);
            buf_puts(&nman, et[q]);
            pos = eb[q];
        }
        buf_put(&nman, man.p + pos, man.n - pos);
        if (nman.oom) { rc = RF_E_NOMEM; goto done; }
        /* read it back: same document except the edited values, which hold what was intended */
        root2 = json_parse(nman.p, nman.n, &ar2, err, sizeof err);
        ok = root2 && relief_elements(root2, &rel2, &nrel2, err, sizeof err) == 0 && nrel2 == 1 &&
             jv_same(root, root2, keep ? sn : NULL, feather ? fp : NULL);
        if (ok && keep) {
            pyint np2;
            long long chkN;
            ok = py_int_or(&ar2, jget(rel2, "sliceNumber"), 0, &np2, err, sizeof err) == 0 && pyint_to_ll(&np2, &chkN) && chkN == Nn;
        }
        if (ok && feather) {
            const jval *f2 = jget(rel2, "featherPower");
            ok = f2 && f2->t == J_INT && f2->u.i.n == 1 && f2->u.i.d[0] == '0';
        }
        if (!ok) {
            note_add(res->note, sizeof res->note, "rewritten manifest.json does not read back as intended");
            rc = RF_E_VERIFY;
            goto done;
        }
        fhash_of(&nman, &R.man_written);
        R.feather_off = feather;
    }
    R.deepest = deepest_mm(&ar, rel, Nn, res->note, sizeof res->note);
    res->deepest_mm = R.deepest;
    if (dry) {
        note_add(res->note, sizeof res->note, "dry run: nothing written");
        rc = 0;
        goto done_norec;
    }
    /* ---- write: record (with what we are about to write), backups, files */
    R.state = "writing";
    R.rc = 0;
    R.t_ms = now_ms() - t0;
    if (rec_write(P.rec, &R, err, sizeof err) != 0) { note_add(res->note, sizeof res->note, "cannot write relief_fix.json: %s", err); rc = RF_E_IO; goto done; }
    if ((chg_img && path_exists(P.img_bak)) || (chg_man && path_exists(P.man_bak))) {
        note_add(res->note, sizeof res->note, "backup appeared while preparing; nothing changed");
        rc = RF_E_STATE;
        goto done;
    }
    if (chg_img) {
        if (!path_copy_new(P.img, P.img_bak)) { note_add(res->note, sizeof res->note, "cannot back up the image (Win32 error %lu)", GetLastError()); rc = RF_E_IO; goto done; }
        backups_made |= 1;
    }
    if (chg_man) {
        if (!path_copy_new(P.man, P.man_bak)) { note_add(res->note, sizeof res->note, "cannot back up manifest.json (Win32 error %lu)", GetLastError()); rc = RF_E_IO; goto rollback; }
        backups_made |= 2;
    }
    if (chg_img) {
        if (write_replace(P.img, nimg.p, nimg.n, err, sizeof err) != 0) { note_add(res->note, sizeof res->note, "%s", err); rc = RF_E_IO; goto rollback; }
        backups_made |= 4;
    }
    if (chg_man) {
        if (write_replace(P.man, nman.p, nman.n, err, sizeof err) != 0) { note_add(res->note, sizeof res->note, "%s", err); rc = RF_E_IO; goto rollback; }
    }
    rc = 0;
    R.state = "done";
    goto done;
rollback:
    /* a file that was replaced gets its backup moved back; a backup of a file that was not replaced is dropped */
    if (backups_made & 1) {
        if (backups_made & 4) {
            if (!path_move_replace(P.img_bak, P.img)) note_add(res->note, sizeof res->note, "ROLLBACK FAILED for the image: original is %s", P.img_bak);
            else note_add(res->note, sizeof res->note, "image restored from backup");
        } else if (!path_delete(P.img_bak)) {
            note_add(res->note, sizeof res->note, "could not delete the unused backup %s", P.img_bak);
        }
    }
    if (backups_made & 2) {
        if (!path_delete(P.man_bak)) note_add(res->note, sizeof res->note, "could not delete the unused backup %s", P.man_bak);
    }
done:
    if (!dry) {
        if (rc != 0) {
            /* after a rollback nothing of this job is on disk, so the record must not claim written files;
             * if a rollback failed, keep the hashes so the next prepare / restore can still sort it out */
            R.state = rc == 1 ? "skipped" : "error";
            R.k = 0;
            if (!(backups_made & 1) || !path_exists(P.img_bak)) {
                R.img_written.have = 0;
                R.tfix = 0;
            }
            if (!(backups_made & 2) || !path_exists(P.man_bak)) {
                R.man_written.have = 0;
                R.feather_off = 0;
            }
            if (R.feather_keep) {
                /* the proxy skips rf_restore_task after a failed prepare: no cut is pending */
                R.feather_keep = 0;
                R.feather_keep_skip = "prepare did not finish";
            }
        }
        R.rc = rc;
        R.t_ms = now_ms() - t0;
        if (rec_write(P.rec, &R, err, sizeof err) != 0) note_add(res->note, sizeof res->note, "cannot write relief_fix.json: %s", err);
    }
done_norec:
    if (rc != 0) res->k = 0;
    free(R.vlost);
    free(R.vlost_v);
    free(covs);
    free(cnt);
    free(pi.gray);
    free(src_man);
    free(src_img_for);
    free(cur_fname);
    buf_free(&man);
    buf_free(&img);
    buf_free(&nimg);
    buf_free(&nman);
    arena_free(&ar);
    arena_free(&ar2);
    prep_paths_free(&P);
    return rc;
}

int rf_restore_task(const char *task_dir_utf8)
{
    static const char *const RESTORE_KEYS[] = {"gcode_bytes", "gcode_mtime", "restored", "restore_note", "restore_rc",
                                               "feather_trim_rc", "feather_trim_note", "feather_passes_found", "feather_kept",
                                               "gcode_bytes_generated", "manifest_sliced_elements"};
    char *man = NULL, *man_bak = NULL, *recp = NULL, *img = NULL, *img_bak = NULL, *fname = NULL, *gc = NULL;
    old_rec orc;
    buf_t b = {0}, out = {0};
    arena_t ar = {0};
    char err[200], note[600], fnote[400];
    int rc = 1, any = 0, i, frc = 1, ftried = 0;
    ftrim_t ft;
    DWORD we;
    note[0] = 0;
    fnote[0] = 0;
    memset(&orc, 0, sizeof orc);
    memset(&ft, 0, sizeof ft);
    if (!task_dir_utf8 || !*task_dir_utf8) return RF_E_ARG;
    man = py_join(task_dir_utf8, "manifest.json");
    recp = py_join(task_dir_utf8, "relief_fix.json");
    man_bak = man ? str_cat(man, ".rf_orig") : NULL;
    if (!man || !recp || !man_bak) { rc = RF_E_NOMEM; goto out; }
    old_rec_read(recp, &orc);
    /* image name: from the record, else from the backed-up manifest, else from the current one */
    if (orc.ok && orc.image) {
        fname = str_dup_n(orc.image, strlen(orc.image));
    } else {
        const char *srcs[2] = {man_bak, man};
        for (i = 0; i < 2 && !fname; i++) {
            const jval *root, *rel;
            size_t nrel;
            if (read_all(srcs[i], &b, &we) != 0) continue;
            root = json_parse(b.p, b.n, &ar, err, sizeof err);
            if (root && relief_elements(root, &rel, &nrel, err, sizeof err) == 0 && nrel == 1) {
                char *ip = element_image_path(task_dir_utf8, rel, &fname);
                free(ip);
            }
            arena_free(&ar);
        }
    }
    if (fname) {
        img = py_join(task_dir_utf8, fname);
        img_bak = img ? str_cat(img, ".rf_orig") : NULL;
    }
    for (i = 0; i < 2; i++) {
        const char *t = i == 0 ? man : img, *bk = i == 0 ? man_bak : img_bak;
        const fhash *wh = i == 0 ? &orc.man_w : &orc.img_w;
        if (!t || !bk || !path_exists(bk)) continue;
        if (orc.ok && !wh->have) {
            /* the last prepare on this folder did not write this file: the backup is not from this job */
            note_add(note, sizeof note, "backup %s was not made by the last prepare; left in place", bk);
            rc = RF_E_STATE;
            continue;
        }
        if (orc.ok && file_matches(t, wh) == 0) {
            note_add(note, sizeof note, "%s was replaced after prepare; backup left as %s", t, bk);
            rc = RF_E_STATE;
            continue;
        }
        if (!path_move_replace(bk, t)) {
            note_add(note, sizeof note, "cannot restore %s (Win32 error %lu)", t, GetLastError());
            rc = RF_E_IO;
            continue;
        }
        any = 1;
        note_add(note, sizeof note, "restored %s", t);
    }
    if (rc == 1 && any) rc = 0;
    if (rc == 1) note_add(note, sizeof note, "nothing to restore");
    /* feather passes after the first n, asked for by this job's prepare (which must have finished) */
    if (orc.ok && orc.feather_keep) {
        ftried = 1;
        if (!orc.done || !orc.started_ft) {
            sset(fnote, sizeof fnote, "the prepare record is not complete: nothing cut");
            frc = 1;
        } else {
            frc = feather_trim(task_dir_utf8, orc.feather_keep, orc.started_ft, &ft, fnote, sizeof fnote);
        }
        if (frc == 0 && rc == 1) rc = 0;
        else if (frc < 0 && rc >= 0) rc = frc;
    }
    /* bind the report to the G-code the generator just wrote */
    if (orc.ok) {
        jval *v;
        buf_free(&b);
        if (read_all(recp, &b, &we) == 0 && (v = json_parse(b.p, b.n, &ar, err, sizeof err)) && v->t == J_OBJ) {
            long long gsz;
            unsigned long long gmt;
            int have_g;
            gc = py_join(task_dir_utf8, "gcode");
            if (gc) {
                char *g2 = py_join(gc, "galvo.gc");
                free(gc);
                gc = g2;
            }
            have_g = gc && path_stat(gc, &gsz, &gmt) == 0;
            buf_puts(&out, "{");
            for (i = 0; i < (int)v->u.obj.n; i++) {
                const jmem *m = &v->u.obj.m[i];
                size_t q;
                int skip = 0;
                for (q = 0; q < ARRLEN(RESTORE_KEYS) && !skip; q++)
                    skip = m->kn == strlen(RESTORE_KEYS[q]) && !memcmp(m->k, RESTORE_KEYS[q], m->kn);
                if (skip) continue;
                buf_puts(&out, out.n > 1 ? ",\n  " : "\n  ");
                jw_str(&out, m->k, m->kn);
                buf_puts(&out, ": ");
                jw_val(&out, m->v);
            }
            buf_printf(&out, ",\n  \"restored\": %s,\n  \"restore_rc\": %d,\n  \"restore_note\": ", any ? "true" : "false", rc);
            jw_cstr(&out, note);
            if (ftried) {
                buf_printf(&out, ",\n  \"feather_trim_rc\": %d,\n  \"feather_trim_note\": ", frc);
                jw_cstr(&out, fnote);
                buf_printf(&out, ",\n  \"feather_passes_found\": %d,\n  \"feather_kept\": ", ft.found);
                if (frc == 0) {
                    buf_printf(&out, "%d,\n  \"gcode_bytes_generated\": %lld,\n  \"manifest_sliced_elements\": [%zu, %zu]", ft.kept,
                               ft.size_in, ft.els_in, ft.els_out);
                } else {
                    buf_puts(&out, "null");
                }
            }
            if (have_g) buf_printf(&out, ",\n  \"gcode_bytes\": %lld,\n  \"gcode_mtime\": %llu", gsz, gmt);
            buf_puts(&out, "\n}\n");
            if (!out.oom) write_replace(recp, out.p, out.n, err, sizeof err);
        }
    }
out:
    free(man);
    free(man_bak);
    free(recp);
    free(img);
    free(img_bak);
    free(fname);
    free(gc);
    old_rec_free(&orc);
    buf_free(&b);
    buf_free(&out);
    arena_free(&ar);
    return rc;
}

/* ================================================================== G-code port (rf_fix_task) */
#define NPOS ((size_t)-1)

/* ---- memory-mapped read-only file */
typedef struct { HANDLE f, m; const unsigned char *p; size_t n; } fmap;
static int fmap_open(const char *path, fmap *fm, DWORD *werr)
{
    wchar_t *w = api_path(path);
    LARGE_INTEGER sz;
    memset(fm, 0, sizeof *fm);
    fm->f = INVALID_HANDLE_VALUE;
    *werr = 0;
    if (!w) { *werr = ERROR_INVALID_NAME; return -1; }
    /* no FILE_SHARE_WRITE: nobody may modify galvo.gc while it is mapped */
    fm->f = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    free(w);
    if (fm->f == INVALID_HANDLE_VALUE) { *werr = GetLastError(); return -1; }
    if (!GetFileSizeEx(fm->f, &sz) || sz.QuadPart <= 0) {
        *werr = ERROR_FILE_INVALID;
        CloseHandle(fm->f);
        fm->f = INVALID_HANDLE_VALUE;
        return -2; /* Python: cannot mmap an empty file */
    }
    fm->n = (size_t)sz.QuadPart;
    fm->m = CreateFileMappingW(fm->f, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!fm->m) { *werr = GetLastError(); CloseHandle(fm->f); fm->f = INVALID_HANDLE_VALUE; return -1; }
    fm->p = MapViewOfFile(fm->m, FILE_MAP_READ, 0, 0, 0);
    if (!fm->p) {
        *werr = GetLastError();
        CloseHandle(fm->m);
        CloseHandle(fm->f);
        fm->m = NULL;
        fm->f = INVALID_HANDLE_VALUE;
        return -1;
    }
    return 0;
}
static void fmap_close(fmap *fm)
{
    if (fm->p) UnmapViewOfFile(fm->p);
    if (fm->m) CloseHandle(fm->m);
    if (fm->f != INVALID_HANDLE_VALUE && fm->f) CloseHandle(fm->f);
    memset(fm, 0, sizeof *fm);
    fm->f = INVALID_HANDLE_VALUE;
}

/* ---- byte search (memchr on the rarest needle byte, then memcmp) */
static int byte_rank(unsigned c)
{
    if ((c >= '0' && c <= '9') || c == '.' || c == '\n' || c == 'A' || c == 'B' || c == 'G' || c == 'S' || c == '-') return 0;
    return c == ' ' ? 1 : 2;
}
static size_t rare_index(const unsigned char *nd, size_t nn)
{
    size_t i, best = 0;
    int br = -1;
    for (i = 0; i < nn; i++) {
        int r = byte_rank(nd[i]);
        if (r > br) { br = r; best = i; }
    }
    return best;
}
/* bytes.find(needle, s, e): first occurrence lying entirely in [s, e) */
static size_t mem_find(const unsigned char *h, size_t s, size_t e, const unsigned char *nd, size_t nn)
{
    size_t a;
    const unsigned char *q, *qe;
    if (nn == 0 || e < s || e - s < nn) return NPOS;
    a = rare_index(nd, nn);
    q = h + s + a;
    qe = q + (e - s - nn + 1);
    while (q < qe) {
        const unsigned char *f = memchr(q, nd[a], (size_t)(qe - q));
        if (!f) return NPOS;
        if (memcmp(f - a, nd, nn) == 0) return (size_t)(f - a - h);
        q = f + 1;
    }
    return NPOS;
}
/* bytes.rfind(needle) over [0, n) */
static size_t mem_rfind(const unsigned char *h, size_t n, const unsigned char *nd, size_t nn)
{
    size_t hi;
    if (nn == 0 || n < nn) return NPOS;
    hi = n - nn + 1; /* candidate starts are < hi */
    for (;;) {
        size_t lo = hi > 65536 ? hi - 65536 : 0, last = NPOS, p = lo, e = hi - 1 + nn;
        for (;;) {
            size_t f = mem_find(h, p, e, nd, nn);
            if (f == NPOS) break;
            last = f;
            p = f + 1;
        }
        if (last != NPOS) return last;
        if (lo == 0) return NPOS;
        hi = lo;
    }
}
/* bytes.rfind(b"\n", 0, off) + 1 */
static size_t line_start(const unsigned char *h, size_t off)
{
    while (off > 0 && h[off - 1] != '\n') off--;
    return off;
}

/* ---- keep_top.py regular expressions */
static size_t num_at(const unsigned char *s, size_t i, size_t n) /* -?\d+(?:\.\d+)? -> end, or NPOS */
{
    size_t j = i;
    if (j < n && s[j] == '-') j++;
    if (j >= n || !is_dig(s[j])) return NPOS;
    while (j < n && is_dig(s[j])) j++;
    if (j + 1 < n && s[j] == '.' && is_dig(s[j + 1])) {
        j += 2;
        while (j < n && is_dig(s[j])) j++;
    }
    return j;
}
/* _VLINE_RE.match(line): ^G200 V(NUM)([ \t]*)F(NUM)[ \t]*$ */
static int vline_match(const unsigned char *s, size_t n, size_t *ns, size_t *ne, int *spaced)
{
    size_t i, j;
    if (n < 6 || memcmp(s, "G200 V", 6) != 0) return 0;
    j = num_at(s, 6, n);
    if (j == NPOS) return 0;
    i = j;
    while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
    if (i >= n || s[i] != 'F') return 0;
    *ns = 6;
    *ne = j;
    *spaced = i > j;
    j = num_at(s, i + 1, n);
    if (j == NPOS) return 0;
    while (j < n && (s[j] == ' ' || s[j] == '\t')) j++;
    return j == n;
}
/* _MOVE_RE.match(line): ^G[01][ ]*[A-Z] */
static int move_match(const unsigned char *s, size_t n)
{
    size_t i = 2;
    if (n < 3 || s[0] != 'G' || (s[1] != '0' && s[1] != '1')) return 0;
    while (i < n && s[i] == ' ') i++;
    return i < n && s[i] >= 'A' && s[i] <= 'Z';
}
/* _F_RE.search(line): first F(NUM) */
static int f_search(const unsigned char *s, size_t n, size_t *fs, size_t *fe)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (s[i] == 'F') {
            size_t j = num_at(s, i + 1, n);
            if (j != NPOS) {
                *fs = i + 1;
                *fe = j;
                return 1;
            }
        }
    return 0;
}
/* _is_g(line, code): startswith(code) and next byte not a digit */
static int is_g(const unsigned char *s, size_t n, const char *code)
{
    size_t k = strlen(code);
    return n >= k && memcmp(s, code, k) == 0 && !(n > k && is_dig(s[k]));
}
static int starts_with(const unsigned char *s, size_t n, const char *p)
{
    size_t k = strlen(p);
    return n >= k && memcmp(s, p, k) == 0;
}
static int is_ws_byte(unsigned c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 0x0B || c == 0x0C; }

/* ---- Decimal (default context: prec 28, ROUND_HALF_EVEN) for the V rewrite */
typedef struct { int neg; char *d; size_t n; long long e; } dec; /* (-1)^neg * d * 10^e, d without leading zeros */
static void dec_free(dec *x) { free(x->d); x->d = NULL; }
static int dec_set(dec *x, int neg, const char *dig, size_t n, long long e)
{
    size_t i = 0;
    while (i + 1 < n && dig[i] == '0') i++;
    x->d = str_dup_n(dig + i, n - i);
    if (!x->d) return -1;
    x->n = n - i;
    x->neg = neg;
    x->e = e;
    return 0;
}
static int dec_is_zero(const dec *x) { return x->n == 1 && x->d[0] == '0'; }
/* Decimal(s) for s matching -?\d+(\.\d+)? */
static int dec_parse(const unsigned char *s, size_t n, dec *x)
{
    char *t = malloc(n + 1);
    size_t i = 0, k = 0, nfrac = 0;
    int neg = 0, frac = 0, r;
    if (!t) return -1;
    if (i < n && s[i] == '-') { neg = 1; i++; }
    for (; i < n; i++) {
        if (s[i] == '.') { frac = 1; continue; }
        t[k++] = (char)s[i];
        if (frac) nfrac++;
    }
    r = dec_set(x, neg, t, k, -(long long)nfrac);
    free(t);
    return r;
}
/* magnitude compare of digit strings aligned at the same exponent (lengths la, lb, no leading zeros) */
static int digcmp(const char *a, size_t la, const char *b, size_t lb)
{
    if (la != lb) return la < lb ? -1 : 1;
    return memcmp(a, b, la) < 0 ? -1 : memcmp(a, b, la) > 0 ? 1 : 0;
}
/* a - b with context rounding; -1 error (overflow / memory) */
static int dec_sub(const dec *a, const dec *b, dec *r)
{
    long long e = a->e < b->e ? a->e : b->e;
    size_t sa = (size_t)(a->e - e), sb = (size_t)(b->e - e), la = a->n + sa, lb = b->n + sb, lm, i;
    char *A, *B, *M;
    int na = a->neg, nb = !b->neg, neg, c;
    if (sa > 4000000 || sb > 4000000) return -1;
    A = malloc(la + 1);
    B = malloc(lb + 1);
    lm = (la > lb ? la : lb) + 1;
    M = malloc(lm + 1);
    if (!A || !B || !M) { free(A); free(B); free(M); return -1; }
    memcpy(A, a->d, a->n);
    memset(A + a->n, '0', sa);
    memcpy(B, b->d, b->n);
    memset(B + b->n, '0', sb);
    /* strip leading zeros (zero operands) */
    {
        size_t za = 0, zb = 0;
        while (za + 1 < la && A[za] == '0') za++;
        while (zb + 1 < lb && B[zb] == '0') zb++;
        memmove(A, A + za, la - za);
        la -= za;
        memmove(B, B + zb, lb - zb);
        lb -= zb;
    }
    memset(M, '0', lm);
    if (na == nb) {
        int carry = 0;
        for (i = 0; i < lm; i++) {
            int x = (i < la ? A[la - 1 - i] - '0' : 0) + (i < lb ? B[lb - 1 - i] - '0' : 0) + carry;
            M[lm - 1 - i] = (char)('0' + x % 10);
            carry = x / 10;
        }
        neg = na;
    } else {
        const char *X = A, *Y = B;
        size_t lx = la, ly = lb;
        int borrow = 0;
        c = digcmp(A, la, B, lb);
        if (c < 0) { X = B; Y = A; lx = lb; ly = la; }
        for (i = 0; i < lm; i++) {
            int x = (i < lx ? X[lx - 1 - i] - '0' : 0) - (i < ly ? Y[ly - 1 - i] - '0' : 0) - borrow;
            borrow = x < 0;
            if (x < 0) x += 10;
            M[lm - 1 - i] = (char)('0' + x);
        }
        neg = c == 0 ? 0 : (c > 0 ? na : nb);
    }
    {
        size_t z = 0, nd;
        while (z + 1 < lm && M[z] == '0') z++;
        nd = lm - z;
        if (nd > 28) {
            size_t drop = nd - 28, k;
            const char *D = M + z;
            int first = D[28] - '0', rest = 0, up;
            for (k = 29; k < nd; k++)
                if (D[k] != '0') { rest = 1; break; }
            up = first > 5 || (first == 5 && (rest || ((D[27] - '0') & 1)));
            if (up) {
                long long q = 27;
                M[z + 28] = 0;
                while (q >= 0) {
                    if (M[z + (size_t)q] == '9') { M[z + (size_t)q] = '0'; q--; }
                    else { M[z + (size_t)q]++; break; }
                }
                if (q < 0) { /* 999..9 -> 1000..0: 29 digits, drop one more zero */
                    M[z] = '1';
                    memset(M + z + 1, '0', 27);
                    drop++;
                }
            }
            e += (long long)drop;
            nd = 28;
        }
        if (e + (long long)nd - 1 > 999999) { free(A); free(B); free(M); return -1; }
        c = dec_set(r, neg, M + z, nd, e);
    }
    free(A);
    free(B);
    free(M);
    return c;
}
/* _fmt_like: quantize to ndec decimals (must be exact and fit 28 digits), format 'f', drop "-" of a zero */
static int dec_fmt_like(const dec *x, size_t ndec, buf_t *o)
{
    long long ex = -(long long)ndec, diff = x->e - ex;
    char *c;
    size_t n, i;
    if (dec_is_zero(x)) {
        c = str_dup_n("0", 1);
        n = 1;
    } else {
        if ((long long)x->n + diff > 28) return -1; /* InvalidOperation */
        if (diff >= 0) {
            n = x->n + (size_t)diff;
            c = malloc(n + 1);
            if (!c) return -1;
            memcpy(c, x->d, x->n);
            memset(c + x->n, '0', (size_t)diff);
            c[n] = 0;
        } else {
            size_t drop = (size_t)(-diff);
            if (drop >= x->n) return -1; /* nonzero digits dropped -> r != value */
            for (i = x->n - drop; i < x->n; i++)
                if (x->d[i] != '0') return -1;
            n = x->n - drop;
            c = str_dup_n(x->d, n);
        }
    }
    if (!c) return -1;
    if (x->neg && !(n == 1 && c[0] == '0')) buf_putc(o, '-');
    if (ndec == 0) {
        buf_put(o, c, n);
    } else if (n > ndec) {
        buf_put(o, c, n - ndec);
        buf_putc(o, '.');
        buf_put(o, c + n - ndec, ndec);
    } else {
        buf_puts(o, "0.");
        for (i = n; i < ndec; i++) buf_putc(o, '0');
        buf_put(o, c, n);
    }
    free(c);
    return o->oom ? -1 : 0;
}
/* str(Decimal) */
static void dec_str(const dec *x, buf_t *o)
{
    long long left = x->e + (long long)x->n, dot, i;
    if (x->neg) buf_putc(o, '-');
    if (x->e <= 0 && left > -6) dot = left;
    else dot = 1;
    if (dot <= 0) {
        buf_puts(o, "0.");
        for (i = 0; i < -dot; i++) buf_putc(o, '0');
        buf_put(o, x->d, x->n);
    } else if (dot >= (long long)x->n) {
        buf_put(o, x->d, x->n);
        for (i = (long long)x->n; i < dot; i++) buf_putc(o, '0');
    } else {
        buf_put(o, x->d, (size_t)dot);
        buf_putc(o, '.');
        buf_put(o, x->d + dot, x->n - (size_t)dot);
    }
    if (left != dot) buf_printf(o, "E%+lld", left - dot);
}

/* ---- keep_top.analyze(mm, paths=False) */
typedef struct {
    size_t start, end, tail;
    int has_v, v_spaced;
    size_t v_off, v_ns, v_ne;   /* V line offset and its number span (absolute) */
    int has_f;
    size_t f_s, f_e;            /* first move's F number span */
} gblk;
typedef struct {
    size_t size, header_end, tok_s, tok_n;
    size_t v0_s, v0_e;
    int has_end;
    size_t end_line, stop;
    gblk *b;
    size_t nb;
    size_t tail_s, tail_e;
    size_t layer_end;
    size_t *lf; /* first block index of each layer */
    size_t nl;
    int one_v;
    int has_cam;
    size_t cam_end;
    size_t n_clean;
    char err[200];
} ganal;
static void ganal_free(ganal *g)
{
    free(g->b);
    free(g->lf);
    g->b = NULL;
    g->lf = NULL;
}
static int g_fail(ganal *g, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    g->err[0] = 0;
    vappend(g->err, sizeof g->err, fmt, ap);
    va_end(ap);
    return -1;
}
static int f_eq(const unsigned char *h, const gblk *a, const gblk *b)
{
    if (a->has_f != b->has_f) return 0;
    if (!a->has_f) return 1;
    return a->f_e - a->f_s == b->f_e - b->f_s && memcmp(h + a->f_s, h + b->f_s, a->f_e - a->f_s) == 0;
}
static int g_analyze(const unsigned char *h, size_t n, ganal *g)
{
    static const unsigned char BS[] = " BITMAP START";
    static const unsigned char ENDL[] = "\n# END\n";
    size_t q, i, *starts = NULL, *tails = NULL, ns = 0, nt = 0, cs = 0, ct = 0;
    int rc = -1;
    memset(g, 0, sizeof *g);
    g->size = n;
    if (memchr(h, '\r', n)) return g_fail(g, "CR found: only LF line endings are supported");
    /* first "# <a> <b> BITMAP START" line (regex search) */
    q = 0;
    for (;;) {
        size_t f = mem_find(h, q, n, BS, 13), ls, k, sp = NPOS, nsp = 0;
        int ok = 1;
        if (f == NPOS) return g_fail(g, "no '# <id> <head> BITMAP START' line");
        q = f + 1;
        if (!(f + 13 == n || h[f + 13] == '\n')) continue;
        ls = line_start(h, f);
        if (f < ls + 5 || h[ls] != '#' || h[ls + 1] != ' ') continue;
        for (k = ls + 2; k < f; k++) {
            if (h[k] == ' ') { sp = k; nsp++; }
            else if (is_ws_byte(h[k])) { ok = 0; break; }
        }
        if (!ok || nsp != 1 || sp == ls + 2 || sp + 1 == f) continue;
        g->header_end = ls;
        g->tok_s = ls + 2;
        g->tok_n = f - (ls + 2);
        break;
    }
    /* V0: last V line of the header */
    {
        size_t pos = 0, he = g->header_end;
        int have = 0;
        while (pos < he) {
            const unsigned char *nl = memchr(h + pos, '\n', he - pos);
            size_t le = nl ? (size_t)(nl - h) : he, a, b;
            int sp;
            if (vline_match(h + pos, le - pos, &a, &b, &sp)) {
                g->v0_s = pos + a;
                g->v0_e = pos + b;
                have = 1;
            }
            pos = le + 1;
        }
        if (!have) return g_fail(g, "no 'G200 V..' line in the header");
    }
    {
        size_t i2 = mem_rfind(h, n, ENDL, 7);
        g->has_end = i2 != NPOS;
        g->end_line = g->has_end ? i2 + 1 : 0;
        g->stop = g->has_end ? g->end_line : n;
    }
    /* START / TAIL lines of this head ('#' scan) */
    cs = 1024;
    ct = 1024;
    starts = malloc(cs * sizeof(size_t));
    tails = malloc(ct * sizeof(size_t));
    if (!starts || !tails) { g_fail(g, "out of memory"); goto out; }
    starts[ns++] = g->header_end;
    {
        size_t pre = 2 + g->tok_n + 8, stop = g->stop, x;
        q = g->header_end + 1;
        while (q < stop) {
            const unsigned char *f = memchr(h + q, '#', stop - q);
            if (!f) break;
            x = (size_t)(f - h);
            q = x + 1;
            if (h[x - 1] != '\n' || x + pre > stop || h[x + 1] != ' ' || memcmp(h + x + 2, h + g->tok_s, g->tok_n) != 0 ||
                memcmp(h + x + 2 + g->tok_n, " BITMAP ", 8) != 0)
                continue;
            if (x + pre + 6 <= stop && memcmp(h + x + pre, "START\n", 6) == 0) {
                if (ns == cs) {
                    size_t *t2 = realloc(starts, (cs *= 2) * sizeof(size_t));
                    if (!t2) { g_fail(g, "out of memory"); goto out; }
                    starts = t2;
                }
                starts[ns++] = x;
            } else if (x + pre + 5 <= stop && memcmp(h + x + pre, "TAIL\n", 5) == 0) {
                if (nt == ct) {
                    size_t *t2 = realloc(tails, (ct *= 2) * sizeof(size_t));
                    if (!t2) { g_fail(g, "out of memory"); goto out; }
                    tails = t2;
                }
                tails[nt++] = x;
            }
        }
    }
    if (ns != nt) { g_fail(g, "%zu BITMAP START vs %zu BITMAP TAIL lines", ns, nt); goto out; }
    for (i = 0; i < ns; i++) {
        size_t nxt = i + 1 < ns ? starts[i + 1] : g->stop;
        if (!(starts[i] < tails[i] && tails[i] < nxt)) { g_fail(g, "block %zu at %zu: START/TAIL out of order", i, starts[i]); goto out; }
    }
    g->tail_s = tails[0];
    g->tail_e = ns > 1 ? starts[1] : g->stop;
    g->nb = ns;
    g->b = calloc(ns, sizeof(gblk));
    if (!g->b) { g_fail(g, "out of memory"); goto out; }
    /* block prologues: lines in [start, min(next, start + 8192)) */
    for (i = 0; i < ns; i++) {
        gblk *B = &g->b[i];
        size_t nxt = i + 1 < ns ? starts[i + 1] : g->stop, pe = nxt < starts[i] + 8192 ? nxt : starts[i] + 8192, pos = starts[i];
        int first = 0;
        B->start = starts[i];
        B->end = nxt;
        B->tail = tails[i];
        while (pos < pe) {
            const unsigned char *nl = memchr(h + pos, '\n', pe - pos);
            size_t le = nl ? (size_t)(nl - h) : pe, a, b;
            int sp;
            if (move_match(h + pos, le - pos)) {
                size_t fs, fe;
                first = 1;
                if (f_search(h + pos, le - pos, &fs, &fe)) {
                    B->has_f = 1;
                    B->f_s = pos + fs;
                    B->f_e = pos + fe;
                }
                break;
            }
            if (vline_match(h + pos, le - pos, &a, &b, &sp)) {
                if (B->has_v) { g_fail(g, "two V lines in the prologue of the block at %zu", starts[i]); goto out; }
                B->has_v = 1;
                B->v_off = pos;
                B->v_ns = pos + a;
                B->v_ne = pos + b;
                B->v_spaced = sp;
            }
            pos = le + 1;
        }
        if (!first) { g_fail(g, "block %zu at %zu: no G0/G1 line in its prologue", i, starts[i]); goto out; }
    }
    /* cleaning blocks: spaced V line or a different first-move F; layer_end = first of them */
    g->layer_end = g->stop;
    for (i = 1; i < ns; i++) {
        const gblk *B = &g->b[i];
        if ((B->has_v && B->v_spaced) || !f_eq(h, B, &g->b[0])) {
            if (!g->n_clean) g->layer_end = B->start;
            g->n_clean++;
        }
    }
    /* layers */
    g->lf = malloc(ns * sizeof(size_t));
    if (!g->lf) { g_fail(g, "out of memory"); goto out; }
    g->lf[g->nl++] = 0;
    for (i = 1; i < ns && g->b[i].start < g->layer_end; i++)
        if (g->b[i].has_v) g->lf[g->nl++] = i;
    /* every non-comment line with a 'V' in the layer content must be a layer prologue V line */
    g->one_v = 1;
    {
        size_t lim = g->layer_end, *voff = malloc((g->nl + 1) * sizeof(size_t)), nv = 0;
        if (!voff) { g_fail(g, "out of memory"); goto out; }
        for (i = 0; i < g->nl; i++)
            if (g->b[g->lf[i]].has_v) voff[nv++] = g->b[g->lf[i]].v_off; /* increasing */
        q = g->header_end;
        while (q < lim) {
            const unsigned char *f = memchr(h + q, 'V', lim - q);
            size_t x, ls, lo = 0, hi = nv;
            int found = 0;
            if (!f) break;
            x = (size_t)(f - h);
            q = x + 1;
            ls = line_start(h, x);
            if (h[ls] == '#') continue;
            while (lo < hi) {
                size_t mid = lo + (hi - lo) / 2;
                if (voff[mid] == ls) { found = 1; break; }
                if (voff[mid] < ls) lo = mid + 1;
                else hi = mid;
            }
            if (!found) { g->one_v = 0; break; }
        }
        free(voff);
    }
    /* camera blocks (G106..G107) in layer 1's first block */
    /* camera blocks (G106..G107) in layer 1's first block; only the end of the last one is used (prefix end).
     * (Python also widens a block's start to a preceding "# take photo block start" line: not needed here.) */
    {
        const gblk *B0 = &g->b[0];
        size_t be = B0->tail, pos = B0->start;
        const unsigned char *sl = h + B0->start; /* start line text "# <tok> BITMAP START" */
        size_t sl_n = 2 + g->tok_n + 13;
        while (pos < be) {
            const unsigned char *nl = memchr(h + pos, '\n', be - pos);
            size_t le = nl ? (size_t)(nl - h) : be;
            if (is_g(h + pos, le - pos, "G106")) {
                size_t j = le + 1, lej = 0, e;
                int found = 0;
                while (j < be) {
                    const unsigned char *n2 = memchr(h + j, '\n', be - j);
                    lej = n2 ? (size_t)(n2 - h) : be;
                    if (is_g(h + j, lej - j, "G107")) { found = 1; break; }
                    if (is_g(h + j, lej - j, "G106")) { g_fail(g, "nested G106 at %zu", j); goto out; }
                    j = lej + 1;
                }
                if (!found) { g_fail(g, "G106 at %zu has no G107 in the block", pos); goto out; }
                e = lej + 1;
                while (e < be) {
                    const unsigned char *n3 = memchr(h + e, '\n', be - e);
                    size_t lee = n3 ? (size_t)(n3 - h) : be, ln = lee - e;
                    int is_tail = ln == sl_n - 1 && memcmp(h + e, "# ", 2) == 0 && memcmp(h + e + 2, h + g->tok_s, g->tok_n) == 0 &&
                                  memcmp(h + e + 2 + g->tok_n, " BITMAP TAIL", 12) == 0;
                    if ((starts_with(h + e, ln, "G1") && !(ln > 2 && is_dig(h[e + 2]))) ||
                        (ln == sl_n && memcmp(h + e, sl, sl_n) == 0) || is_tail)
                        break;
                    e = lee + 1;
                }
                g->has_cam = 1;
                g->cam_end = e < be ? e : be;
                pos = e;
                continue;
            }
            pos = le + 1;
        }
    }
    rc = 0;
out:
    free(starts);
    free(tails);
    if (rc) ganal_free(g);
    return rc;
}

/* ---- output segments */
typedef struct { size_t a, b; unsigned char *lit; size_t litn; } gseg;
typedef struct { gseg *s; size_t n, cap; int oom; } segv;
static void segv_range(segv *v, size_t a, size_t b)
{
    if (v->oom) return;
    if (v->n == v->cap) {
        size_t nc = v->cap ? v->cap * 2 : 64;
        gseg *t = realloc(v->s, nc * sizeof(gseg));
        if (!t) { v->oom = 1; return; }
        v->s = t;
        v->cap = nc;
    }
    v->s[v->n].a = a;
    v->s[v->n].b = b;
    v->s[v->n].lit = NULL;
    v->s[v->n].litn = 0;
    v->n++;
}
static void segv_lit(segv *v, const void *p, size_t n)
{
    unsigned char *c = malloc(n ? n : 1);
    if (!c) { v->oom = 1; return; }
    memcpy(c, p, n);
    segv_range(v, 0, 0);
    if (v->oom) { free(c); return; }
    v->s[v->n - 1].lit = c;
    v->s[v->n - 1].litn = n;
}
static void segv_free(segv *v)
{
    size_t i;
    for (i = 0; i < v->n; i++) free(v->s[i].lit);
    free(v->s);
    memset(v, 0, sizeof *v);
}
static unsigned long long segv_size(const segv *v)
{
    unsigned long long t = 0;
    size_t i;
    for (i = 0; i < v->n; i++) t += v->s[i].lit ? v->s[i].litn : v->s[i].b - v->s[i].a;
    return t;
}

/* python set repr of ints, sorted (CPython prints hash order) */
static void set_repr(char *out, size_t cap, pyint *v, size_t n)
{
    size_t i;
    char t[48];
    if (!n) { sset(out, cap, "set()"); return; }
    sset(out, cap, "{");
    for (i = 0; i < n; i++) {
        pyint_str(&v[i], t, sizeof t);
        sappend(out, cap, "%s%s", i ? ", " : "", t);
    }
    sappend(out, cap, "}");
}
static int pyint_cmp(const pyint *a, const pyint *b)
{
    int s = a->neg ? -1 : 1;
    if (a->neg != b->neg) return a->neg ? -1 : 1;
    if (a->n != b->n) return (a->n < b->n ? -1 : 1) * s;
    return (memcmp(a->d, b->d, a->n) < 0 ? -1 : memcmp(a->d, b->d, a->n) > 0 ? 1 : 0) * s;
}
static void pyset_add(pyint *v, size_t *n, const pyint *x)
{
    size_t i, j;
    for (i = 0; i < *n; i++) {
        if (pyint_eq(&v[i], x)) return;
        if (pyint_cmp(x, &v[i]) < 0) break;
    }
    for (j = *n; j > i; j--) v[j] = v[j - 1];
    v[i] = *x;
    (*n)++;
}

/* strip_cleaning.plan_cleaning.  0 ok (cut range), 1 Skip (note), < 0 error.
 * starts_out (optional): on 0, a malloc'd copy of the nclean cleaning block starts, in file order */
static int plan_cleaning(const char *task, const unsigned char *h, size_t n, const ganal *g, size_t *cut_from, size_t *cut_to,
                         size_t **starts_out, size_t *nstarts_out, char *note, size_t nc)
{
    char *sp = py_join(task, "manifest_sliced.json"), err[200];
    buf_t b = {0}, sig = {0};
    arena_t ar = {0};
    DWORD we;
    const jval *root, *els;
    size_t i, nclean = 0, nspeeds = 0, nlay = 0, end, *bstarts = NULL, nbs = 0, cbs = 0, sign, *sigpos = NULL;
    pyint *speeds = NULL, *lspeeds = NULL, speed;
    int rc = RF_E_JSON, r;
    if (!sp) return RF_E_NOMEM;
    r = read_all(sp, &b, &we);
    if (r != 0) { sset(note, nc, "cannot read manifest_sliced.json (Win32 error %lu)", we); rc = RF_E_IO; goto out; }
    root = json_parse(b.p, b.n, &ar, err, sizeof err);
    if (!root) { sset(note, nc, "manifest_sliced.json: %s", err); goto out; }
    if (root->t != J_OBJ) { sset(note, nc, "manifest_sliced.json is not a dict"); goto out; }
    els = jget(root, "elements");
    if (els && !(els->t == J_ARR || ((els->t == J_OBJ && els->u.obj.n == 0) || (els->t == J_STR && els->u.s.n == 0)))) {
        sset(note, nc, "manifest_sliced.json: 'elements' is not a list");
        goto out;
    }
    if (els && els->t == J_ARR) {
        for (i = 0; i < els->u.arr.n; i++)
            if (els->u.arr.v[i]->t != J_OBJ) { sset(note, nc, "manifest_sliced.json: elements[%zu] is not a dict", i); goto out; }
        speeds = calloc(els->u.arr.n + 1, sizeof(pyint));
        lspeeds = calloc(els->u.arr.n + 1, sizeof(pyint));
        if (!speeds || !lspeeds) { rc = RF_E_NOMEM; goto out; }
        for (i = 0; i < els->u.arr.n; i++)
            if (py_truthy(jget(els->u.arr.v[i], "isCleaningLayer"))) nclean++;
    }
    if (!nclean) { sset(note, nc, "no cleaning layers in manifest_sliced.json"); rc = 1; goto out; }
    for (i = 0; i < els->u.arr.n; i++) {
        const jval *e = els->u.arr.v[i], *s;
        pyint x;
        if (!py_truthy(jget(e, "isCleaningLayer"))) continue;
        s = jget(e, "speed");
        if (!s) { sset(note, nc, "manifest_sliced.json: KeyError 'speed'"); goto out; }
        if (py_int(&ar, s, &x, err, sizeof err) != 0) { sset(note, nc, "manifest_sliced.json speed: %s", err); goto out; }
        pyset_add(speeds, &nspeeds, &x);
    }
    for (i = 0; i < els->u.arr.n; i++) {
        const jval *e = els->u.arr.v[i], *s;
        pyint x;
        if (py_truthy(jget(e, "isCleaningLayer")) || !(s = jget(e, "speed"))) continue;
        if (py_int(&ar, s, &x, err, sizeof err) != 0) { sset(note, nc, "manifest_sliced.json speed: %s", err); goto out; }
        pyset_add(lspeeds, &nlay, &x);
    }
    {
        int inter = 0;
        size_t a2, b2;
        for (a2 = 0; a2 < nspeeds; a2++)
            for (b2 = 0; b2 < nlay; b2++)
                if (pyint_eq(&speeds[a2], &lspeeds[b2])) inter = 1;
        if (nspeeds != 1 || inter) {
            char s1[200], s2[200];
            set_repr(s1, sizeof s1, speeds, nspeeds);
            set_repr(s2, sizeof s2, lspeeds, nlay);
            sset(note, nc, "cleaning speed %s not unique vs layer speeds %s", s1, s2);
            rc = 1;
            goto out;
        }
    }
    speed = speeds[0];
    /* galvo.gc checks (eol is always "\n": analyze rejected CR) */
    if (!g->has_end) { sset(note, nc, "no '# END' line"); rc = 1; goto out; }
    end = g->end_line;
    {
        static const unsigned char BSN[] = " BITMAP START\n";
        size_t q = 0;
        for (;;) {
            size_t f = mem_find(h, q, end, BSN, 14);
            if (f == NPOS) break;
            if (nbs == cbs) {
                size_t *t = realloc(bstarts, (cbs = cbs ? cbs * 2 : 1024) * sizeof(size_t));
                if (!t) { rc = RF_E_NOMEM; goto out; }
                bstarts = t;
            }
            bstarts[nbs++] = line_start(h, f);
            q = f + 1;
        }
    }
    if (nbs <= nclean) { sset(note, nc, "only %zu bitmap blocks for %zu cleaning layers", nbs, nclean); rc = 1; goto out; }
    /* sig = b"\nG1A0B0F%dS0" % speed + eol */
    buf_puts(&sig, "\nG1A0B0F");
    if (speed.neg) buf_putc(&sig, '-');
    buf_put(&sig, speed.d, speed.n);
    buf_puts(&sig, "S0\n");
    if (sig.oom) { rc = RF_E_NOMEM; goto out; }
    sign = sig.n;
    sigpos = malloc((nclean + 1) * sizeof(size_t));
    if (!sigpos) { rc = RF_E_NOMEM; goto out; }
    {
        size_t q = 0, cnt = 0;
        for (;;) {
            size_t f = mem_find(h, q, n, sig.p, sign);
            if (f == NPOS) break;
            if (cnt < nclean) sigpos[cnt] = f;
            cnt++;
            q = f + 1;
        }
        if (cnt != nclean) { sset(note, nc, "cleaning signature found %zu times, expected %zu", cnt, nclean); rc = 1; goto out; }
    }
    {
        size_t *cand = bstarts + (nbs - nclean), k;
        for (k = 0; k < nclean; k++) {
            size_t lo = cand[k], hi = k + 1 < nclean ? cand[k + 1] : end, p = sigpos[k];
            if (!(lo <= p && p < hi) || p - lo > 4096) { sset(note, nc, "cleaning block %zu not where expected", k); rc = 1; goto out; }
        }
        *cut_from = cand[0];
        *cut_to = end;
        if (starts_out) {
            *starts_out = malloc(nclean * sizeof(size_t));
            if (!*starts_out) { rc = RF_E_NOMEM; goto out; }
            memcpy(*starts_out, cand, nclean * sizeof(size_t));
            *nstarts_out = nclean;
        }
    }
    rc = 0;
out:
    free(sp);
    free(speeds);
    free(lspeeds);
    free(bstarts);
    free(sigpos);
    buf_free(&b);
    buf_free(&sig);
    arena_free(&ar);
    return rc;
}

/* write segments of the mapped source to path.tmp and replace path; the source is unmapped first */
static int write_gcode(const char *path, const unsigned char *h, const segv *sv, unsigned long long expect, fmap *src, char *err,
                       size_t ec)
{
    char *tmp = str_cat(path, ".tmp");
    wchar_t *wt = tmp ? api_path(tmp) : NULL, *wp = api_path(path);
    HANDLE f;
    size_t i;
    unsigned long long total = 0;
    LARGE_INTEGER sz;
    int rc = -1;
    if (!wt || !wp) { sset(err, ec, "bad path"); goto out; }
    DeleteFileW(wt);
    f = CreateFileW(wt, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (f == INVALID_HANDLE_VALUE) { sset(err, ec, "cannot create %s (Win32 error %lu)", tmp, GetLastError()); goto out; }
    for (i = 0; i < sv->n; i++) {
        const unsigned char *p = sv->s[i].lit ? sv->s[i].lit : h + sv->s[i].a;
        size_t m = sv->s[i].lit ? sv->s[i].litn : sv->s[i].b - sv->s[i].a, done = 0;
        while (done < m) {
            DWORD wr = 0, chunk = (DWORD)((m - done) > ((size_t)1 << 24) ? ((size_t)1 << 24) : (m - done));
            if (!WriteFile(f, p + done, chunk, &wr, NULL) || wr == 0) {
                sset(err, ec, "write %s failed (Win32 error %lu)", tmp, GetLastError());
                CloseHandle(f);
                DeleteFileW(wt);
                goto out;
            }
            done += wr;
            total += wr;
        }
    }
    FlushFileBuffers(f);
    if (!GetFileSizeEx(f, &sz) || (unsigned long long)sz.QuadPart != expect || total != expect) {
        sset(err, ec, "written size %llu != planned %llu", (unsigned long long)sz.QuadPart, expect);
        CloseHandle(f);
        DeleteFileW(wt);
        goto out;
    }
    CloseHandle(f);
    fmap_close(src); /* the mapping keeps galvo.gc open: close it before the replace */
    if (!move_replace_retry(wt, wp)) {
        sset(err, ec, "cannot replace %s (Win32 error %lu)", path, GetLastError());
        DeleteFileW(wt);
        goto out;
    }
    rc = 0;
out:
    free(tmp);
    free(wt);
    free(wp);
    return rc;
}

/* ---- feather passes 1..7 (rf_restore_task) */
static int jint_eq(const jval *v, long long x)
{
    pyint pi;
    long long y;
    if (!v || v->t != J_INT) return 0;
    pi.neg = v->u.i.neg;
    pi.d = v->u.i.d;
    pi.n = v->u.i.n;
    return pyint_to_ll(&pi, &y) && y == x;
}
/* gcode\ holds no .gc file besides galvo.gc and motor.gc (a job split into several galvo files would) */
static int gcode_dir_plain(const char *gdir)
{
    char *pat = py_join(gdir, "*");
    wchar_t *w = pat ? api_path(pat) : NULL;
    WIN32_FIND_DATAW fd;
    HANDLE h;
    int ok = 1;
    free(pat);
    if (!w) return 0;
    h = FindFirstFileW(w, &fd);
    free(w);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        int L = lstrlenW(fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (L >= 3 && CompareStringOrdinal(fd.cFileName + L - 3, 3, L".gc", 3, TRUE) == CSTR_EQUAL &&
            CompareStringOrdinal(fd.cFileName, -1, L"galvo.gc", -1, TRUE) != CSTR_EQUAL &&
            CompareStringOrdinal(fd.cFileName, -1, L"motor.gc", -1, TRUE) != CSTR_EQUAL) {
            ok = 0;
            break;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return ok;
}
/* b is document a with its "elements" list cut to the first nkeep entries, everything else structurally equal */
static int jdoc_cut_same(const jval *a, const jval *b, size_t nkeep)
{
    size_t i, j;
    if (a->t != J_OBJ || b->t != J_OBJ || a->u.obj.n != b->u.obj.n) return 0;
    for (i = 0; i < a->u.obj.n; i++) {
        const jmem *x = &a->u.obj.m[i], *y = &b->u.obj.m[i];
        if (x->kn != y->kn || memcmp(x->k, y->k, x->kn) != 0) return 0;
        if (x->kn == 8 && !memcmp(x->k, "elements", 8)) {
            if (x->v->t != J_ARR || y->v->t != J_ARR || x->v->u.arr.n < nkeep || y->v->u.arr.n != nkeep) return 0;
            for (j = 0; j < nkeep; j++)
                if (!jv_same(x->v->u.arr.v[j], y->v->u.arr.v[j], NULL, NULL)) return 0;
        } else if (!jv_same(x->v, y->v, NULL, NULL)) {
            return 0;
        }
    }
    return 1;
}
/* Keep the first `keep` (1..7) of the generator's 8 trailing feather passes.  galvo.gc loses [start of pass keep+1,
 * "# END" line), manifest_sliced.json the elements of those passes; everything else stays byte for byte.  On top of
 * every plan_cleaning check: both files newer than this job's prepare (not_before, FILETIME), one galvo file
 * (estimate_result.json, gcode\ listing), exactly 8 passes that are the last 8 blocks before "# END", the 8 cleaning
 * elements are the last 8 and pass i starts with element i's "head" text, and the cut manifest reads back as the
 * original minus those elements.  manifest_sliced.json is written first and put back if galvo.gc cannot be replaced.
 * 0 = cut, 1 = left as generated (note says why), < 0 = error (files as generated, unless the note says otherwise). */
static int feather_trim(const char *task, int keep, unsigned long long not_before, ftrim_t *ft, char *note, size_t nc)
{
    char *gdir = py_join(task, "gcode"), *galvo = gdir ? py_join(gdir, "galvo.gc") : NULL;
    char *msp = py_join(task, "manifest_sliced.json"), *erp = py_join(task, "estimate_result.json"), err[200];
    fmap fm;
    ganal g;
    segv sv = {0};
    buf_t ms = {0}, nms = {0}, er = {0};
    arena_t ar = {0}, ar2 = {0}, ar3 = {0};
    size_t cut_from = 0, cut_to = 0, *cand = NULL, ncand = 0, i, nel, first, gap_a, gap_b;
    const jval *root, *els = NULL, *root2;
    long long gsz, msz;
    unsigned long long gmt, mmt, expect;
    DWORD we = 0;
    int rc = 1, r;

    memset(ft, 0, sizeof *ft);
    memset(&g, 0, sizeof g);
    memset(&fm, 0, sizeof fm);
    fm.f = INVALID_HANDLE_VALUE;
    err[0] = 0;
    if (!gdir || !galvo || !msp || !erp) { rc = RF_E_NOMEM; goto out; }
    if (keep < 1 || keep > 7) { sset(note, nc, "feather_passes %d: only 1..7 are cut", keep); rc = RF_E_ARG; goto out; }
    /* this job's output, one galvo file */
    if (path_stat(galvo, &gsz, &gmt) != 0 || path_stat(msp, &msz, &mmt) != 0) {
        sset(note, nc, "gcode\\galvo.gc or manifest_sliced.json missing: nothing cut");
        goto out;
    }
    if (gmt <= not_before || mmt <= not_before) {
        sset(note, nc, "gcode\\galvo.gc or manifest_sliced.json is older than this job's prepare: nothing cut");
        goto out;
    }
    if (read_all(erp, &er, &we) == 0) {
        const jval *e = json_parse(er.p, er.n, &ar3, err, sizeof err), *fnum, *snum;
        if (!e || e->t != J_OBJ) { sset(note, nc, "estimate_result.json unreadable: nothing cut"); goto out; }
        fnum = jget(e, "galvoFinishedNum");
        snum = jget(e, "galvoSplitNum");
        if ((fnum && !jint_eq(fnum, 1)) || (snum && !jint_eq(snum, 1))) {
            sset(note, nc, "the G-code is split into several files: nothing cut");
            goto out;
        }
    }
    if (!gcode_dir_plain(gdir)) { sset(note, nc, "gcode\\ holds .gc files other than galvo.gc and motor.gc: nothing cut"); goto out; }
    /* galvo.gc: 8 feather passes right after the relief layers, each found by plan_cleaning */
    r = fmap_open(galvo, &fm, &we);
    if (r != 0) {
        sset(note, nc, r == -2 ? "gcode\\galvo.gc is empty: nothing cut" : "cannot open gcode\\galvo.gc (Win32 error %lu)", we);
        rc = r == -2 ? 1 : RF_E_IO;
        goto out;
    }
    ft->size_in = (long long)fm.n;
    if (g_analyze(fm.p, fm.n, &g) != 0) { sset(note, nc, "G-code structure not understood (%s): nothing cut", g.err); goto out; }
    /* The passes are counted from manifest_sliced.json (isCleaningLayer) and located by plan_cleaning: exactly one
     * pass signature per pass, all inside the last blocks before "# END".  g_analyze's own cleaning guess (spaced V
     * line or another first-move feed) is not used: a relief layer whose block (0, 0) is empty is written as a block
     * holding only its V line, and that guess counts such blocks as cleaning (54 of them in one 96-layer job). */
    r = plan_cleaning(task, fm.p, fm.n, &g, &cut_from, &cut_to, &cand, &ncand, err, sizeof err);
    if (r == 1 && strncmp(err, "no cleaning layers", 18) == 0) {
        sset(note, nc, "the generator made no feather passes (the image fits in one block, or featherPower is 0): nothing to cut");
        goto out;
    }
    if (r != 0) { sset(note, nc, "%s: nothing cut", err); rc = r < 0 ? r : 1; goto out; }
    ft->found = (int)ncand;
    if (ncand != 8) { sset(note, nc, "%zu feather passes instead of 8: nothing cut", ncand); goto out; }
    if (cut_from != cand[0] || cut_to != g.end_line) {
        sset(note, nc, "the feather passes are not the last blocks before the end sequence: nothing cut");
        goto out;
    }
    /* manifest_sliced.json: the last 8 elements are the passes, in G-code order */
    if (read_all(msp, &ms, &we) != 0) { sset(note, nc, "cannot read manifest_sliced.json (Win32 error %lu)", we); rc = RF_E_IO; goto out; }
    root = json_parse(ms.p, ms.n, &ar, err, sizeof err);
    if (!root || root->t != J_OBJ || jcount(root, "elements") != 1 || !(els = jget(root, "elements")) || els->t != J_ARR) {
        sset(note, nc, "manifest_sliced.json has no single 'elements' list: nothing cut");
        goto out;
    }
    nel = els->u.arr.n;
    ft->els_in = nel;
    if (nel < 9) { sset(note, nc, "manifest_sliced.json has only %zu elements: nothing cut", nel); goto out; }
    first = nel - 8;
    for (i = 0; i < nel; i++) {
        if (py_truthy(jget(els->u.arr.v[i], "isCleaningLayer")) != (i >= first)) {
            sset(note, nc, "the cleaning elements of manifest_sliced.json are not its last 8: nothing cut");
            goto out;
        }
    }
    for (i = 0; i < 8; i++) {
        const jval *hd = jget(els->u.arr.v[first + i], "head");
        size_t lim = i + 1 < 8 ? cand[i + 1] : g.end_line;
        if (!hd || hd->t != J_STR || hd->u.s.n == 0 || cand[i] + hd->u.s.n > lim || memcmp(fm.p + cand[i], hd->u.s.p, hd->u.s.n) != 0) {
            sset(note, nc, "feather pass %zu does not start with the head of cleaning element %zu: nothing cut", i + 1, first + i);
            goto out;
        }
    }
    /* cut manifest: from the end of the last kept element to the end of the last element */
    gap_a = els->u.arr.v[first + keep - 1]->b;
    gap_b = els->u.arr.v[nel - 1]->b;
    buf_put(&nms, ms.p, gap_a);
    buf_put(&nms, ms.p + gap_b, ms.n - gap_b);
    if (nms.oom) { rc = RF_E_NOMEM; goto out; }
    root2 = json_parse(nms.p, nms.n, &ar2, err, sizeof err);
    if (!root2 || !jdoc_cut_same(root, root2, first + keep)) {
        sset(note, nc, "the cut manifest_sliced.json does not read back as intended: nothing cut");
        rc = RF_E_VERIFY;
        goto out;
    }
    /* cut galvo.gc: everything before pass keep+1, then the "# END" tail */
    segv_range(&sv, 0, cand[keep]);
    segv_range(&sv, g.end_line, fm.n);
    if (sv.oom) { rc = RF_E_NOMEM; goto out; }
    expect = segv_size(&sv);
    if (write_replace(msp, nms.p, nms.n, err, sizeof err) != 0) { sset(note, nc, "%s: nothing cut", err); rc = RF_E_IO; goto out; }
    if (write_gcode(galvo, fm.p, &sv, expect, &fm, err, sizeof err) != 0) {
        char e2[200];
        if (write_replace(msp, ms.p, ms.n, e2, sizeof e2) != 0)
            sset(note, nc, "%s; manifest_sliced.json could NOT be put back (%s)", err, e2);
        else
            sset(note, nc, "%s: nothing cut", err);
        rc = RF_E_IO;
        goto out;
    }
    ft->kept = keep;
    ft->size_out = (long long)expect;
    ft->els_out = first + keep;
    sset(note, nc, "kept the first %d of 8 feather passes: galvo.gc %lld -> %llu bytes, manifest_sliced.json %zu -> %zu elements",
         keep, ft->size_in, expect, nel, first + keep);
    rc = 0;
out:
    fmap_close(&fm);
    ganal_free(&g);
    segv_free(&sv);
    free(cand);
    free(gdir);
    free(galvo);
    free(msp);
    free(erp);
    buf_free(&ms);
    buf_free(&nms);
    buf_free(&er);
    arena_free(&ar);
    arena_free(&ar2);
    arena_free(&ar3);
    return rc;
}

/* strip_cleaning.keep_top_k (11-03 07:27 version): layer_thresholds(n) = [1] + [255 - (256*(n-i))//n for i in 1..n-1],
 * covs[i] = share of pixels with darkness >= thr[i], untouched = share with darkness < thr[k] */
static long long layer_thr(long long i, long long n)
{
    long long t = 255 - (256 * (n - i)) / n; /* 11-03 08:12: max(1, ...), pure white is never burned */
    return i == 0 ? 1 : (t < 1 ? 1 : t);
}
static int keep_top_k_py(const char *task, arena_t *ar, const jval *root, const jval *e, long long n, double pct, int *k_out,
                         double *untouched, char *why, size_t wc, char *note, size_t nc)
{
    char *fname = NULL, *img = element_image_path(task, e, &fname);
    buf_t b = {0};
    DWORD we;
    pngimg pi;
    uint64_t ge[257], total, unt = 0;
    double *covs = NULL;
    long long i, tk;
    int rc = 0, d;
    *k_out = 0;
    *untouched = NAN;
    why[0] = 0;
    memset(&pi, 0, sizeof pi);
    if (n < 2 || !img || !path_isfile(img)) {
        sset(why, wc, "no slicer image or N=%lld", n);
        goto out;
    }
    if (read_all(img, &b, &we) != 0) { sset(note, nc, "cannot read the slicer image (Win32 error %lu)", we); rc = RF_E_IMAGE; goto out; }
    if (png_decode(b.p, b.n, &pi, 0) != 0) { sset(note, nc, "slicer image: %s", pi.err); rc = RF_E_IMAGE; goto out; }
    if (n > RF_MAX_N) { sset(note, nc, "sliceNumber %lld not supported (limit %lld)", n, RF_MAX_N); rc = RF_E_JSON; goto out; }
    total = (uint64_t)pi.w * pi.h;
    ge[256] = 0;
    for (d = 255; d >= 0; d--) ge[d] = ge[d + 1] + pi.hist[255 - d]; /* hist is of gray = 255 - darkness */
    covs = malloc((size_t)n * sizeof(double));
    if (!covs) { rc = RF_E_NOMEM; goto out; }
    for (i = 0; i < n; i++) covs[i] = (double)ge[layer_thr(i, n)] / (double)total;
    *k_out = choose_k(covs, n, pct, why, wc);
    /* lost = element_v_loss(element, params or {}); if k and lost: k = avoid_v_loss(k, n, covs, lost) */
    {
        vline_geo vg;
        int vr = vline_geo_of(ar, root, e, &vg);
        if (vr == -2) { sset(note, nc, "V-line inputs: %s (Python: OverflowError)", vg.why); rc = RF_E_JSON; goto out; }
        if (*k_out > 0 && vr == 1) {
            long long m0, mb;
            *k_out = avoid_v_loss(*k_out, n, covs, &vg, &m0, &mb);
        }
    }
    tk = layer_thr(*k_out, n);
    for (d = 0; d < 256; d++)
        if (d < tk) unt += pi.hist[255 - d];
    *untouched = (double)unt / (double)total;
out:
    free(fname);
    free(img);
    free(covs);
    free(pi.gray);
    buf_free(&b);
    return rc;
}

/* strip_cleaning.declared(): relief_fix.json in "prepare" mode whose int(gcode_bytes) equals the size of galvo.gc.
 * 1 declared (kbuf = str(d.get("k"))), 0 not, -1 Python raises (the file holds JSON that is not an object).
 * *prep_rec: the file is a prepare record (declared or not). */
static int declared_rec(const char *task, const char *galvo, char *kbuf, size_t kc, int *prep_rec)
{
    char *rp = py_join(task, "relief_fix.json"), err[160];
    buf_t b = {0};
    arena_t ar = {0};
    DWORD we;
    const jval *v;
    int rc = 0;
    *prep_rec = 0;
    kbuf[0] = 0;
    if (!rp || read_all(rp, &b, &we) != 0) goto out;
    v = json_parse(b.p, b.n, &ar, err, sizeof err);
    if (!v) goto out;
    if (v->t != J_OBJ) { rc = -1; goto out; }
    if (!jstr_is(jget(v, "mode"), "prepare")) goto out;
    *prep_rec = 1;
    {
        const jval *gb = jget(v, "gcode_bytes");
        pyint x;
        long long gv, gsz;
        unsigned long long gmt;
        if (!gb) goto out; /* int(-1) never equals a file size */
        if (py_int(&ar, gb, &x, err, sizeof err) != 0) goto out;
        if (!pyint_to_ll(&x, &gv) || path_stat(galvo, &gsz, &gmt) != 0 || gv != gsz) goto out;
        {
            buf_t s = {0};
            if (!py_str(jget(v, "k"), &s)) buf_puts(&s, "...");
            sset(kbuf, kc, "%.*s", (int)(s.n > 40 ? 40 : s.n), s.p ? (const char *)s.p : "");
            buf_free(&s);
        }
        rc = 1;
    }
out:
    free(rp);
    buf_free(&b);
    arena_free(&ar);
    return rc;
}

int rf_fix_task(const char *task_dir_utf8, const rf_opts *opts, rf_result *res)
{
    rf_opts o;
    rf_result dummy;
    char *manp = NULL, *gdir = NULL, *galvo = NULL, *repp = NULL, err[256], why[200];
    buf_t man = {0}, rep = {0};
    arena_t ar = {0};
    DWORD we;
    const jval *root, *rel = NULL;
    size_t nrel = 0, cut_from = 0, cut_to = 0, i;
    int rc = 1, cleanup_on = 0, have_cut = 0, have_kp = 0, k = 0, r, kept_cleaning = 0, prepared = 0;
    fmap fm;
    ganal g;
    segv sv = {0};
    dec shift = {0, NULL, 0, 0};
    double t0 = now_ms(), untouched = NAN;
    char notes[512], decl_k[64];
    notes[0] = 0;
    decl_k[0] = 0;
    memset(&g, 0, sizeof g);
    memset(&fm, 0, sizeof fm);
    fm.f = INVALID_HANDLE_VALUE;
    if (!res) res = &dummy;
    res_init(res);
    if (opts) o = *opts;
    else rf_opts_init(&o);
    if (!task_dir_utf8 || !*task_dir_utf8) { sset(res->note, sizeof res->note, "no task folder given"); return RF_E_ARG; }
    manp = py_join(task_dir_utf8, "manifest.json");
    gdir = py_join(task_dir_utf8, "gcode");
    galvo = gdir ? py_join(gdir, "galvo.gc") : NULL;
    if (!manp || !galvo) { rc = RF_E_NOMEM; goto out; }
    /* strip_cleaning.py --test / --once: decl = declared(task_dir) before plan() */
    {
        int prep_rec = 0;
        r = declared_rec(task_dir_utf8, galvo, decl_k, sizeof decl_k, &prep_rec);
        if (r < 0) { sset(res->note, sizeof res->note, "relief_fix.json is not a JSON object (Python: AttributeError)"); rc = RF_E_JSON; goto out; }
        prepared = r;
        /* report: relief_fix.json (read by strip_cleaning.fixed_by_plugin) unless it holds the prepare record */
        repp = py_join(task_dir_utf8, prep_rec ? "relief_fix_gcode.json" : "relief_fix.json");
        if (!repp) { rc = RF_E_NOMEM; goto out; }
    }
    /* manifest.json */
    r = read_all(manp, &man, &we);
    if (r != 0) { sset(res->note, sizeof res->note, "cannot read manifest.json (Win32 error %lu)", we); rc = RF_E_IO; goto out; }
    root = json_parse(man.p, man.n, &ar, err, sizeof err);
    if (!root) { sset(res->note, sizeof res->note, "manifest.json: %s", err); rc = RF_E_JSON; goto out; }
    if (relief_elements(root, &rel, &nrel, err, sizeof err) != 0) { sset(res->note, sizeof res->note, "manifest.json: %s", err); rc = RF_E_JSON; goto out; }
    if (!nrel) { sset(res->note, sizeof res->note, "not a relief job"); rc = 1; goto out; }
    {
        const jval *els = jget(root, "elements");
        for (i = 0; i < els->u.arr.n; i++) {
            const jval *e = els->u.arr.v[i];
            if (jstr_is(jget(e, "processingType"), "RELIEF") && py_truthy(jget(e, "reliefCleanUp"))) { cleanup_on = 1; break; }
        }
    }
    /* galvo.gc */
    r = fmap_open(galvo, &fm, &we);
    if (r != 0) {
        sset(res->note, sizeof res->note, r == -2 ? "cannot mmap an empty galvo.gc" : "cannot open gcode\\galvo.gc (Win32 error %lu)", we);
        rc = r == -2 ? RF_E_GCODE : RF_E_IO;
        goto out;
    }
    res->size_in = (long long)fm.n;
    if (g_analyze(fm.p, fm.n, &g) != 0) { sset(res->note, sizeof res->note, "%s", g.err); rc = RF_E_GCODE; goto out; }
    res->n_layers = (int)g.nl;
    /* 2. cleaning blocks */
    if (cleanup_on) {
        note_add(notes, sizeof notes, "Cleanup on: cleaning passes kept");
    } else if (g.n_clean) {
        if (!o.strip_cleaning) {
            note_add(notes, sizeof notes, "--no-strip: cleaning passes kept");
            kept_cleaning = 1;
        } else {
            r = plan_cleaning(task_dir_utf8, fm.p, fm.n, &g, &cut_from, &cut_to, NULL, NULL, res->note, sizeof res->note);
            if (r != 0) { rc = r; goto out; }
            if (cut_from != g.layer_end) {
                sset(res->note, sizeof res->note, "cleaning start %zu != layer end %zu", cut_from, g.layer_end);
                rc = 1;
                goto out;
            }
            have_cut = 1;
        }
    }
    /* 3. keep top (plan(task_dir, decl); wrapper_installed() is the Python side's own check and is not mirrored) */
    if (prepared) {
        note_add(notes, sizeof notes, "keep-top declared to the slicer before generation (k=%s)", decl_k);
    } else if (!(o.keep_top_pct > 0) && !isnan(o.keep_top_pct)) {
        note_add(notes, sizeof notes, "keep-top off");
    } else if (cleanup_on) {
        note_add(notes, sizeof notes, "keep-top not applied while Cleanup is on");
    } else if (kept_cleaning) {
        note_add(notes, sizeof notes, "keep-top not applied while cleaning passes are kept");
    } else if (nrel != 1) {
        note_add(notes, sizeof notes, "keep-top not applied: %zu relief objects in one job", nrel);
    } else {
        pyint sn;
        long long snv = 0;
        const jval *snj = jget(rel, "sliceNumber");
        if (py_int_or(&ar, snj, 0, &sn, err, sizeof err) != 0) { sset(res->note, sizeof res->note, "sliceNumber: %s", err); rc = RF_E_JSON; goto out; }
        if (!pyint_to_ll(&sn, &snv) || snv != (long long)g.nl) {
            buf_t s = {0};
            if (!py_str(snj, &s)) buf_puts(&s, "<list or dict>");
            note_add(notes, sizeof notes, "keep-top not applied: sliceNumber %.*s != %zu layers in G-code", (int)(s.n > 60 ? 60 : s.n),
                     s.p ? (const char *)s.p : "", g.nl);
            buf_free(&s);
        } else {
            r = keep_top_k_py(task_dir_utf8, &ar, root, rel, snv, o.keep_top_pct, &k, &untouched, why, sizeof why, res->note, sizeof res->note);
            if (r != 0) { rc = r; goto out; }
            if (why[0]) note_add(notes, sizeof notes, "keep-top not applied: %s", why);
            if (k > 0) {
                /* keep_top.plan(mm, k=k, layer_end, info) */
                const unsigned char *h = fm.p;
                dec v0, vk;
                size_t li;
                if (k > (int)g.nl - 1) { sset(res->note, sizeof res->note, "k must be in 0..%zu", g.nl - 1); rc = RF_E_GCODE; goto out; }
                if (!g.one_v) { sset(res->note, sizeof res->note, "V lines found outside layer prologues; refusing to shift"); rc = RF_E_GCODE; goto out; }
                if (dec_parse(h + g.v0_s, g.v0_e - g.v0_s, &v0) != 0) { rc = RF_E_NOMEM; goto out; }
                {
                    const gblk *Bk = &g.b[g.lf[k]];
                    if (dec_parse(h + Bk->v_ns, Bk->v_ne - Bk->v_ns, &vk) != 0) { dec_free(&v0); rc = RF_E_NOMEM; goto out; }
                }
                r = dec_sub(&vk, &v0, &shift);
                dec_free(&v0);
                dec_free(&vk);
                if (r != 0) { sset(res->note, sizeof res->note, "Decimal overflow computing the V shift"); rc = RF_E_GCODE; goto out; }
                if (g.has_cam) {
                    segv_range(&sv, 0, g.cam_end);
                    segv_lit(&sv, h + g.tail_s, g.tail_e - g.tail_s);
                } else {
                    segv_range(&sv, 0, g.header_end);
                }
                {
                    size_t pos = g.b[g.lf[k]].start;
                    for (li = (size_t)k; li < g.nl; li++) {
                        const gblk *B = &g.b[g.lf[li]];
                        dec vl, nv;
                        buf_t s = {0};
                        size_t ndec = 0, q;
                        if (!B->has_v) continue;
                        for (q = B->v_ns; q < B->v_ne; q++)
                            if (h[q] == '.') { ndec = B->v_ne - q - 1; break; }
                        if (dec_parse(h + B->v_ns, B->v_ne - B->v_ns, &vl) != 0) { rc = RF_E_NOMEM; goto out; }
                        r = dec_sub(&vl, &shift, &nv);
                        dec_free(&vl);
                        if (r != 0 || dec_fmt_like(&nv, ndec, &s) != 0) {
                            dec_free(&nv);
                            buf_free(&s);
                            sset(res->note, sizeof res->note, "V of layer %zu cannot be written exactly with %zu decimals", li + 1, ndec);
                            rc = RF_E_GCODE;
                            goto out;
                        }
                        dec_free(&nv);
                        segv_range(&sv, pos, B->v_ns);
                        segv_lit(&sv, s.p, s.n);
                        pos = B->v_ne;
                        buf_free(&s);
                    }
                    segv_range(&sv, pos, g.layer_end);
                }
                have_kp = 1;
            }
        }
    }
    if (!have_cut && !have_kp) {
        sset(res->note, sizeof res->note, "nothing to change%s%s%s", notes[0] ? " (" : "", notes, notes[0] ? ")" : "");
        rc = 1;
        goto out;
    }
    if (!have_kp) segv_range(&sv, 0, g.layer_end);
    if (have_cut) segv_range(&sv, cut_to, fm.n);
    else segv_range(&sv, g.layer_end, fm.n);
    if (sv.oom) { rc = RF_E_NOMEM; goto out; }
    /* result fields */
    res->k = k;
    res->cleaning_cut = have_cut ? (int)g.n_clean : 0;
    {
        buf_t s = {0};
        if (have_kp) dec_str(&shift, &s);
        else buf_puts(&s, "0");
        sset(res->shift, sizeof res->shift, "%s", s.p ? (const char *)s.p : "0");
        buf_free(&s);
    }
    res->size_out = (long long)segv_size(&sv);
    res->untouched = untouched;
    if (deepest_calc(&ar, rel, (long long)g.nl - k, &res->deepest_mm, err, sizeof err) != 0) {
        sset(res->note, sizeof res->note, "zAxisMove/vDecline/vLayers: %s", err);
        rc = RF_E_JSON;
        goto out;
    }
    sset(res->note, sizeof res->note, "%s", notes);
    if (o.dry_run) {
        note_add(res->note, sizeof res->note, "dry run: nothing written");
        rc = 0;
        goto out;
    }
    if (write_gcode(galvo, fm.p, &sv, segv_size(&sv), &fm, err, sizeof err) != 0) {
        note_add(res->note, sizeof res->note, "%s", err);
        rc = RF_E_IO;
        goto out;
    }
    rc = 0;
    /* report */
    buf_printf(&rep, "{\n  \"mode\": \"fix\",\n  \"k\": %d,\n  \"n_layers\": %d,\n  \"cleaning_cut\": %d,\n  \"shift\": ", res->k, res->n_layers,
               res->cleaning_cut);
    jw_cstr(&rep, res->shift);
    buf_printf(&rep, ",\n  \"size_in\": %lld,\n  \"size_out\": %lld,\n  \"untouched\": ", res->size_in, res->size_out);
    jw_double(&rep, res->untouched);
    buf_puts(&rep, ",\n  \"deepest_mm\": ");
    jw_double(&rep, res->deepest_mm);
    buf_puts(&rep, ",\n  \"note\": ");
    jw_cstr(&rep, res->note);
    buf_printf(&rep, ",\n  \"time_ms\": %.1f\n}\n", now_ms() - t0);
    if (!rep.oom && write_replace(repp, rep.p, rep.n, err, sizeof err) != 0) note_add(res->note, sizeof res->note, "report not written: %s", err);
out:
    fmap_close(&fm);
    ganal_free(&g);
    segv_free(&sv);
    dec_free(&shift);
    free(manp);
    free(gdir);
    free(galvo);
    free(repp);
    buf_free(&man);
    buf_free(&rep);
    arena_free(&ar);
    if (rc != 0 && rc != 1) res->k = 0;
    if (rc == 1) res->k = 0;
    return rc;
}

/* ================================================================== misc */
void rf_opts_init(rf_opts *o)
{
    if (!o) return;
    o->keep_top_pct = 5.0;
    o->strip_cleaning = 1;
    o->dry_run = 0;
    o->transparent_as_color = 1;
    o->feather_passes = 0;
}
const char *rf_strerror(int rc)
{
    switch (rc) {
    case 0: return "done";
    case 1: return "nothing to change";
    case RF_E_ARG: return "bad argument";
    case RF_E_IO: return "file system error";
    case RF_E_JSON: return "manifest / json error";
    case RF_E_GCODE: return "G-code structure not accepted";
    case RF_E_IMAGE: return "slicer image unreadable";
    case RF_E_NOMEM: return "out of memory";
    case RF_E_VERIFY: return "self-check failed";
    case RF_E_STATE: return "stale backups";
    case RF_E_LUT: return "layer count off by more than one layer";
    default: return "internal error";
    }
}
