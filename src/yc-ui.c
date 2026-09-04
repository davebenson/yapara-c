/* SPDX-License-Identifier: 0BSD */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include "yc-common.h"
#include "yc-alloc.h"
#include "yc-ui.h"

/* stdout and stderr; the only fds the UI layer captures. */
#define N_CAPTURED_FDS   2
#define FIRST_CAPTURED_FD 1
#define READ_CHUNK       8192

typedef struct YcUILineBuf {
  char *data;
  size_t len, alloced;
} YcUILineBuf;

static void ended_jobs_trim (YcUI *ui);

uint64_t
yc_now_micros (void)
{
  struct timeval tv;
  gettimeofday (&tv, NULL);
  return (uint64_t) tv.tv_sec * 1000000 + (uint64_t) tv.tv_usec;
}

/* --- options --- */

static const YcUIOptions default_options = {
  { YC_UI_INDEX_DECIMAL, 0, false },
  YC_UI_COLOR_AUTO,              /* color */
  NULL,                          /* out_dir */
  0, NULL                        /* extra */
};

void
yc_ui_options_init (YcUIOptions *options)
{
  *options = default_options;
}

void
yc_ui_options_clear (YcUIOptions *options)
{
  size_t i;
  for (i = 0; i < options->n_extra; i++)
    {
      yc_free (options->extra[i].key);
      yc_free (options->extra[i].value);
    }
  yc_free (options->extra);
  options->extra = NULL;
  options->n_extra = 0;
}

void
yc_ui_options_add (YcUIOptions *options, const char *key, const char *value)
{
  /* Appends rather than replacing, so the order the user gave is
     recoverable; yc_ui_options_get() reads backward. */
  options->extra = YC_RENEW (YcUIOptionKV, options->extra,
                             options->n_extra + 1);
  options->extra[options->n_extra].key = yc_strdup (key);
  options->extra[options->n_extra].value = yc_strdup (value);
  options->n_extra++;
}

bool
yc_ui_options_parse (YcUIOptions *options,
                     const char *key_eq_value,
                     char **error_message)
{
  /* First '=' only: a value is allowed to contain one. */
  const char *eq = strchr (key_eq_value, '=');
  char *key;

  if (eq == NULL)
    {
      if (error_message != NULL)
        {
          *error_message = yc_malloc (128);
          snprintf (*error_message, 128,
                    "expected KEY=VALUE, got '%s'", key_eq_value);
        }
      return false;
    }
  if (eq == key_eq_value)
    {
      if (error_message != NULL)
        *error_message = yc_strdup ("KEY=VALUE with an empty key");
      return false;
    }

  key = yc_strndup ((size_t) (eq - key_eq_value), key_eq_value);
  yc_ui_options_add (options, key, eq + 1);
  yc_free (key);
  return true;
}

const char *
yc_ui_options_get (const YcUIOptions *options, const char *key)
{
  size_t i;
  /* Backward, so that a repeated --ui-option wins. */
  for (i = options->n_extra; i > 0; i--)
    if (strcmp (options->extra[i - 1].key, key) == 0)
      return options->extra[i - 1].value;
  return NULL;
}

bool
yc_ui_options_get_bool (const YcUIOptions *options,
                        const char *key,
                        bool default_value)
{
  const char *value = yc_ui_options_get (options, key);
  bool rv;
  if (value == NULL)
    return default_value;
  if (!yc_parse_boolean (value, &rv))
    yc_die ("--ui-option=%s: expected a boolean, got '%s'", key, value);
  return rv;
}

unsigned
yc_ui_options_get_uint (const YcUIOptions *options,
                        const char *key,
                        unsigned default_value)
{
  const char *value = yc_ui_options_get (options, key);
  char *end;
  unsigned long rv;

  if (value == NULL)
    return default_value;
  rv = strtoul (value, &end, 10);
  if (*value == 0 || *end != 0 || rv > 0xffffffffUL)
    yc_die ("--ui-option=%s: expected a number, got '%s'", key, value);
  return (unsigned) rv;
}

/* Built by hand rather than by assembling a printf format at runtime:
   the padding rules are simple enough, and a non-literal format is the
   sort of thing that trips warnings and analyzers later. */
