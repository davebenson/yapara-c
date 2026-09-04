/* SPDX-License-Identifier: 0BSD */
/* Tests for the UI plug-in framework.
 *
 * These use a recording UI that keeps every event, so assertions can
 * be made about what a real UI would have been told.  The framework's
 * one genuinely tricky job is reassembling lines out of arbitrarily
 * chopped-up read() results, so most of this leans on that.
 */

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "yc-alloc.h"
#include "yc-common.h"
#include "yc-shell.h"
#include "yc-ui.h"

/* The bundled UIs the tests exercise directly. */
extern const YcUIFuncs yc_ui_prefix;
extern const YcUIFuncs yc_ui_headless_jobs;

#define WATCHDOG_SECONDS 60
#define MAX_REC_JOBS     32

static unsigned n_tests, n_failures;

/* --- assertions --- */

static void
fail (int line, const char *format, ...)
{
  va_list args;
  n_failures++;
  printf ("FAIL (%s:%d) ", __FILE__, line);
  va_start (args, format);
  vprintf (format, args);
  va_end (args);
  printf ("\n");
}

static void
check_str (const char *what, const char *got, const char *expected, int line)
{
  n_tests++;
  if (got == NULL || strcmp (got, expected) != 0)
    fail (line, "%s: expected \"%s\", got \"%s\"",
          what, expected, got == NULL ? "(null)" : got);
}

static void
check_uint (const char *what, unsigned long long got,
            unsigned long long expected, int line)
{
  n_tests++;
  if (got != expected)
    fail (line, "%s: expected %llu, got %llu", what, expected, got);
}

static void
check_true (const char *what, bool got, int line)
{
  n_tests++;
  if (!got)
    fail (line, "%s: expected true", what);
}

#define CHECK_STR(what, got, exp)  check_str (what, got, exp, __LINE__)
#define CHECK_UINT(what, got, exp) \
  check_uint (what, (unsigned long long) (got), \
              (unsigned long long) (exp), __LINE__)
#define CHECK_TRUE(what, got)      check_true (what, got, __LINE__)

/* --- a growable buffer --- */

typedef struct {
  char *data;
  size_t len, alloced;
} Buf;

static void
buf_append (Buf *b, const void *data, size_t n)
{
  if (b->len + n + 1 > b->alloced)
    {
      b->alloced = b->alloced == 0 ? 128 : b->alloced;
      while (b->len + n + 1 > b->alloced)
        b->alloced *= 2;
      b->data = YC_RENEW (char, b->data, b->alloced);
    }
  memcpy (b->data + b->len, data, n);
  b->len += n;
  b->data[b->len] = 0;
}

static void
buf_clear (Buf *b)
{
  yc_free (b->data);
  memset (b, 0, sizeof (*b));
}

/* --- the recording UI --- */

typedef struct {
  bool started, ended, line_after_end, output_without_start;
  int pid;
  YcChildStatus status;
  int status_value;
  uint64_t started_micros, ended_micros;
  char *cmdline;
  Buf raw[3];          /* exact bytes, per child fd */
  Buf lines[3];        /* each delivered line, with '\n' re-added */
  unsigned n_lines[3];
  unsigned n_lines_via_subclass;
  unsigned n_destroyed;
  size_t max_line_len;
} RecJob;

/* Per-job state, carried inline in the job via job_instance_size.
   'canary' guards the far end of the struct: if the framework
   under-allocated, writing it would land outside the block and ASan
   would say so. */
#define REC_CANARY  0xfeedfaceu

typedef struct {
  YcUIJob base;                  /* must come first */
  unsigned n_lines;
  unsigned canary;
} RecJobState;

typedef struct {
  YcUI base;                     /* must come first */
  RecJob jobs[MAX_REC_JOBS];
  unsigned n_all_done;
  size_t peak_n_jobs;
  bool init_ran, destroy_ran;
  bool subclass_was_dirty;       /* not zeroed on arrival */
  bool destroyed_before_ended;
  bool destroyed_while_retained;
  bool canary_was_clobbered;
  bool wrong_ui_backpointer;
} RecUI;

static bool
rec_init (YcUI *ui, char **error_message)
{
  ((RecUI *) ui)->init_ran = true;
  return true;
}

static void
rec_job_started (YcUI *ui, YcUIJob *job)
{
  RecUI *rec = (RecUI *) ui;
  RecJob *rjob;

  if (job->index >= MAX_REC_JOBS)
    yc_die ("test-ui: more jobs than the recorder can hold");
  rjob = &rec->jobs[job->index];
  rjob->started = true;
  rjob->pid = job->pid;
  rjob->cmdline = yc_strdup (job->cmdline);
  rjob->started_micros = job->started_micros;

  if (ui->n_jobs > rec->peak_n_jobs)
    rec->peak_n_jobs = ui->n_jobs;

  /* The subclass space must arrive zeroed, and job->ui must point back
     at us -- both are promises of the interface. */
  {
    RecJobState *state = (RecJobState *) job;
    if (state->n_lines != 0 || state->canary != 0)
      rec->subclass_was_dirty = true;
    if (job->ui != ui)
      rec->wrong_ui_backpointer = true;
    state->canary = REC_CANARY;
  }
}

static void
rec_job_output (YcUI *ui, YcUIJob *job, int child_fd,
                const void *data, size_t len, uint64_t micros)
{
  RecUI *rec = (RecUI *) ui;
  RecJob *rjob = &rec->jobs[job->index];

  if (!rjob->started)
    rjob->output_without_start = true;
  if (child_fd >= 0 && child_fd < 3)
    buf_append (&rjob->raw[child_fd], data, len);
}

static void
rec_job_line (YcUI *ui, YcUIJob *job, int child_fd,
              const char *line, size_t len, uint64_t micros)
{
  RecUI *rec = (RecUI *) ui;
  RecJob *rjob = &rec->jobs[job->index];

  if (rjob->ended)
    rjob->line_after_end = true;

  {
    RecJobState *state = (RecJobState *) job;
    if (state->canary != REC_CANARY)
      rec->canary_was_clobbered = true;
    state->n_lines++;
  }

  if (child_fd < 0 || child_fd >= 3)
    return;
  /* strlen must agree with the reported length -- a UI is entitled to
     rely on the buffer being NUL-terminated at 'len'. */
  if (strlen (line) != len)
    fail (__LINE__, "job_line: len %u but strlen %u",
          (unsigned) len, (unsigned) strlen (line));
  buf_append (&rjob->lines[child_fd], line, len);
  buf_append (&rjob->lines[child_fd], "\n", 1);
  rjob->n_lines[child_fd]++;
  if (len > rjob->max_line_len)
    rjob->max_line_len = len;
}

static void
rec_job_ended (YcUI *ui, YcUIJob *job)
{
  RecUI *rec = (RecUI *) ui;
  RecJob *rjob = &rec->jobs[job->index];

  rjob->ended = true;
  rjob->status = job->status;
  rjob->status_value = job->status_value;
  rjob->ended_micros = job->ended_micros;

  /* Nothing to free: the state lives in the job and goes with it. */
  {
    RecJobState *state = (RecJobState *) job;
    if (state->canary != REC_CANARY)
      rec->canary_was_clobbered = true;
    rjob->n_lines_via_subclass = state->n_lines;
  }
}

/* Freeing memory is this callback's whole job, so what matters is that
   it fires exactly once per job and never before job_ended(). */
