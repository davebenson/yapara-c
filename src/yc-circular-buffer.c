#include "yc-circular-buffer.h"
#include "yc-alloc.h"
#include "yc-common.h"
#include <string.h>
#include <stdio.h>

typedef struct YcCircularBufferLineInfo {
  uint32_t is_truncated : 1;
  uint32_t line_start : 31;
} YcCircularBufferLineInfo;

struct YcCircularBuffer
{
  uint32_t buffer_size;
  char *buf;
  uint32_t write_pos;
  uint32_t max_lines;
  uint32_t max_line_length;
  uint32_t write_line_index;
  YcCircularBufferLineInfo *lines;
  char *line_data;              // user-provided line-data
  uint32_t buf_occupancy;
  uint32_t num_lines;
  uint32_t line_data_size;
  bool utf8_sensitive_truncation;
};

YcCircularBuffer *yc_circular_buffer_new (YcCircularBufferNewOptions *new_options)
{
  YcCircularBuffer *rv = YC_NEW (YcCircularBuffer);
  assert (new_options->max_line_length <= new_options->buffer_size);
  rv->buffer_size = new_options->buffer_size;
  rv->buf = yc_malloc (new_options->buffer_size);
  rv->max_lines = new_options->max_lines;
  rv->max_line_length = new_options->max_line_length;
  rv->lines = yc_malloc (sizeof (YcCircularBufferLineInfo) * new_options->max_lines);
  rv->line_data = yc_malloc (new_options->line_data_size * new_options->max_lines);
  rv->write_line_index = 0;
  rv->write_pos = 0;
  rv->buf_occupancy = 0;
  rv->num_lines = 0;
  rv->line_data_size = new_options->line_data_size;
  rv->utf8_sensitive_truncation = new_options->utf8_sensitive_truncation;
  return rv;
}

static inline bool is_utf8_initial_char (char c)
{
  uint8_t b = c;
  if ((c & 0x80) == 0)
    return true;
  else
    return (c & 0xc0) == 0xc0;
}

unsigned          yc_circular_buffer_add (YcCircularBuffer *buffer,
                                          size_t len, const char *text,
                                          const void *line_data)
{
  unsigned n_evicted = 0;
  bool truncated = false;
  if (len > buffer->max_line_length)
    {
      truncated = true;
      len = buffer->max_line_length;
      if (buffer->utf8_sensitive_truncation)
        {
          while (len > 0 && !is_utf8_initial_char (text[len]))
            len--;
        }
    }

  while (len + buffer->buf_occupancy > buffer->buffer_size || buffer->max_lines == buffer->num_lines)
    {
      //
      // drop oldest line
      //
      assert(buffer->num_lines > 0);

      // oldest_line_start == (write_line_index - num_lines) MOD max_lines.
      unsigned oldest_line_index = buffer->write_line_index + buffer->max_lines - buffer->num_lines;
      if (oldest_line_index >= buffer->max_lines)
        oldest_line_index -= buffer->max_lines;

      uint32_t oldest_line_start = buffer->lines[oldest_line_index].line_start;

      // the end of the line is the start of the next,
      // of the write_pos, if there's only one line left.
      uint32_t oldest_line_end = (buffer->num_lines == 1)
                               ? buffer->write_pos
                               : oldest_line_index + 1 == buffer->max_lines
                               ? buffer->lines[0].line_start
                               : buffer->lines[oldest_line_index+1].line_start;

      // length = (end - start) MOD buffer_size.
      //
      // start==end is ambiguous: the line is either empty or fills the
      // ring exactly.  With one line left its length is simply the
      // occupancy -- and taking it as 0 would leave the occupancy
      // unchanged and spin this loop forever.
      uint32_t oldest_line_len;
      if (oldest_line_start == oldest_line_end)
        oldest_line_len = buffer->num_lines == 1 ? buffer->buf_occupancy : 0;
      else if (oldest_line_start < oldest_line_end)
        oldest_line_len = oldest_line_end - oldest_line_start;
      else
        oldest_line_len = oldest_line_end + buffer->buffer_size
                        - oldest_line_start;

      buffer->num_lines -= 1;
      buffer->buf_occupancy -= oldest_line_len;
      n_evicted += 1;
    }

  // Add new line record.  Keep the slot: write_line_index moves on
  // below, and the associated data has to land in the *same* slot as
  // the record, which is where get_line() looks for it.
  unsigned line_index = buffer->write_line_index++;
  buffer->lines[line_index] = (YcCircularBufferLineInfo) {
    truncated,
    buffer->write_pos
  };

  // And associated data.
  if (buffer->line_data_size > 0)
    {
      char *dst = buffer->line_data + line_index * buffer->line_data_size;
      if (line_data)
        memcpy (dst, line_data, buffer->line_data_size);
      else
        memset (dst, 0, buffer->line_data_size);
    }
        
  // Finally update the write_line_index.
  if (buffer->write_line_index == buffer->max_lines)
    buffer->write_line_index = 0;
  buffer->num_lines += 1;

  // Write the line, possibly in two parts.
  if (buffer->write_pos + len < buffer->buffer_size)
    {
      memcpy (buffer->buf + buffer->write_pos, text, len);
      buffer->write_pos += len;
    }
  else if (buffer->write_pos + len == buffer->buffer_size)
    {
      memcpy (buffer->buf + buffer->write_pos, text, len);
      buffer->write_pos = 0;
    }
  else
    {
      size_t piece1_size = buffer->buffer_size - buffer->write_pos;
      size_t piece2_size = len - piece1_size;
      memcpy (buffer->buf + buffer->write_pos, text, piece1_size);
      memcpy (buffer->buf, text + piece1_size, piece2_size);
      buffer->write_pos = piece2_size;
    }
  buffer->buf_occupancy += len;

  return n_evicted;
}

