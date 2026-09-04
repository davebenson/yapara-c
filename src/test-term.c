/* SPDX-License-Identifier: 0BSD */
/* Tests for yc-term.c, all headless: frames are rendered into memory
 * and keys are fed in, so there is no terminal involved and nothing to
 * eyeball.  This is the point of yc_term_new_headless() existing.
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "yc-alloc.h"
#include "yc-common.h"
#include "yc-term.h"

static unsigned n_tests, n_failures;

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
check_uint (const char *what, unsigned long long got,
            unsigned long long expected, int line)
{
  n_tests++;
  if (got != expected)
    fail (line, "%s: expected %llu, got %llu", what, expected, got);
}

static void
check_true (const char *what, bool got, int line)
{
  n_tests++;
  if (!got)
    fail (line, "%s: expected true", what);
}

#define CHECK_STR(w, g, e)  check_str (w, g, e, __LINE__)
#define CHECK_UINT(w, g, e) check_uint (w, (unsigned long long) (g), \
                                        (unsigned long long) (e), __LINE__)
#define CHECK_TRUE(w, g)    check_true (w, g, __LINE__)

/* --- collected keys --- */

#define MAX_KEYS 64

typedef struct {
  YcTermKey keys[MAX_KEYS];
  size_t n_keys;
  unsigned n_resizes;
} KeyLog;

static void
log_key (YcTerm *term, const YcTermKey *key, void *user_data)
{
  KeyLog *log = user_data;
  if (log->n_keys < MAX_KEYS)
    log->keys[log->n_keys++] = *key;
}

static void
log_resize (YcTerm *term, void *user_data)
{
  ((KeyLog *) user_data)->n_resizes++;
}

static YcTerm *
new_term (KeyLog *log, unsigned width, unsigned height)
{
  YcTermCallbacks callbacks;
  memset (log, 0, sizeof (*log));
  memset (&callbacks, 0, sizeof (callbacks));
  callbacks.key = log_key;
  callbacks.resize = log_resize;
  return yc_term_new_headless (width, height, &callbacks, log);
}

/* --- frames --- */

static void
test_row_text (void)
{
  KeyLog log;
  YcTerm *term = new_term (&log, 20, 4);

  yc_term_begin_frame (term);
  yc_term_row_begin (term, 0);
  yc_term_row_puts (term, "hello", 5);
  yc_term_row_end (term);
  yc_term_row_begin (term, 2);
  yc_term_row_puts (term, "third row", 9);
  yc_term_row_end (term);
  yc_term_end_frame (term);

  CHECK_STR ("row 0", yc_term_frame_row (term, 0), "hello");
  CHECK_STR ("row 1 is blank", yc_term_frame_row (term, 1), "");
  CHECK_STR ("row 2", yc_term_frame_row (term, 2), "third row");
  CHECK_UINT ("two rows were written", yc_term_rows_written (term), 2);
  yc_term_free (term);
}

/* A long line must be clipped, not wrapped: wrapping would shove the
   rest of the layout down the screen. */
static void
test_clipping (void)
{
  KeyLog log;
  YcTerm *term = new_term (&log, 10, 3);

  yc_term_begin_frame (term);
  yc_term_row_begin (term, 0);
  yc_term_row_puts (term, "0123456789abcdefgh", 18);
  yc_term_row_end (term);
  yc_term_end_frame (term);

  CHECK_STR ("clipped to the width", yc_term_frame_row (term, 0),
             "0123456789");

  /* Two puts that together overflow are clipped at the boundary. */
  yc_term_begin_frame (term);
  yc_term_row_begin (term, 0);
  yc_term_row_puts (term, "abcdefg", 7);
  yc_term_row_puts (term, "XXXXXXXX", 8);
  yc_term_row_end (term);
  yc_term_end_frame (term);
  CHECK_STR ("second put clipped", yc_term_frame_row (term, 0),
             "abcdefgXXX");
  yc_term_free (term);
}

