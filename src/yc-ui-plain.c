/* SPDX-License-Identifier: 0BSD */
/* The reference UI: the smallest thing that exercises the whole
 * interface.  It prints each line as it arrives and a summary at the
 * end, and is here to show the shape a real UI follows --
 *
 *   - subclassing YcUI by setting instance_size and casting,
 *   - per-job state on job->ui_data, allocated in job_started() and
 *     released in job_ended(),
 *   - consuming job_line(), which is what makes the framework capture
 *     stdout/stderr onto pipes in the first place.
 *
 * Note there is no locking or interleaving cleverness: lines arrive one
 * at a time from a single-threaded event loop, already reassembled.
 */

#include <stdio.h>

#include "yc-common.h"
#include "yc-alloc.h"
#include "yc-ui.h"

typedef struct {
  YcUI base;                    /* must come first */
  uint64_t n_lines;
} PlainUI;

typedef struct {
  uint64_t n_lines;
} PlainJob;

static void
plain_job_started (YcUI *ui, YcUIJob *job)
{
  job->ui_data = YC_NEW0 (PlainJob);
}

static void
plain_job_line (YcUI *ui, YcUIJob *job, int child_fd,
                const char *line, size_t len, uint64_t micros)
{
  PlainUI *plain = (PlainUI *) ui;
  PlainJob *pjob = job->ui_data;

  plain->n_lines++;
  pjob->n_lines++;

  /* Both are "" unless colour is on.  With no tag to carry it, the
     colour is the only thing saying which job a line came from. */
  fputs (yc_ui_job_color (ui, job), stdout);
  /* fwrite, not %s: a child may legitimately write a NUL byte, and
     printf would end the line there. */
  fwrite (line, 1, len, stdout);
  fputs (yc_ui_color_reset (ui), stdout);
  putchar ('\n');
}

static void
plain_job_ended (YcUI *ui, YcUIJob *job)
{
  PlainJob *pjob = job->ui_data;
  char index[YC_UI_INDEX_BUF_SIZE];

  /* stdout is block-buffered when it is not a terminal, but stderr
     never is, so without this the complaint below overtakes the output
     it is about. */
  if (job->status != YC_CHILD_STATUS_EXITED || job->status_value != 0)
    fflush (stdout);

  yc_ui_job_index_string (ui, job, index, sizeof (index));

  if (job->status == YC_CHILD_STATUS_KILLED)
    fprintf (stderr, "%s[%s] %s: killed by signal %d%s\n",
             yc_ui_job_color (ui, job), index, job->cmdline,
             job->status_value, yc_ui_color_reset (ui));
  else if (job->status_value != 0)
    fprintf (stderr, "%s[%s] %s: exited with status %d%s\n",
             yc_ui_job_color (ui, job), index, job->cmdline,
             job->status_value, yc_ui_color_reset (ui));

  yc_free (pjob);
  job->ui_data = NULL;
}

static void
plain_all_done (YcUI *ui)
{
  PlainUI *plain = (PlainUI *) ui;

  fflush (stdout);
  if (ui->n_failed > 0)
    fprintf (stderr, "%llu of %llu jobs failed (%llu lines).\n",
             (unsigned long long) ui->n_failed,
             (unsigned long long) ui->n_started,
             (unsigned long long) plain->n_lines);
}

const YcUIFuncs yc_ui_plain = {
  "plain",
  "merge every job's output into one stream, a line at a time, "
  "never splitting a line",
  sizeof (PlainUI),
  0,                            /* flags */
  NULL,                         /* init */
  plain_job_started,
  NULL,                         /* job_output: lines are enough */
  plain_job_line,
  plain_job_ended,
  plain_all_done,
  NULL                          /* destroy */
};
