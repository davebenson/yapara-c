/* SPDX-License-Identifier: 0BSD */
/* The 'prefix' UI: every line of output, tagged with the job it came
 * from and the stream it arrived on.
 *
 *     0O: starting up             job 0, stdout
 *     0E: warning: no such file   job 0, stderr
 *     3!: exited with status 4    job 3, a note from yapara itself
 *
 * Child output all goes to our stdout, stderr lines included: the tag
 * already says which stream it was, and keeping one stream means the
 * ordering survives being piped somewhere.  Only yapara's own notes go
 * to stderr, so they stay out of the data.
 *
 * This UI keeps no state of its own, so instance_size stays 0 -- the
 * counterpart to yc-ui-plain.c, which shows the subclassing case.
 */

#include <stdio.h>

#include "yc-common.h"
#include "yc-ui.h"

/* Anything that is not stderr is output; a UI-managed job only ever
   has fds 1 and 2 captured, but that keeps it honest. */
#define STREAM_CHAR(child_fd)   ((child_fd) == 2 ? 'E' : 'O')
#define NOTE_CHAR               '!'

static void
prefix_job_line (YcUI *ui, YcUIJob *job, int child_fd,
                 const char *line, size_t len, uint64_t micros)
{
  char index[YC_UI_INDEX_BUF_SIZE];

  /* Only the tag is tinted: the payload is the child's own text, which
     may already contain escapes of its own, and colouring it would
     also mean wrapping bytes that need not be text at all. */
  printf ("%s%s%c:%s ", yc_ui_job_color (ui, job),
          yc_ui_job_index_string (ui, job, index, sizeof (index)),
          STREAM_CHAR (child_fd), yc_ui_color_reset (ui));
  /* fwrite rather than %s: a child is entitled to write a NUL byte,
     and printf would silently end the line there. */
  fwrite (line, 1, len, stdout);
  putchar ('\n');
}

static void
prefix_job_ended (YcUI *ui, YcUIJob *job)
{
  char index[YC_UI_INDEX_BUF_SIZE];

  if (job->status == YC_CHILD_STATUS_EXITED && job->status_value == 0)
    return;

  /* stdout is block-buffered when it is not a terminal, but stderr
     never is, so without this the note overtakes the lines it is
     about. */
  fflush (stdout);
  yc_ui_job_index_string (ui, job, index, sizeof (index));

  if (job->status == YC_CHILD_STATUS_KILLED)
    fprintf (stderr, "%s%s%c:%s killed by signal %d\n",
             yc_ui_job_color (ui, job), index, NOTE_CHAR,
             yc_ui_color_reset (ui), job->status_value);
  else
    fprintf (stderr, "%s%s%c:%s exited with status %d\n",
             yc_ui_job_color (ui, job), index, NOTE_CHAR,
             yc_ui_color_reset (ui), job->status_value);
}

static void
prefix_all_done (YcUI *ui)
{
  fflush (stdout);
  if (ui->n_failed > 0)
    fprintf (stderr, "%llu of %llu jobs failed.\n",
             (unsigned long long) ui->n_failed,
             (unsigned long long) ui->n_started);
}

const YcUIFuncs yc_ui_prefix = {
  "prefix",
  "prefixed line-by-line output",
  "Tag each line with the job index and O (stdout) or E (stderr)\n",
  0,                            /* no instance state of its own */
  0,                            /* flags */
  NULL,                         /* init */
  NULL,                         /* job_started */
  NULL,                         /* job_output: lines are enough */
  prefix_job_line,
  prefix_job_ended,
  prefix_all_done,
  NULL                          /* destroy */
};
