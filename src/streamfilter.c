/*
 * Copyright (c) 2026-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

/* Consumer group filter expressions: lexer, parser and evaluator. See
 * streamfilter.h for the grammar. The semantics follow the stream filter
 * module's grammar crate, so a filter matches the same entries in both. */

#include "server.h"
#include "streamfilter.h"
#include "fast_float_strtod.h"

#include <math.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>

enum { SF_OR, SF_AND, SF_CMP, SF_IN, SF_EXISTS };
enum { SF_OP_EQ, SF_OP_NE, SF_OP_LT, SF_OP_LE, SF_OP_GT, SF_OP_GE, SF_OP_CONTAINS };

static const char *sfOpSymbol[] = {"=", "!=", "<", "<=", ">", ">=", "~"};

typedef struct sfValue {
    int is_num;         /* The literal reads as a number. */
    int has_int;        /* A number with no fraction that fits a long long. */
    long long ival;
    double dval;
    sds text;           /* String bytes, or the numeric literal as written
                           (used by '~', which is always bytewise). */
} sfValue;

/* IN members compiled for lookup: strings sorted bytewise, numbers sorted
 * both as exact integers and as normalized f64 bit patterns. */
typedef struct sfInSet {
    sds *strs; size_t nstrs;
    long long *ints; size_t nints;          /* Members that fit a long long. */
    uint64_t *nums; size_t nnums;           /* Every numeric member. */
    uint64_t *nonints; size_t nnonints;     /* Numeric members that are not
                                               long longs. */
} sfInSet;

struct sfNode {
    int type;
    int nchildren;          /* SF_OR, SF_AND: at least two. */
    sfNode **children;
    int field;              /* Index into streamFilter.fields. */
    int op;                 /* SF_CMP */
    int negated;            /* SF_IN, SF_EXISTS */
    sfValue value;          /* SF_CMP */
    sfInSet *set;           /* SF_IN */
};

/* ---------------------------------------------------------------------------
 * Numbers
 * ------------------------------------------------------------------------- */

#define SF_NOT_NUMBER 0
#define SF_NUMBER 1
#define SF_OVERFLOW 2
#define SF_UNDERFLOW 3

/* The one definition of "looks like a number", used both to type literals
 * and to read entry values: an optional '-', one or more digits, optionally
 * a '.' and one or more digits. No '+', no exponent, no inf or nan. A value
 * in that syntax whose nearest double is infinite, or is zero although a
 * digit is nonzero, is out of range rather than a number. */
static int sfReadNumber(const unsigned char *s, size_t len, int *has_int,
                        long long *ival, double *dval)
{
    size_t i = 0;
    int neg = 0;
    if (len && s[0] == '-') { neg = 1; i = 1; }
    size_t int_start = i;
    while (i < len && s[i] >= '0' && s[i] <= '9') i++;
    if (i == int_start) return SF_NOT_NUMBER;
    size_t int_end = i;
    int frac = 0;
    if (i < len) {
        if (s[i] != '.') return SF_NOT_NUMBER;
        i++;
        size_t frac_start = i;
        while (i < len && s[i] >= '0' && s[i] <= '9') i++;
        if (i == frac_start || i != len) return SF_NOT_NUMBER;
        frac = 1;
    }

    *has_int = 0;
    if (!frac) {
        /* Integer fast path. Leading zeros change nothing; 19 digits always
         * fit an unsigned long long. Converting a long long to double rounds
         * to nearest, the same result as parsing its decimal text. */
        size_t k = int_start;
        while (k + 1 < int_end && s[k] == '0') k++;
        if (int_end - k <= 19) {
            unsigned long long acc = 0;
            for (size_t j = k; j < int_end; j++) acc = acc * 10 + (s[j] - '0');
            if (!neg && acc <= (unsigned long long)LLONG_MAX) {
                *ival = (long long)acc;
                *has_int = 1;
                *dval = (double)*ival;
                return SF_NUMBER;
            }
            if (neg && acc <= (unsigned long long)LLONG_MAX + 1ULL) {
                *ival = (acc == (unsigned long long)LLONG_MAX + 1ULL) ?
                        LLONG_MIN : -(long long)acc;
                *has_int = 1;
                *dval = acc ? (double)*ival : -0.0;
                return SF_NUMBER;
            }
        }
    }

    double d = fast_float_strtod((const char *)s, len, NULL);
    if (isinf(d)) return SF_OVERFLOW;
    if (d == 0.0) {
        for (size_t j = int_start; j < len; j++)
            if (s[j] >= '1' && s[j] <= '9') return SF_UNDERFLOW;
    }
    *dval = d;
    return SF_NUMBER;
}

