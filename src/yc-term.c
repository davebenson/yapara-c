/* SPDX-License-Identifier: 0BSD */
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "yc-common.h"
#include "yc-alloc.h"
#include "yc-term.h"

#define DEFAULT_WIDTH   80
#define DEFAULT_HEIGHT  24
#define MAX_PENDING     32       /* longest escape sequence we care about */

/* One row of a frame: the bytes to send, plus how many columns of that
   are actually visible.  Keeping the count as we build is what lets
   puts be clipped without having to parse escapes back out. */
typedef struct {
  char *bytes;
  size_t len, alloced;
  unsigned cols;
} Row;

struct YcTerm {
  uv_loop_t *loop;
  YcTermCallbacks callbacks;
  void *user_data;

  unsigned width, height;

  Row *back, *front;
  size_t n_rows_alloced;
  unsigned current_row;
  bool in_frame;
  unsigned rows_written;

  /* Terminal ownership; all unset when headless. */
  bool headless;
  bool tty_ready;              /* raw mode and alt screen are in effect */
  uv_tty_t tty_in;
  bool tty_in_open;
  uv_signal_t winch;
  bool winch_open;

  /* Escape sequences do not arrive whole. */
  char pending[MAX_PENDING];
  size_t n_pending;

  /* Assembled once per frame, written in one go. */
  char *out;
  size_t out_len, out_alloced;
};

/* --- byte buffers --- */

static void
buf_add (char **data, size_t *len, size_t *alloced,
         const char *text, size_t n)
{
  if (*len + n + 1 > *alloced)
    {
      *alloced = *alloced == 0 ? 128 : *alloced;
      while (*len + n + 1 > *alloced)
        *alloced *= 2;
      *data = YC_RENEW (char, *data, *alloced);
    }
  memcpy (*data + *len, text, n);
  *len += n;
  (*data)[*len] = 0;
}

static void
out_add (YcTerm *term, const char *text, size_t n)
{
  buf_add (&term->out, &term->out_len, &term->out_alloced, text, n);
}

static void
out_addz (YcTerm *term, const char *text)
{
  out_add (term, text, strlen (text));
}

/* --- rows --- */

static void
rows_resize (YcTerm *term, unsigned height)
{
  size_t i;

  if (height <= term->n_rows_alloced)
    return;
  term->back = YC_RENEW (Row, term->back, height);
  term->front = YC_RENEW (Row, term->front, height);
  for (i = term->n_rows_alloced; i < height; i++)
    {
      memset (&term->back[i], 0, sizeof (Row));
      memset (&term->front[i], 0, sizeof (Row));
    }
  term->n_rows_alloced = height;
}

static void
row_reset (Row *row)
{
  row->len = 0;
  row->cols = 0;
  if (row->bytes != NULL)
    row->bytes[0] = 0;
}

/* --- geometry --- */

unsigned
yc_term_width (YcTerm *term)
{
  return term->width;
}

unsigned
yc_term_height (YcTerm *term)
{
  return term->height;
}

void
yc_term_set_size (YcTerm *term, unsigned width, unsigned height)
{
  if (width == 0)
    width = DEFAULT_WIDTH;
  if (height == 0)
    height = DEFAULT_HEIGHT;
  if (width == term->width && height == term->height)
    return;

  term->width = width;
  term->height = height;
  rows_resize (term, height);

  /* The screen is not what the front buffer says any more, so make
     every row differ and force a full repaint next frame. */
  {
    size_t i;
    for (i = 0; i < term->n_rows_alloced; i++)
      row_reset (&term->front[i]);
  }
}

static void
refresh_size_from_tty (YcTerm *term)
{
  int width = DEFAULT_WIDTH, height = DEFAULT_HEIGHT;
  if (uv_tty_get_winsize (&term->tty_in, &width, &height) != 0
   || width <= 0 || height <= 0)
    {
      width = DEFAULT_WIDTH;
      height = DEFAULT_HEIGHT;
    }
  yc_term_set_size (term, (unsigned) width, (unsigned) height);
}

/* --- frames --- */

void
yc_term_begin_frame (YcTerm *term)
{
  size_t i;
  for (i = 0; i < term->height; i++)
    row_reset (&term->back[i]);
  term->in_frame = true;
  term->current_row = 0;
}

void
yc_term_row_begin (YcTerm *term, unsigned row)
{
  term->current_row = row;
  if (row < term->height)
    row_reset (&term->back[row]);
}

void
yc_term_row_attr (YcTerm *term, const char *sgr)
{
  Row *row;
  if (term->current_row >= term->height || sgr == NULL || *sgr == 0)
    return;
  row = &term->back[term->current_row];
  /* Contributes no columns, which is the whole reason cols is tracked
     separately from len. */
  buf_add (&row->bytes, &row->len, &row->alloced, sgr, strlen (sgr));
}

