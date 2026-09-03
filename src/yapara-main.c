/* SPDX-License-Identifier: 0BSD */
#define DEFAULT_MAX_CONCURRENT 10
#define DEFAULT_UI             "plain"

#include <stdio.h>
#include <string.h>
#include "yc-common.h"
#include "yc-child.h"
#include "yc-alloc.h"
#include "yc-shell.h"
#include "yc-ui.h"
#include "yc-cmdline.h"


typedef struct State
{
  FILE *input_fp;
  bool done;
  YcUI *ui;
} State;

/* Reads one line, without its newline; NULL at end-of-file.  Grows to
   fit the line, and does not drop a last line that has no newline --
   job files often end without one. */
char *
stdio_read_line (FILE *fp)
{
  size_t str_alloc = 128;
  size_t str_len = 0;
  char *str = yc_malloc (str_alloc);

  for (;;)
    {
      int c = getc (fp);
      if (c == EOF)
        {
          if (str_len == 0)
            {
              yc_free (str);
              return NULL;
            }
          break;
        }
      if (c == '\n')
        break;
      if (str_len + 1 == str_alloc)
        {
          str_alloc *= 2;
          str = yc_realloc (str, str_alloc);
        }
      str[str_len++] = (char) c;
    }

  str[str_len] = '\0';
  return str;
}

static void
create_child (YcChildContainer *container)
{
  State *state = container->container_data;

  /* Loops so that a blank or commented-out line costs us a slot only
     briefly, rather than ending the run. */
  while (!state->done)
    {
      YcChildCreateInfo *create_info;
      YcShellError shell_error;
      YcChildCreateError child_error;
      char *shell_line = stdio_read_line (state->input_fp);

      if (shell_line == NULL)
        {
          state->done = true;
          return;
        }

      create_info = yc_shell_parse (shell_line,
                                    YC_SHELL_FLAGS_FALLBACK_TO_SH_C,
                                    &shell_error);
      if (create_info == NULL)
        {
          if (shell_error.code == YC_SHELL_ERROR_EMPTY)
            {
              yc_free (shell_line);
              continue;
            }
          yc_die ("%s: %s", shell_line, shell_error.message);
        }

      /* The UI layer takes it from here: it assigns the job index,
         puts stdout/stderr on pipes if this UI shows output, and
         reports the job through the plugin's callbacks. */
      if (yc_ui_spawn (state->ui, container, create_info, shell_line,
                       &child_error) == NULL)
        yc_die ("%s: %s", shell_line, child_error.message);

      yc_shell_free (create_info);
      yc_free (shell_line);        /* yc_ui_spawn() took a copy */
      return;
    }
}

static void
child_container_ready_to_spawn (YcChildContainer *container)
{
  create_child (container);
}

static void
child_container_all_done (YcChildContainer *container)
{
  State *state = container->container_data;
  yc_ui_all_done (state->ui);
}

static void
child_container_destroy (YcChildContainer *container)
{
  State *state = container->container_data;
  if (state->input_fp != NULL && state->input_fp != stdin)
    fclose (state->input_fp);
  state->input_fp = NULL;
}

/* The escape hatch: anything a single backend wants that YcUIOptions
   does not have a field for.  Repeatable, and a later use of the same
   key wins. */
static YC_CMDLINE_CALLBACK_DECLARE (handle_ui_option)
{
  YcUIOptions *ui_options = callback_data;
  return yc_ui_options_parse (ui_options, arg_value, error);
}

/* --ui's help text lists whatever is registered, so a new plugin shows
   up in --help without anything here changing.  yc_cmdline keeps the
   pointer rather than copying, hence the static buffer. */
static const char *
ui_option_description (void)
{
  static char description[1024];
  const YcUIFuncs *const *all;
  size_t n = yc_ui_get_all (&all);
  size_t len, i;

  len = (size_t) snprintf (description, sizeof (description),
                           "user-interface plugin, one of:");
  for (i = 0; i < n && len < sizeof (description); i++)
    len += (size_t) snprintf (description + len, sizeof (description) - len,
                              " %s (%s)%s",
                              all[i]->name,
                              all[i]->description,
                              i + 1 == n ? "" : ";");
  return description;
}

