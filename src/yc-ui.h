/* SPDX-License-Identifier: 0BSD */
/* A plug-in interface for presenting a parallel run.
 *
 * A UI is a vtable plus a chunk of instance state.  The framework owns
 * everything tedious: it assigns each job a stable index, forces the
 * child's stdout/stderr onto pipes when the UI actually consumes
 * output, does the reading, reassembles lines across read boundaries,
 * and keeps a table of the jobs currently running.
 *
 * Writing a UI means filling in a YcUIFuncs and calling
 * yc_ui_register().  The driver then does, per job:
 *
 *     create_info = yc_shell_parse (line, ...);
 *     yc_ui_spawn (ui, container, create_info, line, &error);
 *
 * and never touches a pipe itself.
 *
 * Two rules for a UI implementation:
 *   - Anything you hang off job->ui_data in job_started() must be
 *     released in job_ended(); the job is freed once that returns.
 *   - job_output() gets the child's bytes exactly as they arrived,
 *     with no decoding, so a UI that records a run byte-for-byte can.
 *     job_line() is the convenient view; they are independent and both
 *     fire if you set both.
 */

#ifndef YC_UI_H_
#define YC_UI_H_

#include <stdint.h>
#include "yc-child.h"

typedef struct YcUI YcUI;
typedef struct YcUIJob YcUIJob;
typedef struct YcUIFuncs YcUIFuncs;
typedef struct YcUIOptions YcUIOptions;

/* Wall-clock microseconds; what the binary event log records. */
uint64_t yc_now_micros (void);

/* --- options, mostly straight off the command-line --- */

typedef enum
{
  YC_UI_INDEX_DECIMAL,
  YC_UI_INDEX_HEX
} YcUIIndexBase;

/* How a job index is rendered.  This lives in the shared options
 * rather than in any one backend because every UI that shows an index
 * should show the same one: '--index-width=6 --index-zero-pad' should
 * mean the same thing whichever UI is selected.
 *
 * 'width' is a minimum, as in printf: narrower indices are padded to
 * it, wider ones are not truncated.
 */
typedef struct {
  YcUIIndexBase base;
  unsigned width;              /* 0 for however many digits it takes */
  bool zero_pad;               /* pad with '0' rather than ' ' */
} YcUIIndexFormat;

typedef enum
{
  YC_UI_COLOR_AUTO,            /* a terminal, and NO_COLOR unset */
  YC_UI_COLOR_ALWAYS,
  YC_UI_COLOR_NEVER
} YcUIColorWhen;

typedef struct {
  char *key, *value;
} YcUIOptionKV;

struct YcUIOptions {
  YcUIIndexFormat index_format;

  /* Whether to tint output by job.  Shared for the same reason as the
     index format: --colorize should mean one thing across backends. */
  YcUIColorWhen color;

  /* Where a UI that writes files should put them.  Shared, so that
     --out-dir means the same thing to every such backend.  NULL if it
     was not given; a UI that needs one should say so from init().
     Not owned: it points at the caller's string. */
  const char *out_dir;

  /* --ui-option=KEY=VALUE, in the order given: the escape hatch for
     whatever a single backend cares about and this struct does not. */
  size_t n_extra;
  YcUIOptionKV *extra;
};

/* Enough for any index at the widest padding allowed, plus the NUL. */
#define YC_UI_INDEX_MAX_WIDTH  64
#define YC_UI_INDEX_BUF_SIZE   72

/* Sets the defaults: decimal, no padding, no extras. */
void yc_ui_options_init  (YcUIOptions *options);
void yc_ui_options_clear (YcUIOptions *options);

void yc_ui_options_add   (YcUIOptions *options,
                          const char  *key,
                          const char  *value);

/* Splits "KEY=VALUE" at the first '=', so a value may itself contain
 * one.  false, with *error_message set (caller frees), if there is no
 * '=' or the key is empty. */
bool yc_ui_options_parse (YcUIOptions *options,
                          const char  *key_eq_value,
                          char       **error_message);

/* The last value given for 'key', or NULL: a repeated --ui-option
 * overrides the earlier one rather than being an error. */
const char *yc_ui_options_get (const YcUIOptions *options, const char *key);

/* Both terminate the program if the value is present but malformed --
 * it came from the command-line, so saying so immediately beats
 * quietly falling back to the default. */
bool     yc_ui_options_get_bool (const YcUIOptions *options,
                                 const char *key, bool default_value);
unsigned yc_ui_options_get_uint (const YcUIOptions *options,
                                 const char *key, unsigned default_value);

/* Renders 'index' per 'options' into 'buf', and returns buf. */
const char *yc_ui_format_index (const YcUIOptions *options,
                                uint64_t index,
                                char *buf, size_t buf_size);

/* The same, but padding is always with zeroes whatever the options
 * say: a filename wants to sort lexically, and space padding would put
 * spaces in it.  Base and width are honoured, so '--index-width=6'
 * gives '000042.stdout'. */
const char *yc_ui_format_index_for_filename (const YcUIOptions *options,
                                             uint64_t index,
                                             char *buf, size_t buf_size);

/* Creates options->out_dir, including any missing parents, so that
 * --out-dir=results/run-3 works.  false, with *error_message set
 * (caller frees), if no out_dir was given or it cannot be made. */
bool yc_ui_options_ensure_out_dir (const YcUIOptions *options,
                                   char **error_message);

/* Resolves YC_UI_COLOR_AUTO: a terminal on stdout, NO_COLOR unset,
 * TERM not "dumb".  yc_ui_new() calls this once and caches the answer
 * in ui->colorize. */
