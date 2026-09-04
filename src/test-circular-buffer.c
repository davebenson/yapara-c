/* SPDX-License-Identifier: 0BSD */
/* Tests for yc-circular-buffer.c.
 *
 * Reports pass/fail counts and returns nonzero on failure like the
 * other suites, rather than using assert(): a build with NDEBUG would
 * compile every assert away and the whole file would pass vacuously.
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "yc-circular-buffer.h"

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

#define CHECK_UINT(w, g, e) check_uint (w, (unsigned long long) (g), \
                                        (unsigned long long) (e), __LINE__)
#define CHECK_TRUE(w, g)    check_true (w, g, __LINE__)

/* A line may be stored in two pieces when it straddles the end of the
   ring, so tests compare against the pieces joined back together. */
static void
check_line (const char *what, YcCircularBuffer *buf, size_t age,
            const char *expected, int line)
{
  YcCircularBufferLine got = yc_circular_buffer_get_line (buf, age);
  char joined[512];
  size_t len = 0;

  n_tests++;
  if (got.first_part_len > 0)
    {
      memcpy (joined, got.first_part, got.first_part_len);
      len += got.first_part_len;
    }
  if (got.second_part != NULL && got.second_part_len > 0)
    {
      memcpy (joined + len, got.second_part, got.second_part_len);
      len += got.second_part_len;
    }
  joined[len] = 0;

  if (len != strlen (expected) || memcmp (joined, expected, len) != 0)
    fail (line, "%s (age %u): expected \"%s\", got \"%s\"",
          what, (unsigned) age, expected, joined);
}

#define CHECK_LINE(w, b, age, e) check_line (w, b, age, e, __LINE__)

static YcCircularBuffer *
new_buffer (uint32_t buffer_size, uint32_t max_line_length,
            uint32_t max_lines, uint32_t line_data_size)
{
  YcCircularBufferNewOptions options = YC_CIRCULAR_BUFFER_NEW_OPTIONS_INIT;
  options.buffer_size = buffer_size;
  options.max_line_length = max_line_length;
  options.max_lines = max_lines;
  options.line_data_size = line_data_size;
  return yc_circular_buffer_new (&options);
}

static void
add (YcCircularBuffer *buf, const char *text)
{
  yc_circular_buffer_add (buf, strlen (text), text, NULL);
}

/* --- the original two, with the debug printfs removed --- */

static void
test_one_byte_lines (void)
{
  YcCircularBuffer *buf = new_buffer (16, 2, 256, 0);
  unsigned i, j;

  for (i = 0; i < 16; i++)
    {
      YcCircularBufferStats stats = yc_circular_buffer_get_stats (buf);
      char text[2];

      CHECK_UINT ("filling: n_bytes", stats.n_bytes, i);
      CHECK_UINT ("filling: n_lines", stats.n_lines, i);
      text[0] = (char) ('a' + i);
      yc_circular_buffer_add (buf, 1, text, NULL);

      for (j = 0; j <= i; j++)
        {
          char expected[2];
          expected[0] = (char) ('a' + i - j);
          expected[1] = 0;
          CHECK_LINE ("filling", buf, j, expected);
        }
    }

  /* Full now, so each further line evicts the oldest. */
  for (i = 16; i < 26; i++)
    {
      YcCircularBufferStats stats = yc_circular_buffer_get_stats (buf);
      char text[2];

      CHECK_UINT ("wrapped: n_bytes stays at the cap", stats.n_bytes, 16);
      CHECK_UINT ("wrapped: n_lines stays at the cap", stats.n_lines, 16);
      text[0] = (char) ('a' + i);
      yc_circular_buffer_add (buf, 1, text, NULL);

      for (j = 0; j < 16; j++)
        {
          char expected[2];
          expected[0] = (char) ('a' + i - j);
          expected[1] = 0;
          CHECK_LINE ("wrapped", buf, j, expected);
        }
    }
  yc_circular_buffer_free (buf);
}

static void
test_line_truncation (void)
{
  YcCircularBuffer *buf = new_buffer (16, 8, 256, 0);
  YcCircularBufferStats stats;
  YcCircularBufferLine line;

  add (buf, "0123456789");
  stats = yc_circular_buffer_get_stats (buf);
  CHECK_UINT ("truncation: one line", stats.n_lines, 1);
  CHECK_UINT ("truncation: stored the cap", stats.n_bytes, 8);
  CHECK_LINE ("truncation: first line", buf, 0, "01234567");
  line = yc_circular_buffer_get_line (buf, 0);
  CHECK_TRUE ("truncation: flagged", line.truncated);

  add (buf, "abcdefghij");
  stats = yc_circular_buffer_get_stats (buf);
  CHECK_UINT ("truncation: two lines", stats.n_lines, 2);
  CHECK_UINT ("truncation: 16 bytes", stats.n_bytes, 16);
  CHECK_LINE ("truncation: newest", buf, 0, "abcdefgh");
  CHECK_LINE ("truncation: older", buf, 1, "01234567");

  /* A third line does not fit, so the oldest goes. */
  add (buf, "ABCDEFGHIJ");
  stats = yc_circular_buffer_get_stats (buf);
  CHECK_UINT ("truncation: still two lines", stats.n_lines, 2);
  CHECK_UINT ("truncation: still 16 bytes", stats.n_bytes, 16);
  CHECK_LINE ("truncation: newest again", buf, 0, "ABCDEFGH");
  CHECK_LINE ("truncation: older again", buf, 1, "abcdefgh");

  yc_circular_buffer_free (buf);
}

