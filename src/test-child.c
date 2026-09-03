/* SPDX-License-Identifier: 0BSD */
/* Tests for yc-child.c: these really do spawn processes.
 *
 * Jobs are described as shell command-lines and run through
 * yc_shell_parse(), so this exercises the two files together; the
 * per-job flags below then swap individual fds over to MODE_PIPE,
 * which is the part a command-line cannot express.
 *
 * A watchdog alarm bounds the whole run.  Several of these tests fail
 * by hanging rather than by producing a wrong answer -- the
 * close-on-exec test in particular -- so the alarm is what turns them
 * into a failure you can read.
 */

#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "yc-alloc.h"
#include "yc-common.h"
#include "yc-shell.h"

#define WATCHDOG_SECONDS   60
#define BIG_DATA_SIZE      (256 * 1024)

static unsigned n_tests, n_failures;
static char temp_dir[] = "/tmp/yc-test-child-XXXXXX";

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
check_uint (const char *what, unsigned long got, unsigned long expected,
            int line)
{
  n_tests++;
  if (got != expected)
    fail (line, "%s: expected %lu, got %lu", what, expected, got);
}

static void
check_true (const char *what, bool got, int line)
{
  n_tests++;
  if (!got)
    fail (line, "%s: expected true", what);
}

#define CHECK_STR(what, got, expected) check_str (what, got, expected, __LINE__)
#define CHECK_UINT(what, got, expected) \
  check_uint (what, (unsigned long) (got), (unsigned long) (expected), __LINE__)
#define CHECK_TRUE(what, got) check_true (what, got, __LINE__)

/* --- a growable byte buffer --- */

typedef struct {
  char *data;
  size_t len, alloced;
} Buf;

static void
buf_append (Buf *b, const char *data, size_t n)
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

/* --- jobs --- */

typedef struct Job {
  /* what to run */
  const char *cmdline;
  bool pipe_out, pipe_err;
  bool null_stdin;
  const char *stdin_data;         /* implies a pipe on fd 0 */
  size_t stdin_len;               /* 0 means strlen(stdin_data) */
  int extra_fd;                   /* >= 3: a pipe the child reads from */
  const char *extra_fd_data;
  bool kill_on_output;            /* SIGTERM as soon as it says anything */
  bool kill_others_when_done;
  bool expect_spawn_failure;

  /* what happened */
  Buf out, err;
  size_t stdin_written, extra_written;
  YcChildStatus status;
  int status_value;
  bool done_seen, spawned, spawn_failed, killed, io_after_done;
  YcChildCreateErrorCode spawn_error;
  unsigned n_closed;
  char log[64];                   /* 'i' io, 'c' closed, 'd' done */
  size_t log_len;
} Job;

typedef struct {
  Job *jobs;
  size_t n_jobs, next_job;
  size_t max_concurrent_seen;
  bool all_done_seen;
} Plan;

/* Runs of io events collapse, so the log stays short enough to assert
   an ordering on. */
static void
job_log (Job *job, char c)
{
  if (job->log_len > 0 && job->log[job->log_len - 1] == c && c == 'i')
    return;
  if (job->log_len + 1 < sizeof (job->log))
    {
      job->log[job->log_len++] = c;
      job->log[job->log_len] = 0;
    }
}

static void
job_io (YcChild *child, int child_fd, int pipe_fd)
{
  Job *job = child->user_data;
  YcChildFd *cfd = yc_child_get_fd (child, child_fd);

  if (job->done_seen)
    job->io_after_done = true;
  job_log (job, 'i');

  if ((cfd->revents & YC_CHILD_FD_EVENT_READABLE) != 0)
    {
      char buf[4096];
      ssize_t n = read (pipe_fd, buf, sizeof (buf));
      if (n > 0)
        {
          buf_append (child_fd == 2 ? &job->err : &job->out, buf, (size_t) n);
          if (job->kill_on_output && !job->killed)
            {
              job->killed = true;
              yc_child_kill (child, SIGTERM);
            }
        }
      else if (n == 0 || (errno != EAGAIN && errno != EINTR))
        yc_child_fd_close (child, child_fd);
      return;
    }

  if ((cfd->revents & YC_CHILD_FD_EVENT_WRITABLE) != 0)
    {
      const char *data;
      size_t total, *written;
      ssize_t n;

      if (child_fd == 0)
        {
          data = job->stdin_data;
          total = job->stdin_len;
          written = &job->stdin_written;
        }
      else
        {
          data = job->extra_fd_data;
          total = strlen (job->extra_fd_data);
          written = &job->extra_written;
        }

      if (*written < total)
        {
          n = write (pipe_fd, data + *written, total - *written);
          if (n > 0)
            *written += (size_t) n;
          else if (n < 0 && errno != EAGAIN && errno != EINTR)
            {
              yc_child_fd_close (child, child_fd);
              return;
            }
        }
      /* Closing is what gives the child its EOF. */
      if (*written == total)
        yc_child_fd_close (child, child_fd);
    }
}

