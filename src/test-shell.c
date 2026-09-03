/* SPDX-License-Identifier: 0BSD */
/* Tests for yc_shell_parse().
 *
 * Each case renders the parsed YcChildCreateInfo (or the error) into
 * one canonical line and compares it against a literal, so that a
 * failure shows what was produced next to what was wanted.
 *
 * The rendering only mentions fds that were actually redirected, and
 * only environment entries beginning with YCT_ -- those can only come
 * from an assignment on the line under test.  Variables that the tests
 * *expand* are named YCS_ so that they never show up as overrides.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "yc-shell.h"

static unsigned n_tests, n_failures;

/* --- rendering --- */

typedef struct {
  char buf[2048];
  size_t len;
} Rendering;

static void
render_append (Rendering *r, const char *format, ...)
{
  va_list args;
  int n;
  va_start (args, format);
  n = vsnprintf (r->buf + r->len, sizeof (r->buf) - r->len, format, args);
  va_end (args);
  if (n > 0)
    r->len += (size_t) n;
  if (r->len >= sizeof (r->buf))
    r->len = sizeof (r->buf) - 1;
}

static const char *
error_code_name (YcShellErrorCode code)
{
  switch (code)
    {
    case YC_SHELL_ERROR_NONE:        return "NONE";
    case YC_SHELL_ERROR_EMPTY:       return "EMPTY";
    case YC_SHELL_ERROR_NO_COMMAND:  return "NO_COMMAND";
    case YC_SHELL_ERROR_SYNTAX:      return "SYNTAX";
    case YC_SHELL_ERROR_UNSUPPORTED: return "UNSUPPORTED";
    }
  return "???";
}

static void
render_fd (Rendering *r, int fd, const YcChildCreateFdInfo *info)
{
  switch (info->mode)
    {
    case YC_CHILD_FD_MODE_INHERIT:
      return;                        /* the default; too noisy to print */
    case YC_CHILD_FD_MODE_FILE:
      render_append (r, " %d=%s:%s", fd,
                     info->append ? "append" : "file", info->filename);
      return;
    case YC_CHILD_FD_MODE_DUP:
      render_append (r, " %d=dup:%d", fd, info->dup_fd);
      return;
    case YC_CHILD_FD_MODE_PIPE:
      render_append (r, " %d=pipe", fd);
      return;
    case YC_CHILD_FD_MODE_NULL:
      render_append (r, " %d=null", fd);
      return;
    }
  render_append (r, " %d=mode%u", fd, (unsigned) info->mode);
}

static void
render_create_info (Rendering *r, YcChildCreateInfo *ci)
{
  /* Sorted by fd, so the test does not depend on the order that
     redirections happen to be stored in. */
  int fds[3 + 16];
  const YcChildCreateFdInfo *infos[3 + 16];
  size_t n_fds = 0, i, j;

  if (ci->program != NULL)
    render_append (r, "program=%s ", ci->program);
  render_append (r, "argv=[");
  for (i = 0; ci->argv[i] != NULL; i++)
    render_append (r, "%s%s", i == 0 ? "" : "|", ci->argv[i]);
  render_append (r, "]");

  if (ci->env != NULL)
    {
      render_append (r, " env=[");
      for (i = 0, j = 0; ci->env[i] != NULL; i++)
        if (strncmp (ci->env[i], "YCT_", 4) == 0)
          render_append (r, "%s%s", j++ == 0 ? "" : "|", ci->env[i]);
      render_append (r, "]");
    }

  for (i = 0; i < 3; i++)
    {
      fds[n_fds] = (int) i;
      infos[n_fds++] = &ci->fd_infos[i];
    }
  for (i = 0; i < ci->n_other_fd_infos && n_fds < 3 + 16; i++)
    {
      fds[n_fds] = ci->other_fd_infos[i].fd;
      infos[n_fds++] = &ci->other_fd_infos[i].info;
    }
  for (i = 1; i < n_fds; i++)
    for (j = i; j > 0 && fds[j] < fds[j - 1]; j--)
      {
        int tmp_fd = fds[j];
        const YcChildCreateFdInfo *tmp_info = infos[j];
        fds[j] = fds[j - 1];
        infos[j] = infos[j - 1];
        fds[j - 1] = tmp_fd;
        infos[j - 1] = tmp_info;
      }

  for (i = 0; i < n_fds; i++)
    render_fd (r, fds[i], infos[i]);
}

/* --- running one case --- */