static const char *
format_index (const YcUIIndexFormat *format,
              uint64_t index,
              char *buf,
              size_t buf_size)
{
  static const char digit_chars[] = "0123456789abcdef";
  unsigned radix = format->base == YC_UI_INDEX_HEX ? 16 : 10;
  char digits[YC_UI_INDEX_BUF_SIZE];
  size_t n_digits = 0, width, n_pad, out = 0;

  if (buf_size == 0)
    return buf;

  do
    {
      digits[n_digits++] = digit_chars[index % radix];
      index /= radix;
    }
  while (index != 0);

  width = format->width > YC_UI_INDEX_MAX_WIDTH
        ? YC_UI_INDEX_MAX_WIDTH
        : format->width;
  n_pad = width > n_digits ? width - n_digits : 0;

  while (n_pad-- > 0 && out + 1 < buf_size)
    buf[out++] = format->zero_pad ? '0' : ' ';
  while (n_digits-- > 0 && out + 1 < buf_size)
    buf[out++] = digits[n_digits];
  buf[out] = 0;

  return buf;
}

const char *
yc_ui_format_index (const YcUIOptions *options,
                    uint64_t index,
                    char *buf,
                    size_t buf_size)
{
  return format_index (&options->index_format, index, buf, buf_size);
}

const char *
yc_ui_format_index_for_filename (const YcUIOptions *options,
                                 uint64_t index,
                                 char *buf,
                                 size_t buf_size)
{
  /* Width and base as asked, but never space padding: '     0.stdout'
     is a legal filename and a thoroughly unpleasant one. */
  YcUIIndexFormat format = options->index_format;
  format.zero_pad = true;
  return format_index (&format, index, buf, buf_size);
}

/* Each component in turn, so --out-dir=results/run-3 works without the
   parent already existing. */
static bool
make_directories (const char *path, char **error_message)
{
  char *copy = yc_strdup (path);
  struct stat statbuf;
  char *p;
  bool ok = true;
  int failed_errno = 0;

  for (p = copy + 1; *p != 0 && ok; p++)
    if (*p == '/')
      {
        *p = 0;
        if (mkdir (copy, 0777) < 0 && errno != EEXIST)
          {
            failed_errno = errno;
            ok = false;
          }
        *p = '/';
      }
  if (ok && mkdir (copy, 0777) < 0 && errno != EEXIST)
    {
      failed_errno = errno;
      ok = false;
    }

  /* EEXIST is only good news if what exists is a directory. */
  if (ok && (stat (copy, &statbuf) < 0 || !S_ISDIR (statbuf.st_mode)))
    {
      if (error_message != NULL)
        {
          *error_message = yc_malloc (256);
          snprintf (*error_message, 256, "%s is not a directory", path);
        }
      yc_free (copy);
      return false;
    }

  if (!ok && error_message != NULL)
    {
      *error_message = yc_malloc (256);
      snprintf (*error_message, 256, "creating %s: %s",
                path, strerror (failed_errno));
    }
  yc_free (copy);
  return ok;
}

/* --- colour --- */

/* Deliberately excludes black, white and bright-black: each of those
   is invisible against one common terminal background or the other.
   Indexing by job number rather than choosing at random means
   consecutive jobs never collide, and a rerun looks the same. */
static const char *const job_colors[] = {
  "\033[31m", "\033[32m", "\033[33m", "\033[34m", "\033[35m", "\033[36m",
  "\033[91m", "\033[92m", "\033[93m", "\033[94m", "\033[95m", "\033[96m"
};
#define N_JOB_COLORS  (sizeof (job_colors) / sizeof (job_colors[0]))
#define COLOR_RESET   "\033[0m"

bool
yc_ui_parse_color_when (const char *text, YcUIColorWhen *out)
{
  bool as_boolean;

  /* '--colorize' with no argument. */
  if (text == NULL)
    {
      *out = YC_UI_COLOR_ALWAYS;
      return true;
    }
  if (strcmp (text, "auto") == 0)
    {
      *out = YC_UI_COLOR_AUTO;
      return true;
    }
  if (strcmp (text, "always") == 0)
    {
      *out = YC_UI_COLOR_ALWAYS;
      return true;
    }
  if (strcmp (text, "never") == 0)
    {
      *out = YC_UI_COLOR_NEVER;
      return true;
    }
  /* Anything yc_parse_boolean understands, so --colorize=no works. */
  if (yc_parse_boolean (text, &as_boolean))
    {
      *out = as_boolean ? YC_UI_COLOR_ALWAYS : YC_UI_COLOR_NEVER;
      return true;
    }
  return false;
}

