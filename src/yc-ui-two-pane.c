/* SPDX-License-Identifier: 0BSD */
/* The 'two-pane' UI: a list of jobs on top, the selected job's output
 * below.
 *
 *   idx  status   command
 * > 003  running  convert c.png c.jpg
 *   004  running  convert d.png d.jpg
 *   001  exit 1   convert a.png a.jpg
 *   ------------------------------------------------
 *   converting c.png
 *   done
 *
 * Each job's output goes into a circular buffer sized once at startup,
 * so a job that prints for an hour costs the same as one that prints a
 * line.  Finished jobs stay listed -- ui->max_ended_jobs is set from
 * init(), and the buffer is released in job_destroyed() -- because the
 * job you actually want to read is usually the one that just failed.
 *
 * Redraws are coalesced onto a timer rather than done per line: a
 * thousand lines a second would otherwise be a thousand repaints, and
 * only the last of them would ever be seen.
 */

#include <stdio.h>
#include <string.h>

#include "yc-circular-buffer.h"
#include "yc-common.h"
#include "yc-alloc.h"
#include "yc-term.h"
#include "yc-ui.h"

#define REDRAW_INTERVAL_MS   33          /* ~30Hz */
#define DEFAULT_JOB_ROWS     8
#define MIN_JOB_ROWS         1
#define MIN_OUTPUT_ROWS      1

/* What we remember about each line, alongside the text itself. */
typedef struct {
  uint64_t is_stderr : 1;
  uint64_t micros : 63;
} LineInfo;

typedef struct {
  YcUI base;                             /* must come first */
  YcCircularBufferNewOptions buffer_options;

  YcTerm *term;
  uv_timer_t redraw_timer;
  bool timer_open;
  bool dirty;
  bool quit;

  unsigned job_rows;                     /* height of the top pane */
  uint64_t selected;                     /* job index, not a row number */
  size_t output_scroll;                  /* lines back from the newest */

  /* Rebuilt each frame: running jobs then finished ones. */
  YcUIJob **rows;
  size_t n_rows, rows_alloced;
} TwoPaneUI;

typedef struct {
  YcUIJob base;                          /* must come first */
  YcCircularBuffer *output;
} TwoPaneJob;

/* --- the row list --- */

/* Running first (in start order), then finished newest-first, so a job
   that just failed appears directly under the running ones instead of
   at the bottom of a long history. */
static void
rebuild_rows (TwoPaneUI *tp)
{
  YcUI *ui = &tp->base;
  size_t needed = ui->n_jobs + ui->n_ended_jobs;
  size_t i;

  if (needed > tp->rows_alloced)
    {
      tp->rows_alloced = needed;
      tp->rows = YC_RENEW (YcUIJob *, tp->rows, tp->rows_alloced);
    }
  tp->n_rows = 0;
  for (i = 0; i < ui->n_jobs; i++)
    tp->rows[tp->n_rows++] = ui->jobs[i];
  for (i = ui->n_ended_jobs; i > 0; i--)
    tp->rows[tp->n_rows++] = ui->ended_jobs[i - 1];
}

/* Selection is held as a job index rather than a row so that it stays
   on the same job as rows come and go underneath it. */
static size_t
selected_row (TwoPaneUI *tp)
{
  size_t i;
  for (i = 0; i < tp->n_rows; i++)
    if (tp->rows[i]->index == tp->selected)
      return i;
  return 0;
}

static YcUIJob *
selected_job (TwoPaneUI *tp)
{
  size_t row = selected_row (tp);
  return row < tp->n_rows ? tp->rows[row] : NULL;
}

static void
move_selection (TwoPaneUI *tp, int delta)
{
  size_t row;

  rebuild_rows (tp);
  if (tp->n_rows == 0)
    return;
  row = selected_row (tp);

  if (delta < 0 && row == 0)
    return;
  if (delta > 0 && row + 1 >= tp->n_rows)
    return;
  row = delta < 0 ? row - 1 : row + 1;

  tp->selected = tp->rows[row]->index;
  tp->output_scroll = 0;                 /* a new job starts at its end */
  tp->dirty = true;
}

/* --- drawing --- */

static const char *
status_text (YcUIJob *job, char *buf, size_t buf_size)
{
  if (job->running)
    return "running";
  if (job->status == YC_CHILD_STATUS_KILLED)
    {
      snprintf (buf, buf_size, "sig %d", job->status_value);
      return buf;
    }
  if (job->status_value == 0)
    return "ok";
  snprintf (buf, buf_size, "exit %d", job->status_value);
  return buf;
}