/* Counts a UTF-8 lead byte as one column and continuation bytes as
   none.  Wide characters still count as one, which is wrong for CJK
   but keeps this from needing a width table. */
static unsigned
utf8_columns (const char *text, size_t len)
{
  unsigned cols = 0;
  size_t i;
  for (i = 0; i < len; i++)
    if (((unsigned char) text[i] & 0xc0) != 0x80)
      cols++;
  return cols;
}

void
yc_term_row_puts (YcTerm *term, const char *text, size_t len)
{
  Row *row;
  size_t i, take = 0;
  unsigned room, used = 0;

  if (term->current_row >= term->height || len == 0)
    return;
  row = &term->back[term->current_row];
  if (row->cols >= term->width)
    return;
  room = term->width - row->cols;

  /* Clip rather than wrap, and never split a UTF-8 sequence: walk
     until the next lead byte would exceed the room left. */
  for (i = 0; i < len; i++)
    {
      unsigned char c = (unsigned char) text[i];
      bool is_lead = (c & 0xc0) != 0x80;
      if (is_lead)
        {
          if (used == room)
            break;
          used++;
        }
      /* Control characters would move the cursor or start an escape of
         their own, so they are not passed through. */
      take = i + 1;
    }

  if (take == 0)
    return;

  /* Replace anything below space (and DEL) so a child cannot drive the
     terminal through us. */
  {
    char *clean = yc_malloc (take);
    for (i = 0; i < take; i++)
      {
        unsigned char c = (unsigned char) text[i];
        clean[i] = (c < 0x20 || c == 0x7f) ? '?' : text[i];
      }
    buf_add (&row->bytes, &row->len, &row->alloced, clean, take);
    yc_free (clean);
  }
  row->cols += utf8_columns (text, take);
}

void
yc_term_row_printf (YcTerm *term, const char *format, ...)
{
  char text[1024];
  va_list args;
  int n;

  va_start (args, format);
  n = vsnprintf (text, sizeof (text), format, args);
  va_end (args);
  if (n > 0)
    yc_term_row_puts (term, text,
                      (size_t) n < sizeof (text) ? (size_t) n
                                                 : sizeof (text) - 1);
}

void
yc_term_row_pad (YcTerm *term, unsigned to_col)
{
  Row *row;
  if (term->current_row >= term->height)
    return;
  row = &term->back[term->current_row];
  if (to_col > term->width)
    to_col = term->width;
  while (row->cols < to_col)
    {
      buf_add (&row->bytes, &row->len, &row->alloced, " ", 1);
      row->cols++;
    }
}

void
yc_term_row_end (YcTerm *term)
{
  /* Attributes must not leak into the next row. */
  yc_term_row_attr (term, YC_TERM_RESET);
}

static bool
rows_differ (const Row *a, const Row *b)
{
  if (a->len != b->len)
    return true;
  if (a->len == 0)
    return false;
  return memcmp (a->bytes, b->bytes, a->len) != 0;
}

void
yc_term_end_frame (YcTerm *term)
{
  unsigned row;
  char move[32];

  term->in_frame = false;
  term->rows_written = 0;
  term->out_len = 0;
  if (term->out != NULL)
    term->out[0] = 0;

  for (row = 0; row < term->height; row++)
    {
      Row *back = &term->back[row];
      Row *front = &term->front[row];

      if (!rows_differ (back, front))
        continue;

      /* Only the rows that changed, which is what keeps a 30Hz repaint
         from being a screenful of traffic every tick. */
      snprintf (move, sizeof (move), "\033[%u;1H", row + 1);
      out_addz (term, move);
      if (back->len > 0)
        out_add (term, back->bytes, back->len);
      out_addz (term, "\033[K");        /* clear whatever was longer */
      term->rows_written++;

      front->len = 0;
      front->cols = 0;
      if (back->len > 0)
        buf_add (&front->bytes, &front->len, &front->alloced,
                 back->bytes, back->len);
      else if (front->bytes != NULL)
        front->bytes[0] = 0;
      front->cols = back->cols;
    }

  if (!term->headless && term->tty_ready && term->out_len > 0)
    {
      size_t done = 0;
      while (done < term->out_len)
        {
          ssize_t n = write (STDOUT_FILENO, term->out + done,
                             term->out_len - done);
          if (n > 0)
            done += (size_t) n;
          else if (n < 0 && errno == EINTR)
            continue;
          else
            break;
        }
    }
}

const char *
yc_term_frame_row_raw (YcTerm *term, unsigned row)
{
  if (row >= term->height || term->front[row].bytes == NULL)
    return "";
  return term->front[row].bytes;
}