bool
yc_ui_options_colorize (const YcUIOptions *options)
{
  const char *no_color, *term;

  if (options->color == YC_UI_COLOR_ALWAYS)
    return true;
  if (options->color == YC_UI_COLOR_NEVER)
    return false;

  /* AUTO.  Escapes in a file or a pipe are noise, and NO_COLOR is the
     agreed way to say "never" once and for all (no-color.org: set and
     non-empty). */
  no_color = getenv ("NO_COLOR");
  if (no_color != NULL && no_color[0] != 0)
    return false;
  term = getenv ("TERM");
  if (term != NULL && strcmp (term, "dumb") == 0)
    return false;
  return isatty (STDOUT_FILENO) != 0;
}

const char *
yc_ui_job_color (YcUI *ui, YcUIJob *job)
{
  if (!ui->colorize)
    return "";
  return job_colors[job->index % N_JOB_COLORS];
}

const char *
yc_ui_color_reset (YcUI *ui)
{
  return ui->colorize ? COLOR_RESET : "";
}

bool
yc_ui_options_ensure_out_dir (const YcUIOptions *options,
                              char **error_message)
{
  if (options->out_dir == NULL)
    {
      if (error_message != NULL)
        *error_message = yc_strdup ("no --out-dir was given");
      return false;
    }
  if (options->out_dir[0] == 0)
    {
      if (error_message != NULL)
        *error_message = yc_strdup ("--out-dir is empty");
      return false;
    }
  return make_directories (options->out_dir, error_message);
}

/* --- the registry --- */

static const YcUIFuncs **registry;
static size_t n_registered, registry_alloced;

extern const YcUIFuncs yc_ui_plain;
extern const YcUIFuncs yc_ui_prefix;
extern const YcUIFuncs yc_ui_headless_jobs;
extern const YcUIFuncs yc_ui_passthrough;
extern const YcUIFuncs yc_ui_two_pane;

static void
register_builtins_once (void)
{
  static bool done = false;
  if (done)
    return;
  done = true;
  yc_ui_register (&yc_ui_plain);
  yc_ui_register (&yc_ui_prefix);
  yc_ui_register (&yc_ui_headless_jobs);
  yc_ui_register (&yc_ui_passthrough);
  yc_ui_register (&yc_ui_two_pane);
}

void
yc_ui_register (const YcUIFuncs *funcs)
{
  size_t i;

  if (funcs->name == NULL)
    yc_die ("yc_ui_register: a UI needs a name");
  if (funcs->instance_size != 0 && funcs->instance_size < sizeof (YcUI))
    yc_die ("yc_ui_register: %s: instance_size %u is smaller than YcUI",
            funcs->name, (unsigned) funcs->instance_size);
  if (funcs->job_instance_size != 0
   && funcs->job_instance_size < sizeof (YcUIJob))
    yc_die ("yc_ui_register: %s: job_instance_size %u is smaller than "
            "YcUIJob", funcs->name, (unsigned) funcs->job_instance_size);

  for (i = 0; i < n_registered; i++)
    if (strcmp (registry[i]->name, funcs->name) == 0)
      yc_die ("yc_ui_register: two UIs named '%s'", funcs->name);

  if (n_registered == registry_alloced)
    {
      registry_alloced = registry_alloced == 0 ? 8 : registry_alloced * 2;
      registry = YC_RENEW (const YcUIFuncs *, registry, registry_alloced);
    }
  registry[n_registered++] = funcs;
}

const YcUIFuncs *
yc_ui_lookup (const char *name)
{
  size_t i;
  register_builtins_once ();
  for (i = 0; i < n_registered; i++)
    if (strcmp (registry[i]->name, name) == 0)
      return registry[i];
  return NULL;
}

size_t
yc_ui_get_all (const YcUIFuncs *const **funcs_out)
{
  register_builtins_once ();
  *funcs_out = registry;
  return n_registered;
}

/* --- lifetime --- */

/* Consuming output is what makes capturing it worthwhile; deriving it
   this way means a UI cannot ask for pipes and then ignore them, or
   forget to ask and silently see nothing. */
