/* SPDX-License-Identifier: 0BSD */
/* Recorders: things that observe a run without presenting it.
 *
 * A run has one UI and any number of recorders, which is what lets a
 * terminal UI and an on-disk record of the same run coexist:
 *
 *     yapara --ui=two-pane --recorder=jobs --out-dir=results
 *
 * The interface is deliberately much smaller than YcUIFuncs.  A
 * recorder gets told that a job started, that some bytes arrived, and
 * that it finished; it does not subclass the job, does not see
 * ui->jobs, and never touches the terminal.  That asymmetry is the
 * whole point: teeing two full UIs would mean giving each its own job
 * object and its own mirrored job arrays, whereas a recorder needs
 * none of that and so composes for free.
 *
 * Per-job state comes back from job_started() and is handed to every
 * later call for that job, so there is no casting and no map to keep.
 * Release it in job_ended().
 *
 * job_output() gets the child's bytes exactly as they arrived, which
 * is what a byte-faithful record needs; job_line() is the convenient
 * view.  As with a UI, setting either is what makes the framework
 * capture output at all.
 */

#ifndef YC_RECORDER_H_
#define YC_RECORDER_H_

#include "yc-ui.h"

typedef struct YcRecorder YcRecorder;
typedef struct YcRecorderFuncs YcRecorderFuncs;

struct YcRecorderFuncs {
  const char *name;               /* as given to --recorder=NAME */
  const char *description;        /* one line, for --list-recorders */
  const char *long_description;   /* for --help-record=NAME */
  size_t instance_size;           /* >= sizeof(YcRecorder); 0 means exactly */

  /* Anything that can fail belongs here.  Set *error_message
     (malloced) and return false. */
  bool (*init)         (YcRecorder *recorder, char **error_message);

  /* Returns this recorder's state for this job, or NULL if it needs
     none.  Whatever comes back is passed to every later call. */
  void *(*job_started) (YcRecorder *recorder, YcUIJob *job);

  void (*job_output)   (YcRecorder *recorder, YcUIJob *job, void *job_state,
                        int child_fd, const void *data, size_t len,
                        uint64_t micros);

  void (*job_line)     (YcRecorder *recorder, YcUIJob *job, void *job_state,
                        int child_fd, const char *line, size_t len,
                        uint64_t micros);

  /* The job's output is complete.  Release job_state here. */
  void (*job_ended)    (YcRecorder *recorder, YcUIJob *job, void *job_state);

  void (*all_done)     (YcRecorder *recorder);
  void (*destroy)      (YcRecorder *recorder);
};

struct YcRecorder {
  const YcRecorderFuncs *funcs;

  /* Never NULL; the same options the UI gets, so --out-dir and the
     index format mean one thing across the whole run. */
  const YcUIOptions *options;

  void *user_data;

  /*< private >*/
  size_t slot;                    /* which per-job state slot is ours */
};

/* --- the registry --- */

void yc_recorder_register (const YcRecorderFuncs *funcs);

/* NULL if there is no such recorder. */
const YcRecorderFuncs *yc_recorder_lookup (const char *name);

size_t yc_recorder_get_all (const YcRecorderFuncs *const **funcs_out);

/* --- using one --- */

/* NULL on failure, with *error_message set (caller frees).  'options'
 * may be NULL for the defaults; otherwise it must outlive the
 * recorder. */
YcRecorder *yc_recorder_new (const YcRecorderFuncs *funcs,
                             const YcUIOptions    *options,
                             char                **error_message);

void yc_recorder_free (YcRecorder *recorder);

#endif