/* Strips the SGR escapes back out, so a test can assert on what the
   row says rather than on how it is coloured. */
const char *
yc_term_frame_row (YcTerm *term, unsigned row)
{
  static char text[4096];
  const char *raw = yc_term_frame_row_raw (term, row);
  size_t out = 0, i = 0, len = strlen (raw);

  while (i < len && out + 1 < sizeof (text))
    {
      if (raw[i] == '\033')
        {
          /* Skip to the end of the CSI/SGR sequence. */
          while (i < len && raw[i] != 'm' && raw[i] != 'K' && raw[i] != 'H')
            i++;
          if (i < len)
            i++;
          continue;
        }
      text[out++] = raw[i++];
    }
  while (out > 0 && text[out - 1] == ' ')
    out--;
  text[out] = 0;
  return text;
}

unsigned
yc_term_rows_written (YcTerm *term)
{
  return term->rows_written;
}

/* --- keys --- */

static void
emit_key (YcTerm *term, YcTermKeyType type, uint32_t ch, bool ctrl)
{
  YcTermKey key;
  key.type = type;
  key.ch = ch;
  key.ctrl = ctrl;
  if (term->callbacks.key != NULL)
    term->callbacks.key (term, &key, term->user_data);
}

/* Tries to consume one key from the front of 'pending'.  Returns the
   number of bytes used, or 0 if what is there could still become a
   longer sequence and should be held for the next read. */
static size_t
decode_one (YcTerm *term, const char *p, size_t len)
{
  if (len == 0)
    return 0;

  if (p[0] != '\033')
    {
      unsigned char c = (unsigned char) p[0];
      if (c == '\r' || c == '\n')
        emit_key (term, YC_TERM_KEY_ENTER, 0, false);
      else if (c == 0x7f || c == 0x08)
        emit_key (term, YC_TERM_KEY_BACKSPACE, 0, false);
      else if (c < 0x20)
        emit_key (term, YC_TERM_KEY_CHAR, (uint32_t) (c + 'a' - 1), true);
      else
        emit_key (term, YC_TERM_KEY_CHAR, c, false);
      return 1;
    }

  /* A lone ESC is ambiguous until the next byte arrives; hold it. */
  if (len == 1)
    return 0;

  if (p[1] != '[' && p[1] != 'O')
    {
      emit_key (term, YC_TERM_KEY_ESCAPE, 0, false);
      return 1;
    }
  if (len == 2)
    return 0;

  switch (p[2])
    {
    case 'A': emit_key (term, YC_TERM_KEY_UP, 0, false);    return 3;
    case 'B': emit_key (term, YC_TERM_KEY_DOWN, 0, false);  return 3;
    case 'C': emit_key (term, YC_TERM_KEY_RIGHT, 0, false); return 3;
    case 'D': emit_key (term, YC_TERM_KEY_LEFT, 0, false);  return 3;
    case 'H': emit_key (term, YC_TERM_KEY_HOME, 0, false);  return 3;
    case 'F': emit_key (term, YC_TERM_KEY_END, 0, false);   return 3;
    default:
      break;
    }

  /* ESC [ <digits> ~ */
  if (p[2] >= '0' && p[2] <= '9')
    {
      size_t i = 2;
      unsigned value = 0;
      while (i < len && p[i] >= '0' && p[i] <= '9')
        value = value * 10 + (unsigned) (p[i++] - '0');
      if (i == len)
        return 0;                /* still arriving */
      if (p[i] != '~')
        return i + 1;            /* something we do not handle; drop it */
      switch (value)
        {
        case 1: case 7: emit_key (term, YC_TERM_KEY_HOME, 0, false); break;
        case 4: case 8: emit_key (term, YC_TERM_KEY_END, 0, false); break;
        case 5: emit_key (term, YC_TERM_KEY_PAGE_UP, 0, false); break;
        case 6: emit_key (term, YC_TERM_KEY_PAGE_DOWN, 0, false); break;
        default: break;
        }
      return i + 1;
    }

  return 3;                      /* unrecognised three-byte sequence */
}

void
yc_term_feed_input (YcTerm *term, const void *data, size_t len)
{
  const char *bytes = data;
  size_t i;

  for (i = 0; i < len; i++)
    {
      if (term->n_pending == MAX_PENDING)
        term->n_pending = 0;     /* garbage; resynchronise */
      term->pending[term->n_pending++] = bytes[i];

      for (;;)
        {
          size_t used = decode_one (term, term->pending, term->n_pending);
          if (used == 0)
            break;
          memmove (term->pending, term->pending + used,
                   term->n_pending - used);
          term->n_pending -= used;
          if (term->n_pending == 0)
            break;
        }
    }
}