static bool
ui_captures_output (const YcUIFuncs *funcs)
{
  return funcs->job_output != NULL || funcs->job_line != NULL;
}

YcUI *
yc_ui_new (const YcUIFuncs *funcs,
           uv_loop_t *loop,
           const YcUIOptions *options,
           char **error_message)
{
  size_t size = funcs->instance_size != 0
              ? funcs->instance_size
              : sizeof (YcUI);
  YcUI *ui = yc_malloc0 (size);

  ui->funcs = funcs;
  ui->loop = loop;
  ui->options = options != NULL ? options : &default_options;
  ui->colorize = yc_ui_options_colorize (ui->options);

  if (funcs->init != NULL && !funcs->init (ui, error_message))
    {
      yc_free (ui);
      return NULL;
    }
  return ui;
}

void
yc_ui_free (YcUI *ui)
{
  if (ui == NULL)
    return;
  /* Retained jobs go before the UI's own destroy(), so job_destroyed()
     still runs while whatever it points into is alive. */
  ui->max_ended_jobs = 0;
  ended_jobs_trim (ui);

  if (ui->funcs->destroy != NULL)
    ui->funcs->destroy (ui);
  yc_free (ui->ended_jobs);
  yc_free (ui->jobs);
  yc_free (ui);
}

const char *
yc_ui_job_index_string (YcUI *ui, YcUIJob *job, char *buf, size_t buf_size)
{
  return yc_ui_format_index (ui->options, job->index, buf, buf_size);
}

void
yc_ui_all_done (YcUI *ui)
{
  if (ui->funcs->all_done != NULL)
    ui->funcs->all_done (ui);
}

/* --- the job table --- */

static void
jobs_append (YcUI *ui, YcUIJob *job)
{
  if (ui->n_jobs == ui->jobs_alloced)
    {
      ui->jobs_alloced = ui->jobs_alloced == 0 ? 8 : ui->jobs_alloced * 2;
      ui->jobs = YC_RENEW (YcUIJob *, ui->jobs, ui->jobs_alloced);
    }
  ui->jobs[ui->n_jobs++] = job;
}

/* Ordered removal: the table is small (bounded by max_children) and a
   selection pane that reorders itself under the user is worse than a
   memmove. */
static void
jobs_remove (YcUI *ui, YcUIJob *job)
{
  size_t i;
  for (i = 0; i < ui->n_jobs; i++)
    if (ui->jobs[i] == job)
      {
        memmove (ui->jobs + i, ui->jobs + i + 1,
                 sizeof (YcUIJob *) * (ui->n_jobs - i - 1));
        ui->n_jobs--;
        return;
      }
}

static void
job_free (YcUIJob *job)
{
  size_t i;
  if (job->pending != NULL)
    for (i = 0; i < N_CAPTURED_FDS; i++)
      yc_free (job->pending[i].data);
  yc_free (job->pending);
  yc_free ((char *) job->cmdline);
  yc_free (job);
}

/* The one place a job's memory goes away, so job_destroyed() cannot be
   missed on any of the paths that get here. */
static void
job_destroy (YcUI *ui, YcUIJob *job)
{
  if (ui->funcs->job_destroyed != NULL)
    ui->funcs->job_destroyed (ui, job);
  job_free (job);
}

/* Appended as they finish, so the array is already in end-time order
   and the oldest to evict is always at the front. */
static void
ended_jobs_append (YcUI *ui, YcUIJob *job)
{
  if (ui->n_ended_jobs == ui->ended_jobs_alloced)
    {
      ui->ended_jobs_alloced = ui->ended_jobs_alloced == 0
                             ? 8 : ui->ended_jobs_alloced * 2;
      ui->ended_jobs = YC_RENEW (YcUIJob *, ui->ended_jobs,
                                 ui->ended_jobs_alloced);
    }
  ui->ended_jobs[ui->n_ended_jobs++] = job;
}

static bool
ended_jobs_remove (YcUI *ui, YcUIJob *job)
{
  size_t i;
  for (i = 0; i < ui->n_ended_jobs; i++)
    if (ui->ended_jobs[i] == job)
      {
        memmove (ui->ended_jobs + i, ui->ended_jobs + i + 1,
                 sizeof (YcUIJob *) * (ui->n_ended_jobs - i - 1));
        ui->n_ended_jobs--;
        return true;
      }
  return false;
}