/* Attributes take no columns, so they must not eat into the clip. */
static void
test_attrs_do_not_consume_columns (void)
{
  KeyLog log;
  YcTerm *term = new_term (&log, 8, 2);

  yc_term_begin_frame (term);
  yc_term_row_begin (term, 0);
  yc_term_row_attr (term, YC_TERM_REVERSE);
  yc_term_row_puts (term, "12345678", 8);
  yc_term_row_end (term);
  yc_term_end_frame (term);

  CHECK_STR ("full width still fits", yc_term_frame_row (term, 0),
             "12345678");
  CHECK_TRUE ("the escape really is in the raw bytes",
              strstr (yc_term_frame_row_raw (term, 0), "\033[7m") != NULL);
  CHECK_TRUE ("and the row resets at its end",
              strstr (yc_term_frame_row_raw (term, 0), "\033[0m") != NULL);
  yc_term_free (term);
}

/* Control bytes from a child must not reach the terminal, or a job
   could reposition the cursor and scribble over the layout. */
static void
test_control_characters_are_neutralised (void)
{
  KeyLog log;
  YcTerm *term = new_term (&log, 20, 2);

  yc_term_begin_frame (term);
  yc_term_row_begin (term, 0);
  yc_term_row_puts (term, "a\033[2Jb\tc", 8);
  yc_term_row_end (term);
  yc_term_end_frame (term);

  CHECK_STR ("escapes and tabs replaced", yc_term_frame_row (term, 0),
             "a?[2Jb?c");
  yc_term_free (term);
}

static void
test_pad (void)
{
  KeyLog log;
  YcTerm *term = new_term (&log, 10, 2);

  yc_term_begin_frame (term);
  yc_term_row_begin (term, 0);
  yc_term_row_puts (term, "ab", 2);
  yc_term_row_pad (term, 6);
  yc_term_row_puts (term, "cd", 2);
  yc_term_row_end (term);
  yc_term_end_frame (term);

  /* frame_row trims trailing blanks, so the padding shows up as the
     gap in the middle. */
  CHECK_STR ("padded to a column", yc_term_frame_row (term, 0), "ab    cd");
  yc_term_free (term);
}

/* The diffing is the reason a 30Hz repaint is affordable. */
static void
test_only_changed_rows_are_written (void)
{
  KeyLog log;
  YcTerm *term = new_term (&log, 20, 4);
  unsigned row;

  yc_term_begin_frame (term);
  for (row = 0; row < 4; row++)
    {
      yc_term_row_begin (term, row);
      yc_term_row_printf (term, "row %u", row);
      yc_term_row_end (term);
    }
  yc_term_end_frame (term);
  CHECK_UINT ("first frame writes everything",
              yc_term_rows_written (term), 4);

  /* An identical frame should write nothing at all. */
  yc_term_begin_frame (term);
  for (row = 0; row < 4; row++)
    {
      yc_term_row_begin (term, row);
      yc_term_row_printf (term, "row %u", row);
      yc_term_row_end (term);
    }
  yc_term_end_frame (term);
  CHECK_UINT ("an unchanged frame writes nothing",
              yc_term_rows_written (term), 0);

  /* Change one row; only that row is rewritten. */
  yc_term_begin_frame (term);
  for (row = 0; row < 4; row++)
    {
      yc_term_row_begin (term, row);
      yc_term_row_printf (term, row == 2 ? "changed %u" : "row %u", row);
      yc_term_row_end (term);
    }
  yc_term_end_frame (term);
  CHECK_UINT ("one changed row, one write",
              yc_term_rows_written (term), 1);
  CHECK_STR ("and it holds the new text", yc_term_frame_row (term, 2),
             "changed 2");
  yc_term_free (term);
}

/* A resize invalidates the screen, so the next frame must repaint in
   full rather than trusting the previous contents. */