/* --- the tty --- */

static void
alloc_read_buffer (uv_handle_t *handle, size_t suggested, uv_buf_t *buf)
{
  static char storage[1024];
  buf->base = storage;
  buf->len = sizeof (storage);
}

static void
on_tty_read (uv_stream_t *stream, ssize_t n_read, const uv_buf_t *buf)
{
  YcTerm *term = stream->data;

  if (n_read > 0)
    yc_term_feed_input (term, buf->base, (size_t) n_read);
  else if (n_read < 0)
    yc_term_stop (term);
}

static void
on_winch (uv_signal_t *signal, int signum)
{
  YcTerm *term = signal->data;

  refresh_size_from_tty (term);
  if (term->callbacks.resize != NULL)
    term->callbacks.resize (term, term->user_data);
}

static YcTerm *
term_alloc (const YcTermCallbacks *callbacks, void *user_data)
{
  YcTerm *term = YC_NEW0 (YcTerm);
  if (callbacks != NULL)
    term->callbacks = *callbacks;
  term->user_data = user_data;
  term->width = DEFAULT_WIDTH;
  term->height = DEFAULT_HEIGHT;
  rows_resize (term, term->height);
  return term;
}

YcTerm *
yc_term_new_headless (unsigned width, unsigned height,
                      const YcTermCallbacks *callbacks, void *user_data)
{
  YcTerm *term = term_alloc (callbacks, user_data);
  term->headless = true;
  yc_term_set_size (term, width, height);
  return term;
}

YcTerm *
yc_term_new (uv_loop_t *loop,
             const YcTermCallbacks *callbacks,
             void *user_data,
             char **error_message)
{
  YcTerm *term;
  int err;

  if (!isatty (STDIN_FILENO) || !isatty (STDOUT_FILENO))
    {
      if (error_message != NULL)
        *error_message =
          yc_strdup ("this ui needs a terminal on stdin and stdout");
      return NULL;
    }

  term = term_alloc (callbacks, user_data);
  term->loop = loop;

  err = uv_tty_init (loop, &term->tty_in, STDIN_FILENO, 1);
  if (err != 0)
    {
      if (error_message != NULL)
        {
          *error_message = yc_malloc (128);
          snprintf (*error_message, 128, "uv_tty_init: %s", uv_strerror (err));
        }
      yc_free (term);
      return NULL;
    }
  term->tty_in_open = true;
  term->tty_in.data = term;

  err = uv_tty_set_mode (&term->tty_in, UV_TTY_MODE_RAW);
  if (err != 0)
    {
      if (error_message != NULL)
        {
          *error_message = yc_malloc (128);
          snprintf (*error_message, 128, "raw mode: %s", uv_strerror (err));
        }
      yc_term_free (term);
      return NULL;
    }

  refresh_size_from_tty (term);

  /* Alternate screen, so the scrollback the user had is still there
     when we put the screen back. */
  {
    static const char enter[] = "\033[?1049h\033[?25l\033[2J";
    ssize_t ignored = write (STDOUT_FILENO, enter, sizeof (enter) - 1);
    (void) ignored;
  }
  term->tty_ready = true;

  uv_signal_init (loop, &term->winch);
  term->winch.data = term;
  term->winch_open = true;
  uv_signal_start (&term->winch, on_winch, SIGWINCH);

  uv_read_start ((uv_stream_t *) &term->tty_in, alloc_read_buffer,
                 on_tty_read);
  return term;
}

void
yc_term_stop (YcTerm *term)
{
  if (term->tty_in_open)
    uv_read_stop ((uv_stream_t *) &term->tty_in);
  if (term->winch_open)
    {
      uv_signal_stop (&term->winch);
      uv_close ((uv_handle_t *) &term->winch, NULL);
      term->winch_open = false;
    }
  if (term->tty_in_open)
    {
      uv_close ((uv_handle_t *) &term->tty_in, NULL);
      term->tty_in_open = false;
    }
}

void
yc_term_free (YcTerm *term)
{
  size_t i;

  if (term == NULL)
    return;

  if (term->tty_ready)
    {
      static const char leave[] = "\033[?25h\033[?1049l";
      ssize_t ignored;
      uv_tty_reset_mode ();
      ignored = write (STDOUT_FILENO, leave, sizeof (leave) - 1);
      (void) ignored;
      term->tty_ready = false;
    }
  yc_term_stop (term);

  for (i = 0; i < term->n_rows_alloced; i++)
    {
      yc_free (term->back[i].bytes);
      yc_free (term->front[i].bytes);
    }
  yc_free (term->back);
  yc_free (term->front);
  yc_free (term->out);
  yc_free (term);
}