/* f64 bits with -0.0 folded into 0.0, so IN (0) matches "-0". */
static uint64_t sfNormBits(double d) {
    if (d == 0.0) d = 0.0;
    uint64_t bits;
    memcpy(&bits, &d, sizeof(bits));
    return bits;
}

/* ---------------------------------------------------------------------------
 * Lexer
 * ------------------------------------------------------------------------- */

enum { T_LPAREN, T_RPAREN, T_COMMA, T_OP, T_WORD, T_QUOTED };

typedef struct sfTok {
    int type;
    int op;                 /* T_OP */
    size_t pos;             /* Byte offset in the source. */
    sds text;               /* T_WORD as written, T_QUOTED unescaped. */
} sfTok;

typedef struct sfParser {
    streamFilter *f;
    const unsigned char *src;
    size_t srclen;
    sfTok *toks;
    size_t ntoks;
    size_t i;
    sds err;                /* First error, or NULL. */
} sfParser;

static void sfError(sfParser *p, size_t pos, const char *fmt, ...) {
    if (p->err) return;
    va_list ap;
    va_start(ap, fmt);
    p->err = sdscatprintf(sdsempty(), "syntax error at byte %zu: ", pos);
    p->err = sdscatvprintf(p->err, fmt, ap);
    va_end(ap);
}

static int sfIsSpace(unsigned char b) {
    return b == ' ' || b == '\t' || b == '\n' || b == '\r' || b == '\f' || b == '\v';
}

/* Bytes that end a bareword and are never part of one. */
static int sfIsSpecial(unsigned char b) {
    return sfIsSpace(b) || b == '(' || b == ')' || b == ',' || b == '=' ||
           b == '!' || b == '<' || b == '>' || b == '~' || b == '"';
}

static void sfPushTok(sfParser *p, size_t *cap, int type, int op, size_t pos, sds text) {
    if (p->ntoks == *cap) {
        *cap = *cap ? *cap * 2 : 16;
        p->toks = zrealloc(p->toks, sizeof(sfTok) * (*cap));
    }
    p->toks[p->ntoks].type = type;
    p->toks[p->ntoks].op = op;
    p->toks[p->ntoks].pos = pos;
    p->toks[p->ntoks].text = text;
    p->ntoks++;
}