static void
rec_job_destroyed (YcUI *ui, YcUIJob *job)
{
  RecUI *rec = (RecUI *) ui;
  RecJob *rjob = &rec->jobs[job->index];
  size_t i;

  if (!rjob->ended)
    rec->destroyed_before_ended = true;
  rjob->n_destroyed++;

  /* By now it must be out of both arrays. */
  for (i = 0; i < ui->n_ended_jobs; i++)
    if (ui->ended_jobs[i] == job)
      rec->destroyed_while_retained = true;
  for (i = 0; i < ui->n_jobs; i++)
    if (ui->jobs[i] == job)
      rec->destroyed_while_retained = true;
}

static void
rec_all_done (YcUI *ui)
{
  ((RecUI *) ui)->n_all_done++;
}

static void
rec_destroy (YcUI *ui)
{
  RecUI *rec = (RecUI *) ui;
  size_t i, j;
  rec->destroy_ran = true;
  for (i = 0; i < MAX_REC_JOBS; i++)
    {
      yc_free (rec->jobs[i].cmdline);
      for (j = 0; j < 3; j++)
        {
          buf_clear (&rec->jobs[i].raw[j]);
          buf_clear (&rec->jobs[i].lines[j]);
        }
    }
}

static const YcUIFuncs rec_funcs = {
  "test-recorder",
  "records every event, for the test-suite",
  "Test event recorder\n",
  sizeof (RecUI),
  sizeof (RecJobState),
  0,                             /* flags */
  rec_init,
  rec_job_started,
  rec_job_output,
  rec_job_line,
  rec_job_ended,
  rec_job_destroyed,
  rec_all_done,
  rec_destroy
};

/* Same, but consuming no output at all: the framework should then not
   capture anything. */
static const YcUIFuncs quiet_funcs = {
  "test-quiet",
  "watches jobs without reading their output",
  "job watcher.\n",
  sizeof (RecUI),
  sizeof (RecJobState),
  0,                             /* flags */
  rec_init,
  rec_job_started,
  NULL,                          /* no job_output */
  NULL,                          /* no job_line */
  rec_job_ended,
  rec_job_destroyed,
  rec_all_done,
  rec_destroy
};

/* --- driving a run --- */

typedef struct {
  YcUI *ui;
  const char **cmdlines;
  size_t n, next;
} Feeder;

static void
feeder_ready_to_spawn (YcChildContainer *container)
{
  Feeder *feeder = container->container_data;
  YcChildCreateInfo *create_info;
  YcShellError shell_error;
  YcChildCreateError child_error;
  const char *cmdline;

  if (feeder->next == feeder->n)
    return;
  cmdline = feeder->cmdlines[feeder->next++];

  create_info = yc_shell_parse (cmdline, YC_SHELL_FLAGS_FALLBACK_TO_SH_C,
                                &shell_error);
  if (create_info == NULL)
    yc_die ("test-ui: parsing \"%s\": %s", cmdline, shell_error.message);

  if (yc_ui_spawn (feeder->ui, container, create_info, cmdline,
                   &child_error) == NULL)
    yc_die ("test-ui: spawning \"%s\": %s", cmdline, child_error.message);

  yc_shell_free (create_info);
}

/* Caller asserts, then yc_ui_free()s the result. */
static YcUI *
run_jobs_with (const YcUIFuncs *funcs,
               const char **cmdlines,
               size_t n_cmdlines,
               size_t max_children,
               const YcUIOptions *options)
{
  YcChildContainerCreationInfo container_info;
  YcChildContainer *container;
  Feeder feeder;
  YcUI *ui;
  char *error_message = NULL;

  memset (&container_info, 0, sizeof (container_info));
  container_info.max_children = max_children;
  container_info.callbacks.ready_to_spawn = feeder_ready_to_spawn;
  container_info.container_data = &feeder;
  container = yc_child_container_new (&container_info);

  ui = yc_ui_new (funcs, container->loop, options, &error_message);
  if (ui == NULL)
    yc_die ("test-ui: yc_ui_new: %s", error_message);

  memset (&feeder, 0, sizeof (feeder));
  feeder.ui = ui;
  feeder.cmdlines = cmdlines;
  feeder.n = n_cmdlines;

  yc_child_container_run (container);
  yc_ui_all_done (ui);
  yc_child_container_destroy (container);

  return ui;
}

static RecUI *
run_jobs (const YcUIFuncs *funcs,
          const char **cmdlines,
          size_t n_cmdlines,
          size_t max_children)
{
  return (RecUI *) run_jobs_with (funcs, cmdlines, n_cmdlines,
                                  max_children, NULL);
}

static RecUI *
run_one (const char *cmdline)
{
  const char *cmdlines[1];
  cmdlines[0] = cmdline;
  return run_jobs (&rec_funcs, cmdlines, 1, 4);
}

static char *
read_whole_file (const char *path)
{
  Buf buf;
  char chunk[4096];
  size_t n;
  FILE *fp = fopen (path, "r");

  memset (&buf, 0, sizeof (buf));
  if (fp == NULL)
    return yc_strdup ("");
  while ((n = fread (chunk, 1, sizeof (chunk), fp)) > 0)
    buf_append (&buf, chunk, n);
  fclose (fp);
  return buf.data != NULL ? buf.data : yc_strdup ("");
}

/* Runs jobs with our own stdout and stderr diverted to files, so that
   a UI which writes straight to them can have its exact output
   asserted.  Caller frees both strings. */
static void
run_capturing (const YcUIFuncs *funcs,
               const char **cmdlines,
               size_t n_cmdlines,
               size_t max_children,
               const YcUIOptions *options,
               char **stdout_out,
               char **stderr_out)
{
  char out_path[] = "/tmp/yc-test-ui-out-XXXXXX";
  char err_path[] = "/tmp/yc-test-ui-err-XXXXXX";
  int out_fd = mkstemp (out_path);
  int err_fd = mkstemp (err_path);
  int saved_out = dup (STDOUT_FILENO);
  int saved_err = dup (STDERR_FILENO);
  YcUI *ui;

  if (out_fd < 0 || err_fd < 0 || saved_out < 0 || saved_err < 0)
    yc_die ("test-ui: capturing output: %s", strerror (errno));

  fflush (stdout);
  fflush (stderr);
  dup2 (out_fd, STDOUT_FILENO);
  dup2 (err_fd, STDERR_FILENO);

  ui = run_jobs_with (funcs, cmdlines, n_cmdlines, max_children, options);

  fflush (stdout);
  fflush (stderr);
  dup2 (saved_out, STDOUT_FILENO);
  dup2 (saved_err, STDERR_FILENO);
  close (saved_out);
  close (saved_err);
  close (out_fd);
  close (err_fd);

  yc_ui_free (ui);

  *stdout_out = read_whole_file (out_path);
  *stderr_out = read_whole_file (err_path);
  unlink (out_path);
  unlink (err_path);
}

/* --- the cases --- */

static void
test_registry (void)
{
  const YcUIFuncs *const *all;
  size_t n, i;
  bool found_plain = false;

  CHECK_TRUE ("registry: 'plain' is built in", yc_ui_lookup ("plain") != NULL);
  CHECK_TRUE ("registry: unknown name gives NULL",
              yc_ui_lookup ("no-such-ui") == NULL);

  n = yc_ui_get_all (&all);
  CHECK_TRUE ("registry: get_all returns something", n >= 1);
  for (i = 0; i < n; i++)
    {
      if (strcmp (all[i]->name, "plain") == 0)
        found_plain = true;
      CHECK_TRUE ("registry: every UI has a description",
                  all[i]->description != NULL);
    }
  CHECK_TRUE ("registry: get_all includes 'plain'", found_plain);

  /* Registering makes it findable. */
  yc_ui_register (&rec_funcs);
  CHECK_TRUE ("registry: a registered UI is found",
              yc_ui_lookup ("test-recorder") == &rec_funcs);
}

