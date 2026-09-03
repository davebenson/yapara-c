/* SPDX-License-Identifier: 0BSD */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "yc-common.h"

void
yc_die (const char *format, ...)
{
  va_list args;
  va_start (args, format);
  vfprintf (stderr, format, args);
  va_end (args);
  fputc ('\n', stderr);
  exit (1);
}

void
yc_warn (const char *format, ...)
{
  va_list args;
  va_start (args, format);
  vfprintf (stderr, format, args);
  va_end (args);
  fputc ('\n', stderr);
}
//
// false strings: 0 n no N NO f false F FALSE
// true strings:  1 y yes Y YES t true T TRUE
//
bool
yc_parse_boolean (const char *str,
                   bool *out)
{
  switch (str[0])
    {
    case '0': if (strcmp (str, "0") == 0)     goto is_false; break;
    case '1': if (strcmp (str, "1") == 0)     goto is_true;  break;
    case 'n': if (str[1] == '\0'
               || strcmp (str, "no") == 0)    goto is_false; break;
    case 'y': if (str[1] == '\0'
               || strcmp (str, "yes") == 0)   goto is_true;  break;
    case 'N': if (str[1] == '\0'
               || strcmp (str, "NO") == 0)    goto is_false; break;
    case 'Y': if (str[1] == '\0'
               || strcmp (str, "YES") == 0)   goto is_true;  break;
    case 'f': if (str[1] == '\0'
               || strcmp (str, "false") == 0) goto is_false; break;
    case 't': if (str[1] == '\0'
               || strcmp (str, "true") == 0)  goto is_true;  break;
    case 'F': if (str[1] == '\0'
               || strcmp (str, "FALSE") == 0) goto is_false; break;
    case 'T': if (str[1] == '\0'
               || strcmp (str, "TRUE") == 0)  goto is_true;  break;
    }
    return false;
is_true:
  *out = true;
  return true;
is_false:
  *out = false;
  return true;
}
