/* SPDX-License-Identifier: 0BSD */
/* The 'jobs' recorder: four files per job under --out-dir, for picking
 * over afterwards.
 *
 * A recorder rather than a UI, so it runs alongside whichever UI is
 * presenting the run: '--ui=two-pane --recorder=jobs' watches a run and
 * writes it down at the same time.
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
#include "yc-recorder.h"

typedef struct {
  YcRecorder base;              /* must come first */
  const char *out_dir;
  uint64_t n_jobs, n_failed;
} JobsRecorder;

/* Per-job state, handed back to us on every call for that job. */
typedef struct {
  FILE *stream[2];              /* fd 1, fd 2; opened on first byte */
  uint64_t n_bytes[2];
  char index[YC_UI_INDEX_BUF_SIZE];
} JobsRecord;

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
job_path (JobsRecorder *jr,
          JobsRecord *record,
          const char *suffix,
          char *buf,
          size_t buf_size)
{
  snprintf (buf, buf_size, "%s/%s%s", jr->out_dir, record->index, suffix);
}

/* Losing output silently would be worse than stopping. */
static FILE *
open_job_file (JobsRecorder *jr,
               JobsRecord *record,
               const char *suffix)
{
  char path[1024];
  FILE *fp;

  job_path (jr, record, suffix, path, sizeof (path));
  fp = fopen (path, "w");
  if (fp == NULL)
    yc_die ("error creating %s: %s", path, strerror (errno));
  return fp;
}

/* --- the vtable --- */

static bool
jobs_init (YcRecorder *recorder, char **error_message)
{
  JobsRecorder *jr = (JobsRecorder *) recorder;

  if (recorder->options->out_dir == NULL)
    {
      *error_message =
        yc_strdup ("--recorder=jobs writes files, so it needs --out-dir=DIR");
      return false;
    }
  if (!yc_ui_options_ensure_out_dir (recorder->options, error_message))
    return false;

  jr->out_dir = recorder->options->out_dir;
  return true;
}

static void *
jobs_job_started (YcRecorder *recorder, YcUIJob *job)
{
  JobsRecorder *jr = (JobsRecorder *) recorder;
  JobsRecord *record = YC_NEW0 (JobsRecord);
  FILE *fp;

  yc_ui_format_index_for_filename (recorder->options, job->index,
                                   record->index, sizeof (record->index));

  fp = open_job_file (jr, record, "-start.json");
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

  jr->n_jobs++;
  return record;
}

static void
jobs_job_output (YcRecorder *recorder, YcUIJob *job, void *job_state,
                 int child_fd, const void *data, size_t len,
                 uint64_t micros)
{
  JobsRecorder *jr = (JobsRecorder *) recorder;
  JobsRecord *record = job_state;
  int slot = SLOT_OF_FD (child_fd);

  if (record->stream[slot] == NULL)
    record->stream[slot] = open_job_file (jr, record, FD_SUFFIX (slot));

  if (fwrite (data, 1, len, record->stream[slot]) != len)
    yc_die ("error writing %s%s: %s",
            record->index, FD_SUFFIX (slot), strerror (errno));
  record->n_bytes[slot] += len;
}

static void
jobs_job_ended (YcRecorder *recorder, YcUIJob *job, void *job_state)
{
  JobsRecorder *jr = (JobsRecorder *) recorder;
  JobsRecord *record = job_state;
  bool killed = job->status == YC_CHILD_STATUS_KILLED;
  FILE *fp;
  int slot;

  for (slot = 0; slot < 2; slot++)
    if (record->stream[slot] != NULL)
      {
        if (fclose (record->stream[slot]) != 0)
          yc_die ("error closing %s%s: %s",
                  record->index, FD_SUFFIX (slot), strerror (errno));
        record->stream[slot] = NULL;
      }

  fp = open_job_file (jr, record, "-end.json");
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
           (unsigned long long) record->n_bytes[0]);
  fprintf (fp, "  \"stderr_bytes\": %llu\n",
           (unsigned long long) record->n_bytes[1]);
  fprintf (fp, "}\n");
  fclose (fp);

  if (killed || job->status_value != 0)
    jr->n_failed++;

  /* Ours to release: the framework only keeps the pointer. */
  yc_free (record);
}

/* A recorder shares the terminal with whatever ui is presenting, so it
   says as little as possible -- and nothing at all on a clean run. */
static void
jobs_all_done (YcRecorder *recorder)
{
  JobsRecorder *jr = (JobsRecorder *) recorder;

  /* The ui has already said how many failed; repeating the count would
     just be two voices saying the same thing.  Where to look for the
     output is the part only this recorder knows. */
  if (jr->n_failed > 0)
    fprintf (stderr, "recorded output is in %s\n", jr->out_dir);
}

const YcRecorderFuncs yc_recorder_jobs = {
  "jobs",
  "write per-job .stdout/.stderr and start/end json under --out-dir",
  "Write four files per job, numbered sequentially.\n" \
  "  #-start.json    Information about the job, at its start.\n" \
  "  #-end.json      Information about the job, at its end.\n" \
  "  #.stdout        Standard output, if any is given.\n" \
  "  #.stderr        Standard error, if any is given.\n",
  sizeof (JobsRecorder),
  jobs_init,
  jobs_job_started,
  jobs_job_output,              /* raw bytes: what lands on disk is exact */
  NULL,                         /* job_line: not wanted */
  jobs_job_ended,
  jobs_all_done,
  NULL                          /* destroy */
};