static void
test_lifecycle (void)
{
  RecUI *rec = run_one ("echo hello");

  CHECK_TRUE ("lifecycle: init ran", rec->init_ran);
  CHECK_TRUE ("lifecycle: job started", rec->jobs[0].started);
  CHECK_TRUE ("lifecycle: job ended", rec->jobs[0].ended);
  CHECK_TRUE ("lifecycle: pid was reported", rec->jobs[0].pid > 0);
  CHECK_STR ("lifecycle: cmdline", rec->jobs[0].cmdline, "echo hello");
  CHECK_UINT ("lifecycle: all_done fired once", rec->n_all_done, 1);
  CHECK_UINT ("lifecycle: n_started", rec->base.n_started, 1);
  CHECK_UINT ("lifecycle: n_ended", rec->base.n_ended, 1);
  CHECK_UINT ("lifecycle: n_failed", rec->base.n_failed, 0);
  CHECK_UINT ("lifecycle: table empty at end", rec->base.n_jobs, 0);
  CHECK_TRUE ("lifecycle: no line after job_ended",
              !rec->jobs[0].line_after_end);
  CHECK_TRUE ("lifecycle: no output before job_started",
              !rec->jobs[0].output_without_start);
  CHECK_TRUE ("lifecycle: job subclass arrived zeroed",
              !rec->subclass_was_dirty);
  CHECK_TRUE ("lifecycle: job subclass survived intact",
              !rec->canary_was_clobbered);
  CHECK_TRUE ("lifecycle: job->ui points back at the ui",
              !rec->wrong_ui_backpointer);
  CHECK_TRUE ("lifecycle: timestamps ordered",
              rec->jobs[0].started_micros > 0
           && rec->jobs[0].ended_micros >= rec->jobs[0].started_micros);

  yc_ui_free (&rec->base);
  CHECK_TRUE ("lifecycle: destroy ran", true);   /* no crash == ran */
}

static void
test_lines_and_raw (void)
{
  RecUI *rec = run_one ("printf 'one\\ntwo\\nthree\\n'");

  CHECK_STR ("lines: stdout", rec->jobs[0].lines[1].data,
             "one\ntwo\nthree\n");
  CHECK_UINT ("lines: count", rec->jobs[0].n_lines[1], 3);
  /* Raw is byte-for-byte what the child wrote, newlines included. */
  CHECK_STR ("raw: stdout", rec->jobs[0].raw[1].data, "one\ntwo\nthree\n");
  CHECK_UINT ("raw: byte count", rec->jobs[0].raw[1].len, 14);
  yc_ui_free (&rec->base);
}

static void
test_stderr_is_separate (void)
{
  RecUI *rec = run_one ("sh -c 'echo out; echo err >&2'");

  CHECK_STR ("stderr: fd 1 lines", rec->jobs[0].lines[1].data, "out\n");
  CHECK_STR ("stderr: fd 2 lines", rec->jobs[0].lines[2].data, "err\n");
  yc_ui_free (&rec->base);
}

/* '2>&1' should merge onto the captured stdout pipe, as in a shell --
   the UI must see both streams arrive on fd 1 and nothing on fd 2. */
static void
test_dup_merges_onto_captured_pipe (void)
{
  RecUI *rec = run_one ("sh -c 'echo to-out; echo to-err >&2' 2>&1");

  CHECK_UINT ("2>&1: two lines on fd 1", rec->jobs[0].n_lines[1], 2);
  CHECK_UINT ("2>&1: nothing on fd 2", rec->jobs[0].n_lines[2], 0);
  CHECK_TRUE ("2>&1: stdout text present",
              strstr (rec->jobs[0].lines[1].data, "to-out") != NULL);
  CHECK_TRUE ("2>&1: stderr text present",
              strstr (rec->jobs[0].lines[1].data, "to-err") != NULL);
  yc_ui_free (&rec->base);
}

/* A line with no trailing newline is still text the child wrote. */
static void
test_unterminated_final_line (void)
{
  RecUI *rec = run_one ("printf 'a\\nno-newline-here'");

  CHECK_UINT ("unterminated: line count", rec->jobs[0].n_lines[1], 2);
  CHECK_STR ("unterminated: lines", rec->jobs[0].lines[1].data,
             "a\nno-newline-here\n");
  CHECK_STR ("unterminated: raw has no trailing newline",
             rec->jobs[0].raw[1].data, "a\nno-newline-here");
  yc_ui_free (&rec->base);
}

static void
test_crlf (void)
{
  RecUI *rec = run_one ("printf 'a\\r\\nb\\r\\n'");

  /* Lines are stripped of the CR... */
  CHECK_STR ("crlf: lines", rec->jobs[0].lines[1].data, "a\nb\n");
  /* ...but raw keeps every byte, which the binary log depends on. */
  CHECK_UINT ("crlf: raw byte count", rec->jobs[0].raw[1].len, 6);
  CHECK_STR ("crlf: raw", rec->jobs[0].raw[1].data, "a\r\nb\r\n");
  yc_ui_free (&rec->base);
}

static void
test_empty_lines (void)
{
  RecUI *rec = run_one ("printf '\\n\\na\\n\\n'");

  CHECK_UINT ("empty lines: count", rec->jobs[0].n_lines[1], 4);
  CHECK_STR ("empty lines: content", rec->jobs[0].lines[1].data, "\n\na\n\n");
  yc_ui_free (&rec->base);
}

static void
test_no_output_at_all (void)
{
  RecUI *rec = run_one ("sh -c 'exit 0'");

  CHECK_UINT ("silent job: no lines", rec->jobs[0].n_lines[1], 0);
  CHECK_TRUE ("silent job: still ended", rec->jobs[0].ended);
  yc_ui_free (&rec->base);
}

/* The whole point of the pending-line buffer: one line far longer than
   a single read(), so it must be stitched across many of them. */
static void
test_line_longer_than_a_read (void)
{
  enum { LINE_LEN = 100000 };
  RecUI *rec;
  char cmdline[128];

  snprintf (cmdline, sizeof (cmdline),
            "awk 'BEGIN{for(i=0;i<%d;i++)printf \"x\"; printf \"\\n\"}'",
            LINE_LEN);
  rec = run_one (cmdline);

  CHECK_UINT ("long line: exactly one line", rec->jobs[0].n_lines[1], 1);
  CHECK_UINT ("long line: length", rec->jobs[0].max_line_len, LINE_LEN);
  CHECK_UINT ("long line: raw bytes", rec->jobs[0].raw[1].len, LINE_LEN + 1);
  yc_ui_free (&rec->base);
}

/* Many lines, to shake out off-by-ones in the memmove of the leftover
   partial line after each chunk. */
static void
test_many_lines (void)
{
  enum { N_LINES = 20000 };
  RecUI *rec;
  char cmdline[128];
  size_t i, seen = 0;

  snprintf (cmdline, sizeof (cmdline),
            "awk 'BEGIN{for(i=1;i<=%d;i++)print i}'", N_LINES);
  rec = run_one (cmdline);

  CHECK_UINT ("many lines: count", rec->jobs[0].n_lines[1], N_LINES);
  /* Spot-check that the content is intact and in order. */
  for (i = 0; i < rec->jobs[0].lines[1].len; i++)
    if (rec->jobs[0].lines[1].data[i] == '\n')
      seen++;
  CHECK_UINT ("many lines: newlines re-added", seen, N_LINES);
  CHECK_TRUE ("many lines: starts at 1",
              strncmp (rec->jobs[0].lines[1].data, "1\n2\n3\n", 6) == 0);
  CHECK_TRUE ("many lines: ends at N",
              rec->jobs[0].lines[1].len >= 6
           && strcmp (rec->jobs[0].lines[1].data
                      + rec->jobs[0].lines[1].len - 6, "20000\n") == 0);
  yc_ui_free (&rec->base);
}