static int sfLex(sfParser *p) {
    const unsigned char *s = p->src;
    size_t len = p->srclen, i = 0, cap = 0;
    while (i < len) {
        size_t pos = i;
        unsigned char b = s[i];
        if (sfIsSpace(b)) { i++; continue; }
        switch (b) {
        case '(': sfPushTok(p, &cap, T_LPAREN, 0, pos, NULL); i++; continue;
        case ')': sfPushTok(p, &cap, T_RPAREN, 0, pos, NULL); i++; continue;
        case ',': sfPushTok(p, &cap, T_COMMA, 0, pos, NULL); i++; continue;
        case '=': sfPushTok(p, &cap, T_OP, SF_OP_EQ, pos, NULL); i++; continue;
        case '~': sfPushTok(p, &cap, T_OP, SF_OP_CONTAINS, pos, NULL); i++; continue;
        case '!':
            if (i + 1 < len && s[i+1] == '=') {
                sfPushTok(p, &cap, T_OP, SF_OP_NE, pos, NULL);
                i += 2;
                continue;
            }
            sfError(p, pos, "expected '!=' (bare '!' is not an operator)");
            return 0;
        case '<':
        case '>': {
            int le = i + 1 < len && s[i+1] == '=';
            int op = b == '<' ? (le ? SF_OP_LE : SF_OP_LT) : (le ? SF_OP_GE : SF_OP_GT);
            sfPushTok(p, &cap, T_OP, op, pos, NULL);
            i += le ? 2 : 1;
            continue;
        }
        case '"': {
            sds buf = sdsempty();
            i++;
            for (;;) {
                if (i >= len) {
                    sdsfree(buf);
                    sfError(p, pos, "unterminated quoted literal");
                    return 0;
                }
                if (s[i] == '"') { i++; break; }
                if (s[i] == '\\') {
                    if (i + 1 >= len) {
                        sdsfree(buf);
                        sfError(p, pos, "unterminated quoted literal");
                        return 0;
                    }
                    if (s[i+1] != '"' && s[i+1] != '\\') {
                        sdsfree(buf);
                        sfError(p, i, "unsupported escape (only \\\" and \\\\ are allowed)");
                        return 0;
                    }
                    buf = sdscatlen(buf, s + i + 1, 1);
                    i += 2;
                    continue;
                }
                buf = sdscatlen(buf, s + i, 1);
                i++;
            }
            sfPushTok(p, &cap, T_QUOTED, 0, pos, buf);
            continue;
        }
        default: {
            size_t start = i;
            while (i < len && !sfIsSpecial(s[i])) i++;
            sfPushTok(p, &cap, T_WORD, 0, pos, sdsnewlen(s + start, i - start));
            continue;
        }
        }
    }
    return 1;
}

/* ---------------------------------------------------------------------------
 * Parser
 * ------------------------------------------------------------------------- */

static sfTok *sfPeek(sfParser *p) {
    return p->i < p->ntoks ? &p->toks[p->i] : NULL;
}

static size_t sfPos(sfParser *p) {
    sfTok *t = sfPeek(p);
    return t ? t->pos : p->srclen;
}

static int sfIsKw(sfTok *t, const char *kw) {
    size_t kl = strlen(kw);
    return t && t->type == T_WORD && sdslen(t->text) == kl &&
           strncasecmp(t->text, kw, kl) == 0;
}

/* A token for an error message. The caller frees the result. */
static sds sfDescribe(sfTok *t) {
    switch (t->type) {
    case T_LPAREN: return sdsnew("'('");
    case T_RPAREN: return sdsnew("')'");
    case T_COMMA: return sdsnew("','");
    case T_OP: return sdscatprintf(sdsempty(), "operator '%s'", sfOpSymbol[t->op]);
    case T_WORD: {
        sds d = sdsnew("'");
        d = sdscatsds(d, t->text);
        return sdscat(d, "'");
    }
    default: return sdsnew("quoted literal");
    }
}

/* A literal for an error message: quoted, cut to its head and tail when
 * long, with its length. */
static sds sfShownLiteral(sds w) {
    size_t len = sdslen(w);
    if (len <= 20 + 8 + 3) return sdscatfmt(sdsempty(), "'%S'", w);
    sds d = sdsnew("'");
    d = sdscatlen(d, w, 20);
    d = sdscat(d, "...");
    d = sdscatlen(d, w + len - 8, 8);
    return sdscatprintf(d, "' (%zu bytes)", len);
}

static int sfFieldIndex(sfParser *p, sds name) {
    streamFilter *f = p->f;
    for (int j = 0; j < f->nfields; j++) {
        if (sdslen(f->fields[j]) == sdslen(name) &&
            memcmp(f->fields[j], name, sdslen(name)) == 0) return j;
    }
    f->fields = zrealloc(f->fields, sizeof(sds) * (f->nfields + 1));
    f->fields[f->nfields] = sdsdup(name);
    return f->nfields++;
}