int main(int argc, char **argv)
{
  yc_cmdline_init ("run programs in parallel",
                   "Reads command-lines, one per line, and runs them "
                   "concurrently, at most --max at a time.  Blank lines "
                   "and lines beginning with '#' are skipped.  Simple "
                   "lines are executed directly; anything needing a real "
                   "shell is handed to 'sh -c'.  How the run is presented "
                   "is chosen with --ui.  Exits nonzero if any job "
                   "failed.",
                   NULL,
                   0);

  const char *input_filename = NULL;
  yc_cmdline_add_string ("input", "file with commandlines", "FILENAME",
                         YC_CMDLINE_MANDATORY, &input_filename);

  unsigned max_concurrent = DEFAULT_MAX_CONCURRENT;
  yc_cmdline_add_uint ("max", "max number of concurrent processes",
                       "MAX", 0, &max_concurrent);

  const char *ui_name = DEFAULT_UI;
  yc_cmdline_add_string ("ui", ui_option_description (), "NAME",
                         YC_CMDLINE_PRINT_DEFAULT, &ui_name);

  /* Options shared by every ui, so that '--index-width=6' means the
     same thing whichever one is selected. */
  YcUIOptions ui_options;
  yc_ui_options_init (&ui_options);

  unsigned index_width = 0;
  yc_cmdline_add_uint ("index-width", "pad job indexes to this many digits",
                       "WIDTH", 0, &index_width);

  bool index_zero_pad = false;
  yc_cmdline_add_boolean ("index-zero-pad",
                          "pad job indexes with zeroes rather than spaces",
                          NULL, 0, &index_zero_pad);

  bool index_hex = false;
  yc_cmdline_add_boolean ("index-hex", "print job indexes in hexadecimal",
                          NULL, 0, &index_hex);

  const char *out_dir = NULL;
  yc_cmdline_add_string ("out-dir",
                         "directory for per-job files, "
                         "for those uis that write them",
                         "DIR", 0, &out_dir);

  yc_cmdline_add_func ("ui-option",
                       "pass an arbitrary setting through to the ui",
                       "KEY=VALUE",
                       YC_CMDLINE_TAKES_ARGUMENT | YC_CMDLINE_REPEATABLE,
                       handle_ui_option, &ui_options);

  yc_cmdline_process_args (&argc, &argv);

  ui_options.out_dir = out_dir;
  ui_options.index_format.width = index_width;
  ui_options.index_format.zero_pad = index_zero_pad;
  ui_options.index_format.base = index_hex ? YC_UI_INDEX_HEX
                                           : YC_UI_INDEX_DECIMAL;

  const YcUIFuncs *ui_funcs = yc_ui_lookup (ui_name);
  if (ui_funcs == NULL)
    yc_die ("no such ui '%s' (try --help for the list)", ui_name);

  FILE *input_file = strcmp (input_filename, "-") == 0
                   ? stdin
                   : fopen (input_filename, "r");
  if (input_file == NULL)
    yc_die ("error opening %s: %m", input_filename);

  State state = {
    input_file,
    false,
    NULL                  // ui: set once we have a loop to give it
  };

  YcChildContainerCreationInfo container_creation_info = {
    max_concurrent,
    {
      child_container_ready_to_spawn,
      child_container_all_done,
      child_container_destroy,
    },
    &state,               // container_data
    NULL,                 // loop: NULL gets us a private one
  };

  YcChildContainer *container = yc_child_container_new(&container_creation_info);

  /* A UI may want handles of its own -- a TUI reads the terminal --
     so it gets the container's loop. */
  char *ui_error = NULL;
  state.ui = yc_ui_new (ui_funcs, container->loop, &ui_options, &ui_error);
  if (state.ui == NULL)
    yc_die ("error starting ui '%s': %s", ui_name, ui_error);

  // blocks on main loop until processes finish and callbacks don't add more.
  // ready_to_spawn() is invoked from in there, so there is no need to
  // prime the pump out here.
  yc_child_container_run(container);

  int exit_status = state.ui->n_failed > 0 ? 1 : 0;

  yc_ui_free (state.ui);
  yc_child_container_destroy(container);
  yc_ui_options_clear (&ui_options);

  return exit_status;
}
