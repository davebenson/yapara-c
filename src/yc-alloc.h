/* SPDX-License-Identifier: 0BSD */
#include <stddef.h>

/* typed-memory allocation macros */
#define YC_NEW(type)             ((type*) yc_malloc (sizeof(type)))
#define YC_NEW0(type)            ((type*) yc_malloc0 (sizeof(type)))
#define YC_NEW_ARRAY(n, type)    ((type*) yc_malloc (sizeof(type) * (n)))
#define YC_NEW0_ARRAY(n, type)   ((type*) yc_malloc0 (sizeof(type) * (n)))
#define YC_RENEW(type, array, n) ((type*) yc_realloc ((array), sizeof(type) * (n)))

void *yc_malloc (size_t);
void *yc_malloc0 (size_t);
void  yc_free (void *);
void *yc_realloc (void *, size_t);
char *yc_strdup (const char *str);
char *yc_strndup (size_t len, const char *str);