static void sfValueFree(sfValue *v) {
    sdsfree(v->text);
    v->text = NULL;
}

static void sfInSetFree(sfInSet *set) {
    if (!set) return;
    for (size_t j = 0; j < set->nstrs; j++) sdsfree(set->strs[j]);
    zfree(set->strs);
    zfree(set->ints);
    zfree(set->nums);
    zfree(set->nonints);
    zfree(set);
}

static void sfNodeFree(sfNode *n) {
    if (!n) return;
    for (int j = 0; j < n->nchildren; j++) sfNodeFree(n->children[j]);
    zfree(n->children);
    sfValueFree(&n->value);
    sfInSetFree(n->set);
    zfree(n);
}

static sfNode *sfNodeNew(int type) {
    sfNode *n = zcalloc(sizeof(*n));
    n->type = type;
    return n;
}

/* Type a bareword literal: a number when it reads as one, otherwise a
 * string. A number outside the f64 range is an error, never a string. */
static int sfClassify(sfParser *p, sfTok *t, sfValue *v) {
    int has_int = 0;
    long long ival = 0;
    double dval = 0;
    int kind = sfReadNumber((unsigned char *)t->text, sdslen(t->text), &has_int, &ival, &dval);
    if (kind == SF_NUMBER) {
        v->is_num = 1;
        v->has_int = has_int;
        v->ival = ival;
        v->dval = dval;
        v->text = sdsdup(t->text);
        return 1;
    }
    if (kind == SF_NOT_NUMBER) {
        v->is_num = 0;
        v->text = sdsdup(t->text);
        return 1;
    }
    sds shown = sfShownLiteral(t->text);
    sfError(p, t->pos, "numeric literal %s %s; quote it to compare bytewise", shown,
            kind == SF_OVERFLOW ? "is too large for f64 (it rounds to infinity)" :
                                  "is too small for f64 (it rounds to zero)");
    sdsfree(shown);
    return 0;
}

static int sfParseValue(sfParser *p, sfValue *v) {
    sfTok *t = sfPeek(p);
    if (!t) {
        sfError(p, p->srclen, "expected a value");
        return 0;
    }
    if (t->type == T_WORD) {
        p->i++;
        return sfClassify(p, t, v);
    }
    if (t->type == T_QUOTED) {
        p->i++;
        v->is_num = 0;
        v->text = sdsdup(t->text);
        return 1;
    }
    sds d = sfDescribe(t);
    sfError(p, t->pos, "expected a value, found %s", d);
    sdsfree(d);
    return 0;
}

static int sfBytesCmp(const unsigned char *a, size_t al, const unsigned char *b, size_t bl) {
    size_t m = al < bl ? al : bl;
    int c = m ? memcmp(a, b, m) : 0;
    if (c) return c;
    return al < bl ? -1 : (al > bl ? 1 : 0);
}

static int sfSdsCmp(const void *a, const void *b) {
    sds x = *(sds *)a, y = *(sds *)b;
    return sfBytesCmp((unsigned char *)x, sdslen(x), (unsigned char *)y, sdslen(y));
}

static int sfLLCmp(const void *a, const void *b) {
    long long x = *(long long *)a, y = *(long long *)b;
    return x < y ? -1 : (x > y);
}

static int sfU64Cmp(const void *a, const void *b) {
    uint64_t x = *(uint64_t *)a, y = *(uint64_t *)b;
    return x < y ? -1 : (x > y);
}