/* --- added coverage --- */

/* A line whose bytes span the end of the ring comes back in two
   pieces; joining them must give the line back intact.  This is the
   part of the implementation with the most arithmetic in it. */
static void
test_wraparound_splits_a_line (void)
{
  YcCircularBuffer *buf = new_buffer (10, 10, 256, 0);
  YcCircularBufferLine line;
  bool saw_split = false;
  unsigned i;

  /* Walk the write position around the ring so that some line has to
     straddle the join. */
  add (buf, "abcde");            /* occupies 0..4 */
  add (buf, "fgh");              /* occupies 5..7 */
  CHECK_LINE ("wrap: before wrapping (newest)", buf, 0, "fgh");
  CHECK_LINE ("wrap: before wrapping (oldest)", buf, 1, "abcde");

  /* 5 more bytes: only 2 free, so the oldest is evicted and the new
     line starts at 8 and runs over the end. */
  add (buf, "ijklm");
  CHECK_LINE ("wrap: the straddling line", buf, 0, "ijklm");
  line = yc_circular_buffer_get_line (buf, 0);
  if (line.second_part != NULL && line.second_part_len > 0)
    saw_split = true;
  CHECK_TRUE ("wrap: really did come back in two pieces", saw_split);
  CHECK_UINT ("wrap: the pieces add up", line.first_part_len
                                         + line.second_part_len, 5);

  /* Keep going round a few times; every line must still read back. */
  for (i = 0; i < 20; i++)
    {
      char text[4];
      snprintf (text, sizeof (text), "%03u", i);
      add (buf, text);
      CHECK_LINE ("wrap: newest after many turns", buf, 0, text);
    }
  yc_circular_buffer_free (buf);
}

/* max_lines caps the count independently of the byte capacity. */
static void
test_max_lines_cap (void)
{
  YcCircularBuffer *buf = new_buffer (1024, 64, 3, 0);
  YcCircularBufferStats stats;

  add (buf, "one");
  add (buf, "two");
  add (buf, "three");
  stats = yc_circular_buffer_get_stats (buf);
  CHECK_UINT ("max_lines: at the cap", stats.n_lines, 3);

  /* Plenty of bytes free, but no line slots, so the oldest goes. */
  add (buf, "four");
  stats = yc_circular_buffer_get_stats (buf);
  CHECK_UINT ("max_lines: still at the cap", stats.n_lines, 3);
  CHECK_LINE ("max_lines: newest", buf, 0, "four");
  CHECK_LINE ("max_lines: middle", buf, 1, "three");
  CHECK_LINE ("max_lines: oldest", buf, 2, "two");

  yc_circular_buffer_free (buf);
}

/* add() reports how many lines it had to evict, which is how a caller
   knows its own view has shifted. */
static void
test_eviction_count (void)
{
  YcCircularBuffer *buf = new_buffer (10, 10, 256, 0);

  CHECK_UINT ("evicted: nothing yet",
              yc_circular_buffer_add (buf, 5, "aaaaa", NULL), 0);
  CHECK_UINT ("evicted: still room",
              yc_circular_buffer_add (buf, 5, "bbbbb", NULL), 0);
  /* Full: one 5-byte line must go to make room for another. */
  CHECK_UINT ("evicted: one",
              yc_circular_buffer_add (buf, 5, "ccccc", NULL), 1);
  /* A line needing the whole buffer evicts both of the others. */
  CHECK_UINT ("evicted: all of them",
              yc_circular_buffer_add (buf, 10, "dddddddddd", NULL), 2);
  CHECK_UINT ("evicted: one line left",
              yc_circular_buffer_get_stats (buf).n_lines, 1);
  CHECK_LINE ("evicted: and it is the new one", buf, 0, "dddddddddd");

  yc_circular_buffer_free (buf);
}

/* Per-line data rides along with the line -- this is what the two-pane
   UI uses to remember which stream a line came from. */
