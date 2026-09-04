
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>


// An efficient circular buffer for lines of text.
//
// There are size constraints imposed so that we
// never allocate memory during its lifetime:
//   - the max number of bytes of character data to hold (buffer_size)
//   - the max number of lines to preserve (max_lines)
//   - the max line length (max_line_length)
//
// We require that max_line_length <= buffer_size.
//
// If line truncation is required, we optionally ensure
// that valid UTF-8 is only truncated and unicode codepoint boundaries.
//
// Note that we don't support or use NUL-termination,
// all strings must have their lengths given.
//

typedef struct YcCircularBuffer YcCircularBuffer;
typedef struct YcCircularBufferNewOptions YcCircularBufferNewOptions;

// Options for configuring the buffer.
struct YcCircularBufferNewOptions {
  uint32_t buffer_size;
  uint32_t max_line_length;
  uint32_t max_lines;
  uint32_t line_data_size;
  bool utf8_sensitive_truncation;
};

// Default bufferr values.
#define YC_CIRCULAR_BUFFER_NEW_OPTIONS_INIT (YcCircularBufferNewOptions) { \
  64 * 1024,    \
  1024,         \
  256,          \
  0,            \
  true          \
}

typedef struct YcCircularBufferStats 
{
  size_t n_lines;
  size_t n_bytes;
} YcCircularBufferStats;


typedef struct YcCircularBufferLine
{
  const char *first_part;
  size_t first_part_len;
  const char *second_part;      // null if only one part
  size_t second_part_len;       // 0 if only one part
  bool truncated;
  const void *line_data;
} YcCircularBufferLine;

YcCircularBuffer *yc_circular_buffer_new (YcCircularBufferNewOptions *new_options);

// Returns the number of lines evicted.
unsigned          yc_circular_buffer_add (YcCircularBuffer *buffer,
                                          size_t len,
                                          const char *text,
                                          const void *line_data);

// line_age==0 is the most recent line.
YcCircularBufferLine
                  yc_circular_buffer_get_line
                                         (YcCircularBuffer *buf,
                                          size_t line_age);


YcCircularBufferStats yc_circular_buffer_get_stats (YcCircularBuffer *buf);

void yc_circular_buffer_free (YcCircularBuffer *buffer);
