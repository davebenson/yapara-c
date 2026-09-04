#include "yc-circular-buffer.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>


static void
test_one_byte_lines (void)
{
  YcCircularBufferNewOptions new_opts = YC_CIRCULAR_BUFFER_NEW_OPTIONS_INIT;
  new_opts.buffer_size = 16;
  new_opts.max_line_length = 2;
  YcCircularBuffer *buf = yc_circular_buffer_new (&new_opts);

  for (unsigned i = 0; i < 16; i++)
    {
      YcCircularBufferStats stats = yc_circular_buffer_get_stats (buf);
      assert (stats.n_bytes == i);
      assert (stats.n_lines == i);
      char line = 'a' + i;
      yc_circular_buffer_add (buf, 1, &line, NULL);
      printf("i=%u\n",i);

      for (unsigned j = 0; j <= i; j++)
        {
          printf("j=%u\n",j);
          char expected = 'a' + i - j;
          YcCircularBufferLine line = yc_circular_buffer_get_line (buf, j);
          assert (line.first_part_len == 1);
          assert (line.first_part[0] == expected);
          assert (line.second_part_len == 0);
          assert (line.second_part == NULL);
          assert (!line.truncated);
        }
    }

  for (unsigned i = 16; i < 26; i++)
    {
      YcCircularBufferStats stats = yc_circular_buffer_get_stats (buf);
      assert (stats.n_bytes == 16);
      assert (stats.n_lines == 16);
      char line = 'a' + i;
      yc_circular_buffer_add (buf, 1, &line, NULL);

      for (unsigned j = 0; j < 16; j++)
        {
          char expected = 'a' + i - j;
          YcCircularBufferLine line = yc_circular_buffer_get_line (buf, j);
          assert (line.first_part_len == 1);
          assert (line.first_part[0] == expected);
          assert (line.second_part_len == 0);
          assert (line.second_part == NULL);
          assert (!line.truncated);
        }
    }
  yc_circular_buffer_free (buf);
}

static void
test_line_truncation (void)
{
  YcCircularBufferNewOptions new_opts = YC_CIRCULAR_BUFFER_NEW_OPTIONS_INIT;
  new_opts.buffer_size = 16;
  new_opts.max_line_length = 8;
  YcCircularBuffer *buf = yc_circular_buffer_new (&new_opts);
  YcCircularBufferStats stats;
  YcCircularBufferLine line;

  // Line 1.
  yc_circular_buffer_add(buf, 10, "0123456789", NULL);
  stats = yc_circular_buffer_get_stats (buf);
  assert (stats.n_lines == 1);
  assert (stats.n_bytes == 8);
  line = yc_circular_buffer_get_line (buf, 0);
  assert (line.first_part_len == 8);
  assert (memcmp (line.first_part, "01234567", 8) == 0);
  assert (line.second_part == NULL);
  assert (line.second_part_len == 0);
  assert (line.truncated);

  // Line 2.
  yc_circular_buffer_add(buf, 10, "abcdefghij", NULL);
  stats = yc_circular_buffer_get_stats (buf);
  assert (stats.n_lines == 2);
  assert (stats.n_bytes == 16);
  line = yc_circular_buffer_get_line (buf, 0);
  assert (line.first_part_len == 8);
  assert (memcmp (line.first_part, "abcdefgh", 8) == 0);
  assert (line.second_part == NULL);
  assert (line.second_part_len == 0);
  assert (line.truncated);
  line = yc_circular_buffer_get_line (buf, 1);
  assert (line.first_part_len == 8);
  assert (memcmp (line.first_part, "01234567", 8) == 0);
  assert (line.second_part == NULL);
  assert (line.second_part_len == 0);
  assert (line.truncated);

  // Line 3.
  yc_circular_buffer_add(buf, 10, "ABCDEFGHIJ", NULL);
  stats = yc_circular_buffer_get_stats (buf);
  assert (stats.n_lines == 2);
  assert (stats.n_bytes == 16);
  line = yc_circular_buffer_get_line (buf, 0);
  assert (line.first_part_len == 8);
  assert (memcmp (line.first_part, "ABCDEFGH", 8) == 0);
  assert (line.second_part == NULL);
  assert (line.second_part_len == 0);
  assert (line.truncated);
  line = yc_circular_buffer_get_line (buf, 1);
  assert (line.first_part_len == 8);
  assert (memcmp (line.first_part, "abcdefgh", 8) == 0);
  assert (line.second_part == NULL);
  assert (line.second_part_len == 0);
  assert (line.truncated);

  yc_circular_buffer_free (buf);
}



int main()
{
  test_one_byte_lines();
  test_line_truncation();
  return 0;
}