static void
job_closed (YcChild *child, int child_fd, int pipe_fd)
{
  Job *job = child->user_data;
  (void) child_fd;
  (void) pipe_fd;
  job->n_closed++;
  job_log (job, 'c');
}

static void
job_done (YcChild *child)
{
  Job *job = child->user_data;

  job_log (job, 'd');
  job->done_seen = true;
  job->status = child->status;
  job->status_value = child->status_value;

  if (job->kill_others_when_done)
    {
      /* 'child' is still in the container at this point, so skip it. */
      YcChildContainer *container = child->container;
      size_t i;
      for (i = 0; i < container->n_children; i++)
        if (container->children[i] != child)
          yc_child_kill (container->children[i], SIGKILL);
    }
}

static YcChildCallbacks job_callbacks = { job_io, job_closed, job_done };

/* --- running a plan --- */

static void
set_pipe (YcChildCreateFdInfo *info, bool child_reads)
{
  info->mode = YC_CHILD_FD_MODE_PIPE;
  info->in = child_reads;
  info->out = !child_reads;
  info->filename = NULL;
}

static bool
spawn_job (YcChildContainer *container, Job *job)
{
  YcShellError shell_error;
  YcChildCreateError error;
  YcChildCreateInfo *ci;

  ci = yc_shell_parse (job->cmdline, YC_SHELL_FLAGS_FALLBACK_TO_SH_C,
                       &shell_error);
  if (ci == NULL)
    yc_die ("yc_shell_parse (\"%s\"): %s", job->cmdline, shell_error.message);

  if (job->stdin_data != NULL)
    {
      if (job->stdin_len == 0)
        job->stdin_len = strlen (job->stdin_data);
      set_pipe (&ci->fd_infos[0], true);
    }
  else if (job->null_stdin)
    {
      ci->fd_infos[0].mode = YC_CHILD_FD_MODE_NULL;
      ci->fd_infos[0].in = true;
      ci->fd_infos[0].out = false;
    }
  if (job->pipe_out)
    set_pipe (&ci->fd_infos[1], false);
  if (job->pipe_err)
    set_pipe (&ci->fd_infos[2], false);

  if (job->extra_fd >= 3)
    {
      ci->other_fd_infos = YC_NEW0_ARRAY (1, YcChildCreateOtherFdInfo);
      ci->n_other_fd_infos = 1;
      ci->other_fd_infos[0].fd = job->extra_fd;
      set_pipe (&ci->other_fd_infos[0].info, true);
    }

  ci->callbacks = &job_callbacks;
  ci->user_data = job;

  if (yc_child_new (container, ci, &error) == NULL)
    {
      job->spawn_failed = true;
      job->spawn_error = error.code;
      if (!job->expect_spawn_failure)
        fail (__LINE__, "yc_child_new (\"%s\"): %s",
              job->cmdline, error.message);
      yc_shell_free (ci);
      return false;
    }

  job->spawned = true;
  yc_shell_free (ci);
  return true;
}

/* Note the loop: a job that fails to spawn is not a reason to stall
   the queue, and the container only re-offers a slot when a child
   finishes -- so if nothing is running, declining here ends the run. */
static void
plan_ready_to_spawn (YcChildContainer *container)
{
  Plan *plan = container->container_data;

  while (plan->next_job < plan->n_jobs)
    {
      Job *job = &plan->jobs[plan->next_job++];
      if (spawn_job (container, job))
        {
          if (container->n_children > plan->max_concurrent_seen)
            plan->max_concurrent_seen = container->n_children;
          return;
        }
    }
}