static void
test_resize_forces_full_repaint (void)
{
  KeyLog log;
  YcTerm *term = new_term (&log, 20, 3);
  unsigned row;

  yc_term_begin_frame (term);
  for (row = 0; row < 3; row++)
    {
      yc_term_row_begin (term, row);
      yc_term_row_puts (term, "x", 1);
      yc_term_row_end (term);
    }
  yc_term_end_frame (term);
  CHECK_UINT ("painted", yc_term_rows_written (term), 3);

  yc_term_set_size (term, 30, 5);
  CHECK_UINT ("new width", yc_term_width (term), 30);
  CHECK_UINT ("new height", yc_term_height (term), 5);

  yc_term_begin_frame (term);
  for (row = 0; row < 5; row++)
    {
      yc_term_row_begin (term, row);
      yc_term_row_puts (term, "x", 1);
      yc_term_row_end (term);
    }
  yc_term_end_frame (term);
  CHECK_UINT ("everything repainted after a resize",
              yc_term_rows_written (term), 5);
  yc_term_free (term);
}

/* Writing outside the screen must be dropped, not crash. */
static void
test_rows_past_the_bottom_are_ignored (void)
{
  KeyLog log;
  YcTerm *term = new_term (&log, 10, 2);

  yc_term_begin_frame (term);
  yc_term_row_begin (term, 99);
  yc_term_row_puts (term, "nowhere", 7);
  yc_term_row_pad (term, 10);
  yc_term_row_end (term);
  yc_term_end_frame (term);

  CHECK_UINT ("nothing written", yc_term_rows_written (term), 0);
  CHECK_STR ("row 0 untouched", yc_term_frame_row (term, 0), "");
  yc_term_free (term);
}

/* --- keys --- */

static void
test_plain_keys (void)
{
  KeyLog log;
  YcTerm *term = new_term (&log, 20, 4);

  yc_term_feed_input (term, "abq", 3);
  CHECK_UINT ("three chars", log.n_keys, 3);
  CHECK_UINT ("first is 'a'", log.keys[0].ch, 'a');
  CHECK_UINT ("type is CHAR", log.keys[0].type, YC_TERM_KEY_CHAR);
  CHECK_TRUE ("not ctrl", !log.keys[0].ctrl);
  CHECK_UINT ("third is 'q'", log.keys[2].ch, 'q');
  yc_term_free (term);
}

static void
test_control_keys (void)
{
  KeyLog log;
  YcTerm *term = new_term (&log, 20, 4);

  yc_term_feed_input (term, "\003", 1);           /* Ctrl-C */
  CHECK_UINT ("one key", log.n_keys, 1);
  CHECK_TRUE ("ctrl flagged", log.keys[0].ctrl);
  CHECK_UINT ("reported as 'c'", log.keys[0].ch, 'c');

  yc_term_feed_input (term, "\r", 1);
  CHECK_UINT ("enter", log.keys[1].type, YC_TERM_KEY_ENTER);
  yc_term_feed_input (term, "\177", 1);
  CHECK_UINT ("backspace", log.keys[2].type, YC_TERM_KEY_BACKSPACE);
  yc_term_free (term);
}

static void
test_arrow_keys (void)
{
  KeyLog log;
  YcTerm *term = new_term (&log, 20, 4);

  yc_term_feed_input (term, "\033[A\033[B\033[C\033[D", 12);
  CHECK_UINT ("four arrows", log.n_keys, 4);
  CHECK_UINT ("up", log.keys[0].type, YC_TERM_KEY_UP);
  CHECK_UINT ("down", log.keys[1].type, YC_TERM_KEY_DOWN);
  CHECK_UINT ("right", log.keys[2].type, YC_TERM_KEY_RIGHT);
  CHECK_UINT ("left", log.keys[3].type, YC_TERM_KEY_LEFT);

  /* The application-cursor form some terminals send. */
  yc_term_feed_input (term, "\033OA", 3);
  CHECK_UINT ("ESC O A is also up", log.keys[4].type, YC_TERM_KEY_UP);
  yc_term_free (term);
}

static void
test_page_and_home_keys (void)
{
  KeyLog log;
  YcTerm *term = new_term (&log, 20, 4);

  yc_term_feed_input (term, "\033[5~\033[6~", 8);
  CHECK_UINT ("page up", log.keys[0].type, YC_TERM_KEY_PAGE_UP);
  CHECK_UINT ("page down", log.keys[1].type, YC_TERM_KEY_PAGE_DOWN);

  yc_term_feed_input (term, "\033[H\033[F", 6);
  CHECK_UINT ("home", log.keys[2].type, YC_TERM_KEY_HOME);
  CHECK_UINT ("end", log.keys[3].type, YC_TERM_KEY_END);

  yc_term_feed_input (term, "\033[1~\033[4~", 8);
  CHECK_UINT ("home, numeric form", log.keys[4].type, YC_TERM_KEY_HOME);
  CHECK_UINT ("end, numeric form", log.keys[5].type, YC_TERM_KEY_END);
  yc_term_free (term);
}