static void
check_parse (const char *cmdline,
             YcShellFlags flags,
             const char *expected,
             int source_line)
{
  YcShellError error;
  YcChildCreateInfo *ci;
  Rendering r;

  memset (&r, 0, sizeof (r));
  n_tests++;

  ci = yc_shell_parse (cmdline, flags, &error);
  if (ci == NULL)
    render_append (&r, "error=%s", error_code_name (error.code));
  else
    render_create_info (&r, ci);

  if (strcmp (r.buf, expected) != 0)
    {
      n_failures++;
      printf ("FAIL (%s:%d)\n", __FILE__, source_line);
      printf ("      input: %s\n", cmdline);
      printf ("   expected: %s\n", expected);
      printf ("        got: %s\n", r.buf);
      if (ci == NULL)
        printf ("    message: %s (at byte %u)\n",
                error.message, (unsigned) error.position);
    }

  yc_shell_free (ci);
}

/* Both forms of the fallback flag, since whether a line needs a real
   shell is most of what this parser decides. */
#define CHECK(cmdline, expected) \
  check_parse ((cmdline), YC_SHELL_FLAGS_FALLBACK_TO_SH_C, (expected), __LINE__)
#define CHECK_NO_FALLBACK(cmdline, expected) \
  check_parse ((cmdline), 0, (expected), __LINE__)

/* --- the cases --- */

static void
test_words (void)
{
  CHECK ("echo hello world", "argv=[echo|hello|world]");
  CHECK ("   echo   spaced   out   ", "argv=[echo|spaced|out]");
  CHECK ("echo 'single  quoted'", "argv=[echo|single  quoted]");
  CHECK ("echo \"double  quoted\"", "argv=[echo|double  quoted]");
  CHECK ("echo bare\\ space", "argv=[echo|bare space]");
  CHECK ("echo a'b'c\"d\"e", "argv=[echo|abcde]");
  CHECK ("echo ''", "argv=[echo|]");
  CHECK ("echo \"\"", "argv=[echo|]");
  CHECK ("echo it\\'s", "argv=[echo|it's]");
  CHECK ("echo 'a$b'", "argv=[echo|a$b]");
  CHECK ("echo \"a\\$b\"", "argv=[echo|a$b]");
  /* A backslash keeps its literal self inside double quotes unless it
     precedes one of $ " \ ` . */
  CHECK ("echo \"a\\zb\"", "argv=[echo|a\\zb]");
  CHECK ("echo x=y", "argv=[echo|x=y]");
  /* An assignment is only an assignment before the command word. */
  CHECK ("echo YCT_A=1", "argv=[echo|YCT_A=1]");
}

static void
test_interpolation (void)
{
  CHECK ("echo $YCS_ONE", "argv=[echo|one]");
  CHECK ("echo ${YCS_ONE}", "argv=[echo|one]");
  CHECK ("echo x${YCS_ONE}y", "argv=[echo|xoney]");
  CHECK ("echo \"[$YCS_ONE]\"", "argv=[echo|[one]]");
  CHECK ("echo '$YCS_ONE'", "argv=[echo|$YCS_ONE]");
  CHECK ("echo $YCS_UNDEFINED_VAR", "argv=[echo]");
  CHECK ("echo \"$YCS_UNDEFINED_VAR\"", "argv=[echo|]");

  /* Unquoted expansions are split into fields; quoted ones are not. */
  CHECK ("echo $YCS_WORDS", "argv=[echo|a|b|c]");
  CHECK ("echo \"$YCS_WORDS\"", "argv=[echo|a b  c]");
  CHECK ("echo -$YCS_WORDS-", "argv=[echo|-a|b|c-]");
  CHECK ("echo $YCS_SPACED", "argv=[echo|x|y]");

  /* An empty expansion contributes a field only when quoted. */
  CHECK ("echo a $YCS_EMPTY b", "argv=[echo|a|b]");
  CHECK ("echo a \"$YCS_EMPTY\" b", "argv=[echo|a||b]");

  /* A '$' that cannot begin a name is just a '$'. */
  CHECK ("echo 100$", "argv=[echo|100$]");
  CHECK ("echo a$-b", "argv=[echo|a$-b]");
}