static void
plan_all_done (YcChildContainer *container)
{
  Plan *plan = container->container_data;
  plan->all_done_seen = true;
}

static void
run_plan (Plan *plan, size_t max_children)
{
  YcChildContainerCreationInfo info;
  YcChildContainer *container;

  memset (&info, 0, sizeof (info));
  info.max_children = max_children;
  info.callbacks.ready_to_spawn = plan_ready_to_spawn;
  info.callbacks.all_done = plan_all_done;
  info.container_data = plan;

  container = yc_child_container_new (&info);
  yc_child_container_run (container);

  n_tests++;
  if (container->n_children != 0)
    fail (__LINE__, "run returned with %u children still live",
          (unsigned) container->n_children);

  yc_child_container_destroy (container);
}

/* Convenience for the single-job cases. */
static void
run_one (Job *job, size_t max_children)
{
  Plan plan;
  memset (&plan, 0, sizeof (plan));
  plan.jobs = job;
  plan.n_jobs = 1;
  run_plan (&plan, max_children);
}

static void
job_clear (Job *job)
{
  buf_clear (&job->out);
  buf_clear (&job->err);
}

static char *
temp_path (const char *name)
{
  static char path[256];
  snprintf (path, sizeof (path), "%s/%s", temp_dir, name);
  return path;
}

static char *
read_file (const char *path)
{
  static char buf[8192];
  FILE *fp = fopen (path, "r");
  size_t n;
  if (fp == NULL)
    return NULL;
  n = fread (buf, 1, sizeof (buf) - 1, fp);
  fclose (fp);
  buf[n] = 0;
  return buf;
}

/* --- the cases --- */

static void
test_exit_status (void)
{
  Job job;

  memset (&job, 0, sizeof (job));
  job.cmdline = "sh -c 'exit 0'";
  run_one (&job, 4);
  CHECK_TRUE ("exit 0: done", job.done_seen);
  CHECK_UINT ("exit 0: status", job.status, YC_CHILD_STATUS_EXITED);
  CHECK_UINT ("exit 0: value", job.status_value, 0);
  job_clear (&job);

  memset (&job, 0, sizeof (job));
  job.cmdline = "sh -c 'exit 7'";
  run_one (&job, 4);
  CHECK_UINT ("exit 7: status", job.status, YC_CHILD_STATUS_EXITED);
  CHECK_UINT ("exit 7: value", job.status_value, 7);
  job_clear (&job);
}

static void
test_spawn_failure (void)
{
  Job job;
  memset (&job, 0, sizeof (job));
  job.cmdline = "/nonexistent/yc-no-such-program";
  job.expect_spawn_failure = true;
  run_one (&job, 4);
  CHECK_TRUE ("missing program: reported", job.spawn_failed);
  CHECK_UINT ("missing program: error code", job.spawn_error,
              YC_CHILD_CREATE_ERROR_SPAWN_FAILED);
  CHECK_TRUE ("missing program: never done", !job.done_seen);
  job_clear (&job);
}

static void
test_stdout_and_stderr (void)
{
  Job job;

  memset (&job, 0, sizeof (job));
  job.cmdline = "echo hello world";
  job.pipe_out = true;
  run_one (&job, 4);
  CHECK_STR ("echo: stdout", job.out.data, "hello world\n");
  CHECK_UINT ("echo: status", job.status_value, 0);
  /* done() must come after every io and close on the child's pipes. */
  CHECK_STR ("echo: event order", job.log, "icd");
  CHECK_TRUE ("echo: no io after done", !job.io_after_done);
  job_clear (&job);

  /* Separate pipes stay separate. */
  memset (&job, 0, sizeof (job));
  job.cmdline = "sh -c 'echo to-out; echo to-err >&2'";
  job.pipe_out = true;
  job.pipe_err = true;
  run_one (&job, 4);
  CHECK_STR ("two pipes: stdout", job.out.data, "to-out\n");
  CHECK_STR ("two pipes: stderr", job.err.data, "to-err\n");
  CHECK_UINT ("two pipes: closes", job.n_closed, 2);
  job_clear (&job);
}