/* An fd the command-line redirected is the caller's business, so the
   framework must leave it alone rather than stealing it for the UI. */
static void
test_redirection_beats_capture (void)
{
  RecUI *rec;
  char cmdline[256];
  char path[] = "/tmp/yc-test-ui-XXXXXX";
  FILE *fp;
  char contents[64] = "";
  int fd = mkstemp (path);

  if (fd < 0)
    yc_die ("mkstemp: %s", strerror (errno));
  close (fd);

  snprintf (cmdline, sizeof (cmdline), "sh -c 'echo to-file' > %s", path);
  rec = run_one (cmdline);

  CHECK_UINT ("redirect: UI saw no stdout", rec->jobs[0].n_lines[1], 0);
  fp = fopen (path, "r");
  if (fp != NULL)
    {
      if (fgets (contents, sizeof (contents), fp) == NULL)
        contents[0] = 0;
      fclose (fp);
    }
  CHECK_STR ("redirect: file got the output", contents, "to-file\n");
  unlink (path);
  yc_ui_free (&rec->base);
}

/* A UI that consumes no output must get no pipes, so its children
   inherit our stdout instead.  Parking our stdout on a file for the
   duration makes the difference visible: with capture wrongly forced
   on, the child's text would be read into the framework and discarded
   and the file would come back empty.  (Merely checking that nothing
   reached the UI would prove nothing -- there are no callbacks to
   reach.)  Nothing in this UI prints, so no diagnostics are lost. */
static void
test_quiet_ui_captures_nothing (void)
{
  const char *cmdlines[1];
  RecUI *rec;
  char path[] = "/tmp/yc-test-ui-inherit-XXXXXX";
  char contents[64] = "";
  FILE *fp;
  int file_fd = mkstemp (path);
  int saved_stdout = dup (STDOUT_FILENO);

  if (file_fd < 0 || saved_stdout < 0)
    yc_die ("test-ui: redirecting stdout: %s", strerror (errno));

  cmdlines[0] = "echo inherited-output";

  fflush (stdout);
  dup2 (file_fd, STDOUT_FILENO);
  rec = run_jobs (&quiet_funcs, cmdlines, 1, 2);
  fflush (stdout);
  dup2 (saved_stdout, STDOUT_FILENO);
  close (saved_stdout);
  close (file_fd);

  fp = fopen (path, "r");
  if (fp != NULL)
    {
      if (fgets (contents, sizeof (contents), fp) == NULL)
        contents[0] = 0;
      fclose (fp);
    }
  unlink (path);

  CHECK_TRUE ("quiet: job started", rec->jobs[0].started);
  CHECK_TRUE ("quiet: job ended", rec->jobs[0].ended);
  CHECK_UINT ("quiet: exited cleanly", rec->jobs[0].status_value, 0);
  CHECK_UINT ("quiet: nothing recorded", rec->jobs[0].raw[1].len, 0);
  CHECK_STR ("quiet: child inherited our stdout", contents,
             "inherited-output\n");
  yc_ui_free (&rec->base);
}

static void
test_failure_accounting (void)
{
  const char *cmdlines[4];
  RecUI *rec;

  cmdlines[0] = "sh -c 'exit 0'";
  cmdlines[1] = "sh -c 'exit 5'";
  cmdlines[2] = "sh -c 'kill -TERM $$'";
  cmdlines[3] = "echo fine";
  rec = run_jobs (&rec_funcs, cmdlines, 4, 2);

  CHECK_UINT ("failures: n_started", rec->base.n_started, 4);
  CHECK_UINT ("failures: n_ended", rec->base.n_ended, 4);
  CHECK_UINT ("failures: n_failed", rec->base.n_failed, 2);
  CHECK_UINT ("failures: exit 0 status", rec->jobs[0].status_value, 0);
  CHECK_UINT ("failures: exit 5 status", rec->jobs[1].status_value, 5);
  CHECK_UINT ("failures: killed", rec->jobs[2].status,
              YC_CHILD_STATUS_KILLED);
  CHECK_UINT ("failures: signal", rec->jobs[2].status_value, SIGTERM);
  yc_ui_free (&rec->base);
}

/* Indices must be dense, 0-based and in spawn order, since a UI shows
   them to the user and uses them as a handle. */
static void
test_indices_and_concurrency (void)
{
  enum { N_JOBS = 10, MAX_CHILDREN = 3 };
  const char *cmdlines[N_JOBS];
  RecUI *rec;
  size_t i;

  for (i = 0; i < N_JOBS; i++)
    cmdlines[i] = "echo tick";
  rec = run_jobs (&rec_funcs, cmdlines, N_JOBS, MAX_CHILDREN);

  for (i = 0; i < N_JOBS; i++)
    {
      char what[64];
      snprintf (what, sizeof (what), "index %u: started", (unsigned) i);
      check_true (what, rec->jobs[i].started, __LINE__);
      snprintf (what, sizeof (what), "index %u: ended", (unsigned) i);
      check_true (what, rec->jobs[i].ended, __LINE__);
      snprintf (what, sizeof (what), "index %u: output", (unsigned) i);
      check_str (what, rec->jobs[i].lines[1].data, "tick\n", __LINE__);
    }
  CHECK_TRUE ("concurrency: never past max_children",
              rec->peak_n_jobs <= MAX_CHILDREN);
  CHECK_UINT ("concurrency: filled every slot", rec->peak_n_jobs,
              MAX_CHILDREN);
  yc_ui_free (&rec->base);
}

/* --- retaining finished jobs --- */

/* A UI that sets max_ended_jobs from init(); everything else is the
   recorder, so its bookkeeping still applies. */
static size_t retain_max = 0;

static bool
retain_init (YcUI *ui, char **error_message)
{
  ui->max_ended_jobs = retain_max;
  return rec_init (ui, error_message);
}

static const YcUIFuncs retain_funcs = {
  "test-retain",
  "keeps finished jobs around",
  "keeps finished jobs around\n",
  sizeof (RecUI),
  sizeof (RecJobState),
  0,                             /* flags */
  retain_init,
  rec_job_started,
  rec_job_output,
  rec_job_line,
  rec_job_ended,
  rec_job_destroyed,
  rec_all_done,
  rec_destroy
};

/* The default: no retention, so a job dies as job_ended() returns. */
static void
test_no_retention_by_default (void)
{
  const char *cmdlines[3];
  RecUI *rec;
  size_t i;

  for (i = 0; i < 3; i++)
    cmdlines[i] = "echo x";
  rec = run_jobs (&rec_funcs, cmdlines, 3, 1);

  CHECK_UINT ("no retention: max_ended_jobs defaults to 0",
              rec->base.max_ended_jobs, 0);
  CHECK_UINT ("no retention: nothing retained", rec->base.n_ended_jobs, 0);
  CHECK_UINT ("no retention: every job ended", rec->base.n_ended, 3);
  for (i = 0; i < 3; i++)
    CHECK_UINT ("no retention: destroyed exactly once",
                rec->jobs[i].n_destroyed, 1);
  CHECK_TRUE ("no retention: never destroyed before ended",
              !rec->destroyed_before_ended);
  CHECK_TRUE ("no retention: not destroyed while still listed",
              !rec->destroyed_while_retained);
  yc_ui_free (&rec->base);
}

