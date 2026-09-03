/* SPDX-License-Identifier: 0BSD */
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "yc-common.h"
#include "yc-alloc.h"
#include "yc-shell.h"

extern char **environ;

/* --- errors --- */

typedef struct {
  const char *start;
  const char *p;
} ParseState;

static void
set_shell_error (YcShellError *error,
                 YcShellErrorCode code,
                 const ParseState *st,
                 const char *format,
                 ...)
{
  va_list args;
  if (error == NULL)
    return;
  error->code = code;
  error->position = st == NULL ? 0 : (size_t) (st->p - st->start);
  va_start (args, format);
  vsnprintf (error->message, sizeof (error->message), format, args);
  va_end (args);
}

/* --- a growable string --- */

typedef struct {
  char *data;
  size_t len, alloced;
} Str;

static void
str_append (Str *s, const char *text, size_t n)
{
  if (s->len + n + 1 > s->alloced)
    {
      s->alloced = s->alloced == 0 ? 32 : s->alloced;
      while (s->len + n + 1 > s->alloced)
        s->alloced *= 2;
      s->data = YC_RENEW (char, s->data, s->alloced);
    }
  memcpy (s->data + s->len, text, n);
  s->len += n;
  s->data[s->len] = 0;
}

static void
str_append_c (Str *s, char c)
{
  str_append (s, &c, 1);
}

/* Hands off the buffer and empties 's'.  A word can legitimately be
   empty -- "" or "$UNSET" in quotes -- so never return NULL. */
static char *
str_steal (Str *s)
{
  char *rv = s->data != NULL ? s->data : yc_strdup ("");
  memset (s, 0, sizeof (*s));
  return rv;
}

static void
str_clear (Str *s)
{
  yc_free (s->data);
  memset (s, 0, sizeof (*s));
}

/* --- a growable NULL-terminated string vector --- */

typedef struct {
  char **argv;
  size_t n, alloced;
} Argv;

static void
argv_push (Argv *a, char *str)
{
  if (a->n + 2 > a->alloced)
    {
      a->alloced = a->alloced == 0 ? 8 : a->alloced * 2;
      a->argv = YC_RENEW (char *, a->argv, a->alloced);
    }
  a->argv[a->n++] = str;
  a->argv[a->n] = NULL;
}

static void
free_string_array (char **strs)
{
  size_t i;
  if (strs == NULL)
    return;
  for (i = 0; strs[i] != NULL; i++)
    yc_free (strs[i]);
  yc_free (strs);
}

static void
argv_clear (Argv *a)
{
  free_string_array (a->argv);
  memset (a, 0, sizeof (*a));
}

/* --- redirections, keyed by the child's fd --- */

typedef struct {
  int fd;
  YcChildCreateFdInfo info;
  unsigned order;              /* 1-based position of this redirection */
} FdSlot;

typedef struct {
  FdSlot *slots;
  size_t n, alloced;
} FdMap;

static FdSlot *
fdmap_get (FdMap *m, int fd)
{
  size_t i;
  for (i = 0; i < m->n; i++)
    if (m->slots[i].fd == fd)
      return &m->slots[i];
  return NULL;
}

/* Reset rather than add if we have seen this fd before: a line may
   redirect the same fd twice, and the last one wins. */
static FdSlot *
fdmap_set (FdMap *m, int fd)
{
  FdSlot *slot = fdmap_get (m, fd);
  if (slot == NULL)
    {
      if (m->n == m->alloced)
        {
          m->alloced = m->alloced == 0 ? 4 : m->alloced * 2;
          m->slots = YC_RENEW (FdSlot, m->slots, m->alloced);
        }
      slot = &m->slots[m->n++];
      memset (slot, 0, sizeof (*slot));
    }
  else
    {
      yc_free ((char *) slot->info.filename);
      memset (&slot->info, 0, sizeof (slot->info));
    }
  slot->fd = fd;
  return slot;
}

/* 'own_filenames' is false once they have been handed to the
   YcChildCreateInfo. */
