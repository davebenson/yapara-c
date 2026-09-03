/* SPDX-License-Identifier: 0BSD */
/* The 'passthrough' UI: get out of the way.
 *
 * It sets neither job_output nor job_line, and that is the whole
 * trick: the framework only puts pipes on a child when some UI is
 * going to read them, so here the children inherit our stdout and
 * stderr and write straight to them.  Nothing is copied through this
 * process at all.
 *
 * That is worth more than it sounds.  Putting a pipe on a child's
 * stdout makes its libc switch from line buffering to 4kB block
 * buffering, so a chatty job stops emitting whole lines and starts
 * emitting late blocks that splice into other jobs' output mid-line.
 * Leaving the terminal in place keeps every child line-buffered, which
 * is why concurrent output here stays about as readable as 'cmd &' in
 * a shell -- and why this cannot be had by capturing raw bytes and
 * writing them back out.
 *
 * The cost is that there is no per-job attribution: nothing here knows
 * which job wrote what, so there is nothing to tag or colour.  Use
 * --ui=prefix for that, or --ui=headless-jobs to get each job's bytes
 * separately and exactly.
 *
 * Job stdin still goes to /dev/null, as with every UI-managed job,
 * unless the command-line redirected it.
 */

#include <stdio.h>

#include "yc-common.h"
#include "yc-ui.h"

static void
passthrough_job_ended (YcUI *ui, YcUIJob *job)
{
  char index[YC_UI_INDEX_BUF_SIZE];

  if (job->status == YC_CHILD_STATUS_EXITED && job->status_value == 0)
    return;

  yc_ui_job_index_string (ui, job, index, sizeof (index));

  /* The child owns stdout, so a note about it belongs on stderr --
     and there is no buffered stdout of ours to flush first. */
  if (job->status == YC_CHILD_STATUS_KILLED)
    fprintf (stderr, "%s[%s] %s: killed by signal %d%s\n",
             yc_ui_job_color (ui, job), index, job->cmdline,
             job->status_value, yc_ui_color_reset (ui));
  else
    fprintf (stderr, "%s[%s] %s: exited with status %d%s\n",
             yc_ui_job_color (ui, job), index, job->cmdline,
             job->status_value, yc_ui_color_reset (ui));
}

static void
passthrough_all_done (YcUI *ui)
{
  if (ui->n_failed > 0)
    fprintf (stderr, "%llu of %llu jobs failed.\n",
             (unsigned long long) ui->n_failed,
             (unsigned long long) ui->n_started);
}

const YcUIFuncs yc_ui_passthrough = {
  "passthrough",
  "let jobs write straight to this terminal; no capture, no tagging",
  0,                            /* no instance state of its own */
  0,                            /* flags */
  NULL,                         /* init */
  NULL,                         /* job_started */
  NULL,                         /* job_output: deliberately unset -- */
  NULL,                         /* job_line:   this is what skips capture */
  passthrough_job_ended,
  passthrough_all_done,
  NULL                          /* destroy */
};
