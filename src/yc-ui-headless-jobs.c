/* SPDX-License-Identifier: 0BSD */
/* The 'headless-jobs' UI: nothing on the terminal, four files per job
 * under --out-dir, for picking over afterwards.
 *
 *     000042-start.json    index, pid, cmdline, when it began
 *     000042-end.json      how it finished, how long it took, byte counts
 *     000042.stdout        exactly the bytes the child wrote
 *     000042.stderr
 *
 * The index comes from the shared index format, with padding forced to
 * zeroes, so '--index-width=6' gets you names that sort lexically.
 *
 * The .stdout/.stderr files are written from job_output() rather than
 * job_line(), so what lands on disk is byte-for-byte what the child
 * produced: no line splitting, no CRLF rewriting, no assumption that
 * the output was text at all.
 *
 * They are created on first write, so a job that never touched stderr
 * leaves no .stderr file behind.  The byte counts in -end.json say
 * what to expect, so absence is unambiguous rather than something a
 * reader has to guess about.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "yc-common.h"
#include "yc-alloc.h"
#include "yc-ui.h"

typedef struct {
  YcUI base;                    /* must come first */
  const char *out_dir;
} HeadlessJobsUI;

typedef struct {
  FILE *stream[2];              /* fd 1, fd 2; opened on first byte */
  uint64_t n_bytes[2];
  char index[YC_UI_INDEX_BUF_SIZE];
} HeadlessJobsJob;

#define SLOT_OF_FD(child_fd)   ((child_fd) == 2 ? 1 : 0)
#define FD_SUFFIX(slot)        ((slot) == 1 ? ".stderr" : ".stdout")

/* --- json --- */

/* Bytes at or above 0x80 pass through untouched, which is right for
   UTF-8 and the best available guess for anything else: re-encoding
   them would change the command-line we are reporting. */
static void
write_json_string (FILE *fp, const char *str)
{
  const unsigned char *p;

  fputc ('"', fp);
  for (p = (const unsigned char *) str; *p != 0; p++)
    switch (*p)
      {
      case '"':  fputs ("\\\"", fp); break;
      case '\\': fputs ("\\\\", fp); break;
      case '\n': fputs ("\\n", fp);  break;
      case '\r': fputs ("\\r", fp);  break;
      case '\t': fputs ("\\t", fp);  break;
      case '\b': fputs ("\\b", fp);  break;
      case '\f': fputs ("\\f", fp);  break;
      default:
        if (*p < 0x20)
          fprintf (fp, "\\u%04x", (unsigned) *p);
        else
          fputc (*p, fp);
        break;
      }
  fputc ('"', fp);
}

/* --- paths --- */

static void
job_path (HeadlessJobsUI *hj,
          HeadlessJobsJob *hjob,
          const char *suffix,
          char *buf,
          size_t buf_size)
{
  snprintf (buf, buf_size, "%s/%s%s", hj->out_dir, hjob->index, suffix);
}

/* Losing output silently would be worse than stopping. */
static FILE *
open_job_file (HeadlessJobsUI *hj,
               HeadlessJobsJob *hjob,
               const char *suffix)
{
  char path[1024];
  FILE *fp;

  job_path (hj, hjob, suffix, path, sizeof (path));
  fp = fopen (path, "w");
  if (fp == NULL)
    yc_die ("error creating %s: %s", path, strerror (errno));
  return fp;
}

/* --- the vtable --- */

static bool
headless_jobs_init (YcUI *ui, char **error_message)
{
  HeadlessJobsUI *hj = (HeadlessJobsUI *) ui;

  if (ui->options->out_dir == NULL)
    {
      *error_message =
        yc_strdup ("this ui writes files, so it needs --out-dir=DIR");
      return false;
    }
  if (!yc_ui_options_ensure_out_dir (ui->options, error_message))
    return false;

  hj->out_dir = ui->options->out_dir;
  return true;
}

static void
headless_jobs_job_started (YcUI *ui, YcUIJob *job)
{
  HeadlessJobsUI *hj = (HeadlessJobsUI *) ui;
  HeadlessJobsJob *hjob = YC_NEW0 (HeadlessJobsJob);
  FILE *fp;

  job->ui_data = hjob;
  yc_ui_format_index_for_filename (ui->options, job->index,
                                   hjob->index, sizeof (hjob->index));

  fp = open_job_file (hj, hjob, "-start.json");
  fprintf (fp, "{\n");
  fprintf (fp, "  \"index\": %llu,\n", (unsigned long long) job->index);
  fprintf (fp, "  \"pid\": %d,\n", job->pid);
  fprintf (fp, "  \"cmdline\": ");
  write_json_string (fp, job->cmdline);
  fprintf (fp, ",\n");
  fprintf (fp, "  \"started_micros\": %llu\n",
           (unsigned long long) job->started_micros);
  fprintf (fp, "}\n");
  fclose (fp);
}