/* IN "(" value ( "," value )* ")" */
static sfInSet *sfParseInList(sfParser *p) {
    sfTok *t = sfPeek(p);
    if (!t) {
        sfError(p, p->srclen, "expected '(' after IN");
        return NULL;
    }
    if (t->type != T_LPAREN) {
        sds d = sfDescribe(t);
        sfError(p, t->pos, "expected '(' after IN, found %s", d);
        sdsfree(d);
        return NULL;
    }
    p->i++;

    sfValue *members = NULL;
    size_t n = 0, cap = 0;
    int ok = 0;
    for (;;) {
        if (n == cap) {
            cap = cap ? cap * 2 : 8;
            members = zrealloc(members, sizeof(sfValue) * cap);
        }
        memset(&members[n], 0, sizeof(sfValue));
        if (!sfParseValue(p, &members[n])) break;
        n++;
        if (n > STREAM_FILTER_MAX_IN_MEMBERS) {
            sfError(p, sfPos(p), "IN list longer than %d members", STREAM_FILTER_MAX_IN_MEMBERS);
            break;
        }
        t = sfPeek(p);
        if (!t) {
            sfError(p, p->srclen, "unterminated IN list");
            break;
        }
        if (t->type == T_COMMA) { p->i++; continue; }
        if (t->type == T_RPAREN) { p->i++; ok = 1; break; }
        sds d = sfDescribe(t);
        sfError(p, t->pos, "expected ',' or ')' in IN list, found %s", d);
        sdsfree(d);
        break;
    }

    sfInSet *set = NULL;
    if (ok) {
        set = zcalloc(sizeof(*set));
        for (size_t j = 0; j < n; j++) {
            sfValue *v = &members[j];
            if (!v->is_num) {
                set->strs = zrealloc(set->strs, sizeof(sds) * (set->nstrs + 1));
                set->strs[set->nstrs++] = v->text;
                v->text = NULL;
                continue;
            }
            uint64_t bits = sfNormBits(v->dval);
            set->nums = zrealloc(set->nums, sizeof(uint64_t) * (set->nnums + 1));
            set->nums[set->nnums++] = bits;
            if (v->has_int) {
                set->ints = zrealloc(set->ints, sizeof(long long) * (set->nints + 1));
                set->ints[set->nints++] = v->ival;
            } else {
                set->nonints = zrealloc(set->nonints, sizeof(uint64_t) * (set->nnonints + 1));
                set->nonints[set->nnonints++] = bits;
            }
        }
        if (set->nstrs) qsort(set->strs, set->nstrs, sizeof(sds), sfSdsCmp);
        if (set->nints) qsort(set->ints, set->nints, sizeof(long long), sfLLCmp);
        if (set->nnums) qsort(set->nums, set->nnums, sizeof(uint64_t), sfU64Cmp);
        if (set->nnonints) qsort(set->nonints, set->nnonints, sizeof(uint64_t), sfU64Cmp);
    }
    /* A value that failed to parse allocated nothing. */
    for (size_t j = 0; j < n; j++) sfValueFree(&members[j]);
    zfree(members);
    return set;
}

static sfNode *sfParseExpr(sfParser *p, int depth);

static sfNode *sfParseAfterField(sfParser *p, sds field) {
    sfTok *t = sfPeek(p);
    if (!t) {
        sfError(p, p->srclen, "expected an operator, IN, NOT or EXISTS after field '%s'", field);
        return NULL;
    }
    if (t->type == T_OP) {
        p->i++;
        sfNode *n = sfNodeNew(SF_CMP);
        n->op = t->op;
        if (!sfParseValue(p, &n->value)) {
            sfNodeFree(n);
            return NULL;
        }
        n->field = sfFieldIndex(p, field);
        return n;
    }
    if (sfIsKw(t, "IN") || sfIsKw(t, "EXISTS")) {
        int in = sfIsKw(t, "IN");
        p->i++;
        sfNode *n = sfNodeNew(in ? SF_IN : SF_EXISTS);
        if (in && (n->set = sfParseInList(p)) == NULL) {
            sfNodeFree(n);
            return NULL;
        }
        n->field = sfFieldIndex(p, field);
        return n;
    }
    if (sfIsKw(t, "NOT")) {
        p->i++;
        t = sfPeek(p);
        if (sfIsKw(t, "IN") || sfIsKw(t, "EXISTS")) {
            int in = sfIsKw(t, "IN");
            p->i++;
            sfNode *n = sfNodeNew(in ? SF_IN : SF_EXISTS);
            n->negated = 1;
            if (in && (n->set = sfParseInList(p)) == NULL) {
                sfNodeFree(n);
                return NULL;
            }
            n->field = sfFieldIndex(p, field);
            return n;
        }
        if (t) {
            sds d = sfDescribe(t);
            sfError(p, t->pos, "expected IN or EXISTS after NOT, found %s", d);
            sdsfree(d);
        } else {
            sfError(p, p->srclen, "expected IN or EXISTS after NOT");
        }
        return NULL;
    }
    sds d = sfDescribe(t);
    sfError(p, t->pos, "expected an operator, IN, NOT or EXISTS after field '%s', found %s", field, d);
    sdsfree(d);
    return NULL;
}