/* The point of MODE_DUP: fd 2 must reach the same pipe as fd 1, which
   is something re-opening a name could not do. */
static void
test_dup_redirection (void)
{
  Job job;

  memset (&job, 0, sizeof (job));
  job.cmdline = "sh -c 'echo first; echo second >&2' 2>&1";
  job.pipe_out = true;
  run_one (&job, 4);
  CHECK_TRUE ("2>&1: stdout captured",
              job.out.data != NULL
           && strstr (job.out.data, "first") != NULL);
  CHECK_TRUE ("2>&1: stderr came down the same pipe",
              job.out.data != NULL
           && strstr (job.out.data, "second") != NULL);
  CHECK_UINT ("2>&1: only one pipe closed", job.n_closed, 1);
  job_clear (&job);
}

static void
test_file_redirection (void)
{
  Job job;
  char cmdline[512];
  const char *path = temp_path ("redirect.txt");

  memset (&job, 0, sizeof (job));
  snprintf (cmdline, sizeof (cmdline), "sh -c 'echo written' > %s", path);
  job.cmdline = cmdline;
  run_one (&job, 4);
  CHECK_STR ("'>': file contents", read_file (path), "written\n");
  job_clear (&job);

  /* '>>' must add to it rather than truncate. */
  memset (&job, 0, sizeof (job));
  snprintf (cmdline, sizeof (cmdline), "sh -c 'echo appended' >> %s", path);
  job.cmdline = cmdline;
  run_one (&job, 4);
  CHECK_STR ("'>>': file contents", read_file (path),
             "written\nappended\n");
  job_clear (&job);

  /* '>' on the same name truncates. */
  memset (&job, 0, sizeof (job));
  snprintf (cmdline, sizeof (cmdline), "sh -c 'echo third' > %s", path);
  job.cmdline = cmdline;
  run_one (&job, 4);
  CHECK_STR ("'>' again: truncated", read_file (path), "third\n");
  job_clear (&job);

  /* '<' feeds the child. */
  memset (&job, 0, sizeof (job));
  snprintf (cmdline, sizeof (cmdline), "cat < %s", path);
  job.cmdline = cmdline;
  job.pipe_out = true;
  run_one (&job, 4);
  CHECK_STR ("'<': child read the file", job.out.data, "third\n");
  job_clear (&job);

  unlink (path);
}

static void
test_stdin_pipe (void)
{
  Job job;

  memset (&job, 0, sizeof (job));
  job.cmdline = "cat";
  job.stdin_data = "round trip\n";
  job.pipe_out = true;
  run_one (&job, 4);
  CHECK_STR ("cat: round trip", job.out.data, "round trip\n");
  CHECK_UINT ("cat: wrote all of stdin", job.stdin_written, 11);
  CHECK_UINT ("cat: status", job.status_value, 0);
  job_clear (&job);

  /* MODE_NULL gives the child an immediate EOF. */
  memset (&job, 0, sizeof (job));
  job.cmdline = "cat";
  job.null_stdin = true;
  job.pipe_out = true;
  run_one (&job, 4);
  CHECK_STR ("null stdin: no output", job.out.data == NULL ? "" : job.out.data,
             "");
  CHECK_UINT ("null stdin: status", job.status_value, 0);
  job_clear (&job);
}

/* Enough data to need many partial reads and writes, in both
   directions at once: if either poll direction were mishandled this
   would truncate or deadlock. */
static void
test_large_transfer (void)
{
  Job job;
  char *data = YC_NEW_ARRAY (BIG_DATA_SIZE + 1, char);
  size_t i;

  for (i = 0; i < BIG_DATA_SIZE; i++)
    data[i] = (char) ('a' + (i % 26));
  data[BIG_DATA_SIZE] = 0;

  memset (&job, 0, sizeof (job));
  job.cmdline = "cat";
  job.stdin_data = data;
  job.stdin_len = BIG_DATA_SIZE;
  job.pipe_out = true;
  run_one (&job, 4);

  CHECK_UINT ("big transfer: bytes written", job.stdin_written,
              BIG_DATA_SIZE);
  CHECK_UINT ("big transfer: bytes read back", job.out.len, BIG_DATA_SIZE);
  n_tests++;
  if (job.out.data == NULL
   || memcmp (job.out.data, data, BIG_DATA_SIZE) != 0)
    fail (__LINE__, "big transfer: contents differ");
  job_clear (&job);
  yc_free (data);
}