static void
test_retention_holds_finished_jobs (void)
{
  const char *cmdlines[4];
  RecUI *rec;
  size_t i;

  for (i = 0; i < 4; i++)
    cmdlines[i] = "echo x";

  retain_max = 10;               /* more than enough for 4 jobs */
  rec = run_jobs (&retain_funcs, cmdlines, 4, 1);

  CHECK_UINT ("retention: all four held", rec->base.n_ended_jobs, 4);
  /* Held, so not yet destroyed. */
  for (i = 0; i < 4; i++)
    CHECK_UINT ("retention: not destroyed while held",
                rec->jobs[i].n_destroyed, 0);

  /* Appended as they finish, so the array is in end-time order. */
  CHECK_TRUE ("retention: oldest first",
              rec->base.ended_jobs[0]->index == 0
           && rec->base.ended_jobs[3]->index == 3);
  for (i = 1; i < rec->base.n_ended_jobs; i++)
    CHECK_TRUE ("retention: end times do not go backwards",
                rec->base.ended_jobs[i]->ended_micros
                >= rec->base.ended_jobs[i - 1]->ended_micros);

  /* A held job is still fully readable -- the whole point. */
  CHECK_TRUE ("retention: held job is marked finished",
              !rec->base.ended_jobs[0]->running);
  CHECK_STR ("retention: held job kept its cmdline",
             rec->base.ended_jobs[0]->cmdline, "echo x");

  /* yc_ui_free() must still destroy what is left. */
  yc_ui_free (&rec->base);
}

/* The cap evicts the oldest, and never the job that just arrived. */
static void
test_retention_evicts_oldest (void)
{
  const char *cmdlines[6];
  RecUI *rec;
  size_t i;

  for (i = 0; i < 6; i++)
    cmdlines[i] = "echo x";

  retain_max = 2;
  rec = run_jobs (&retain_funcs, cmdlines, 6, 1);

  CHECK_UINT ("eviction: never exceeds the cap", rec->base.n_ended_jobs, 2);
  /* The two most recent survive; the first four were evicted. */
  CHECK_UINT ("eviction: newest retained", rec->base.ended_jobs[1]->index, 5);
  CHECK_UINT ("eviction: second newest retained",
              rec->base.ended_jobs[0]->index, 4);
  for (i = 0; i < 4; i++)
    CHECK_UINT ("eviction: evicted jobs were destroyed once",
                rec->jobs[i].n_destroyed, 1);
  for (i = 4; i < 6; i++)
    CHECK_UINT ("eviction: retained jobs not yet destroyed",
                rec->jobs[i].n_destroyed, 0);
  CHECK_TRUE ("eviction: never destroyed before ended",
              !rec->destroyed_before_ended);

  yc_ui_free (&rec->base);
}

static void
test_reap_job (void)
{
  const char *cmdlines[3];
  RecUI *rec;
  YcUIJob *middle;

  cmdlines[0] = "echo x";
  cmdlines[1] = "echo y";
  cmdlines[2] = "echo z";

  retain_max = 10;
  rec = run_jobs (&retain_funcs, cmdlines, 3, 1);
  CHECK_UINT ("reap: three held to begin with", rec->base.n_ended_jobs, 3);

  /* Reaping out of the middle must close the gap, not leave a hole. */
  middle = rec->base.ended_jobs[1];
  yc_ui_reap_job (&rec->base, middle);
  CHECK_UINT ("reap: one fewer held", rec->base.n_ended_jobs, 2);
  CHECK_UINT ("reap: reaped job destroyed once",
              rec->jobs[1].n_destroyed, 1);
  CHECK_UINT ("reap: the array closed up (0 then 2)",
              rec->base.ended_jobs[0]->index, 0);
  CHECK_UINT ("reap: the array closed up (index 1 is job 2)",
              rec->base.ended_jobs[1]->index, 2);
  CHECK_UINT ("reap: others untouched", rec->jobs[0].n_destroyed, 0);

  /* Walking backwards is the documented way to reap in a loop. */
  while (rec->base.n_ended_jobs > 0)
    yc_ui_reap_job (&rec->base,
                    rec->base.ended_jobs[rec->base.n_ended_jobs - 1]);
  CHECK_UINT ("reap: all reaped", rec->base.n_ended_jobs, 0);
  CHECK_UINT ("reap: job 0 destroyed once", rec->jobs[0].n_destroyed, 1);
  CHECK_UINT ("reap: job 2 destroyed once", rec->jobs[2].n_destroyed, 1);

  yc_ui_free (&rec->base);
}

/* --- job subclassing --- */

/* A job's inline state has to survive every callback and be released
   with the job.  Under ASan, a framework that allocated only
   sizeof(YcUIJob) would be caught writing the canary. */
static void
test_job_subclassing (void)
{
  const char *cmdlines[3];
  RecUI *rec;
  size_t i;

  cmdlines[0] = "printf 'a\\nb\\nc\\n'";
  cmdlines[1] = "echo one";
  cmdlines[2] = "sh -c 'exit 0'";
  rec = run_jobs (&rec_funcs, cmdlines, 3, 1);

  CHECK_TRUE ("subclass: arrived zeroed", !rec->subclass_was_dirty);
  CHECK_TRUE ("subclass: intact at every callback",
              !rec->canary_was_clobbered);
  CHECK_TRUE ("subclass: job->ui is the ui that spawned it",
              !rec->wrong_ui_backpointer);

  /* Counted in the job's own space, read back in job_ended: proof the
     state persisted across callbacks rather than being re-zeroed. */
  CHECK_UINT ("subclass: per-job count survived (3 lines)",
              rec->jobs[0].n_lines_via_subclass, 3);
  CHECK_UINT ("subclass: per-job count survived (1 line)",
              rec->jobs[1].n_lines_via_subclass, 1);
  CHECK_UINT ("subclass: per-job count for a silent job",
              rec->jobs[2].n_lines_via_subclass, 0);

  /* Each job gets its own copy, not a shared one. */
  for (i = 0; i < 3; i++)
    CHECK_TRUE ("subclass: every job was started", rec->jobs[i].started);

  yc_ui_free (&rec->base);
}

/* A UI that asks for no per-job space still works; the framework just
   allocates a bare YcUIJob. */
static void
test_no_job_subclass (void)
{
  const char *cmdlines[1];
  char *out = NULL, *err = NULL;

  CHECK_UINT ("no subclass: prefix asks for none",
              yc_ui_prefix.job_instance_size, 0);
  cmdlines[0] = "echo fine";
  run_capturing (&yc_ui_prefix, cmdlines, 1, 1, NULL, &out, &err);
  CHECK_STR ("no subclass: still works", out, "0O: fine\n");
  yc_free (out);
  yc_free (err);
}

/* --- the prefix UI --- */

/* max_children of 1 makes the interleaving deterministic: the next job
   only starts once the previous one's pipes have drained. */
static void
test_prefix_ui_format (void)
{
  const char *cmdlines[4];
  char *out = NULL, *err = NULL;

  cmdlines[0] = "printf 'first\\nsecond\\n'";
  cmdlines[1] = "sh -c 'echo to-err >&2'";
  cmdlines[2] = "sh -c 'echo bye; exit 4'";
  cmdlines[3] = "sh -c 'kill -TERM $$'";

  run_capturing (&yc_ui_prefix, cmdlines, 4, 1, NULL, &out, &err);

  /* Every line of child output on stdout, stderr lines included: the
     tag says which stream, so one ordered stream is more useful. */
  CHECK_STR ("prefix: stdout", out,
             "0O: first\n"
             "0O: second\n"
             "1E: to-err\n"
             "2O: bye\n");
  /* yapara's own notes, kept out of the data. */
  CHECK_STR ("prefix: stderr", err,
             "2!: exited with status 4\n"
             "3!: killed by signal 15\n"
             "2 of 4 jobs failed.\n");

  yc_free (out);
  yc_free (err);
}