static void
test_assignments (void)
{
  CHECK ("YCT_A=1 prog", "argv=[prog] env=[YCT_A=1]");
  CHECK ("YCT_A=1 YCT_B=2 prog x", "argv=[prog|x] env=[YCT_A=1|YCT_B=2]");
  CHECK ("YCT_A= prog", "argv=[prog] env=[YCT_A=]");
  CHECK ("YCT_A=$YCS_ONE prog", "argv=[prog] env=[YCT_A=one]");
  CHECK ("YCT_A=\"a b\" prog", "argv=[prog] env=[YCT_A=a b]");
  /* An assignment value is not field-split, so this is one variable
     and not two words. */
  CHECK ("YCT_A=$YCS_WORDS prog", "argv=[prog] env=[YCT_A=a b  c]");
  /* Later assignment of the same name wins. */
  CHECK ("YCT_A=1 YCT_A=2 prog", "argv=[prog] env=[YCT_A=2]");
  /* 'FOO=a b' runs 'b' -- the value stops at the space. */
  CHECK ("YCT_A=a prog", "argv=[prog] env=[YCT_A=a]");
  /* No assignments at all means 'inherit', i.e. a NULL env. */
  CHECK ("prog", "argv=[prog]");

  /* As in sh, an assignment on this line is not visible to expansions
     on the same line: 'YCS_ONE=2 echo $YCS_ONE' echoes the old value. */
  CHECK ("YCT_SHADOW=$YCT_SHADOW prog", "argv=[prog] env=[YCT_SHADOW=]");
}

static void
test_redirection (void)
{
  CHECK ("prog > out.txt", "argv=[prog] 1=file:out.txt");
  CHECK ("prog >out.txt", "argv=[prog] 1=file:out.txt");
  CHECK ("prog >> out.log", "argv=[prog] 1=append:out.log");
  CHECK ("prog < in.txt", "argv=[prog] 0=file:in.txt");
  CHECK ("prog 2> err.txt", "argv=[prog] 2=file:err.txt");
  CHECK ("prog 2>>err.txt", "argv=[prog] 2=append:err.txt");
  CHECK ("prog < in.txt > out.txt 2> err.txt",
         "argv=[prog] 0=file:in.txt 1=file:out.txt 2=file:err.txt");
  CHECK ("> out.txt prog", "argv=[prog] 1=file:out.txt");
  CHECK ("prog arg1 > out.txt arg2", "argv=[prog|arg1|arg2] 1=file:out.txt");
  CHECK ("prog > $YCS_ONE", "argv=[prog] 1=file:one");
  CHECK ("prog > 'my file'", "argv=[prog] 1=file:my file");

  /* Redirecting the same fd twice: the last one wins. */
  CHECK ("prog > a > b", "argv=[prog] 1=file:b");

  /* Descriptors above 2 become other_fd_infos. */
  CHECK ("prog 3< extra.dat", "argv=[prog] 3=file:extra.dat");
  CHECK ("prog 7> seven.out 3< three.in",
         "argv=[prog] 3=file:three.in 7=file:seven.out");

  /* POSIX tokenization: digits are an fd number only when they are the
     whole token, so 'foo2>bar' is the word 'foo2' plus '1>bar'. */
  CHECK ("echo foo2>bar", "argv=[echo|foo2] 1=file:bar");
  CHECK ("echo 2 > bar", "argv=[echo|2] 1=file:bar");
}

static void
test_dup_redirection (void)
{
  CHECK ("prog 2>&1", "argv=[prog] 2=dup:1");
  CHECK ("prog >log 2>&1", "argv=[prog] 1=file:log 2=dup:1");
  CHECK ("prog >>log 2>&1", "argv=[prog] 1=append:log 2=dup:1");
  CHECK ("prog 1>&2", "argv=[prog] 1=dup:2");
  CHECK ("prog 3>&1", "argv=[prog] 3=dup:1");
  /* Redirecting a descriptor to itself is a no-op. */
  CHECK ("prog 1>&1 arg", "argv=[prog|arg]");

  /* '2>&1 >log' means 'fd 2 gets the OLD stdout', which a create-info
     cannot express, so it has to go to a real shell. */
  CHECK ("prog 2>&1 >log", "program=/bin/sh argv=[sh|-c|prog 2>&1 >log]");
  CHECK_NO_FALLBACK ("prog 2>&1 >log", "error=UNSUPPORTED");
  /* Same reasoning, mutually. */
  CHECK_NO_FALLBACK ("prog 1>&2 2>&1", "error=UNSUPPORTED");

  CHECK_NO_FALLBACK ("prog 2>&-", "error=UNSUPPORTED");
  CHECK_NO_FALLBACK ("prog &> log", "error=UNSUPPORTED");
  CHECK_NO_FALLBACK ("prog >& log", "error=UNSUPPORTED");
  CHECK_NO_FALLBACK ("prog << EOF", "error=UNSUPPORTED");
  CHECK_NO_FALLBACK ("prog <> rw.dat", "error=UNSUPPORTED");
  CHECK_NO_FALLBACK ("prog >| clobber", "error=UNSUPPORTED");
}