static void
draw_job_pane (TwoPaneUI *tp)
{
  YcUI *ui = &tp->base;
  YcTerm *term = tp->term;
  unsigned width = yc_term_width (term);
  size_t selected = selected_row (tp);
  size_t first = 0;
  unsigned row;

  /* Keep the selection on screen without moving it more than needed. */
  if (tp->job_rows > 1 && selected >= tp->job_rows - 1)
    first = selected - (tp->job_rows - 2);

  yc_term_row_begin (term, 0);
  yc_term_row_attr (term, YC_TERM_REVERSE);
  yc_term_row_printf (term, " %llu running, %llu done, %llu failed ",
                      (unsigned long long) ui->n_jobs,
                      (unsigned long long) ui->n_ended,
                      (unsigned long long) ui->n_failed);
  yc_term_row_pad (term, width);
  yc_term_row_end (term);

  for (row = 1; row < tp->job_rows; row++)
    {
      size_t i = first + row - 1;
      YcUIJob *job;
      char index[YC_UI_INDEX_BUF_SIZE];
      char status[32];

      yc_term_row_begin (term, row);
      if (i >= tp->n_rows)
        {
          yc_term_row_end (term);
          continue;
        }
      job = tp->rows[i];

      if (i == selected)
        yc_term_row_attr (term, YC_TERM_REVERSE);
      else
        yc_term_row_attr (term, yc_ui_job_color (&tp->base, job));

      yc_ui_job_index_string (&tp->base, job, index, sizeof (index));
      yc_term_row_printf (term, "%s %-7s %-8s ",
                          i == selected ? ">" : " ",
                          index,
                          status_text (job, status, sizeof (status)));
      yc_term_row_puts (term, job->cmdline, strlen (job->cmdline));
      if (i == selected)
        yc_term_row_pad (term, width);
      yc_term_row_end (term);
    }
}

static void
draw_divider (TwoPaneUI *tp)
{
  YcTerm *term = tp->term;
  unsigned width = yc_term_width (term);
  YcUIJob *job = selected_job (tp);
  char index[YC_UI_INDEX_BUF_SIZE];
  unsigned col;

  yc_term_row_begin (term, tp->job_rows);
  yc_term_row_attr (term, YC_TERM_DIM);
  if (job != NULL)
    {
      yc_ui_job_index_string (&tp->base, job, index, sizeof (index));
      if (tp->output_scroll > 0)
        yc_term_row_printf (term, "-- job %s (scrolled %llu) ", index,
                            (unsigned long long) tp->output_scroll);
      else
        yc_term_row_printf (term, "-- job %s ", index);
    }
  else
    yc_term_row_puts (term, "-- no jobs ", 11);
  for (col = 0; col < width; col++)
    yc_term_row_puts (term, "-", 1);
  yc_term_row_end (term);
}

static void
draw_output_pane (TwoPaneUI *tp)
{
  YcTerm *term = tp->term;
  unsigned height = yc_term_height (term);
  unsigned first_row = tp->job_rows + 1;
  YcUIJob *job = selected_job (tp);
  TwoPaneJob *tpjob = (TwoPaneJob *) job;
  YcCircularBufferStats stats;
  unsigned n_output_rows, row;

  if (first_row >= height)
    return;
  n_output_rows = height - first_row;

  if (job == NULL || tpjob->output == NULL)
    {
      for (row = first_row; row < height; row++)
        {
          yc_term_row_begin (term, row);
          yc_term_row_end (term);
        }
      return;
    }

  stats = yc_circular_buffer_get_stats (tpjob->output);

  /* Fill bottom-up: the newest line sits on the last row, so output
     does not jump around as it arrives. */
  for (row = 0; row < n_output_rows; row++)
    {
      unsigned screen_row = height - 1 - row;
      size_t age = tp->output_scroll + row;

      yc_term_row_begin (term, screen_row);
      if (age < stats.n_lines)
        {
          YcCircularBufferLine line =
            yc_circular_buffer_get_line (tpjob->output, age);
          const LineInfo *info = line.line_data;

          if (info != NULL && info->is_stderr)
            yc_term_row_attr (term, "\033[31m");
          if (line.first_part_len > 0)
            yc_term_row_puts (term, line.first_part, line.first_part_len);
          if (line.second_part != NULL && line.second_part_len > 0)
            yc_term_row_puts (term, line.second_part, line.second_part_len);
          if (line.truncated)
            {
              yc_term_row_attr (term, YC_TERM_DIM);
              yc_term_row_puts (term, "\xe2\x80\xa6", 3);   /* ellipsis */
            }
        }
      yc_term_row_end (term);
    }
}