/* max_ended_jobs is public and a UI may lower it mid-run, so this
   loops rather than dropping a single job. */
static void
ended_jobs_trim (YcUI *ui)
{
  while (ui->n_ended_jobs > ui->max_ended_jobs)
    {
      YcUIJob *oldest = ui->ended_jobs[0];
      ended_jobs_remove (ui, oldest);
      job_destroy (ui, oldest);
    }
}

void
yc_ui_reap_job (YcUI *ui, YcUIJob *job)
{
  if (!ended_jobs_remove (ui, job))
    yc_die ("yc_ui_reap_job: job %llu is not a retained finished job",
            (unsigned long long) job->index);
  job_destroy (ui, job);
}

/* --- line reassembly --- */

static YcUILineBuf *
job_line_buf (YcUIJob *job, int child_fd)
{
  int slot = child_fd - FIRST_CAPTURED_FD;
  if (slot < 0 || slot >= N_CAPTURED_FDS)
    return NULL;
  if (job->pending == NULL)
    job->pending = YC_NEW0_ARRAY (N_CAPTURED_FDS, YcUILineBuf);
  return &job->pending[slot];
}

static void
line_buf_append (YcUILineBuf *buf, const char *data, size_t len)
{
  if (buf->len + len + 1 > buf->alloced)
    {
      buf->alloced = buf->alloced == 0 ? 256 : buf->alloced;
      while (buf->len + len + 1 > buf->alloced)
        buf->alloced *= 2;
      buf->data = YC_RENEW (char, buf->data, buf->alloced);
    }
  memcpy (buf->data + buf->len, data, len);
  buf->len += len;
  buf->data[buf->len] = 0;
}

/* Strips the CR of a CRLF too, so a UI never has to think about it. */
static void
emit_line (YcUI *ui, YcUIJob *job, int child_fd,
           char *line, size_t len, uint64_t micros)
{
  if (len > 0 && line[len - 1] == '\r')
    len--;
  line[len] = 0;
  ui->funcs->job_line (ui, job, child_fd, line, len, micros);
}

/* Everything goes through the pending buffer, including chunks that
   need no stitching.  Special-casing that would cost a malloc per line
   to get a writable, NUL-terminatable copy -- more than the one memcpy
   it saves. */
static void
deliver_lines (YcUI *ui, YcUIJob *job, int child_fd,
               const char *data, size_t len, uint64_t micros)
{
  YcUILineBuf *buf = job_line_buf (job, child_fd);
  size_t start = 0, i;

  if (buf == NULL)
    return;

  line_buf_append (buf, data, len);

  for (i = 0; i < buf->len; i++)
    if (buf->data[i] == '\n')
      {
        /* emit_line() writes a NUL over the newline; harmless, since
           the scan never looks back before 'start'. */
        emit_line (ui, job, child_fd, buf->data + start, i - start, micros);
        start = i + 1;
      }

  if (start > 0)
    {
      memmove (buf->data, buf->data + start, buf->len - start);
      buf->len -= start;
      buf->data[buf->len] = 0;
    }
}

/* A child that exits without a final newline still wrote that text. */
static void
flush_pending_lines (YcUI *ui, YcUIJob *job, uint64_t micros)
{
  size_t i;
  if (job->pending == NULL || ui->funcs->job_line == NULL)
    return;
  for (i = 0; i < N_CAPTURED_FDS; i++)
    {
      YcUILineBuf *buf = &job->pending[i];
      if (buf->len > 0)
        {
          size_t len = buf->len;
          buf->len = 0;
          emit_line (ui, job, (int) i + FIRST_CAPTURED_FD,
                     buf->data, len, micros);
        }
    }
}

/* --- turning child reads into UI events --- */

