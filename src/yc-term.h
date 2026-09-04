/* SPDX-License-Identifier: 0BSD */
/* A small terminal layer: raw mode, keys, and a diffed frame buffer.
 *
 * Knows nothing about yapara.  It exists so that a full-screen UI can
 * be written as "build a frame, read keys" without any escape
 * sequences in it, and -- more to the point -- so that it can be
 * tested without a terminal.  yc_term_new_headless() renders into
 * memory and takes fed keystrokes, so a UI's layout and key handling
 * are assertable byte-for-byte.
 *
 * A frame is built a row at a time, left to right:
 *
 *     yc_term_begin_frame (t);
 *     yc_term_row_begin (t, 0);
 *     yc_term_row_attr (t, YC_TERM_REVERSE);
 *     yc_term_row_puts (t, "header", 6);
 *     yc_term_row_pad (t, yc_term_width (t));
 *     yc_term_row_end (t);
 *     ...
 *     yc_term_end_frame (t);      // writes only the rows that changed
 *
 * Puts past the right edge are clipped rather than wrapping, so a long
 * line cannot shove the rest of the layout down the screen.
 */

#ifndef YC_TERM_H_
#define YC_TERM_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <uv.h>

typedef struct YcTerm YcTerm;

/* SGR sequences, for yc_term_row_attr().  Plain strings so that a UI
   can pass its own (yc_ui_job_color() returns one of these). */
#define YC_TERM_RESET      "\033[0m"
#define YC_TERM_REVERSE    "\033[7m"
#define YC_TERM_BOLD       "\033[1m"
#define YC_TERM_DIM        "\033[2m"

typedef enum {
  YC_TERM_KEY_CHAR,            /* 'ch' holds it; 'ctrl' if it was ^X */
  YC_TERM_KEY_UP,
  YC_TERM_KEY_DOWN,
  YC_TERM_KEY_LEFT,
  YC_TERM_KEY_RIGHT,
  YC_TERM_KEY_PAGE_UP,
  YC_TERM_KEY_PAGE_DOWN,
  YC_TERM_KEY_HOME,
  YC_TERM_KEY_END,
  YC_TERM_KEY_ENTER,
  YC_TERM_KEY_BACKSPACE,
  YC_TERM_KEY_ESCAPE
} YcTermKeyType;

typedef struct {
  YcTermKeyType type;
  uint32_t ch;                 /* CHAR only */
  bool ctrl;
} YcTermKey;

typedef struct {
  void (*key)    (YcTerm *term, const YcTermKey *key, void *user_data);
  void (*resize) (YcTerm *term, void *user_data);
} YcTermCallbacks;

/* Takes over the terminal: raw mode, alternate screen, cursor hidden.
 * NULL with *error_message set (caller frees) if stdout is not a tty
 * or raw mode is refused. */
YcTerm *yc_term_new (uv_loop_t *loop,
                     const YcTermCallbacks *callbacks,
                     void *user_data,
                     char **error_message);

/* Renders into memory instead, for tests: no tty, no escapes written
 * anywhere, and keys arrive only from yc_term_feed_input(). */
YcTerm *yc_term_new_headless (unsigned width, unsigned height,
                              const YcTermCallbacks *callbacks,
                              void *user_data);

/* Puts the terminal back the way it was found. */
void yc_term_free (YcTerm *term);

/* Stops reading keys, so a loop with nothing else left can finish. */
void yc_term_stop (YcTerm *term);

unsigned yc_term_width  (YcTerm *term);
unsigned yc_term_height (YcTerm *term);

void yc_term_begin_frame (YcTerm *term);
void yc_term_row_begin   (YcTerm *term, unsigned row);
void yc_term_row_attr    (YcTerm *term, const char *sgr);
void yc_term_row_puts    (YcTerm *term, const char *text, size_t len);
void yc_term_row_printf  (YcTerm *term, const char *format, ...);
void yc_term_row_pad     (YcTerm *term, unsigned to_col);
void yc_term_row_end     (YcTerm *term);
void yc_term_end_frame   (YcTerm *term);

/* --- for tests --- */

/* Decodes 'data' as if it had arrived from the terminal, delivering
 * keys through the callback.  Escape sequences split across calls are
 * held and completed, which is what a real read() does to them. */
void yc_term_feed_input (YcTerm *term, const void *data, size_t len);

/* The visible text of a row of the last completed frame, with the
 * attribute escapes removed and trailing blanks trimmed. */
const char *yc_term_frame_row (YcTerm *term, unsigned row);

/* The row's bytes as they would go to the terminal, escapes included. */
const char *yc_term_frame_row_raw (YcTerm *term, unsigned row);

/* How many rows end_frame() actually rewrote, which is what says the
 * diffing is doing its job. */
unsigned yc_term_rows_written (YcTerm *term);

void yc_term_set_size (YcTerm *term, unsigned width, unsigned height);

#endif