static sfNode *sfParseClause(sfParser *p, int depth) {
    sfTok *t = sfPeek(p);
    if (!t) {
        sfError(p, p->srclen, "expected a field name or '('");
        return NULL;
    }
    if (t->type == T_LPAREN) {
        if (depth + 1 > STREAM_FILTER_MAX_DEPTH) {
            sfError(p, t->pos, "parentheses nested deeper than %d levels", STREAM_FILTER_MAX_DEPTH);
            return NULL;
        }
        p->i++;
        sfNode *inner = sfParseExpr(p, depth + 1);
        if (!inner) return NULL;
        t = sfPeek(p);
        if (t && t->type == T_RPAREN) {
            p->i++;
            return inner;
        }
        if (t) {
            sds d = sfDescribe(t);
            sfError(p, t->pos, "expected ')', found %s", d);
            sdsfree(d);
        } else {
            sfError(p, p->srclen, "missing ')'");
        }
        sfNodeFree(inner);
        return NULL;
    }
    if (t->type == T_WORD || t->type == T_QUOTED) {
        p->i++;
        return sfParseAfterField(p, t->text);
    }
    sds d = sfDescribe(t);
    sfError(p, t->pos, "expected a field name or '(', found %s", d);
    sdsfree(d);
    return NULL;
}

/* Build an OR or AND node over 'items', flattening nested nodes of the same
 * type and collapsing a single child. Takes ownership of the items. */
static sfNode *sfMakeList(int type, sfNode **items, int n) {
    if (n == 1) {
        sfNode *only = items[0];
        zfree(items);
        return only;
    }
    sfNode *list = sfNodeNew(type);
    for (int j = 0; j < n; j++) {
        sfNode *it = items[j];
        int add = it->type == type ? it->nchildren : 1;
        list->children = zrealloc(list->children, sizeof(sfNode *) * (list->nchildren + add));
        if (it->type == type) {
            memcpy(list->children + list->nchildren, it->children, sizeof(sfNode *) * it->nchildren);
            list->nchildren += it->nchildren;
            zfree(it->children);
            it->children = NULL;
            it->nchildren = 0;
            sfNodeFree(it);
        } else {
            list->children[list->nchildren++] = it;
        }
    }
    zfree(items);
    return list;
}

static sfNode *sfParseList(sfParser *p, int depth, int type) {
    const char *kw = type == SF_OR ? "OR" : "AND";
    sfNode **items = NULL;
    int n = 0;
    for (;;) {
        sfNode *it = type == SF_OR ? sfParseList(p, depth, SF_AND) : sfParseClause(p, depth);
        if (!it) {
            for (int j = 0; j < n; j++) sfNodeFree(items[j]);
            zfree(items);
            return NULL;
        }
        items = zrealloc(items, sizeof(sfNode *) * (n + 1));
        items[n++] = it;
        if (!sfIsKw(sfPeek(p), kw)) break;
        p->i++;
    }
    return sfMakeList(type, items, n);
}

static sfNode *sfParseExpr(sfParser *p, int depth) {
    return sfParseList(p, depth, SF_OR);
}

/* ---------------------------------------------------------------------------
 * Construction, accounting, copying
 * ------------------------------------------------------------------------- */