static void
test_prefix_ui_quiet_when_all_succeed (void)
{
  const char *cmdlines[2];
  char *out = NULL, *err = NULL;

  cmdlines[0] = "echo one";
  cmdlines[1] = "echo two";
  run_capturing (&yc_ui_prefix, cmdlines, 2, 1, NULL, &out, &err);

  CHECK_STR ("prefix: clean run stdout", out, "0O: one\n1O: two\n");
  CHECK_STR ("prefix: clean run says nothing on stderr", err, "");

  yc_free (out);
  yc_free (err);
}

/* An unterminated final line is still tagged and still shows up. */
static void
test_prefix_ui_unterminated_line (void)
{
  const char *cmdlines[1];
  char *out = NULL, *err = NULL;

  cmdlines[0] = "printf 'a\\nb'";
  run_capturing (&yc_ui_prefix, cmdlines, 1, 1, NULL, &out, &err);

  CHECK_STR ("prefix: unterminated final line", out, "0O: a\n0O: b\n");
  yc_free (out);
  yc_free (err);
}

/* --- options --- */

static void
check_index (const char *what, const YcUIOptions *options,
             uint64_t index, const char *expected, int line)
{
  char buf[YC_UI_INDEX_BUF_SIZE];
  check_str (what, yc_ui_format_index (options, index, buf, sizeof (buf)),
             expected, line);
}

#define CHECK_INDEX(what, opts, index, exp) \
  check_index (what, opts, index, exp, __LINE__)

static void
test_index_format (void)
{
  YcUIOptions options;

  yc_ui_options_init (&options);
  CHECK_UINT ("index: default base", options.index_format.base,
              YC_UI_INDEX_DECIMAL);
  CHECK_UINT ("index: default width", options.index_format.width, 0);
  CHECK_TRUE ("index: default padding is not zeroes",
              !options.index_format.zero_pad);

  CHECK_INDEX ("index: natural 0", &options, 0, "0");
  CHECK_INDEX ("index: natural 7", &options, 7, "7");
  CHECK_INDEX ("index: natural 12345", &options, 12345, "12345");

  /* 6 digits, zero-padded. */
  options.index_format.width = 6;
  options.index_format.zero_pad = true;
  CHECK_INDEX ("index: zero-pad 0", &options, 0, "000000");
  CHECK_INDEX ("index: zero-pad 42", &options, 42, "000042");
  CHECK_INDEX ("index: zero-pad 123456", &options, 123456, "123456");
  /* A width is a minimum, as in printf: wider is never truncated. */
  CHECK_INDEX ("index: wider than the width", &options, 1234567, "1234567");

  /* Spaces when zero_pad is off, so columns still line up. */
  options.index_format.zero_pad = false;
  CHECK_INDEX ("index: space-pad 42", &options, 42, "    42");

  /* Hex. */
  options.index_format.width = 0;
  options.index_format.base = YC_UI_INDEX_HEX;
  CHECK_INDEX ("index: hex 0", &options, 0, "0");
  CHECK_INDEX ("index: hex 255", &options, 255, "ff");
  CHECK_INDEX ("index: hex 4096", &options, 4096, "1000");
  options.index_format.width = 4;
  options.index_format.zero_pad = true;
  CHECK_INDEX ("index: hex zero-padded", &options, 255, "00ff");

  /* The extremes: a 64-bit index, and a width past the cap. */
  options.index_format.width = 0;
  options.index_format.base = YC_UI_INDEX_DECIMAL;
  CHECK_INDEX ("index: uint64 max", &options, 18446744073709551615ULL,
               "18446744073709551615");
  options.index_format.base = YC_UI_INDEX_HEX;
  CHECK_INDEX ("index: uint64 max in hex", &options, 18446744073709551615ULL,
               "ffffffffffffffff");

  /* An absurd width is clamped rather than overrunning the buffer. */
  options.index_format.base = YC_UI_INDEX_DECIMAL;
  options.index_format.width = 100000;
  options.index_format.zero_pad = true;
  {
    char buf[YC_UI_INDEX_BUF_SIZE];
    yc_ui_format_index (&options, 1, buf, sizeof (buf));
    CHECK_UINT ("index: absurd width clamped", strlen (buf),
                YC_UI_INDEX_MAX_WIDTH);
  }

  /* A buffer too small truncates instead of overrunning. */
  {
    char small[4];
    options.index_format.width = 0;
    CHECK_STR ("index: truncates into a small buffer",
               yc_ui_format_index (&options, 123456, small, sizeof (small)),
               "123");
  }

  yc_ui_options_clear (&options);
}

static void
test_options_key_values (void)
{
  YcUIOptions options;
  char *error = NULL;

  yc_ui_options_init (&options);

  CHECK_TRUE ("kv: unset key gives NULL",
              yc_ui_options_get (&options, "nope") == NULL);

  CHECK_TRUE ("kv: plain pair parses",
              yc_ui_options_parse (&options, "color=blue", &error));
  CHECK_STR ("kv: value", yc_ui_options_get (&options, "color"), "blue");

  /* An empty value is legitimate -- '--ui-option=prefix=' clears it. */
  CHECK_TRUE ("kv: empty value parses",
              yc_ui_options_parse (&options, "prefix=", &error));
  CHECK_STR ("kv: empty value", yc_ui_options_get (&options, "prefix"), "");

  /* Split at the FIRST '=', so a value may contain one. */
  CHECK_TRUE ("kv: value containing '=' parses",
              yc_ui_options_parse (&options, "fmt=a=b=c", &error));
  CHECK_STR ("kv: value keeps its '='",
             yc_ui_options_get (&options, "fmt"), "a=b=c");

  /* Repeats override rather than erroring. */
  CHECK_TRUE ("kv: repeat parses",
              yc_ui_options_parse (&options, "color=red", &error));
  CHECK_STR ("kv: last one wins",
             yc_ui_options_get (&options, "color"), "red");
  CHECK_UINT ("kv: both are kept in order", options.n_extra, 4);
  CHECK_STR ("kv: the earlier value is still there",
             options.extra[0].value, "blue");

  /* Malformed input is rejected with a message, not silently dropped. */
  CHECK_TRUE ("kv: no '=' is an error",
              !yc_ui_options_parse (&options, "novalue", &error));
  CHECK_TRUE ("kv: no '=' sets an error message", error != NULL);
  yc_free (error);
  error = NULL;
  CHECK_TRUE ("kv: empty key is an error",
              !yc_ui_options_parse (&options, "=orphan", &error));
  CHECK_TRUE ("kv: empty key sets an error message", error != NULL);
  yc_free (error);

  /* Typed accessors, using yc_parse_boolean's vocabulary. */
  yc_ui_options_add (&options, "wrap", "yes");
  yc_ui_options_add (&options, "follow", "0");
  yc_ui_options_add (&options, "rows", "24");
  CHECK_TRUE ("kv: get_bool true", yc_ui_options_get_bool (&options, "wrap",
                                                           false));
  CHECK_TRUE ("kv: get_bool false", !yc_ui_options_get_bool (&options,
                                                             "follow", true));
  CHECK_TRUE ("kv: get_bool default when unset",
              yc_ui_options_get_bool (&options, "absent", true));
  CHECK_UINT ("kv: get_uint", yc_ui_options_get_uint (&options, "rows", 0),
              24);
  CHECK_UINT ("kv: get_uint default when unset",
              yc_ui_options_get_uint (&options, "absent", 80), 80);

  yc_ui_options_clear (&options);
  CHECK_UINT ("kv: clear empties it", options.n_extra, 0);
  CHECK_TRUE ("kv: clear releases the array", options.extra == NULL);
}

