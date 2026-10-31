/* Small in-place JSON parser: enough for the schedule manifest. */
#include "common.h"
#include "json.h"

#define POOL_NODES 4096
struct jpool { struct jpool *next; int used; jnode nodes[POOL_NODES]; };

static jnode *alloc_node(jpool **pool) {
    if (!*pool || (*pool)->used == POOL_NODES) {
        jpool *p = calloc(1, sizeof *p);
        if (!p) return NULL;
        p->next = *pool;
        *pool = p;
    }
    jnode *n = &(*pool)->nodes[(*pool)->used++];
    memset(n, 0, sizeof *n);
    return n;
}

void json_free(jpool *pool) {
    while (pool) { jpool *n = pool->next; free(pool); pool = n; }
}

typedef struct { char *p; jpool **pool; } P;

static void ws(P *x) { while (*x->p == ' ' || *x->p == '\n' || *x->p == '\r' || *x->p == '\t') x->p++; }

static int utf8(char *o, unsigned cp) {
    if (cp < 0x80) { o[0] = (char)cp; return 1; }
    if (cp < 0x800) { o[0] = (char)(0xC0 | cp >> 6); o[1] = (char)(0x80 | (cp & 63)); return 2; }
    if (cp < 0x10000) { o[0] = (char)(0xE0 | cp >> 12); o[1] = (char)(0x80 | ((cp >> 6) & 63)); o[2] = (char)(0x80 | (cp & 63)); return 3; }
    o[0] = (char)(0xF0 | cp >> 18); o[1] = (char)(0x80 | ((cp >> 12) & 63));
    o[2] = (char)(0x80 | ((cp >> 6) & 63)); o[3] = (char)(0x80 | (cp & 63)); return 4;
}

/* x->p at opening quote; unescapes in place; returns start. */
static char *str(P *x) {
    char *start = ++x->p, *o = start;
    while (*x->p && *x->p != '"') {
        if (*x->p == '\\') {
            x->p++;
            switch (*x->p) {
            case 'n': *o++ = '\n'; break; case 't': *o++ = '\t'; break;
            case 'r': *o++ = '\r'; break; case 'b': *o++ = '\b'; break;
            case 'f': *o++ = '\f'; break;
            case 'u': {
                unsigned cp = (unsigned)strtoul((char[5]){x->p[1], x->p[2], x->p[3], x->p[4], 0}, NULL, 16);
                x->p += 4;
                if (cp >= 0xD800 && cp < 0xDC00 && x->p[1] == '\\' && x->p[2] == 'u') {
                    unsigned lo = (unsigned)strtoul((char[5]){x->p[3], x->p[4], x->p[5], x->p[6], 0}, NULL, 16);
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    x->p += 6;
                }
                o += utf8(o, cp);
                break;
            }
            default: *o++ = *x->p;
            }
            x->p++;
        } else *o++ = *x->p++;
    }
    if (*x->p != '"') return NULL;
    x->p++;
    *o = 0;
    return start;
}

static jnode *value(P *x, int depth);

static jnode *value(P *x, int depth) {
    if (depth > 64) return NULL;
    ws(x);
    jnode *n = alloc_node(x->pool);
    if (!n) return NULL;
    char c = *x->p;
    if (c == '{' || c == '[') {
        n->t = c == '{' ? J_OBJ : J_ARR;
        x->p++;
        jnode **tail = &n->child;
        ws(x);
        if (*x->p == (c == '{' ? '}' : ']')) { x->p++; return n; }
        for (;;) {
            const char *key = NULL;
            if (n->t == J_OBJ) {
                ws(x);
                if (*x->p != '"' || !(key = str(x))) return NULL;
                ws(x);
                if (*x->p++ != ':') return NULL;
            }
            jnode *v = value(x, depth + 1);
            if (!v) return NULL;
            v->key = key;
            *tail = v; tail = &v->next;
            ws(x);
            if (*x->p == ',') { x->p++; continue; }
            if (*x->p == (n->t == J_OBJ ? '}' : ']')) { x->p++; return n; }
            return NULL;
        }
    }
    if (c == '"') { n->t = J_STR; n->s = str(x); return n->s ? n : NULL; }
    if (!strncmp(x->p, "true", 4)) { n->t = J_BOOL; n->n = 1; x->p += 4; return n; }
    if (!strncmp(x->p, "false", 5)) { n->t = J_BOOL; n->n = 0; x->p += 5; return n; }
    if (!strncmp(x->p, "null", 4)) { n->t = J_NULL; x->p += 4; return n; }
    char *end;
    n->t = J_NUM;
    n->n = strtod(x->p, &end);
    if (end == x->p) return NULL;
    x->p = end;
    return n;
}

jnode *json_parse(char *text, jpool **pool) {
    *pool = NULL;
    P x = { text, pool };
    jnode *root = value(&x, 0);
    if (!root) { json_free(*pool); *pool = NULL; }
    return root;
}

const jnode *json_get(const jnode *o, const char *key) {
    if (!o || o->t != J_OBJ) return NULL;
    for (const jnode *c = o->child; c; c = c->next)
        if (c->key && !strcmp(c->key, key)) return c;
    return NULL;
}

const char *json_str(const jnode *o, const char *key, const char *d) {
    const jnode *v = json_get(o, key);
    return v && v->t == J_STR ? v->s : d;
}

double json_num(const jnode *o, const char *key, double d) {
    const jnode *v = json_get(o, key);
    return v && (v->t == J_NUM || v->t == J_BOOL) ? v->n : d;
}

int json_bool(const jnode *o, const char *key, int d) {
    const jnode *v = json_get(o, key);
    return v && v->t == J_BOOL ? (int)v->n : d;
}