YcCircularBufferLine
yc_circular_buffer_get_line (YcCircularBuffer *buf,
                             size_t idx)
{
  assert (idx < buf->num_lines);

  unsigned arr_idx = buf->write_line_index + buf->max_lines - idx - 1;
  if (arr_idx >= buf->max_lines)
    arr_idx -= buf->max_lines;

  size_t start = buf->lines[arr_idx].line_start;
  size_t end = idx == 0 ? buf->write_pos
    : arr_idx + 1 == buf->max_lines
    ? buf->lines[0].line_start
    : buf->lines[arr_idx+1].line_start;

  const void *line_data = buf->line_data + arr_idx * buf->line_data_size;
  bool truncated = buf->lines[arr_idx].is_truncated;

  // start==end means empty, or exactly the whole ring.  Only a lone
  // line can be the latter, and then its length is the occupancy.
  if (start == end && buf->num_lines == 1 && buf->buf_occupancy > 0)
    {
      if (start + buf->buf_occupancy <= buf->buffer_size)
        return (YcCircularBufferLine) {
          buf->buf + start, buf->buf_occupancy, NULL, 0, truncated, line_data
        };
      return (YcCircularBufferLine) {
        buf->buf + start, buf->buffer_size - start,
        buf->buf, buf->buf_occupancy - (buf->buffer_size - start),
        truncated, line_data
      };
    }

  if (start <= end)
    {
      return (YcCircularBufferLine) {
        buf->buf + start,
        end - start,
        NULL,
        0,
        truncated,
        line_data
      };
    }
  else if (end == 0)
    {
      return (YcCircularBufferLine) {
        buf->buf + start,
        buf->buffer_size - start,
        NULL,
        0,
        truncated,
        line_data
      };
    }
  else
    {
      return (YcCircularBufferLine) {
        buf->buf + start,
        buf->buffer_size - start,
        buf->buf,
        end,
        truncated,
        line_data
      };
    }
}


YcCircularBufferStats
yc_circular_buffer_get_stats (YcCircularBuffer *buf)
{
  return (YcCircularBufferStats) {
    buf->num_lines,
    buf->buf_occupancy
  };
}

void yc_circular_buffer_free (YcCircularBuffer *buffer)
{
  yc_free (buffer->buf);
  yc_free (buffer->lines);
  yc_free (buffer->line_data);
  yc_free (buffer);
}