bool yc_ui_options_colorize (const YcUIOptions *options);

/* Parses auto/always/never, and the booleans yc_parse_boolean knows,
 * so --colorize=yes and --colorize=no also work.  NULL (the option
 * given with no argument) means ALWAYS. */
bool yc_ui_parse_color_when (const char *text, YcUIColorWhen *out);

struct YcUIJob {
  uint64_t index;                 /* 0-based, monotonic, stable */
  const char *cmdline;            /* owned by the job */
  int pid;

  bool running;
  YcChildStatus status;           /* meaningful once !running */
  int status_value;
  uint64_t started_micros, ended_micros;

  void *ui_data;                  /* yours; free it in job_ended() */

  /*< private >*/
  YcUI *ui;
  struct YcUILineBuf *pending;    /* partial lines, per captured fd */
};

typedef enum
{
  /* The UI drives the terminal itself (raw mode, its own uv_tty_t on
     stdin).  The framework will not write to stdout on its own
     behalf, and children never inherit the terminal. */
  YC_UI_INTERACTIVE = (1<<0)
} YcUIFlags;

struct YcUIFuncs {
  const char *name;               /* as given to --ui=NAME */
  const char *description;        /* one line, for --list-backends */
  const char *long_description;   /* one line, for --help-ui=UI */
  size_t instance_size;           /* >= sizeof(YcUI); 0 means exactly */
  YcUIFlags flags;

  /* Anything that can fail belongs here rather than in the vtable's
     first use.  Set *error_message (malloced) and return false. */
  bool (*init)        (YcUI *ui, char **error_message);

  void (*job_started) (YcUI *ui, YcUIJob *job);

  /* Raw bytes, exactly as read.  Setting either this or job_line() is
     what tells the framework to capture output at all. */
  void (*job_output)  (YcUI *ui, YcUIJob *job, int child_fd,
                       const void *data, size_t len, uint64_t micros);

  /* One line, with the trailing newline (and any CR before it)
     stripped.  A final line with no newline is still delivered, when
     the job ends. */
  void (*job_line)    (YcUI *ui, YcUIJob *job, int child_fd,
                       const char *line, size_t len, uint64_t micros);

  void (*job_ended)   (YcUI *ui, YcUIJob *job);
  void (*all_done)    (YcUI *ui);
  void (*destroy)     (YcUI *ui);
};

struct YcUI {
  const YcUIFuncs *funcs;
  uv_loop_t *loop;                /* for a UI that needs its own handles */

  /* Never NULL: yc_ui_new() substitutes the defaults.  Owned by the
     caller, who must keep it alive as long as the UI. */
  const YcUIOptions *options;

  /* options->color resolved once at construction, so a UI need not
     re-check isatty() for every line. */
  bool colorize;

  /* Jobs currently running, in the order they started -- stable, so a
     selection pane does not jump around.  Finished jobs are gone from
     here by the time job_ended() returns. */
  YcUIJob **jobs;
  size_t n_jobs;

  uint64_t n_started, n_ended, n_failed;

  void *user_data;

  /*< private >*/
  size_t jobs_alloced;
};

/* --- the registry --- */

void              yc_ui_register (const YcUIFuncs *funcs);

/* NULL if there is no such UI. */
const YcUIFuncs  *yc_ui_lookup   (const char *name);

/* Every registered UI, for building a --help listing. */
size_t            yc_ui_get_all  (const YcUIFuncs *const **funcs_out);

/* --- using a UI --- */

/* NULL on failure, with *error_message set (caller frees).  'options'
 * may be NULL for the defaults; otherwise it must outlive the UI. */
YcUI *yc_ui_new  (const YcUIFuncs   *funcs,
                  uv_loop_t         *loop,
                  const YcUIOptions *options,
                  char             **error_message);

void  yc_ui_free (YcUI *ui);

/* What a UI calls to render one of its own job's indices. */
const char *yc_ui_job_index_string (YcUI *ui, YcUIJob *job,
                                    char *buf, size_t buf_size);

/* An SGR escape identifying this job, or "" when colour is off -- so
 * printing it unconditionally is always safe.  The colour is chosen
 * from the job index rather than at random: it is then stable across
 * reruns, and consecutive jobs are guaranteed to differ, which random
 * choice cannot promise.
 *
 * The colour identifies the JOB, not the stream.  Use --ui=prefix if
 * you need to tell stdout from stderr. */
const char *yc_ui_job_color   (YcUI *ui, YcUIJob *job);
const char *yc_ui_color_reset (YcUI *ui);

/* Assigns the next job index, points stdout/stderr at pipes if this UI
 * consumes output, installs the callbacks that turn reads into UI
 * events, and spawns.  An fd the command-line redirected for itself is
 * left alone, so 'job > out.txt' still writes to the file and
 * 'job 2>&1' still merges onto one pipe.
 *
 * Returns NULL exactly when yc_child_new() does, with *error_out set;
 * no job is reported to the UI in that case.
 *
 * The UI layer owns fds 1 and 2 of the children it spawns.  fd 0 is
 * pointed at /dev/null unless the command-line redirected it, since
 * concurrent jobs sharing a terminal would only fight over it.
 */
YcChild *yc_ui_spawn (YcUI                *ui,
                      YcChildContainer    *container,
                      YcChildCreateInfo   *create_info,
                      const char          *cmdline,
                      YcChildCreateError  *error_out);

/* Call once the container's run has finished. */
void yc_ui_all_done (YcUI *ui);

#endif
