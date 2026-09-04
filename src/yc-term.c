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

/* Decodes one codepoint.  Returns the bytes consumed, or 0 if what is
   there is not well-formed UTF-8 (truncated, overlong, a stray
   continuation byte, or a surrogate) -- the caller substitutes a
   replacement rather than passing dubious bytes to the terminal. */
static size_t
utf8_decode (const char *text, size_t len, uint32_t *cp_out)
{
  const unsigned char *p = (const unsigned char *) text;
  uint32_t cp;
  size_t need, i;

  if (len == 0)
    return 0;

  if (p[0] < 0x80)
    {
      *cp_out = p[0];
      return 1;
    }
  if (p[0] < 0xc2)               /* continuation byte, or overlong C0/C1 */
    return 0;
  else if (p[0] < 0xe0) { need = 2; cp = p[0] & 0x1f; }
  else if (p[0] < 0xf0) { need = 3; cp = p[0] & 0x0f; }
  else if (p[0] < 0xf5) { need = 4; cp = p[0] & 0x07; }
  else
    return 0;

  if (len < need)
    return 0;
  for (i = 1; i < need; i++)
    {
      if ((p[i] & 0xc0) != 0x80)
        return 0;
      cp = (cp << 6) | (uint32_t) (p[i] & 0x3f);
    }

  /* Overlong forms and UTF-16 surrogates are both ill-formed. */
  if ((need == 3 && cp < 0x800) || (need == 4 && cp < 0x10000))
    return 0;
  if (cp >= 0xd800 && cp <= 0xdfff)
    return 0;

  *cp_out = cp;
  return need;
}

typedef struct { uint32_t first, last; } CpRange;

/* Zero-width: combining marks, and the format characters that a
   terminal advances nothing for.  Not the complete Unicode Mn/Me/Cf
   set -- the blocks that actually turn up in program output. */
static const CpRange zero_width_ranges[] = {
  { 0x0300, 0x036f }, { 0x0483, 0x0489 }, { 0x0591, 0x05bd },
  { 0x05bf, 0x05bf }, { 0x05c1, 0x05c2 }, { 0x05c4, 0x05c5 },
  { 0x05c7, 0x05c7 }, { 0x0610, 0x061a }, { 0x064b, 0x065f },
  { 0x0670, 0x0670 }, { 0x06d6, 0x06dc }, { 0x06df, 0x06e4 },
  { 0x06e7, 0x06e8 }, { 0x06ea, 0x06ed }, { 0x0711, 0x0711 },
  { 0x0730, 0x074a }, { 0x07a6, 0x07b0 }, { 0x07eb, 0x07f3 },
  { 0x0816, 0x0819 }, { 0x081b, 0x0823 }, { 0x0825, 0x0827 },
  { 0x0829, 0x082d }, { 0x0900, 0x0903 }, { 0x093a, 0x093c },
  { 0x0941, 0x0948 }, { 0x094d, 0x094d }, { 0x0951, 0x0957 },
  { 0x0e31, 0x0e31 }, { 0x0e34, 0x0e3a }, { 0x0e47, 0x0e4e },
  { 0x135d, 0x135f }, { 0x1ab0, 0x1aff }, { 0x1dc0, 0x1dff },
  { 0x200b, 0x200f },                       /* ZWSP, ZWNJ, ZWJ, LRM, RLM */
  { 0x2028, 0x202e }, { 0x2060, 0x2064 }, { 0x2066, 0x206f },
  { 0x20d0, 0x20f0 }, { 0x302a, 0x302f }, { 0x3099, 0x309a },
  { 0xfe00, 0xfe0f },                       /* variation selectors */
  { 0xfe20, 0xfe2f },                       /* combining half marks */
  { 0xfeff, 0xfeff },                       /* ZWNBSP / BOM */
  { 0xfff9, 0xfffb },
  { 0xe0100, 0xe01ef }                      /* variation selectors sup. */
};

/* Two columns: East Asian Wide and Fullwidth, plus the emoji blocks
   that terminals almost universally render double-width. */
static const CpRange wide_ranges[] = {
  { 0x1100, 0x115f },                       /* Hangul Jamo initial */
  { 0x2e80, 0x303e }, { 0x3041, 0x33ff },
  { 0x3400, 0x4dbf }, { 0x4e00, 0x9fff },
  { 0xa000, 0xa4cf },                       /* Yi */
  { 0xac00, 0xd7a3 },                       /* Hangul syllables */
  { 0xf900, 0xfaff },                       /* CJK compatibility */
  { 0xfe10, 0xfe19 }, { 0xfe30, 0xfe6f },
  { 0xff00, 0xff60 }, { 0xffe0, 0xffe6 },   /* fullwidth forms */
  { 0x1f300, 0x1f64f }, { 0x1f680, 0x1f6ff },
  { 0x1f900, 0x1f9ff }, { 0x1fa70, 0x1faff },
  { 0x20000, 0x2fffd }, { 0x30000, 0x3fffd }
};

static bool
in_ranges (uint32_t cp, const CpRange *ranges, size_t n)
{
  size_t lo = 0, hi = n;
  while (lo < hi)
    {
      size_t mid = lo + (hi - lo) / 2;
      if (cp < ranges[mid].first)
        hi = mid;
      else if (cp > ranges[mid].last)
        lo = mid + 1;
      else
        return true;
    }
  return false;
}

/* How many columns the terminal will advance for this codepoint.
 *
 * An approximation, and worth being precise about which way: it is
 * per-codepoint, so it gets combining marks, zero-width formatting and
 * East Asian width right, but it has no notion of grapheme clusters.
 * An emoji ZWJ sequence or a regional-indicator flag pair is several
 * codepoints that a terminal draws as one glyph, and this will
 * over-count those.  Getting that right needs cluster segmentation, and
 * terminals disagree about it anyway.
 */
static unsigned
codepoint_columns (uint32_t cp)
{
  if (cp == 0)
    return 0;
  if (in_ranges (cp, zero_width_ranges,
                 sizeof (zero_width_ranges) / sizeof (zero_width_ranges[0])))
    return 0;
  if (in_ranges (cp, wide_ranges,
                 sizeof (wide_ranges) / sizeof (wide_ranges[0])))
    return 2;
  return 1;
}

void
yc_term_row_puts (YcTerm *term, const char *text, size_t len)
{
  Row *row;
  size_t i = 0;

  if (term->current_row >= term->height || len == 0)
    return;
  row = &term->back[term->current_row];

  /* Clip rather than wrap, a codepoint at a time: a wide character
     that would straddle the right edge is dropped rather than half
     drawn, and a combining mark costs nothing so it stays with the
     character it belongs to. */
  while (i < len && row->cols < term->width)
    {
      uint32_t cp = 0;
      size_t n_bytes = utf8_decode (text + i, len - i, &cp);
      unsigned cols;

      /* Ill-formed bytes, control characters and DEL all become a
         single '?': a child must not be able to drive the terminal
         through us, and half a sequence must not reach it either. */
      if (n_bytes == 0 || cp < 0x20 || cp == 0x7f)
        {
          buf_add (&row->bytes, &row->len, &row->alloced, "?", 1);
          row->cols += 1;
          i += n_bytes == 0 ? 1 : n_bytes;
          continue;
        }

      cols = codepoint_columns (cp);
      if (row->cols + cols > term->width)
        break;
      buf_add (&row->bytes, &row->len, &row->alloced, text + i, n_bytes);
      row->cols += cols;
      i += n_bytes;
    }
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