static void
redraw (TwoPaneUI *tp)
{
  unsigned height = yc_term_height (tp->term);

  rebuild_rows (tp);

  /* Clamp the split to whatever the window can actually hold; a
     resize can make the saved value nonsense. */
  if (tp->job_rows + 1 + MIN_OUTPUT_ROWS > height)
    tp->job_rows = height > (1 + MIN_OUTPUT_ROWS)
                 ? height - 1 - MIN_OUTPUT_ROWS
                 : MIN_JOB_ROWS;
  if (tp->job_rows < MIN_JOB_ROWS)
    tp->job_rows = MIN_JOB_ROWS;

  yc_term_begin_frame (tp->term);
  draw_job_pane (tp);
  draw_divider (tp);
  draw_output_pane (tp);
  yc_term_end_frame (tp->term);

  tp->dirty = false;
}

static void
on_redraw_timer (uv_timer_t *timer)
{
  TwoPaneUI *tp = timer->data;
  if (tp->dirty && !tp->quit)
    redraw (tp);
}

/* --- input --- */

static void
scroll_output (TwoPaneUI *tp, int delta_lines)
{
  YcUIJob *job = selected_job (tp);
  TwoPaneJob *tpjob = (TwoPaneJob *) job;
  YcCircularBufferStats stats;
  size_t max_scroll;

  if (job == NULL || tpjob->output == NULL)
    return;
  stats = yc_circular_buffer_get_stats (tpjob->output);

  /* Do not scroll past the oldest line we still hold. */
  max_scroll = stats.n_lines > 1 ? stats.n_lines - 1 : 0;
  if (delta_lines < 0)
    {
      size_t back = (size_t) -delta_lines;
      tp->output_scroll = tp->output_scroll > back
                        ? tp->output_scroll - back : 0;
    }
  else
    {
      tp->output_scroll += (size_t) delta_lines;
      if (tp->output_scroll > max_scroll)
        tp->output_scroll = max_scroll;
    }
  tp->dirty = true;
}

static void
on_key (YcTerm *term, const YcTermKey *key, void *user_data)
{
  TwoPaneUI *tp = user_data;
  unsigned page = yc_term_height (term) > (tp->job_rows + 2)
                ? yc_term_height (term) - tp->job_rows - 2
                : 1;

  switch (key->type)
    {
    case YC_TERM_KEY_UP:        move_selection (tp, -1); return;
    case YC_TERM_KEY_DOWN:      move_selection (tp, +1); return;
    case YC_TERM_KEY_PAGE_UP:   scroll_output (tp, (int) page); return;
    case YC_TERM_KEY_PAGE_DOWN: scroll_output (tp, -(int) page); return;
    case YC_TERM_KEY_HOME:      scroll_output (tp, 1 << 20); return;
    case YC_TERM_KEY_END:       tp->output_scroll = 0; tp->dirty = true;
                                return;
    case YC_TERM_KEY_CHAR:
      break;
    default:
      return;
    }

  if (key->ctrl && key->ch == 'c')
    {
      tp->quit = true;
      yc_term_stop (tp->term);
      return;
    }

  switch (key->ch)
    {
    case 'q':
      tp->quit = true;
      yc_term_stop (tp->term);
      break;
    case 'k': move_selection (tp, -1); break;
    case 'j': move_selection (tp, +1); break;
    case '[':
      if (tp->job_rows > MIN_JOB_ROWS)
        {
          tp->job_rows--;
          tp->dirty = true;
        }
      break;
    case ']':
      tp->job_rows++;                    /* redraw() clamps it */
      tp->dirty = true;
      break;
    default:
      break;
    }
}

static void
on_resize (YcTerm *term, void *user_data)
{
  TwoPaneUI *tp = user_data;
  tp->dirty = true;
}

/* --- the vtable --- */

