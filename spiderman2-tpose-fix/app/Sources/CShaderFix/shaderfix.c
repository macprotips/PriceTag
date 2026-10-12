/* See shaderfix.h. A line-by-line port of shader-fix/typed2raw.py and name_values.py;
 * tests check that both give byte-identical output. */
#include "shaderfix.h"

#include <ctype.h>
#include <regex.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint64_t sf_shader_hash(const void *data, size_t size)
{
    const uint8_t *p = data;
    uint64_t h = 0xcbf29ce484222325ull;
    size_t i;

    for (i = 0; i < size; i++)
        h = (h * 0x100000001b3ull) ^ p[i];
    return h;
}

int sf_find_container(const uint8_t *data, size_t size, uint64_t hash, size_t *offset, size_t *found_size)
{
    size_t i;

    for (i = 0; i + 32 <= size; i++)
    {
        uint32_t total;

        if (data[i] != 'D' || memcmp(data + i, "DXBC", 4))
            continue;
        /* "DXBC", 16-byte digest, version 1.0, total size, part count */
        if (data[i + 20] != 1 || data[i + 21] || data[i + 22] || data[i + 23])
            continue;
        memcpy(&total, data + i + 24, 4);
        if (total < 32 || total > size - i)
            continue;
        if (sf_shader_hash(data + i, total) == hash)
        {
            *offset = i;
            *found_size = total;
            return 1;
        }
    }
    return 0;
}

/* ---- small helpers: a growable line list and a string builder */

struct lines { char **v; size_t n, cap; };

static int lines_push(struct lines *l, char *s)
{
    if (!s)
        return 0;
    if (l->n == l->cap)
    {
        size_t cap = l->cap ? l->cap * 2 : 1024;
        char **v = realloc(l->v, cap * sizeof(*v));
        if (!v)
            return 0;
        l->v = v;
        l->cap = cap;
    }
    l->v[l->n++] = s;
    return 1;
}

static void lines_free(struct lines *l)
{
    size_t i;

    for (i = 0; i < l->n; i++)
        free(l->v[i]);
    free(l->v);
    memset(l, 0, sizeof(*l));
}

static char *dup_range(const char *s, size_t n)
{
    char *d = malloc(n + 1);

    if (d)
    {
        memcpy(d, s, n);
        d[n] = 0;
    }
    return d;
}

/* Splits on '\n' exactly like Python's str.split('\n'). */
static int split_lines(const char *text, struct lines *out)
{
    const char *start = text, *nl;

    while ((nl = strchr(start, '\n')))
    {
        if (!lines_push(out, dup_range(start, (size_t)(nl - start))))
            return 0;
        start = nl + 1;
    }
    return lines_push(out, dup_range(start, strlen(start)));
}

static char *join_lines(const struct lines *l)
{
    size_t i, len = 0, at = 0;
    char *s;

    for (i = 0; i < l->n; i++)
        len += strlen(l->v[i]) + 1;
    if (!(s = malloc(len + 1)))
        return NULL;
    for (i = 0; i < l->n; i++)
    {
        size_t n = strlen(l->v[i]);
        memcpy(s + at, l->v[i], n);
        at += n;
        if (i + 1 < l->n)
            s[at++] = '\n';
    }
    s[at] = 0;
    return s;
}

static char *format(const char *fmt, ...)
{
    va_list ap;
    int n;
    char *s;

    va_start(ap, fmt);
    n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0 || !(s = malloc((size_t)n + 1)))
        return NULL;
    va_start(ap, fmt);
    vsnprintf(s, (size_t)n + 1, fmt, ap);
    va_end(ap);
    return s;
}

static char *group(const char *s, const regmatch_t *m)
{
    return dup_range(s + m->rm_so, (size_t)(m->rm_eo - m->rm_so));
}

static int in_set(char **set, size_t n, const char *s)
{
    size_t i;

    for (i = 0; i < n; i++)
        if (!strcmp(set[i], s))
            return 1;
    return 0;
}

static int starts_with(const char *s, const char *prefix)
{
    return !strncmp(s, prefix, strlen(prefix));
}

static void set_error(char *error, size_t size, const char *msg)
{
    if (error && size)
        snprintf(error, size, "%s", msg);
}

/* Python's \w and \s, in POSIX ERE */
#define ID "%[A-Za-z0-9_.]+"
#define IDF "%%[A-Za-z0-9_.]+"   /* the same, inside format() */
#define SP "[[:space:]]*"