/* A large stdout with no stdin: all of it must arrive before done(). */
static void
test_large_output (void)
{
  Job job;
  size_t lines = 0, i;

  memset (&job, 0, sizeof (job));
  job.cmdline = "awk 'BEGIN{for(i=1;i<=20000;i++)print i}'";
  job.pipe_out = true;
  run_one (&job, 4);

  for (i = 0; i < job.out.len; i++)
    if (job.out.data[i] == '\n')
      lines++;
  CHECK_UINT ("large output: line count", lines, 20000);
  CHECK_TRUE ("large output: ends with the last line",
              job.out.len > 6
           && strcmp (job.out.data + job.out.len - 6, "20000\n") == 0);
  job_clear (&job);
}

static void
test_extra_fd (void)
{
  Job job;

  memset (&job, 0, sizeof (job));
  job.cmdline = "sh -c 'cat <&3'";
  job.extra_fd = 3;
  job.extra_fd_data = "via fd three\n";
  job.pipe_out = true;
  run_one (&job, 4);
  CHECK_STR ("fd 3: child read it", job.out.data, "via fd three\n");
  CHECK_UINT ("fd 3: all of it written", job.extra_written, 13);
  job_clear (&job);
}

static void
test_kill (void)
{
  Job job;

  memset (&job, 0, sizeof (job));
  job.cmdline = "sh -c 'echo started; sleep 30'";
  job.pipe_out = true;
  job.kill_on_output = true;
  run_one (&job, 4);
  CHECK_STR ("kill: got the output first", job.out.data, "started\n");
  CHECK_UINT ("kill: status", job.status, YC_CHILD_STATUS_KILLED);
  CHECK_UINT ("kill: signal", job.status_value, SIGTERM);
  job_clear (&job);
}

static void
test_concurrency (void)
{
  enum { N_JOBS = 12, MAX_CHILDREN = 3 };
  Job jobs[N_JOBS];
  Plan plan;
  size_t i;

  memset (jobs, 0, sizeof (jobs));
  for (i = 0; i < N_JOBS; i++)
    {
      jobs[i].cmdline = "sh -c 'echo tick'";
      jobs[i].pipe_out = true;
    }

  memset (&plan, 0, sizeof (plan));
  plan.jobs = jobs;
  plan.n_jobs = N_JOBS;
  run_plan (&plan, MAX_CHILDREN);

  CHECK_TRUE ("concurrency: all_done fired", plan.all_done_seen);
  CHECK_UINT ("concurrency: every job started", plan.next_job, N_JOBS);
  n_tests++;
  if (plan.max_concurrent_seen > MAX_CHILDREN)
    fail (__LINE__, "concurrency: ran %u at once, limit was %u",
          (unsigned) plan.max_concurrent_seen, MAX_CHILDREN);
  /* The slots should actually get used, not just respected. */
  CHECK_UINT ("concurrency: filled every slot", plan.max_concurrent_seen,
              MAX_CHILDREN);

  for (i = 0; i < N_JOBS; i++)
    {
      char what[64];
      snprintf (what, sizeof (what), "concurrency: job %u output",
                (unsigned) i);
      check_str (what, jobs[i].out.data, "tick\n", __LINE__);
      CHECK_TRUE ("concurrency: job done", jobs[i].done_seen);
      job_clear (&jobs[i]);
    }
}

/* Our end of a pipe must be close-on-exec.  If it is not, the write
   end of job 0's stdin leaks into job 1, 'cat' never sees EOF, and
   this test hangs until the watchdog fires. */