static size_t sfNodeAllocSize(sfNode *n) {
    size_t size = zmalloc_usable_size(n);
    if (n->children) size += zmalloc_usable_size(n->children);
    for (int j = 0; j < n->nchildren; j++) size += sfNodeAllocSize(n->children[j]);
    if (n->value.text) size += sdsAllocSize(n->value.text);
    if (n->set) {
        sfInSet *set = n->set;
        size += zmalloc_usable_size(set);
        if (set->strs) size += zmalloc_usable_size(set->strs);
        for (size_t j = 0; j < set->nstrs; j++) size += sdsAllocSize(set->strs[j]);
        if (set->ints) size += zmalloc_usable_size(set->ints);
        if (set->nums) size += zmalloc_usable_size(set->nums);
        if (set->nonints) size += zmalloc_usable_size(set->nonints);
    }
    return size;
}

static void sfComputeAllocSize(streamFilter *f) {
    size_t size = zmalloc_usable_size(f) + sdsAllocSize(f->source);
    if (f->fields) size += zmalloc_usable_size(f->fields);
    for (int j = 0; j < f->nfields; j++) size += sdsAllocSize(f->fields[j]);
    if (f->root) size += sfNodeAllocSize(f->root);
    if (f->pos_field) size += zmalloc_usable_size(f->pos_field);
    if (f->pos_index) size += zmalloc_usable_size(f->pos_index);
    f->alloc_size = size;
}

streamFilter *streamFilterParse(const char *src, size_t len, sds *err) {
    *err = NULL;
    if (len > STREAM_FILTER_MAX_EXPR_LEN) {
        *err = sdscatprintf(sdsempty(), "syntax error at byte 0: expression is %zu bytes, the limit is %d",
                            len, STREAM_FILTER_MAX_EXPR_LEN);
        return NULL;
    }
    streamFilter *f = zcalloc(sizeof(*f));
    sfParser p = {.f = f, .src = (const unsigned char *)src, .srclen = len};

    if (sfLex(&p)) {
        if (p.ntoks == 0) {
            sfError(&p, 0, "empty expression");
        } else {
            f->root = sfParseExpr(&p, 0);
            sfTok *t = sfPeek(&p);
            if (f->root && t) {
                sds d = sfDescribe(t);
                sfError(&p, t->pos, "unexpected %s after a complete expression", d);
                sdsfree(d);
            }
        }
    }
    for (size_t j = 0; j < p.ntoks; j++) sdsfree(p.toks[j].text);
    zfree(p.toks);

    if (p.err) {
        *err = p.err;
        streamFilterFree(f);
        return NULL;
    }
    f->source = sdsnewlen(src, len);
    if (f->nfields) {
        f->pos_field = zmalloc(sizeof(*f->pos_field) * f->nfields);
        f->pos_index = zmalloc(sizeof(*f->pos_index) * f->nfields);
    }
    sfComputeAllocSize(f);
    return f;
}

streamFilter *streamFilterCreateElse(void) {
    streamFilter *f = zcalloc(sizeof(*f));
    f->is_else = 1;
    f->source = sdsnew("ELSE");
    sfComputeAllocSize(f);
    return f;
}

streamFilter *streamFilterDup(const streamFilter *f) {
    if (f->is_else) return streamFilterCreateElse();
    sds err;
    streamFilter *copy = streamFilterParse(f->source, sdslen(f->source), &err);
    serverAssert(copy != NULL); /* It parsed once, so it parses again. */
    return copy;
}

void streamFilterFree(streamFilter *f) {
    if (!f) return;
    sfNodeFree(f->root);
    for (int j = 0; j < f->nfields; j++) sdsfree(f->fields[j]);
    zfree(f->fields);
    zfree(f->pos_field);
    zfree(f->pos_index);
    sdsfree(f->source);
    zfree(f);
}

/* ---------------------------------------------------------------------------
 * Evaluation
 * ------------------------------------------------------------------------- */