static void
ui_child_io (YcChild *child, int child_fd, int pipe_fd)
{
  YcUIJob *job = child->user_data;
  YcUI *ui = job->ui;
  YcChildFd *cfd = yc_child_get_fd (child, child_fd);
  char buf[READ_CHUNK];
  ssize_t n_read;
  uint64_t micros;

  if ((cfd->revents & YC_CHILD_FD_EVENT_READABLE) == 0)
    {
      /* We only ever asked to read.  A writable pipe here means the
         driver set one up behind our back; leaving it watched would
         spin the loop, so drop it. */
      yc_child_fd_watch (child, child_fd, 0);
      return;
    }

  n_read = read (pipe_fd, buf, sizeof (buf));
  if (n_read > 0)
    {
      micros = yc_now_micros ();
      if (ui->funcs->job_output != NULL)
        ui->funcs->job_output (ui, job, child_fd, buf, (size_t) n_read,
                              micros);
      if (ui->funcs->job_line != NULL)
        deliver_lines (ui, job, child_fd, buf, (size_t) n_read, micros);
      return;
    }

  /* read() returning 0 is the real end of the stream -- not the
     hangup, which can arrive with data still buffered. */
  if (n_read == 0 || (errno != EAGAIN && errno != EINTR))
    yc_child_fd_close (child, child_fd);
}

static void
ui_child_done (YcChild *child)
{
  YcUIJob *job = child->user_data;
  YcUI *ui = job->ui;

  job->ended_micros = yc_now_micros ();
  flush_pending_lines (ui, job, job->ended_micros);

  job->running = false;
  job->status = child->status;
  job->status_value = child->status_value;

  ui->n_ended++;
  if (job->status != YC_CHILD_STATUS_EXITED || job->status_value != 0)
    ui->n_failed++;

  /* Move it across before the callback, so a UI that redraws from
     either array during job_ended() sees one consistent world: gone
     from the running list, already in the finished one. */
  jobs_remove (ui, job);
  if (ui->max_ended_jobs > 0)
    {
      /* Append first, then trim: trimming to the cap beforehand would
         still leave room for this one to push the total to max + 1.
         The new job is at the back, so it is never what gets evicted
         (max is at least 1 here). */
      ended_jobs_append (ui, job);
      ended_jobs_trim (ui);
    }

  if (ui->funcs->job_ended != NULL)
    ui->funcs->job_ended (ui, job);

  /* Nowhere to retain it, so this is also the end of its life. */
  if (ui->max_ended_jobs == 0)
    job_destroy (ui, job);
}

static YcChildCallbacks ui_child_callbacks = {
  ui_child_io,
  NULL,                      /* 'closed' adds nothing over 'done' here */
  ui_child_done
};

/* --- spawning --- */

YcChild *
yc_ui_spawn (YcUI *ui,
             YcChildContainer *container,
             YcChildCreateInfo *create_info,
             const char *cmdline,
             YcChildCreateError *error_out)
{
  YcUIJob *job;
  YcChild *child;
  int fd;

  if (ui_captures_output (ui->funcs))
    for (fd = FIRST_CAPTURED_FD; fd < FIRST_CAPTURED_FD + N_CAPTURED_FDS; fd++)
      {
        /* INHERIT is the shell parser's default, i.e. "not redirected".
           Anything else the command-line asked for on purpose: leave
           '> out.txt' writing to the file, and leave the MODE_DUP of
           '2>&1' pointing at fd 1 -- which, once fd 1 becomes a pipe,
           merges both streams onto it exactly as a shell would. */
        if (create_info->fd_infos[fd].mode == YC_CHILD_FD_MODE_INHERIT)
          {
            create_info->fd_infos[fd].mode = YC_CHILD_FD_MODE_PIPE;
            create_info->fd_infos[fd].in = false;
            create_info->fd_infos[fd].out = true;
          }
      }

  if (create_info->fd_infos[0].mode == YC_CHILD_FD_MODE_INHERIT)
    create_info->fd_infos[0].mode = YC_CHILD_FD_MODE_NULL;

  /* The UI may have asked for a bigger job than YcUIJob, to keep its
     own per-job state inline.  Zeroed, so a subclass starts blank. */
  job = yc_malloc0 (ui->funcs->job_instance_size != 0
                    ? ui->funcs->job_instance_size
                    : sizeof (YcUIJob));
  job->ui = ui;
  job->index = ui->n_started;
  job->cmdline = yc_strdup (cmdline);
  job->running = true;
  job->started_micros = yc_now_micros ();

  create_info->callbacks = &ui_child_callbacks;
  create_info->user_data = job;

  child = yc_child_new (container, create_info, error_out);
  if (child == NULL)
    {
      job_free (job);
      return NULL;
    }

  job->pid = child->pid;
  ui->n_started++;
  jobs_append (ui, job);

  if (ui->funcs->job_started != NULL)
    ui->funcs->job_started (ui, job);

  return child;
}