static void
test_pipes_do_not_leak_into_later_children (void)
{
  Job jobs[2];
  Plan plan;

  memset (jobs, 0, sizeof (jobs));
  jobs[0].cmdline = "cat";
  jobs[0].stdin_data = "no leak\n";
  jobs[0].pipe_out = true;
  jobs[0].kill_others_when_done = true;   /* release the sleeper */

  jobs[1].cmdline = "sh -c 'sleep 30'";

  memset (&plan, 0, sizeof (plan));
  plan.jobs = jobs;
  plan.n_jobs = 2;
  run_plan (&plan, 2);

  CHECK_STR ("cloexec: cat still saw EOF", jobs[0].out.data, "no leak\n");
  CHECK_UINT ("cloexec: cat exited cleanly", jobs[0].status,
              YC_CHILD_STATUS_EXITED);
  CHECK_UINT ("cloexec: cat status", jobs[0].status_value, 0);
  CHECK_TRUE ("cloexec: the sleeper was reaped too", jobs[1].done_seen);
  job_clear (&jobs[0]);
  job_clear (&jobs[1]);
}

static void
test_container_full (void)
{
  Job job;
  YcChildContainerCreationInfo info;
  YcChildContainer *container;
  YcChildCreateInfo *ci;
  YcChildCreateError error;
  Plan plan;

  memset (&job, 0, sizeof (job));
  job.cmdline = "sh -c 'exit 0'";
  memset (&plan, 0, sizeof (plan));
  plan.jobs = &job;
  plan.n_jobs = 1;

  memset (&info, 0, sizeof (info));
  info.max_children = 1;
  info.container_data = &plan;
  container = yc_child_container_new (&info);

  CHECK_TRUE ("full: first spawn works", spawn_job (container, &job));

  ci = yc_shell_parse ("sh -c 'exit 0'", 0, NULL);
  ci->callbacks = &job_callbacks;
  ci->user_data = &job;
  n_tests++;
  if (yc_child_new (container, ci, &error) != NULL)
    fail (__LINE__, "full: a second child was allowed past max_children");
  else
    CHECK_UINT ("full: error code", error.code,
                YC_CHILD_CREATE_ERROR_CONTAINER_FULL);
  yc_shell_free (ci);

  yc_child_container_run (container);
  yc_child_container_destroy (container);
  job_clear (&job);
}

/* destroy() with a child still running must kill it and return, not
   block forever on a 30-second sleep. */
static void
test_destroy_kills_survivors (void)
{
  Job job;
  YcChildContainerCreationInfo info;
  YcChildContainer *container;

  memset (&job, 0, sizeof (job));
  job.cmdline = "sh -c 'sleep 30'";

  memset (&info, 0, sizeof (info));
  info.max_children = 2;
  container = yc_child_container_new (&info);

  CHECK_TRUE ("destroy: spawned the sleeper", spawn_job (container, &job));
  CHECK_UINT ("destroy: one child live", container->n_children, 1);
  yc_child_container_destroy (container);
  /* Reaching here at all is the assertion. */
  CHECK_TRUE ("destroy: returned", true);
  job_clear (&job);
}

/* --- watchdog --- */

static void
on_alarm (int signum)
{
  static const char message[] =
    "\ntest-child: WATCHDOG FIRED -- a test is hung\n";
  ssize_t ignored = write (2, message, sizeof (message) - 1);
  (void) ignored;
  (void) signum;
  _exit (1);
}

int
main (void)
{
  /* Writing to a child that has already exited must give us EPIPE
     rather than killing us. */
  signal (SIGPIPE, SIG_IGN);
  signal (SIGALRM, on_alarm);
  alarm (WATCHDOG_SECONDS);

  if (mkdtemp (temp_dir) == NULL)
    yc_die ("mkdtemp: %s", strerror (errno));

  test_exit_status ();
  test_spawn_failure ();
  test_stdout_and_stderr ();
  test_dup_redirection ();
  test_file_redirection ();
  test_stdin_pipe ();
  test_large_transfer ();
  test_large_output ();
  test_extra_fd ();
  test_kill ();
  test_concurrency ();
  test_pipes_do_not_leak_into_later_children ();
  test_container_full ();
  test_destroy_kills_survivors ();

  rmdir (temp_dir);

  if (n_failures > 0)
    {
      printf ("\n%u of %u tests FAILED.\n", n_failures, n_tests);
      return 1;
    }
  printf ("all %u tests passed.\n", n_tests);
  return 0;
}