/* read() splits escape sequences wherever it likes, so the decoder has
   to hold a partial one until the rest turns up. */
static void
test_escape_split_across_reads (void)
{
  KeyLog log;
  YcTerm *term = new_term (&log, 20, 4);

  yc_term_feed_input (term, "\033", 1);
  CHECK_UINT ("nothing yet after ESC", log.n_keys, 0);
  yc_term_feed_input (term, "[", 1);
  CHECK_UINT ("nothing yet after ESC [", log.n_keys, 0);
  yc_term_feed_input (term, "A", 1);
  CHECK_UINT ("now the arrow arrives", log.n_keys, 1);
  CHECK_UINT ("and it is up", log.keys[0].type, YC_TERM_KEY_UP);

  /* Split in the middle of a numeric sequence too. */
  yc_term_feed_input (term, "\033[5", 3);
  CHECK_UINT ("still waiting", log.n_keys, 1);
  yc_term_feed_input (term, "~", 1);
  CHECK_UINT ("page up completed", log.keys[1].type, YC_TERM_KEY_PAGE_UP);
  yc_term_free (term);
}

/* Several keys in one read, mixing plain and escaped. */
static void
test_batched_input (void)
{
  KeyLog log;
  YcTerm *term = new_term (&log, 20, 4);

  yc_term_feed_input (term, "a\033[Bb\033[Aq", 9);
  CHECK_UINT ("five keys", log.n_keys, 5);
  CHECK_UINT ("a", log.keys[0].ch, 'a');
  CHECK_UINT ("down", log.keys[1].type, YC_TERM_KEY_DOWN);
  CHECK_UINT ("b", log.keys[2].ch, 'b');
  CHECK_UINT ("up", log.keys[3].type, YC_TERM_KEY_UP);
  CHECK_UINT ("q", log.keys[4].ch, 'q');
  yc_term_free (term);
}

/* An ESC followed by an ordinary character is a bare Escape, not the
   start of anything. */
static void
test_bare_escape (void)
{
  KeyLog log;
  YcTerm *term = new_term (&log, 20, 4);

  yc_term_feed_input (term, "\033x", 2);
  CHECK_UINT ("two keys", log.n_keys, 2);
  CHECK_UINT ("escape", log.keys[0].type, YC_TERM_KEY_ESCAPE);
  CHECK_UINT ("then the char", log.keys[1].ch, 'x');
  yc_term_free (term);
}

/* Garbage must not wedge the decoder. */
static void
test_unrecognised_sequence_recovers (void)
{
  KeyLog log;
  YcTerm *term = new_term (&log, 20, 4);

  yc_term_feed_input (term, "\033[Z", 3);         /* shift-tab; ignored */
  yc_term_feed_input (term, "a", 1);
  CHECK_UINT ("still decoding afterwards", log.keys[log.n_keys - 1].ch, 'a');
  yc_term_free (term);
}

int
main (void)
{
  test_row_text ();
  test_clipping ();
  test_attrs_do_not_consume_columns ();
  test_control_characters_are_neutralised ();
  test_pad ();
  test_only_changed_rows_are_written ();
  test_resize_forces_full_repaint ();
  test_rows_past_the_bottom_are_ignored ();
  test_plain_keys ();
  test_control_keys ();
  test_arrow_keys ();
  test_page_and_home_keys ();
  test_escape_split_across_reads ();
  test_batched_input ();
  test_bare_escape ();
  test_unrecognised_sequence_recovers ();

  if (n_failures > 0)
    {
      printf ("\n%u of %u tests FAILED.\n", n_failures, n_tests);
      return 1;
    }
  printf ("all %u tests passed.\n", n_tests);
  return 0;
}
