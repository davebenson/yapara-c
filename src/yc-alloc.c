/* SPDX-License-Identifier: 0BSD */
#include "yc-alloc.h"
#include "yc-common.h"
#include <stdlib.h>
#include <string.h>

void *
yc_malloc (size_t size)
{
  void *rv;
  if (size == 0)
    return NULL;
  rv = malloc (size);
  if (rv == NULL)
    yc_die ("out-of-memory allocating %u bytes", (unsigned) size);
  return rv;
}
void *
yc_malloc0 (size_t size)
{
  void *rv;
  if (size == 0)
    return NULL;
  rv = malloc (size);
  if (rv == NULL)
    yc_die ("out-of-memory allocating %u bytes", (unsigned) size);
  memset (rv, 0, size);
  return rv;
}
void
yc_free (void *ptr)
{
  if (ptr)
    free (ptr);
}
void *
yc_realloc (void *ptr, size_t size)
{
  if (ptr == NULL)
    return yc_malloc (size);
  else if (size == 0)
    {
      yc_free (ptr);
      return NULL;
    }
  else
    {
      void *rv = realloc (ptr, size);
      if (rv == NULL)
        yc_die ("out-of-memory re-allocating %u bytes", (unsigned) size);
      return rv;
    }
}

char *
yc_strdup (const char *str)
{
  if (str == NULL)
    return NULL;
  else
    {
      unsigned length = strlen (str);
      char *rv = yc_malloc (length + 1);
      memcpy (rv, str, length + 1);
      return rv;
    }
}
char *
yc_strndup (size_t length, const char *str)
{
  char *rv = yc_malloc (length + 1);
  memcpy (rv, str, length);
  rv[length] = 0;
  return rv;
}