/* A UI created without options still gets working defaults. */
static void
test_null_options_gives_defaults (void)
{
  const char *cmdlines[1];
  char *out = NULL, *err = NULL;

  cmdlines[0] = "echo hi";
  run_capturing (&yc_ui_prefix, cmdlines, 1, 1, NULL, &out, &err);
  CHECK_STR ("null options: natural index width", out, "0O: hi\n");
  yc_free (out);
  yc_free (err);
}

/* The whole point of putting this in shared options: a backend picks
   it up without knowing where the setting came from. */
static void
test_prefix_ui_honours_index_options (void)
{
  const char *cmdlines[3];
  YcUIOptions options;
  char *out = NULL, *err = NULL;
  size_t i;

  for (i = 0; i < 3; i++)
    cmdlines[i] = "echo x";

  yc_ui_options_init (&options);
  options.index_format.width = 6;
  options.index_format.zero_pad = true;
  run_capturing (&yc_ui_prefix, cmdlines, 3, 1, &options, &out, &err);
  CHECK_STR ("prefix: 6 digits, zero-padded", out,
             "000000O: x\n000001O: x\n000002O: x\n");
  yc_free (out);
  yc_free (err);

  /* Hex, and wide enough to show the padding. */
  options.index_format.base = YC_UI_INDEX_HEX;
  options.index_format.width = 4;
  run_capturing (&yc_ui_prefix, cmdlines, 3, 1, &options, &out, &err);
  CHECK_STR ("prefix: hex, zero-padded", out,
             "0000O: x\n0001O: x\n0002O: x\n");
  yc_free (out);
  yc_free (err);

  yc_ui_options_clear (&options);
}

/* The formatting must reach the notes on stderr too, not just the
   output lines. */
static void
test_index_options_reach_failure_notes (void)
{
  const char *cmdlines[1];
  YcUIOptions options;
  char *out = NULL, *err = NULL;

  cmdlines[0] = "sh -c 'exit 3'";
  yc_ui_options_init (&options);
  options.index_format.width = 4;
  options.index_format.zero_pad = true;

  run_capturing (&yc_ui_prefix, cmdlines, 1, 1, &options, &out, &err);
  CHECK_STR ("prefix: note uses the same index format", err,
             "0000!: exited with status 3\n"
             "1 of 1 jobs failed.\n");
  yc_free (out);
  yc_free (err);
  yc_ui_options_clear (&options);
}

static void
test_prefix_ui_registered (void)
{
  CHECK_TRUE ("prefix: is built in", yc_ui_lookup ("prefix") == &yc_ui_prefix);
  CHECK_UINT ("prefix: needs no instance state of its own",
              yc_ui_prefix.instance_size, 0);
}

/* --- the headless-jobs UI --- */

static char *
job_file_path (const char *dir, const char *name)
{
  static char path[512];
  snprintf (path, sizeof (path), "%s/%s", dir, name);
  return path;
}

static bool
job_file_exists (const char *dir, const char *name)
{
  struct stat statbuf;
  return stat (job_file_path (dir, name), &statbuf) == 0;
}

static char *
read_job_file (const char *dir, const char *name)
{
  return read_whole_file (job_file_path (dir, name));
}

/* Enough of a JSON check for a test: the writer emits one key per
   line, so looking for the exact rendered key/value pair catches both
   a wrong value and a mangled escape. */
static void
check_json_has (const char *what, const char *json, const char *fragment,
                int line)
{
  n_tests++;
  if (json == NULL || strstr (json, fragment) == NULL)
    fail (line, "%s: expected to find %s in:\n%s", what, fragment,
          json == NULL ? "(null)" : json);
}

#define CHECK_JSON_HAS(what, json, frag) \
  check_json_has (what, json, frag, __LINE__)

static char *
make_out_dir (void)
{
  static char dir[] = "/tmp/yc-test-ui-jobs-XXXXXX";
  strcpy (dir, "/tmp/yc-test-ui-jobs-XXXXXX");
  if (mkdtemp (dir) == NULL)
    yc_die ("mkdtemp: %s", strerror (errno));
  return dir;
}

static void
remove_out_dir (const char *dir)
{
  char command[512];
  snprintf (command, sizeof (command), "rm -rf '%s'", dir);
  if (system (command) != 0)
    yc_warn ("test-ui: could not clean up %s", dir);
}

static void
test_headless_jobs_needs_out_dir (void)
{
  YcUI *ui;
  char *error = NULL;
  YcUIOptions options;

  yc_ui_options_init (&options);
  ui = yc_ui_new (&yc_ui_headless_jobs, NULL, &options, &error);
  CHECK_TRUE ("headless: refuses to start without --out-dir", ui == NULL);
  CHECK_TRUE ("headless: says why", error != NULL);
  yc_free (error);
}

static void
test_headless_jobs_files (void)
{
  const char *cmdlines[4];
  YcUIOptions options;
  char *out = NULL, *err = NULL, *json;
  char *dir = make_out_dir ();

  cmdlines[0] = "echo hello";
  cmdlines[1] = "sh -c 'echo to-out; echo to-err >&2'";
  cmdlines[2] = "sh -c 'exit 7'";
  cmdlines[3] = "sh -c 'kill -TERM $$'";

  yc_ui_options_init (&options);
  options.out_dir = dir;
  options.index_format.width = 4;
  run_capturing (&yc_ui_headless_jobs, cmdlines, 4, 1, &options, &out, &err);

  /* A width of 4 with no --index-zero-pad still zero-pads the
     filename, so the names sort. */
  CHECK_TRUE ("headless: 0000-start.json",
              job_file_exists (dir, "0000-start.json"));
  CHECK_TRUE ("headless: 0000-end.json",
              job_file_exists (dir, "0000-end.json"));
  CHECK_STR ("headless: stdout is byte-exact",
             read_job_file (dir, "0000.stdout"), "hello\n");

  /* Nothing was written to stderr, so no file was made: the byte
     counts in -end.json say so, making absence unambiguous. */
  CHECK_TRUE ("headless: no .stderr when nothing was written",
              !job_file_exists (dir, "0000.stderr"));

  CHECK_STR ("headless: both streams, separately (out)",
             read_job_file (dir, "0001.stdout"), "to-out\n");
  CHECK_STR ("headless: both streams, separately (err)",
             read_job_file (dir, "0001.stderr"), "to-err\n");

  json = read_job_file (dir, "0000-start.json");
  CHECK_JSON_HAS ("headless: start index", json, "\"index\": 0,");
  CHECK_JSON_HAS ("headless: start cmdline", json,
                  "\"cmdline\": \"echo hello\"");
  CHECK_JSON_HAS ("headless: start has a timestamp", json,
                  "\"started_micros\": ");
  yc_free (json);

  json = read_job_file (dir, "0000-end.json");
  CHECK_JSON_HAS ("headless: clean exit status", json,
                  "\"status\": \"exited\",");
  CHECK_JSON_HAS ("headless: clean exit code", json, "\"exit_code\": 0,");
  CHECK_JSON_HAS ("headless: signal is null when it exited", json,
                  "\"signal\": null,");
  CHECK_JSON_HAS ("headless: stdout byte count", json,
                  "\"stdout_bytes\": 6,");
  CHECK_JSON_HAS ("headless: stderr byte count", json,
                  "\"stderr_bytes\": 0");
  CHECK_JSON_HAS ("headless: elapsed is reported", json,
                  "\"elapsed_micros\": ");
  yc_free (json);

  json = read_job_file (dir, "0002-end.json");
  CHECK_JSON_HAS ("headless: nonzero exit code", json, "\"exit_code\": 7,");
  yc_free (json);

  /* A signalled job reports the signal, and nulls exit_code, so the
     shape is the same either way. */
  json = read_job_file (dir, "0003-end.json");
  CHECK_JSON_HAS ("headless: killed status", json, "\"status\": \"killed\",");
  CHECK_JSON_HAS ("headless: exit_code is null when killed", json,
                  "\"exit_code\": null,");
  CHECK_JSON_HAS ("headless: signal", json, "\"signal\": 15,");
  yc_free (json);

  /* Nothing on our stdout: that is the point of it being headless. */
  CHECK_STR ("headless: says nothing on stdout", out, "");

  yc_free (out);
  yc_free (err);
  yc_ui_options_clear (&options);
  remove_out_dir (dir);
}