static int sfOrdered(int op, int cmp) {
    switch (op) {
    case SF_OP_EQ: return cmp == 0;
    case SF_OP_NE: return cmp != 0;
    case SF_OP_LT: return cmp < 0;
    case SF_OP_LE: return cmp <= 0;
    case SF_OP_GT: return cmp > 0;
    case SF_OP_GE: return cmp >= 0;
    default: return 0;
    }
}

static int sfContains(const unsigned char *h, size_t hl, const unsigned char *n, size_t nl) {
    if (nl == 0) return 1;
    if (nl > hl) return 0;
    for (size_t i = 0; i + nl <= hl; i++) {
        if (h[i] == n[0] && memcmp(h + i, n, nl) == 0) return 1;
    }
    return 0;
}

static int sfCompare(int op, const unsigned char *a, size_t al, const sfValue *v) {
    if (op == SF_OP_CONTAINS)
        return sfContains(a, al, (unsigned char *)v->text, sdslen(v->text));
    if (!v->is_num)
        return sfOrdered(op, sfBytesCmp(a, al, (unsigned char *)v->text, sdslen(v->text)));

    int has_int;
    long long ai;
    double ad;
    if (sfReadNumber(a, al, &has_int, &ai, &ad) != SF_NUMBER) return 0;
    if (has_int && v->has_int)
        return sfOrdered(op, ai < v->ival ? -1 : (ai > v->ival));
    return sfOrdered(op, ad < v->dval ? -1 : (ad > v->dval));
}

static int sfHasStr(const sfInSet *set, const unsigned char *a, size_t al) {
    size_t lo = 0, hi = set->nstrs;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        sds m = set->strs[mid];
        int c = sfBytesCmp(a, al, (unsigned char *)m, sdslen(m));
        if (c == 0) return 1;
        if (c < 0) hi = mid; else lo = mid + 1;
    }
    return 0;
}

static int sfHasLL(const long long *v, size_t n, long long x) {
    return n && bsearch(&x, v, n, sizeof(long long), sfLLCmp) != NULL;
}

static int sfHasU64(const uint64_t *v, size_t n, uint64_t x) {
    return n && bsearch(&x, v, n, sizeof(uint64_t), sfU64Cmp) != NULL;
}

static int sfInSetContains(const sfInSet *set, const unsigned char *a, size_t al) {
    if (set->nstrs && sfHasStr(set, a, al)) return 1;
    if (set->nnums == 0) return 0;
    int has_int;
    long long ai;
    double ad;
    if (sfReadNumber(a, al, &has_int, &ai, &ad) != SF_NUMBER) return 0;
    if (has_int)
        return sfHasLL(set->ints, set->nints, ai) ||
               sfHasU64(set->nonints, set->nnonints, sfNormBits(ad));
    return sfHasU64(set->nums, set->nnums, sfNormBits(ad));
}

static int sfEvalNode(const sfNode *n, const unsigned char **vals, const int64_t *lens) {
    switch (n->type) {
    case SF_OR:
        for (int j = 0; j < n->nchildren; j++)
            if (sfEvalNode(n->children[j], vals, lens)) return 1;
        return 0;
    case SF_AND:
        for (int j = 0; j < n->nchildren; j++)
            if (!sfEvalNode(n->children[j], vals, lens)) return 0;
        return 1;
    case SF_EXISTS:
        return (vals[n->field] != NULL) != n->negated;
    case SF_CMP:
        if (!vals[n->field]) return 0;
        return sfCompare(n->op, vals[n->field], lens[n->field], &n->value);
    case SF_IN:
        if (!vals[n->field]) return 0;
        return sfInSetContains(n->set, vals[n->field], lens[n->field]) != n->negated;
    }
    return 0;
}

int streamFilterEval(const streamFilter *f, const unsigned char **vals, const int64_t *lens) {
    serverAssert(!f->is_else && f->root);
    return sfEvalNode(f->root, vals, lens);
}