static void
test_line_data (void)
{
  typedef struct { uint32_t stream; uint32_t serial; } Info;
  YcCircularBuffer *buf = new_buffer (1024, 64, 8, sizeof (Info));
  unsigned i;

  for (i = 0; i < 5; i++)
    {
      Info info;
      char text[16];
      info.stream = i % 2;
      info.serial = 1000 + i;
      snprintf (text, sizeof (text), "line-%u", i);
      yc_circular_buffer_add (buf, strlen (text), text, &info);
    }

  for (i = 0; i < 5; i++)
    {
      /* age 0 is the newest, so age i is line 4-i. */
      YcCircularBufferLine line = yc_circular_buffer_get_line (buf, i);
      const Info *info = line.line_data;
      unsigned which = 4 - i;
      char expected[16];

      snprintf (expected, sizeof (expected), "line-%u", which);
      CHECK_LINE ("line_data: text", buf, i, expected);
      CHECK_TRUE ("line_data: present", info != NULL);
      if (info == NULL)
        continue;
      CHECK_UINT ("line_data: serial travelled with its line",
                  info->serial, 1000 + which);
      CHECK_UINT ("line_data: stream travelled with its line",
                  info->stream, which % 2);
    }

  /* NULL line_data must read back as zeroes rather than stale bytes. */
  {
    YcCircularBufferLine line;
    const Info *info;
    add (buf, "no-info");
    line = yc_circular_buffer_get_line (buf, 0);
    info = line.line_data;
    CHECK_TRUE ("line_data: zeroed when none was given",
                info != NULL && info->stream == 0 && info->serial == 0);
  }

  yc_circular_buffer_free (buf);
}

/* Truncation must not split a UTF-8 sequence when asked not to. */
static void
test_utf8_sensitive_truncation (void)
{
  YcCircularBufferNewOptions options = YC_CIRCULAR_BUFFER_NEW_OPTIONS_INIT;
  YcCircularBuffer *buf;
  YcCircularBufferLine line;

  /* Four 3-byte characters, with a cap of 8 bytes: cutting at 8 would
     land in the middle of the third, so it should stop at 6. */
  options.buffer_size = 64;
  options.max_line_length = 8;
  options.max_lines = 8;
  options.utf8_sensitive_truncation = true;
  buf = yc_circular_buffer_new (&options);

  add (buf, "\xe4\xb8\x80\xe4\xb8\x80\xe4\xb8\x80\xe4\xb8\x80");
  line = yc_circular_buffer_get_line (buf, 0);
  CHECK_TRUE ("utf8: flagged as truncated", line.truncated);
  CHECK_UINT ("utf8: cut on a character boundary",
              line.first_part_len + line.second_part_len, 6);
  CHECK_LINE ("utf8: whole characters only", buf, 0,
              "\xe4\xb8\x80\xe4\xb8\x80");
  yc_circular_buffer_free (buf);

  /* With the check off, the cut is at the byte limit regardless. */
  options.utf8_sensitive_truncation = false;
  buf = yc_circular_buffer_new (&options);
  add (buf, "\xe4\xb8\x80\xe4\xb8\x80\xe4\xb8\x80\xe4\xb8\x80");
  line = yc_circular_buffer_get_line (buf, 0);
  CHECK_UINT ("utf8: unchecked cut is at the byte limit",
              line.first_part_len + line.second_part_len, 8);
  yc_circular_buffer_free (buf);
}

static void
test_empty_and_single (void)
{
  YcCircularBuffer *buf = new_buffer (32, 16, 4, 0);
  YcCircularBufferStats stats;

  stats = yc_circular_buffer_get_stats (buf);
  CHECK_UINT ("empty: no lines", stats.n_lines, 0);
  CHECK_UINT ("empty: no bytes", stats.n_bytes, 0);

  /* A zero-length line is still a line. */
  add (buf, "");
  stats = yc_circular_buffer_get_stats (buf);
  CHECK_UINT ("empty line: counted", stats.n_lines, 1);
  CHECK_UINT ("empty line: no bytes", stats.n_bytes, 0);
  CHECK_LINE ("empty line: reads back empty", buf, 0, "");

  add (buf, "after");
  CHECK_LINE ("empty line: the next one is fine", buf, 0, "after");
  CHECK_LINE ("empty line: and it is still there", buf, 1, "");

  yc_circular_buffer_free (buf);
}

int
main (void)
{
  test_one_byte_lines ();
  test_line_truncation ();
  test_wraparound_splits_a_line ();
  test_max_lines_cap ();
  test_eviction_count ();
  test_line_data ();
  test_utf8_sensitive_truncation ();
  test_empty_and_single ();

  if (n_failures > 0)
    {
      printf ("\n%u of %u tests FAILED.\n", n_failures, n_tests);
      return 1;
    }
  printf ("all %u tests passed.\n", n_tests);
  return 0;
}