static void
fdmap_clear (FdMap *m, bool own_filenames)
{
  size_t i;
  if (own_filenames)
    for (i = 0; i < m->n; i++)
      yc_free ((char *) m->slots[i].info.filename);
  yc_free (m->slots);
  memset (m, 0, sizeof (*m));
}

/* --- lexical helpers --- */

static bool
is_ifs (char c)
{
  return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/* Length of 'NAME=' if 'p' starts a variable assignment, else 0. */
static size_t
assignment_name_length (const char *p)
{
  size_t i;
  if (!(isalpha ((unsigned char) p[0]) || p[0] == '_'))
    return 0;
  for (i = 1; isalnum ((unsigned char) p[i]) || p[i] == '_'; i++)
    ;
  return p[i] == '=' ? i + 1 : 0;
}

/* POSIX IO_NUMBER: digits are an fd only when they are the whole token
   and butt right up against the operator.  So '2>x' redirects fd 2,
   while 'foo2>x' is the word 'foo2' plus a redirect of fd 1. */
static bool
is_redirect_start (const char *p)
{
  while (*p >= '0' && *p <= '9')
    p++;
  return *p == '<' || *p == '>';
}

/* --- words --- */

static bool expand_dollar (ParseState *st, Str *word, bool *has_content,
                           bool split, Argv *out, YcShellError *error);

/* Parses one word, appending the field(s) it produces to 'out'.
   'split' is for command words, where an unquoted expansion undergoes
   field splitting and so can yield several fields (or none); for
   assignment values and redirection targets it is false, and 'out'
   gets exactly one field unless the word turned out to be absent. */
static bool
parse_word (ParseState *st, bool split, Argv *out, YcShellError *error)
{
  Str word;
  bool has_content = false;
  bool done = false;

  memset (&word, 0, sizeof (word));

  while (!done)
    {
      char c = *st->p;
      switch (c)
        {
        case 0:
        case ' ': case '\t': case '\n': case '\r':
        case '<': case '>':          /* left for the caller's loop */
          done = true;
          break;

        case '\'':
          st->p++;
          has_content = true;
          while (*st->p != '\'')
            {
              if (*st->p == 0)
                {
                  set_shell_error (error, YC_SHELL_ERROR_SYNTAX, st,
                                   "unterminated single quote");
                  goto fail;
                }
              str_append_c (&word, *st->p++);
            }
          st->p++;
          break;

        case '"':
          st->p++;
          has_content = true;
          for (;;)
            {
              if (*st->p == 0)
                {
                  set_shell_error (error, YC_SHELL_ERROR_SYNTAX, st,
                                   "unterminated double quote");
                  goto fail;
                }
              if (*st->p == '"')
                {
                  st->p++;
                  break;
                }
              if (*st->p == '\\')
                {
                  /* Inside double quotes a backslash is literal unless
                     it precedes one of the four characters that keep
                     their meaning there. */
                  char next = st->p[1];
                  if (next == '$' || next == '"' || next == '\\'
                   || next == '`')
                    {
                      str_append_c (&word, next);
                      st->p += 2;
                    }
                  else if (next == '\n')
                    st->p += 2;
                  else
                    str_append_c (&word, *st->p++);
                  continue;
                }
              if (*st->p == '`')
                {
                  set_shell_error (error, YC_SHELL_ERROR_UNSUPPORTED, st,
                                   "`...` command substitution");
                  goto fail;
                }
              if (*st->p == '$')
                {
                  if (!expand_dollar (st, &word, &has_content, false, NULL,
                                      error))
                    goto fail;
                  continue;
                }
              str_append_c (&word, *st->p++);
            }
          break;

        case '\\':
          if (st->p[1] == 0)
            {
              set_shell_error (error, YC_SHELL_ERROR_SYNTAX, st,
                               "trailing backslash");
              goto fail;
            }
          if (st->p[1] == '\n')
            {
              st->p += 2;            /* line continuation */
              break;
            }
          str_append_c (&word, st->p[1]);
          has_content = true;
          st->p += 2;
          break;

        case '$':
          if (!expand_dollar (st, &word, &has_content, split, out, error))
            goto fail;
          break;

        case '~':
          /* Only expanded at the start of a word, so elsewhere it is an
             ordinary character. */
          if (!has_content && word.len == 0)
            {
              set_shell_error (error, YC_SHELL_ERROR_UNSUPPORTED, st,
                               "~ (tilde expansion)");
              goto fail;
            }
          str_append_c (&word, c);
          has_content = true;
          st->p++;
          break;

        case '|': case '&': case ';': case '(': case ')':
        case '*': case '?': case '[': case ']':
        case '{': case '}': case '`': case '!':
          set_shell_error (error, YC_SHELL_ERROR_UNSUPPORTED, st,
                           "'%c'", c);
          goto fail;

        default:
          str_append_c (&word, c);
          has_content = true;
          st->p++;
          break;
        }
    }

  /* has_content without any bytes is a real, empty field: '""'. */
  if (has_content || word.len > 0)
    argv_push (out, str_steal (&word));
  else
    str_clear (&word);
  return true;

fail:
  str_clear (&word);
  return false;
}

/* At a '$'.  Appends the variable's value, splitting it into fields if
   we are unquoted in a command word. */
static bool
expand_dollar (ParseState *st,
               Str *word,
               bool *has_content,
               bool split,
               Argv *out,
               YcShellError *error)
{
  const char *dollar_at = st->p;
  char name[128];
  size_t namelen = 0;
  bool braced = false;
  const char *value;

  st->p++;

  if (*st->p == '{')
    {
      braced = true;
      st->p++;
    }
  else if (*st->p == '(')
    {
      st->p = dollar_at;
      set_shell_error (error, YC_SHELL_ERROR_UNSUPPORTED, st,
                       "$(...) command substitution");
      return false;
    }

  if (!(isalpha ((unsigned char) *st->p) || *st->p == '_'))
    {
      char bad = *st->p;
      if (braced)
        {
          st->p = dollar_at;
          set_shell_error (error, YC_SHELL_ERROR_UNSUPPORTED, st,
                           "only a plain ${NAME} is supported");
          return false;
        }
      if (bad == '$' || bad == '?' || bad == '!' || bad == '#'
       || bad == '@' || bad == '*' || (bad >= '0' && bad <= '9'))
        {
          st->p = dollar_at;
          set_shell_error (error, YC_SHELL_ERROR_UNSUPPORTED, st,
                           "special parameter $%c", bad);
          return false;
        }
      /* A '$' that cannot begin a name is just a '$'. */
      str_append_c (word, '$');
      *has_content = true;
      return true;
    }

  while (isalnum ((unsigned char) *st->p) || *st->p == '_')
    {
      if (namelen + 1 == sizeof (name))
        {
          st->p = dollar_at;
          set_shell_error (error, YC_SHELL_ERROR_UNSUPPORTED, st,
                           "variable name is absurdly long");
          return false;
        }
      name[namelen++] = *st->p++;
    }
  name[namelen] = 0;

  if (braced)
    {
      if (*st->p != '}')
        {
          set_shell_error (error, YC_SHELL_ERROR_UNSUPPORTED, st,
                           "${%s...}: only a plain ${NAME} is supported",
                           name);
          return false;
        }
      st->p++;
    }

  /* Note that assignments on this same line are deliberately not
     visible here: 'FOO=2 echo $FOO' prints the old FOO in sh too. */
  value = getenv (name);
  if (value == NULL)
    value = "";                      /* unset expands to nothing */

  if (!split)
    {
      str_append (word, value, strlen (value));
      return true;
    }

  for (; *value != 0; value++)
    if (is_ifs (*value))
      {
        if (*has_content || word->len > 0)
          {
            argv_push (out, str_steal (word));
            *has_content = false;
          }
      }
    else
      {
        str_append_c (word, *value);
        *has_content = true;
      }
  return true;
}

/* --- assignments --- */

static bool
parse_assignment (ParseState *st, Argv *assigns, YcShellError *error)
{
  size_t namelen = assignment_name_length (st->p);
  Argv value;
  Str acc;

  memset (&value, 0, sizeof (value));
  memset (&acc, 0, sizeof (acc));

  /* 'NAME=' is plain text by construction; only the value expands. */
  str_append (&acc, st->p, namelen);
  st->p += namelen;

  if (!parse_word (st, false, &value, error))
    {
      str_clear (&acc);
      argv_clear (&value);
      return false;
    }
  if (value.n > 0)
    str_append (&acc, value.argv[0], strlen (value.argv[0]));
  argv_clear (&value);

  argv_push (assigns, str_steal (&acc));
  return true;
}

/* --- redirections --- */

static bool
parse_redirect (ParseState *st,
                FdMap *fdmap,
                unsigned order,
                YcShellError *error)
{
  const char *op_at = st->p;
  int fd = -1;
  bool input;
  bool append = false;
  FdSlot *slot;

  if (*st->p >= '0' && *st->p <= '9')
    {
      long v = 0;
      while (*st->p >= '0' && *st->p <= '9')
        {
          v = v * 10 + (*st->p - '0');
          if (v > 1024)
            {
              st->p = op_at;
              set_shell_error (error, YC_SHELL_ERROR_UNSUPPORTED, st,
                               "implausible fd number");
              return false;
            }
          st->p++;
        }
      fd = (int) v;
    }

  if (*st->p == '<')
    {
      input = true;
      st->p++;
      if (*st->p == '<')
        {
          st->p = op_at;
          set_shell_error (error, YC_SHELL_ERROR_UNSUPPORTED, st,
                           "here-documents");
          return false;
        }
      if (*st->p == '>')
        {
          st->p = op_at;
          set_shell_error (error, YC_SHELL_ERROR_UNSUPPORTED, st,
                           "<> (read-write) redirection");
          return false;
        }
      if (fd < 0)
        fd = 0;
    }
  else
    {
      input = false;
      st->p++;                       /* '>' */
      if (*st->p == '>')
        {
          append = true;
          st->p++;
        }
      else if (*st->p == '|')
        {
          st->p = op_at;
          set_shell_error (error, YC_SHELL_ERROR_UNSUPPORTED, st,
                           ">| (noclobber override)");
          return false;
        }
      if (fd < 0)
        fd = 1;
    }

  /* 'N>&M' / 'N<&M' */
  if (*st->p == '&')
    {
      long target = 0;
      st->p++;
      if (*st->p == '-')
        {
          st->p = op_at;
          set_shell_error (error, YC_SHELL_ERROR_UNSUPPORTED, st,
                           "closing a descriptor ('>&-')");
          return false;
        }
      if (!(*st->p >= '0' && *st->p <= '9'))
        {
          /* bash's '>&file' / '&>file'; not POSIX. */
          st->p = op_at;
          set_shell_error (error, YC_SHELL_ERROR_UNSUPPORTED, st,
                           "'>&' with a filename rather than an fd");
          return false;
        }
      while (*st->p >= '0' && *st->p <= '9')
        {
          target = target * 10 + (*st->p - '0');
          if (target > 1024)
            {
              st->p = op_at;
              set_shell_error (error, YC_SHELL_ERROR_UNSUPPORTED, st,
                               "implausible fd number");
              return false;
            }
          st->p++;
        }
      if (*st->p != 0 && !is_ifs (*st->p) && *st->p != '<' && *st->p != '>')
        {
          set_shell_error (error, YC_SHELL_ERROR_SYNTAX, st,
                           "unexpected '%c' after a '>&' redirection",
                           *st->p);
          return false;
        }
      if ((int) target == fd)
        return true;                 /* '1>&1' is a no-op */

      slot = fdmap_set (fdmap, fd);
      slot->order = order;
      slot->info.mode = YC_CHILD_FD_MODE_DUP;
      slot->info.dup_fd = (int) target;
      slot->info.in = input;
      slot->info.out = !input;
      return true;
    }

  while (is_ifs (*st->p))
    st->p++;

  {
    Argv target;
    memset (&target, 0, sizeof (target));

    /* POSIX does not field-split a redirection target, so one word in
       always means one filename out. */
    if (!parse_word (st, false, &target, error))
      return false;
    if (target.n == 0 || target.argv[0][0] == 0)
      {
        argv_clear (&target);
        st->p = op_at;
        set_shell_error (error, YC_SHELL_ERROR_SYNTAX, st,
                         "redirection with no filename");
        return false;
      }

    slot = fdmap_set (fdmap, fd);
    slot->order = order;
    slot->info.mode = YC_CHILD_FD_MODE_FILE;
    slot->info.filename = target.argv[0];      /* ownership moves here */
    slot->info.append = append;
    slot->info.in = input;
    slot->info.out = !input;
    yc_free (target.argv);                     /* the vector, not the string */
    return true;
  }
}

/* '>log 2>&1' is fine: fd 1 has settled by the time fd 2 borrows it.
   '2>&1 >log' is not -- sh gives fd 2 the *old* stdout, but a
   YcChildCreateInfo describes only final states, so there is no way to
   say that.  Hand those to a real shell instead of quietly sending
   both streams to the log. */
static bool
check_dup_order (FdMap *fdmap, const ParseState *st, YcShellError *error)
{
  size_t i;
  for (i = 0; i < fdmap->n; i++)
    {
      FdSlot *slot = &fdmap->slots[i];
      FdSlot *src;
      if (slot->info.mode != YC_CHILD_FD_MODE_DUP)
        continue;
      src = fdmap_get (fdmap, slot->info.dup_fd);
      if (src != NULL && src->order > slot->order)
        {
          set_shell_error (error, YC_SHELL_ERROR_UNSUPPORTED, st,
                           "fd %d copies fd %d, which is redirected "
                           "later on the line",
                           slot->fd, slot->info.dup_fd);
          return false;
        }
    }
  return true;
}

/* --- assembling the result --- */

static void
set_default_fd_infos (YcChildCreateInfo *ci)
{
  size_t i;
  for (i = 0; i < 3; i++)
    {
      ci->fd_infos[i].mode = YC_CHILD_FD_MODE_INHERIT;
      ci->fd_infos[i].in = (i == 0);
      ci->fd_infos[i].out = (i != 0);
    }
}

/* NULL means 'inherit', so only build a vector if the line actually
   set something. */
static char **
build_env (Argv *assigns)
{
  size_t n_environ = 0, n = 0, i, j;
  char **rv;

  if (assigns->n == 0)
    return NULL;

  while (environ[n_environ] != NULL)
    n_environ++;
  rv = YC_NEW_ARRAY (n_environ + assigns->n + 1, char *);
  for (i = 0; i < n_environ; i++)
    rv[n++] = yc_strdup (environ[i]);

  for (i = 0; i < assigns->n; i++)
    {
      const char *assign = assigns->argv[i];
      size_t namelen = strcspn (assign, "=") + 1;   /* including the '=' */
      for (j = 0; j < n; j++)
        if (strncmp (rv[j], assign, namelen) == 0)
          {
            yc_free (rv[j]);
            rv[j] = yc_strdup (assign);
            break;
          }
      if (j == n)
        rv[n++] = yc_strdup (assign);
    }
  rv[n] = NULL;
  return rv;
}

static YcChildCreateInfo *
build_create_info (Argv *args, Argv *assigns, FdMap *fdmap)
{
  YcChildCreateInfo *ci = YC_NEW0 (YcChildCreateInfo);
  size_t i, n_other = 0;

  set_default_fd_infos (ci);
  ci->argv = args->argv;               /* stolen, NULL-terminated */
  memset (args, 0, sizeof (*args));
  ci->env = build_env (assigns);

  for (i = 0; i < fdmap->n; i++)
    if (fdmap->slots[i].fd < 3)
      ci->fd_infos[fdmap->slots[i].fd] = fdmap->slots[i].info;
    else
      n_other++;

  if (n_other > 0)
    {
      ci->other_fd_infos = YC_NEW0_ARRAY (n_other, YcChildCreateOtherFdInfo);
      ci->n_other_fd_infos = n_other;
      n_other = 0;
      for (i = 0; i < fdmap->n; i++)
        if (fdmap->slots[i].fd >= 3)
          {
            ci->other_fd_infos[n_other].fd = fdmap->slots[i].fd;
            ci->other_fd_infos[n_other].info = fdmap->slots[i].info;
            n_other++;
          }
    }

  return ci;
}

static YcChildCreateInfo *
make_sh_c (const char *cmdline)
{
  YcChildCreateInfo *ci = YC_NEW0 (YcChildCreateInfo);
  set_default_fd_infos (ci);
  ci->program = yc_strdup ("/bin/sh");
  ci->argv = YC_NEW_ARRAY (4, char *);
  ci->argv[0] = yc_strdup ("sh");
  ci->argv[1] = yc_strdup ("-c");
  ci->argv[2] = yc_strdup (cmdline);
  ci->argv[3] = NULL;
  return ci;
}

/* --- the line --- */

static bool
parse_line (ParseState *st,
            Argv *args,
            Argv *assigns,
            FdMap *fdmap,
            YcShellError *error)
{
  unsigned order = 0;

  for (;;)
    {
      while (is_ifs (*st->p))
        st->p++;

      /* '#' only begins a comment at the start of a word, which is
         exactly where we are. */
      if (*st->p == 0 || *st->p == '#')
        break;

      if (is_redirect_start (st->p))
        {
          if (!parse_redirect (st, fdmap, ++order, error))
            return false;
        }
      else if (args->n == 0 && assignment_name_length (st->p) > 0)
        {
          if (!parse_assignment (st, assigns, error))
            return false;
        }
      else if (!parse_word (st, true, args, error))
        return false;
    }

  return check_dup_order (fdmap, st, error);
}

YcChildCreateInfo *
yc_shell_parse (const char *cmdline, YcShellFlags flags, YcShellError *error)
{
  ParseState st;
  Argv args, assigns;
  FdMap fdmap;
  YcChildCreateInfo *rv;
  /* Used unconditionally, so that the fallback below can tell why the
     parse failed even when the caller passed no error argument. */
  YcShellError local_error;

  memset (&local_error, 0, sizeof (local_error));
  if (error != NULL)
    memset (error, 0, sizeof (*error));

  if (cmdline == NULL)
    {
      set_shell_error (error, YC_SHELL_ERROR_EMPTY, NULL, "no command-line");
      return NULL;
    }

  st.start = cmdline;
  st.p = cmdline;
  memset (&args, 0, sizeof (args));
  memset (&assigns, 0, sizeof (assigns));
  memset (&fdmap, 0, sizeof (fdmap));

  if (!parse_line (&st, &args, &assigns, &fdmap, &local_error))
    {
      argv_clear (&args);
      argv_clear (&assigns);
      fdmap_clear (&fdmap, true);
      if (local_error.code == YC_SHELL_ERROR_UNSUPPORTED
       && (flags & YC_SHELL_FLAGS_FALLBACK_TO_SH_C) != 0)
        return make_sh_c (cmdline);
      if (error != NULL)
        *error = local_error;
      return NULL;
    }

  if (args.n == 0)
    {
      YcShellErrorCode code = (assigns.n > 0 || fdmap.n > 0)
                            ? YC_SHELL_ERROR_NO_COMMAND
                            : YC_SHELL_ERROR_EMPTY;
      st.p = cmdline;
      set_shell_error (error, code, &st,
                       code == YC_SHELL_ERROR_EMPTY
                       ? "blank line"
                       : "assignments and redirections but no command");
      argv_clear (&args);
      argv_clear (&assigns);
      fdmap_clear (&fdmap, true);
      return NULL;
    }

  rv = build_create_info (&args, &assigns, &fdmap);
  argv_clear (&args);
  argv_clear (&assigns);
  fdmap_clear (&fdmap, false);         /* filenames now belong to 'rv' */
  return rv;
}

void
yc_shell_free (YcChildCreateInfo *ci)
{
  size_t i;

  if (ci == NULL)
    return;

  free_string_array (ci->argv);
  free_string_array (ci->env);
  yc_free ((char *) ci->program);
  for (i = 0; i < 3; i++)
    yc_free ((char *) ci->fd_infos[i].filename);
  for (i = 0; i < ci->n_other_fd_infos; i++)
    yc_free ((char *) ci->other_fd_infos[i].info.filename);
  yc_free (ci->other_fd_infos);
  yc_free (ci);
}