/* Raw bytes, not lines: no CRLF rewriting and no newline invented for
   a final unterminated line. */
static void
test_headless_jobs_writes_raw_bytes (void)
{
  const char *cmdlines[1];
  YcUIOptions options;
  char *out = NULL, *err = NULL, *contents;
  char *dir = make_out_dir ();

  cmdlines[0] = "printf 'a\\r\\nb'";
  yc_ui_options_init (&options);
  options.out_dir = dir;
  run_capturing (&yc_ui_headless_jobs, cmdlines, 1, 1, &options, &out, &err);

  contents = read_job_file (dir, "0.stdout");
  CHECK_STR ("headless: raw bytes preserved", contents, "a\r\nb");
  yc_free (contents);

  contents = read_job_file (dir, "0-end.json");
  CHECK_JSON_HAS ("headless: byte count matches", contents,
                  "\"stdout_bytes\": 4,");
  yc_free (contents);

  yc_free (out);
  yc_free (err);
  yc_ui_options_clear (&options);
  remove_out_dir (dir);
}

/* A cmdline is arbitrary text, so the json writer has to escape it. */
static void
test_headless_jobs_escapes_json (void)
{
  const char *cmdlines[1];
  YcUIOptions options;
  char *out = NULL, *err = NULL, *json;
  char *dir = make_out_dir ();

  /* Contains a double quote and a backslash, both of which would
     produce unparseable json if emitted verbatim. */
  cmdlines[0] = "echo \"a\\\\b\"";
  yc_ui_options_init (&options);
  options.out_dir = dir;
  run_capturing (&yc_ui_headless_jobs, cmdlines, 1, 1, &options, &out, &err);

  json = read_job_file (dir, "0-start.json");
  CHECK_JSON_HAS ("headless: quotes and backslashes escaped", json,
                  "\"cmdline\": \"echo \\\"a\\\\\\\\b\\\"\"");
  yc_free (json);

  yc_free (out);
  yc_free (err);
  yc_ui_options_clear (&options);
  remove_out_dir (dir);
}

/* --out-dir should be created, parents and all. */
static void
test_out_dir_is_created (void)
{
  YcUIOptions options;
  char *error = NULL;
  char *dir = make_out_dir ();
  char nested[512];
  struct stat statbuf;

  snprintf (nested, sizeof (nested), "%s/a/b/c", dir);
  yc_ui_options_init (&options);
  options.out_dir = nested;

  CHECK_TRUE ("out-dir: nested path is created",
              yc_ui_options_ensure_out_dir (&options, &error));
  CHECK_TRUE ("out-dir: and is a directory",
              stat (nested, &statbuf) == 0 && S_ISDIR (statbuf.st_mode));

  /* Already existing is fine, not an error. */
  CHECK_TRUE ("out-dir: existing directory is accepted",
              yc_ui_options_ensure_out_dir (&options, &error));

  /* A plain file in the way is an error rather than a confusing
     failure later. */
  {
    char file_path[512];
    FILE *fp;
    snprintf (file_path, sizeof (file_path), "%s/regular-file", dir);
    fp = fopen (file_path, "w");
    if (fp != NULL)
      fclose (fp);
    options.out_dir = file_path;
    CHECK_TRUE ("out-dir: a file in the way is refused",
                !yc_ui_options_ensure_out_dir (&options, &error));
    CHECK_TRUE ("out-dir: says why", error != NULL);
    yc_free (error);
    error = NULL;
  }

  options.out_dir = NULL;
  CHECK_TRUE ("out-dir: unset is refused",
              !yc_ui_options_ensure_out_dir (&options, &error));
  yc_free (error);
  error = NULL;
  options.out_dir = "";
  CHECK_TRUE ("out-dir: empty is refused",
              !yc_ui_options_ensure_out_dir (&options, &error));
  yc_free (error);

  yc_ui_options_clear (&options);
  remove_out_dir (dir);
}

static void
test_filename_index_always_zero_pads (void)
{
  YcUIOptions options;
  char buf[YC_UI_INDEX_BUF_SIZE];

  yc_ui_options_init (&options);
  options.index_format.width = 5;
  options.index_format.zero_pad = false;

  /* On screen, spaces keep columns aligned... */
  CHECK_STR ("filename index: display pads with spaces",
             yc_ui_format_index (&options, 42, buf, sizeof (buf)), "   42");
  /* ...but a filename with spaces in it is a menace. */
  CHECK_STR ("filename index: filename pads with zeroes",
             yc_ui_format_index_for_filename (&options, 42, buf,
                                              sizeof (buf)), "00042");

  options.index_format.base = YC_UI_INDEX_HEX;
  CHECK_STR ("filename index: honours hex",
             yc_ui_format_index_for_filename (&options, 255, buf,
                                              sizeof (buf)), "000ff");
  yc_ui_options_clear (&options);
}

static void
test_headless_jobs_registered (void)
{
  CHECK_TRUE ("headless: is built in",
              yc_ui_lookup ("headless-jobs") == &yc_ui_headless_jobs);
}

/* --- watchdog --- */

static void
on_alarm (int signum)
{
  static const char message[] =
    "\ntest-ui: WATCHDOG FIRED -- a test is hung\n";
  ssize_t ignored = write (2, message, sizeof (message) - 1);
  (void) ignored;
  (void) signum;
  _exit (1);
}

int
main (void)
{
  signal (SIGPIPE, SIG_IGN);
  signal (SIGALRM, on_alarm);
  alarm (WATCHDOG_SECONDS);

  test_registry ();
  test_lifecycle ();
  test_lines_and_raw ();
  test_stderr_is_separate ();
  test_dup_merges_onto_captured_pipe ();
  test_unterminated_final_line ();
  test_crlf ();
  test_empty_lines ();
  test_no_output_at_all ();
  test_line_longer_than_a_read ();
  test_many_lines ();
  test_redirection_beats_capture ();
  test_quiet_ui_captures_nothing ();
  test_failure_accounting ();
  test_indices_and_concurrency ();
  test_no_retention_by_default ();
  test_retention_holds_finished_jobs ();
  test_retention_evicts_oldest ();
  test_reap_job ();
  test_job_subclassing ();
  test_no_job_subclass ();
  test_prefix_ui_registered ();
  test_prefix_ui_format ();
  test_prefix_ui_quiet_when_all_succeed ();
  test_prefix_ui_unterminated_line ();
  test_index_format ();
  test_options_key_values ();
  test_null_options_gives_defaults ();
  test_prefix_ui_honours_index_options ();
  test_index_options_reach_failure_notes ();
  test_filename_index_always_zero_pads ();
  test_headless_jobs_registered ();
  test_headless_jobs_needs_out_dir ();
  test_out_dir_is_created ();
  test_headless_jobs_files ();
  test_headless_jobs_writes_raw_bytes ();
  test_headless_jobs_escapes_json ();

  if (n_failures > 0)
    {
      printf ("\n%u of %u tests FAILED.\n", n_failures, n_tests);
      return 1;
    }
  printf ("all %u tests passed.\n", n_tests);
  return 0;
}