static void
test_comments_and_empty (void)
{
  CHECK ("echo done # trailing comment", "argv=[echo|done]");
  CHECK ("echo done#not-a-comment", "argv=[echo|done#not-a-comment]");
  CHECK ("# just a comment", "error=EMPTY");
  CHECK ("", "error=EMPTY");
  CHECK ("    ", "error=EMPTY");
  CHECK (NULL, "error=EMPTY");
  /* A line that sets things up but runs nothing is a mistake in a job
     file, so it is reported rather than sent to a shell. */
  CHECK ("YCT_A=1", "error=NO_COMMAND");
  CHECK ("> out.txt", "error=NO_COMMAND");
}

static void
test_unsupported (void)
{
  static const char *needs_a_shell[] = {
    "a | b",
    "a && b",
    "a || b",
    "a ; b",
    "prog &",
    "ls *.c",
    "ls file?.txt",
    "ls [ab].c",
    "echo $(date)",
    "echo `date`",
    "echo \"`date`\"",
    "echo ~/x",
    "echo {a,b}",
    "(subshell)",
    "! prog",
    "echo $1",
    "echo $?",
    "echo $$",
    "echo $@",
    "echo ${YCS_ONE:-default}",
    "echo ${#YCS_ONE}"
  };
  size_t i;

  for (i = 0; i < sizeof (needs_a_shell) / sizeof (needs_a_shell[0]); i++)
    {
      char expected[512];
      snprintf (expected, sizeof (expected),
                "program=/bin/sh argv=[sh|-c|%s]", needs_a_shell[i]);
      check_parse (needs_a_shell[i], YC_SHELL_FLAGS_FALLBACK_TO_SH_C,
                   expected, __LINE__);
      check_parse (needs_a_shell[i], 0, "error=UNSUPPORTED", __LINE__);
    }
}

static void
test_syntax_errors (void)
{
  /* Broken for any shell, so the fallback does not apply: we can point
     at a column and a shell could not. */
  CHECK ("echo 'unterminated", "error=SYNTAX");
  CHECK ("echo \"unterminated", "error=SYNTAX");
  CHECK ("echo trailing\\", "error=SYNTAX");
  CHECK ("prog >", "error=SYNTAX");
  CHECK ("prog > ", "error=SYNTAX");
  CHECK ("prog 2>", "error=SYNTAX");
  CHECK ("prog > $YCS_UNDEFINED_VAR", "error=SYNTAX");
}

static void
test_error_positions (void)
{
  YcShellError error;
  n_tests++;
  if (yc_shell_parse ("echo a | b", 0, &error) != NULL
   || error.position != 7)
    {
      n_failures++;
      printf ("FAIL (%s:%d) expected UNSUPPORTED at byte 7, got %u (%s)\n",
              __FILE__, __LINE__, (unsigned) error.position, error.message);
    }
}

/* The child inherits the rest of our environment, not just the
   assignments from the line. */
static void
test_env_is_inherited (void)
{
  YcChildCreateInfo *ci;
  bool found_path = false;
  size_t i;

  n_tests++;
  ci = yc_shell_parse ("YCT_A=1 prog", 0, NULL);
  if (ci == NULL || ci->env == NULL)
    {
      n_failures++;
      printf ("FAIL (%s:%d) expected an env vector\n", __FILE__, __LINE__);
      yc_shell_free (ci);
      return;
    }
  for (i = 0; ci->env[i] != NULL; i++)
    if (strncmp (ci->env[i], "PATH=", 5) == 0)
      found_path = true;
  if (!found_path)
    {
      n_failures++;
      printf ("FAIL (%s:%d) assignments dropped the inherited environment\n",
              __FILE__, __LINE__);
    }
  yc_shell_free (ci);
}

int
main (void)
{
  setenv ("YCS_ONE", "one", 1);
  setenv ("YCS_WORDS", "a b  c", 1);
  setenv ("YCS_SPACED", "  x y  ", 1);
  setenv ("YCS_EMPTY", "", 1);
  unsetenv ("YCS_UNDEFINED_VAR");
  unsetenv ("YCT_SHADOW");

  test_words ();
  test_interpolation ();
  test_assignments ();
  test_redirection ();
  test_dup_redirection ();
  test_comments_and_empty ();
  test_unsupported ();
  test_syntax_errors ();
  test_error_positions ();
  test_env_is_inherited ();

  if (n_failures > 0)
    {
      printf ("\n%u of %u tests FAILED.\n", n_failures, n_tests);
      return 1;
    }
  printf ("all %u tests passed.\n", n_tests);
  return 0;
}
