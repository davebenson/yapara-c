/* SPDX-License-Identifier: 0BSD */
#include <assert.h>
#include <stdbool.h>

void yc_die(const char *format, ...);
void yc_warn(const char *format, ...);
bool yc_parse_boolean (const char *str, bool *out);

#define yc_assert assert

#if defined(__GNUC__)
# define YC_LIKELY(condition)    __builtin_expect (!!(condition), 1)
# define YC_UNLIKELY(condition)  __builtin_expect (!!(condition), 0)
#else
# define YC_LIKELY(condition)    (condition)
# define YC_UNLIKELY(condition)  (condition)
#endif

/* Complain and bail out of a void function whose caller broke a
   documented precondition. */
#define yc_return_if_fail(condition, message)               \
  do {                                                      \
    if (YC_UNLIKELY (!(condition)))                         \
      {                                                     \
        yc_warn ("%s:%d: %s: %s",                           \
                 __FILE__, __LINE__, __func__, (message));  \
        return;                                             \
      }                                                     \
  } while (0)