char *sf_typed_to_raw(const char *ir, int space, int reg, int *handles_out, int *loads_out, char *error, size_t error_size)
{
    struct lines lines = {0}, out = {0};
    char *created[64], *annotated[64], *result = NULL, *pattern = NULL, *attr = NULL;
    size_t ncreated = 0, nannotated = 0, i;
    regex_t meta_re, bind_re, annot_re, load_re, decl_re;
    int have_meta = 0, have_bind = 0, have_annot = 0, have_load = 0, have_decl = 0, found_meta = 0, loads = 0;
    regmatch_t m[8];

    if (!split_lines(ir, &lines))
        goto oom;

    /* 1. the resource's metadata entry: TypedBuffer (10) -> RawBuffer (11) */
    pattern = format("^(![0-9]+ = !\\{i32 ([0-9]+), (%%[^*]+)\\* undef, !\"\", i32 %d, i32 %d, i32 1, )i32 10, i32 0, ![0-9]+\\}$",
            space, reg);
    if (!pattern || regcomp(&meta_re, pattern, REG_EXTENDED))
        goto bad_regex;
    have_meta = 1;
    for (i = 0; i < lines.n; i++)
    {
        if (!regexec(&meta_re, lines.v[i], 2, m, 0))
        {
            char *head = group(lines.v[i], &m[1]), *line = head ? format("%si32 11, i32 0, null}", head) : NULL;
            free(head);
            if (!line)
                goto oom;
            free(lines.v[i]);
            lines.v[i] = line;
            found_meta = 1;
            break;
        }
    }
    if (!found_meta)
    {
        char msg[96];
        snprintf(msg, sizeof(msg), "typed SRV space %d register %d not found in the resource metadata", space, reg);
        set_error(error, error_size, msg);
        goto done;
    }

    /* 2. handles created for that binding (SRV class 0) */
    free(pattern);
    if (reg == 0 && space == 0)
        pattern = format("^" SP "(" IDF ") = call %%dx\\.types\\.Handle @dx\\.op\\.createHandleFromBinding\\(i32 217, "
                "%%dx\\.types\\.ResBind (zeroinitializer|\\{ i32 0, i32 0, i32 0, i8 0 \\})");
    else
        pattern = format("^" SP "(" IDF ") = call %%dx\\.types\\.Handle @dx\\.op\\.createHandleFromBinding\\(i32 217, "
                "%%dx\\.types\\.ResBind \\{ i32 %d, i32 %d, i32 %d, i8 0 \\}", reg, reg, space);
    if (!pattern || regcomp(&bind_re, pattern, REG_EXTENDED))
        goto bad_regex;
    have_bind = 1;
    for (i = 0; i < lines.n; i++)
        if (!regexec(&bind_re, lines.v[i], 2, m, 0) && ncreated < 64)
            if (!(created[ncreated++] = group(lines.v[i], &m[1])))
                goto oom;
    if (!ncreated)
    {
        set_error(error, error_size, "no handle created for the binding");
        goto done;
    }

    /* 3. their annotations: {10, 261} (typed u32) -> {11, 0} (raw) */
    if (regcomp(&annot_re, "^(" SP "(" ID ") = call %dx\\.types\\.Handle @dx\\.op\\.annotateHandle\\(i32 216, "
            "%dx\\.types\\.Handle (" ID "), %dx\\.types\\.ResourceProperties )\\{ i32 10, i32 261 \\}(\\).*)$", REG_EXTENDED))
        goto bad_regex;
    have_annot = 1;
    for (i = 0; i < lines.n; i++)
    {
        char *source, *head, *tail, *t, *line;

        if (regexec(&annot_re, lines.v[i], 5, m, 0))
            continue;
        if (!(source = group(lines.v[i], &m[3])))
            goto oom;
        if (!in_set(created, ncreated, source))
        {
            free(source);
            continue;
        }
        free(source);
        head = group(lines.v[i], &m[1]);
        tail = group(lines.v[i], &m[4]);
        if (nannotated < 64)
            annotated[nannotated++] = group(lines.v[i], &m[2]);
        if (!head || !tail || !annotated[nannotated - 1])
            goto oom;
        /* the comment names the resource type: TypedBuffer<U32> -> ByteAddressBuffer (all occurrences) */
        while ((t = strstr(tail, "TypedBuffer<U32>")))
        {
            char *fixed = format("%.*sByteAddressBuffer%s", (int)(t - tail), tail, t + strlen("TypedBuffer<U32>"));
            free(tail);
            if (!(tail = fixed))
                goto oom;
        }
        line = format("%s{ i32 11, i32 0 }%s", head, tail);
        free(head);
        free(tail);
        if (!line)
            goto oom;
        free(lines.v[i]);
        lines.v[i] = line;
    }

    /* 4. loads: bufferLoad(index) -> rawBufferLoad(index << 2) */
    if (regcomp(&load_re, "^(" SP ")(" ID ") = call %dx\\.types\\.ResRet\\.i32 @dx\\.op\\.bufferLoad\\.i32\\(i32 68, "
            "%dx\\.types\\.Handle (" ID "), i32 ([^,]+), i32 undef\\)(.*)$", REG_EXTENDED))
        goto bad_regex;
    have_load = 1;
    for (i = 0; i < lines.n; i++)
    {
        char *ind, *val, *h, *idx;

        if (!regexec(&load_re, lines.v[i], 6, m, 0))
        {
            if (!(h = group(lines.v[i], &m[3])))
                goto oom;
            if (in_set(annotated, nannotated, h))
            {
                ind = group(lines.v[i], &m[1]);
                val = group(lines.v[i], &m[2]);
                idx = group(lines.v[i], &m[4]);
                loads++;
                if (!ind || !val || !idx
                        || !lines_push(&out, format("%s%%t2r%d = shl i32 %s, 2", ind, loads, idx))
                        || !lines_push(&out, format("%s%s = call %%dx.types.ResRet.i32 @dx.op.rawBufferLoad.i32(i32 139, "
                                "%%dx.types.Handle %s, i32 %%t2r%d, i32 undef, i8 1, i32 4)  ; "
                                "RawBufferLoad(srv,index,elementOffset,mask,alignment)", ind, val, h, loads)))
                {
                    free(ind); free(val); free(idx); free(h);
                    goto oom;
                }
                free(ind); free(val); free(idx); free(h);
                continue;
            }
            free(h);
        }
        if (!lines_push(&out, lines.v[i]))
            goto oom;
        lines.v[i] = NULL;
    }
    if (!loads)
    {
        set_error(error, error_size, "no typed loads found on the handle");
        goto done;
    }

    /* 5. declare rawBufferLoad (after bufferLoad's declaration, with its attributes) */
    {
        int has_raw = 0, any_buffer_load_call = 0;

        for (i = 0; i < out.n; i++)
        {
            if (starts_with(out.v[i], "declare %dx.types.ResRet.i32 @dx.op.rawBufferLoad.i32("))
                has_raw = 1;
        }
        if (regcomp(&decl_re, "^declare %dx\\.types\\.ResRet\\.i32 @dx\\.op\\.bufferLoad\\.i32\\(i32, %dx\\.types\\.Handle, i32, i32\\) (#[0-9]+)$",
                REG_EXTENDED))
            goto bad_regex;
        have_decl = 1;
        if (!has_raw)
        {
            for (i = 0; i < out.n; i++)
            {
                if (!regexec(&decl_re, out.v[i], 2, m, 0))
                {
                    char *raw;
                    size_t k;

                    if (!(attr = group(out.v[i], &m[1])))
                        goto oom;
                    raw = format("declare %%dx.types.ResRet.i32 @dx.op.rawBufferLoad.i32(i32, %%dx.types.Handle, i32, i32, i8, i32) %s", attr);
                    /* insert "", "; Function Attrs: nounwind readonly", raw after line i */
                    if (!raw || !lines_push(&out, dup_range("", 0)) || !lines_push(&out, dup_range("", 0)) || !lines_push(&out, dup_range("", 0)))
                    {
                        free(raw);
                        goto oom;
                    }
                    free(out.v[out.n - 1]);
                    free(out.v[out.n - 2]);
                    free(out.v[out.n - 3]);
                    for (k = out.n - 1; k > i + 3; k--)
                        out.v[k] = out.v[k - 3];
                    out.v[i + 1] = dup_range("", 0);
                    out.v[i + 2] = format("; Function Attrs: nounwind readonly");
                    out.v[i + 3] = raw;
                    if (!out.v[i + 1] || !out.v[i + 2])
                        goto oom;
                    break;
                }
            }
        }

        /* 6. drop bufferLoad's declaration (and its attribute comment) once nothing calls it */
        for (i = 0; i < out.n; i++)
            if (strstr(out.v[i], "call %dx.types.ResRet.i32 @dx.op.bufferLoad.i32("))
                any_buffer_load_call = 1;
        if (!any_buffer_load_call)
        {
            size_t w = 0;

            for (i = 0; i < out.n; i++)
            {
                if (starts_with(out.v[i], "declare %dx.types.ResRet.i32 @dx.op.bufferLoad.i32("))
                {
                    free(out.v[i]);
                    if (w > 0 && starts_with(out.v[w - 1], "; Function Attrs:"))
                        free(out.v[--w]);
                    continue;
                }
                out.v[w++] = out.v[i];
            }
            out.n = w;
        }
    }

    if (!(result = join_lines(&out)))
        goto oom;
    if (handles_out)
        *handles_out = (int)nannotated;
    if (loads_out)
        *loads_out = loads;
    goto done;

bad_regex:
    set_error(error, error_size, "internal error: bad pattern");
    goto done;
oom:
    set_error(error, error_size, "out of memory");
    free(result);
    result = NULL;
done:
    if (have_meta) regfree(&meta_re);
    if (have_bind) regfree(&bind_re);
    if (have_annot) regfree(&annot_re);
    if (have_load) regfree(&load_re);
    if (have_decl) regfree(&decl_re);
    for (i = 0; i < ncreated; i++) free(created[i]);
    for (i = 0; i < nannotated; i++) free(annotated[i]);
    free(pattern);
    free(attr);
    lines_free(&lines);
    lines_free(&out);
    return result;
}

