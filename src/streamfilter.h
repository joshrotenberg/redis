/*
 * Copyright (c) 2026-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

#ifndef __STREAM_FILTER_H
#define __STREAM_FILTER_H

#include <stddef.h>
#include <stdint.h>
#include "sds.h"

/* Consumer group filters: a filter expression stored on a consumer group.
 * XREADGROUP with ">" delivers to the group only the entries the expression
 * matches; the group's cursor still moves past every entry it examines.
 *
 *   expr     := and-expr ( OR and-expr )*
 *   and-expr := clause ( AND clause )*
 *   clause   := field op value
 *             | field [NOT] IN "(" value ( "," value )* ")"
 *             | field [NOT] EXISTS
 *             | "(" expr ")"
 *   op       := "=" | "!=" | "<" | "<=" | ">" | ">=" | "~"
 *
 * The literal decides the comparison: a bareword that reads as a number
 * compares numerically, anything else (and every quoted literal) bytewise.
 * A missing field makes every clause false except NOT EXISTS. When an entry
 * repeats a field, its first occurrence is the value. */

#define STREAM_FILTER_MAX_EXPR_LEN 4096
#define STREAM_FILTER_MAX_IN_MEMBERS 10000
#define STREAM_FILTER_MAX_DEPTH 16

typedef struct sfNode sfNode;

typedef struct streamFilter {
    sds source;         /* The expression as given, returned by XINFO GROUPS. */
    int is_else;        /* ELSE: what no other filtered group on the stream
                           matches. Has no expression. */
    sfNode *root;       /* Parsed expression, NULL for ELSE. */
    int nfields;        /* Distinct field names the expression reads. */
    sds *fields;
    size_t alloc_size;  /* Usable bytes of every allocation the filter owns,
                           charged to the stream's alloc_size. */
    /* Where the fields sit among the master fields of one listpack node, for
     * the node's entries flagged SAMEFIELDS. Filled by t_stream.c, which
     * keys it on the node and on the read in progress; never persisted. */
    const unsigned char *pos_lp;
    unsigned long long pos_read;
    int pos_count;      /* Fields the node's master entry has. */
    int *pos_field;     /* Index into 'fields' of each, in master field order. */
    int64_t *pos_index; /* Their positions among the master fields, ascending. */
} streamFilter;

/* Parse 'len' bytes at 'src'. On error returns NULL and sets '*err' to a new
 * sds "syntax error at byte N: ..." that the caller frees. */
streamFilter *streamFilterParse(const char *src, size_t len, sds *err);
streamFilter *streamFilterCreateElse(void);
streamFilter *streamFilterDup(const streamFilter *f);
void streamFilterFree(streamFilter *f);

/* Evaluate against one entry. 'vals[i]' and 'lens[i]' hold the value of
 * f->fields[i] in the entry, or vals[i] == NULL when the entry lacks it.
 * Returns 1 on a match. Not valid for an ELSE filter. */
int streamFilterEval(const streamFilter *f, const unsigned char **vals, const int64_t *lens);

#endif