static bool
two_pane_init (YcUI *ui, char **error_message)
{
  TwoPaneUI *tp = (TwoPaneUI *) ui;
  YcTermCallbacks callbacks;

  tp->buffer_options = YC_CIRCULAR_BUFFER_NEW_OPTIONS_INIT;
  tp->buffer_options.line_data_size = sizeof (LineInfo);
  tp->buffer_options.buffer_size =
    yc_ui_options_get_uint (ui->options, "buffer-size",
                            tp->buffer_options.buffer_size);
  tp->buffer_options.max_lines =
    yc_ui_options_get_uint (ui->options, "max-lines",
                            tp->buffer_options.max_lines);

  /* Finished jobs have to stay selectable for this ui to be any use. */
  ui->max_ended_jobs =
    yc_ui_options_get_uint (ui->options, "keep-jobs", 200);

  tp->job_rows = yc_ui_options_get_uint (ui->options, "job-rows",
                                         DEFAULT_JOB_ROWS);
  if (tp->job_rows < MIN_JOB_ROWS)
    tp->job_rows = MIN_JOB_ROWS;

  memset (&callbacks, 0, sizeof (callbacks));
  callbacks.key = on_key;
  callbacks.resize = on_resize;
  tp->term = yc_term_new (ui->loop, &callbacks, tp, error_message);
  if (tp->term == NULL)
    return false;

  uv_timer_init (ui->loop, &tp->redraw_timer);
  tp->redraw_timer.data = tp;
  tp->timer_open = true;
  uv_timer_start (&tp->redraw_timer, on_redraw_timer,
                  REDRAW_INTERVAL_MS, REDRAW_INTERVAL_MS);

  tp->dirty = true;
  return true;
}

static void
two_pane_job_started (YcUI *ui, YcUIJob *job)
{
  TwoPaneUI *tp = (TwoPaneUI *) ui;
  TwoPaneJob *tpjob = (TwoPaneJob *) job;

  tpjob->output = yc_circular_buffer_new (&tp->buffer_options);

  /* Without this the first job would start unselected. */
  if (ui->n_jobs == 1 && ui->n_ended_jobs == 0)
    tp->selected = job->index;
  tp->dirty = true;
}

static void
two_pane_job_line (YcUI *ui, YcUIJob *job, int child_fd,
                   const char *line, size_t len, uint64_t micros)
{
  TwoPaneUI *tp = (TwoPaneUI *) ui;
  TwoPaneJob *tpjob = (TwoPaneJob *) job;
  LineInfo info;

  info.is_stderr = child_fd == 2 ? 1 : 0;
  info.micros = micros & 0x7fffffffffffffffULL;
  yc_circular_buffer_add (tpjob->output, len, line, &info);

  /* Only the pane being looked at needs a repaint. */
  if (job->index == tp->selected)
    tp->dirty = true;
}

static void
two_pane_job_ended (YcUI *ui, YcUIJob *job)
{
  ((TwoPaneUI *) ui)->dirty = true;
}

static void
two_pane_job_destroyed (YcUI *ui, YcUIJob *job)
{
  TwoPaneJob *tpjob = (TwoPaneJob *) job;

  if (tpjob->output != NULL)
    {
      yc_circular_buffer_free (tpjob->output);
      tpjob->output = NULL;
    }
  ((TwoPaneUI *) ui)->dirty = true;
}

/* The loop only unwinds once the terminal stops reading keys, so by
   the time this runs the user has already quit. */
static void
two_pane_all_done (YcUI *ui)
{
  if (ui->n_failed > 0)
    fprintf (stderr, "%llu of %llu jobs failed.\n",
             (unsigned long long) ui->n_failed,
             (unsigned long long) ui->n_started);
}

static void
two_pane_destroy (YcUI *ui)
{
  TwoPaneUI *tp = (TwoPaneUI *) ui;

  if (tp->timer_open)
    {
      uv_timer_stop (&tp->redraw_timer);
      uv_close ((uv_handle_t *) &tp->redraw_timer, NULL);
      tp->timer_open = false;
    }
  yc_term_free (tp->term);
  tp->term = NULL;
  yc_free (tp->rows);
}

const YcUIFuncs yc_ui_two_pane = {
  "two-pane",
  "full-screen: job list on top, selected job's output below",
  "A job list in the top pane and the selected job's output below.\n"
  "  Up / Down / k / j   select a job\n"
  "  PgUp / PgDn         scroll the output\n"
  "  Home / End          oldest / newest output\n"
  "  [ / ]               move the divider\n"
  "  q / Ctrl-C          quit\n"
  "Finished jobs stay listed so their output can still be read.\n"
  "--ui-option settings: buffer-size, max-lines, keep-jobs, job-rows.\n",
  sizeof (TwoPaneUI),
  sizeof (TwoPaneJob),
  YC_UI_INTERACTIVE,
  two_pane_init,
  two_pane_job_started,
  NULL,                                  /* job_output: lines are enough */
  two_pane_job_line,
  two_pane_job_ended,
  two_pane_job_destroyed,
  two_pane_all_done,
  two_pane_destroy
};