/* %N -> %vN where N is followed by a non-word character (Python's %(\d+)\b) */
static char *rename_locals(const char *s)
{
    size_t len = strlen(s), cap = len * 2 + 16, at = 0, i = 0;
    char *d = malloc(cap);

    if (!d)
        return NULL;
    while (i < len)
    {
        if (s[i] == '%' && isdigit((unsigned char)s[i + 1]))
        {
            size_t j = i + 1;

            while (isdigit((unsigned char)s[j]))
                j++;
            if (!(isalnum((unsigned char)s[j]) || s[j] == '_'))
            {
                if (at + (j - i) + 2 >= cap)
                {
                    char *nd = realloc(d, cap = cap * 2 + (j - i) + 16);
                    if (!nd) { free(d); return NULL; }
                    d = nd;
                }
                d[at++] = '%';
                d[at++] = 'v';
                memcpy(d + at, s + i + 1, j - i - 1);
                at += j - i - 1;
                i = j;
                continue;
            }
        }
        if (at + 2 >= cap)
        {
            char *nd = realloc(d, cap *= 2);
            if (!nd) { free(d); return NULL; }
            d = nd;
        }
        d[at++] = s[i++];
    }
    d[at] = 0;
    return d;
}

char *sf_name_values(const char *ir)
{
    struct lines lines = {0}, out = {0};
    char *result = NULL;
    int inside = 0;
    size_t i;

    if (!split_lines(ir, &lines))
        goto done;
    for (i = 0; i < lines.n; i++)
    {
        const char *line = lines.v[i];

        if (starts_with(line, "define "))
        {
            inside = 1;
            if (!lines_push(&out, dup_range(line, strlen(line))) || !lines_push(&out, dup_range("v0:", 3)))
                goto done;
            continue;
        }
        if (inside && !strcmp(line, "}"))
            inside = 0;
        if (inside)
        {
            if (starts_with(line, "; <label>:") && isdigit((unsigned char)line[10]))
            {
                const char *p = line + 10;
                size_t n = 0;
                char *rest;

                while (isdigit((unsigned char)p[n]))
                    n++;
                if (!(rest = rename_locals(p + n)) || !lines_push(&out, format("v%.*s:%s", (int)n, p, rest)))
                {
                    free(rest);
                    goto done;
                }
                free(rest);
                continue;
            }
            if (!lines_push(&out, rename_locals(line)))
                goto done;
        }
        else if (!lines_push(&out, dup_range(line, strlen(line))))
            goto done;
    }
    result = join_lines(&out);
done:
    lines_free(&lines);
    lines_free(&out);
    return result;
}