static void
headless_jobs_job_output (YcUI *ui, YcUIJob *job, int child_fd,
                          const void *data, size_t len, uint64_t micros)
{
  HeadlessJobsUI *hj = (HeadlessJobsUI *) ui;
  HeadlessJobsJob *hjob = job->ui_data;
  int slot = SLOT_OF_FD (child_fd);

  if (hjob->stream[slot] == NULL)
    hjob->stream[slot] = open_job_file (hj, hjob, FD_SUFFIX (slot));

  if (fwrite (data, 1, len, hjob->stream[slot]) != len)
    yc_die ("error writing %s%s: %s",
            hjob->index, FD_SUFFIX (slot), strerror (errno));
  hjob->n_bytes[slot] += len;
}

static void
headless_jobs_job_ended (YcUI *ui, YcUIJob *job)
{
  HeadlessJobsUI *hj = (HeadlessJobsUI *) ui;
  HeadlessJobsJob *hjob = job->ui_data;
  bool killed = job->status == YC_CHILD_STATUS_KILLED;
  FILE *fp;
  int slot;

  for (slot = 0; slot < 2; slot++)
    if (hjob->stream[slot] != NULL)
      {
        if (fclose (hjob->stream[slot]) != 0)
          yc_die ("error closing %s%s: %s",
                  hjob->index, FD_SUFFIX (slot), strerror (errno));
        hjob->stream[slot] = NULL;
      }

  fp = open_job_file (hj, hjob, "-end.json");
  fprintf (fp, "{\n");
  fprintf (fp, "  \"index\": %llu,\n", (unsigned long long) job->index);
  fprintf (fp, "  \"pid\": %d,\n", job->pid);
  fprintf (fp, "  \"cmdline\": ");
  write_json_string (fp, job->cmdline);
  fprintf (fp, ",\n");
  fprintf (fp, "  \"started_micros\": %llu,\n",
           (unsigned long long) job->started_micros);
  fprintf (fp, "  \"ended_micros\": %llu,\n",
           (unsigned long long) job->ended_micros);
  fprintf (fp, "  \"elapsed_micros\": %llu,\n",
           (unsigned long long) (job->ended_micros - job->started_micros));
  fprintf (fp, "  \"status\": \"%s\",\n", killed ? "killed" : "exited");
  /* Both keys always appear, with null for whichever does not apply,
     so that a reader can rely on one shape. */
  if (killed)
    fprintf (fp, "  \"exit_code\": null,\n  \"signal\": %d,\n",
             job->status_value);
  else
    fprintf (fp, "  \"exit_code\": %d,\n  \"signal\": null,\n",
             job->status_value);
  fprintf (fp, "  \"stdout_bytes\": %llu,\n",
           (unsigned long long) hjob->n_bytes[0]);
  fprintf (fp, "  \"stderr_bytes\": %llu\n",
           (unsigned long long) hjob->n_bytes[1]);
  fprintf (fp, "}\n");
  fclose (fp);

  yc_free (hjob);
  job->ui_data = NULL;
}

static void
headless_jobs_all_done (YcUI *ui)
{
  HeadlessJobsUI *hj = (HeadlessJobsUI *) ui;

  /* The only thing this ui says on a terminal, and only when there is
     something to say. */
  if (ui->n_failed > 0)
    fprintf (stderr, "%llu of %llu jobs failed; see %s\n",
             (unsigned long long) ui->n_failed,
             (unsigned long long) ui->n_started,
             hj->out_dir);
}

const YcUIFuncs yc_ui_headless_jobs = {
  "headless-jobs",
  "write per-job .stdout/.stderr and start/end json under --out-dir",
  sizeof (HeadlessJobsUI),
  0,                            /* flags */
  headless_jobs_init,
  headless_jobs_job_started,
  headless_jobs_job_output,     /* raw bytes: what lands on disk is exact */
  NULL,                         /* job_line: not wanted */
  headless_jobs_job_ended,
  headless_jobs_all_done,
  NULL                          /* destroy */
};
