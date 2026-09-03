/* SPDX-License-Identifier: 0BSD */
#include <string.h>
#include <stdlib.h>
#include <alloca.h>
#include <stdio.h>
#include <stdarg.h>
#include "yc-rbtree-macros.h"
#include "yc-common.h"
#include "yc-alloc.h"
#include "yc-buffer.h"
#include "yc-cmdline.h"

typedef struct _YcCmdlineMode YcCmdlineMode;
typedef struct _YcCmdlineArg YcCmdlineArg;
typedef struct _ExclNode ExclNode;

#define FIRST_SINGLE_CHAR_CMDLINE_ARG   '!'
#define LAST_SINGLE_CHAR_CMDLINE_ARG    '~'
#define N_SINGLE_CHAR_CMDLINE_ARGS  (LAST_SINGLE_CHAR_CMDLINE_ARG-FIRST_SINGLE_CHAR_CMDLINE_ARG+1)

/* bah, it just seems unnecessary to support an
   arbitrary number of aliases. */
#define YC_CMDLINE_MAX_ALIASES 5

static const char *cmdline_prgname = "PROGRAM_NAME";

struct _YcCmdlineMode
{
  const char *mode_name;                /* NULL for top-level */
  const char *short_desc;
  const char *long_desc;
  const char *non_option_arg_desc;
  bool permit_unknown_options;
  bool permit_extra_arguments;
  bool permit_help;
  YcCmdlineArgumentHandler argument_handler;

  unsigned n_aliases;
  const char *aliases[YC_CMDLINE_MAX_ALIASES];

  YcCmdlineMode *parent;
  YcCmdlineMode *next_sibling;
  YcCmdlineMode *first_child;
  YcCmdlineMode *last_child;

  YcCmdlineArg *arg_tree;
  YcCmdlineArg *args_single_char[N_SINGLE_CHAR_CMDLINE_ARGS];

  void *user_data;
  YcVoidFunc callback;

  ExclNode *first_excl_node, *last_excl_node;
};

static YcCmdlineMode toplevel_mode = {
  .permit_help = true
};

static YcCmdlineMode *configuring = &toplevel_mode;

void *yc_cmdline_mode_user_data = NULL;        /* public */

static bool swallow_arguments = true;

#define _YC_CMDLINE_OPTION_USED                (1<<31)

struct _YcCmdlineArg
{
  const char *option_name;
  char c;
  const char *description;
  const char *arg_description;
  YcCmdlineFlags flags;
  void *value_ptr;
  YcCmdlineCallback callback;
  bool (*func)(YcCmdlineArg *arg,
                      const char    *str,
                      char     **error);


  YcCmdlineArg *left, *right, *parent;
  bool is_red;
};

#define COMPARE_STR_TO_ARG_NODE(a,b, rv)  rv = strcmp(a, b->option_name)
#define COMPARE_CMDLINE_ARG_NODES(a,b, rv)  COMPARE_STR_TO_ARG_NODE(a->option_name,b,rv)

#define CMDLINE_ARG_GET_TREE(mode) \
  (mode)->arg_tree, YcCmdlineArg*,  \
  YC_STD_GET_IS_RED, YC_STD_SET_IS_RED, \
  parent, left, right, \
  COMPARE_CMDLINE_ARG_NODES

static int
compare_equal_terminated_str (const char *option,
                              YcCmdlineArg *arg)
{
  const char *at = arg->option_name;
  char a,b;
  while (*option && *option != '=' && *at && *option == *at)
    {
      option++;
      at++;
    }
  a = (*option && *option != '=') ? *option : 0;
  b = *at;
  return a < b ? -1 : a > b ? 1 : 0;
}

#define COMPARE_STR_EQUAL_TO_ARG_NODE(a,b, rv)  rv = compare_equal_terminated_str(a,b)

void set_error(char **err_out, const char *format, ...)
{
  va_list args;
  va_start (args, format);
  vasprintf (err_out, format, args);
  va_end (args);
}

void yc_add_error_prefix (char **err_inout, const char *format, ...)
{
  va_list args;
  va_start (args, format);
  char *tmp;
  vasprintf (&tmp, format, args);
  va_end (args);

  char *rv;
  asprintf(&rv, "%s: %s", tmp, *err_inout);
  free (*err_inout);
  free (tmp);
  *err_inout = rv;
}

/* For mutually-exclusive argument handling */
struct _ExclNode
{
  bool one_required;
  unsigned n_args;
  YcCmdlineArg **args;
  ExclNode *next;
};


void yc_cmdline_init        (const char     *short_desc,
                              const char     *long_desc,
                              const char     *non_option_arg_desc,
                              YcCmdlineInitFlags flags)
{
  configuring->short_desc = short_desc;
  configuring->long_desc = long_desc;
  configuring->non_option_arg_desc = non_option_arg_desc;
  if (flags & YC_CMDLINE_PERMIT_ARGUMENTS)
    configuring->permit_extra_arguments = true;
  if (flags & YC_CMDLINE_PERMIT_UNKNOWN_OPTIONS)
    configuring->permit_unknown_options = true;

  /* By default, modify argc/argv */
  swallow_arguments = (flags & YC_CMDLINE_DO_NOT_MODIFY_ARGV) == 0;
}

static YcCmdlineArg *
try_option (const char *option_name)
{
  YcCmdlineArg *rv;
  YC_RBTREE_LOOKUP_COMPARATOR (CMDLINE_ARG_GET_TREE (configuring),
                                option_name,
                                COMPARE_STR_TO_ARG_NODE, rv);
  return rv;
}

static YcCmdlineArg *
add_option (const char *option_name)
{
  YcCmdlineArg *rv = try_option (option_name);
  YcCmdlineArg *conflict;
  if (rv != NULL)
    yc_die ("option %s added twice", option_name);
  rv = YC_NEW (YcCmdlineArg);
  rv->option_name = option_name;
  rv->c = 0;
  rv->description = rv->arg_description = NULL;
  rv->flags = 0;
  rv->value_ptr = NULL;
  rv->func = NULL;
  YC_RBTREE_INSERT (CMDLINE_ARG_GET_TREE (configuring), rv, conflict);
  yc_assert (conflict == NULL);
  return rv;
}

static bool
cmdline_handle_int (YcCmdlineArg *arg,
                    const char    *str,
                    char     **error)
{
  char *end;
  long v = strtol (str, &end, 0);
  if (str == end)
    {
      set_error (error, "error parsing integer");
      return false;
    }
  if (*end != '\0')
    {
      set_error (error, "garbage at end of integer");
      return false;
    }
  * (int *) arg->value_ptr = v;
  return true;
}

void yc_cmdline_add_int     (const char     *option_name,
                              const char     *description,
			      const char     *arg_description,
                              YcCmdlineFlags flags,
                              int            *value_out)
{
  YcCmdlineArg *arg = add_option (option_name);
  yc_assert (description != NULL);
  yc_assert (arg_description != NULL);
  yc_assert ((flags & YC_CMDLINE_OPTIONAL) == 0);
  arg->description = description;
  arg->arg_description = arg_description;
  arg->flags = flags | YC_CMDLINE_TAKES_ARGUMENT;
  arg->value_ptr = value_out;
  arg->func = cmdline_handle_int;
}

static bool
cmdline_handle_uint (YcCmdlineArg *arg,
                    const char    *str,
                    char     **error)
{
  char *end;
  long v = strtoul (str, &end, 0);
  if (str == end)
    {
      set_error (error, "error parsing unsigned integer");
      return false;
    }
  if (*end != '\0')
    {
      set_error (error, "garbage at end of unsigned integer");
      return false;
    }
  * (int *) arg->value_ptr = v;
  return true;
}

void yc_cmdline_add_uint    (const char     *option_name,
                              const char     *description,
			      const char     *arg_description,
                              YcCmdlineFlags flags,
                              unsigned       *value_out)
{
  YcCmdlineArg *arg = add_option (option_name);
  yc_assert (description != NULL);
  yc_assert (arg_description != NULL);
  yc_assert ((flags & YC_CMDLINE_OPTIONAL) == 0);
  arg->description = description;
  arg->arg_description = arg_description;
  arg->flags = flags | YC_CMDLINE_TAKES_ARGUMENT;
  arg->value_ptr = value_out;
  arg->func = cmdline_handle_uint;
}

static bool
cmdline_handle_int64 (YcCmdlineArg *arg,
                      const char    *str,
                      char     **error)
{
  char *end;
  // TODO: use strtoq if necessary.
  long v = strtoll (str, &end, 0);
  if (str == end)
    {
      set_error (error, "error parsing integer");
      return false;
    }
  if (*end != '\0')
    {
      set_error (error, "garbage at end of integer");
      return false;
    }
  * (int *) arg->value_ptr = v;
  return true;
}

void yc_cmdline_add_int64   (const char     *option_name,
                              const char     *description,
			      const char     *arg_description,
                              YcCmdlineFlags flags,
                              int64_t        *value_out)
{
  YcCmdlineArg *arg = add_option (option_name);
  yc_assert (description != NULL);
  yc_assert (arg_description != NULL);
  yc_assert ((flags & YC_CMDLINE_OPTIONAL) == 0);
  arg->description = description;
  arg->arg_description = arg_description;
  arg->flags = flags | YC_CMDLINE_TAKES_ARGUMENT;
  arg->value_ptr = value_out;
  arg->func = cmdline_handle_int64;
}

static bool
cmdline_handle_uint64 (YcCmdlineArg *arg,
                       const char    *str,
                       char     **error)
{
  char *end;
  // TODO: use strtouq if necessary.
  long v = strtoull (str, &end, 0);
  if (str == end)
    {
      set_error (error, "error parsing unsigned integer");
      return false;
    }
  if (*end != '\0')
    {
      set_error (error, "garbage at end of unsigned integer");
      return false;
    }
  * (int *) arg->value_ptr = v;
  return true;
}

void yc_cmdline_add_uint64  (const char     *option_name,
                              const char     *description,
			      const char     *arg_description,
                              YcCmdlineFlags flags,
                              uint64_t       *value_out)
{
  YcCmdlineArg *arg = add_option (option_name);
  yc_assert (description != NULL);
  yc_assert (arg_description != NULL);
  yc_assert ((flags & YC_CMDLINE_OPTIONAL) == 0);
  arg->description = description;
  arg->arg_description = arg_description;
  arg->flags = flags | YC_CMDLINE_TAKES_ARGUMENT;
  arg->value_ptr = value_out;
  arg->func = cmdline_handle_uint64;
}


static bool
cmdline_handle_double (YcCmdlineArg *arg,
                       const char    *str,
                       char     **error)
{
  char *end;
  double v = strtod (str, &end);
  if (str == end)
    {
      set_error (error, "error parsing floating-point number");
      return false;
    }
  if (*end != '\0')
    {
      set_error (error, "garbage at end of floating-point number");
      return false;
    }
  * (int *) arg->value_ptr = v;
  return true;
}

void yc_cmdline_add_double  (const char     *option_name,
                              const char     *description,
			      const char     *arg_description,
                              YcCmdlineFlags flags,
                              double         *value_out)
{
  YcCmdlineArg *arg = add_option (option_name);
  yc_assert (description != NULL);
  yc_assert (arg_description != NULL);
  yc_assert ((flags & YC_CMDLINE_OPTIONAL) == 0);
  arg->description = description;
  arg->arg_description = arg_description;
  arg->flags = flags | YC_CMDLINE_TAKES_ARGUMENT;
  arg->value_ptr = value_out;
  arg->func = cmdline_handle_double;
}

static bool
cmdline_handle_boolean (YcCmdlineArg *arg,
                        const char    *str,
                        char     **error)
{
  if (str == NULL)
    {
      * (bool *) arg->value_ptr = true;
    }
  else
    {
      if (!yc_parse_boolean (str, arg->value_ptr))
        {
          set_error (error, "error parsing boolean from string");
          return false;
        }
    }
  if (arg->flags & YC_CMDLINE_REVERSED)
    * (bool *) arg->value_ptr = ! (* (bool *) arg->value_ptr);
  return true;
}

void yc_cmdline_add_boolean (const char     *option_name,
                              const char     *description,
                              const char     *arg_description,
                              YcCmdlineFlags flags,
                              bool    *value_out)
{
  YcCmdlineArg *arg = add_option (option_name);
  yc_assert (description != NULL);
  arg->description = description;
  arg->arg_description = arg_description;
  yc_assert ((flags & YC_CMDLINE_OPTIONAL) == 0);
  if (arg_description)
    arg->flags = flags | YC_CMDLINE_TAKES_ARGUMENT;
  else
    arg->flags = flags & ~YC_CMDLINE_TAKES_ARGUMENT;
  arg->value_ptr = value_out;
  arg->func = cmdline_handle_boolean;
}

static bool
cmdline_handle_string (YcCmdlineArg *arg,
                       const char    *str,
                       char     **error)
{
  (void) error;
  * (const char **) arg->value_ptr = str;
  return true;
}

void yc_cmdline_add_string  (const char     *option_name,
                              const char     *description,
			      const char     *arg_description,
                              YcCmdlineFlags flags,
                              const char    **value_out)
{
  YcCmdlineArg *arg = add_option (option_name);
  yc_assert (description != NULL);
  yc_assert (arg_description != NULL);
  yc_assert ((flags & YC_CMDLINE_OPTIONAL) == 0);
  arg->description = description;
  arg->arg_description = arg_description;
  arg->flags = flags | YC_CMDLINE_TAKES_ARGUMENT;
  arg->value_ptr = value_out;
  arg->func = cmdline_handle_string;
}

static bool
cmdline_handle_callback (YcCmdlineArg *arg,
                        const char    *str,
                        char     **error)
{
  return arg->callback (arg->option_name, str, arg->value_ptr, error);
}

void
yc_cmdline_add_func    (const char        *option_name,
                         const char        *description,
                         const char        *arg_description,
                         YcCmdlineFlags    flags,
                         YcCmdlineCallback callback,
                         void              *callback_data)
{
  YcCmdlineArg *arg = add_option (option_name);
  yc_assert (description != NULL);
  arg->description = description;
  arg->arg_description = arg_description;
  if (arg_description)
    arg->flags = flags | YC_CMDLINE_TAKES_ARGUMENT;
  arg->value_ptr = callback_data;
  arg->callback = callback;
  arg->func = cmdline_handle_callback;
}

void yc_cmdline_add_shortcut(char            shortcut,
                              const char     *option_name)
{
  YcCmdlineArg *arg = try_option (option_name);
  yc_assert (arg != NULL);
  yc_assert (FIRST_SINGLE_CHAR_CMDLINE_ARG <= shortcut
                      && shortcut <= LAST_SINGLE_CHAR_CMDLINE_ARG);
  yc_assert (configuring->args_single_char[shortcut - FIRST_SINGLE_CHAR_CMDLINE_ARG] == NULL);
  arg->c = shortcut;
  configuring->args_single_char[shortcut - FIRST_SINGLE_CHAR_CMDLINE_ARG] = arg;
}

void yc_cmdline_mutually_exclusive (bool     one_required,
                                     const char     *arg_1,
                                     ...)
{
  va_list args;
  unsigned n = 1;
  char **arg_array;
  char *a;
  va_start (args, arg_1);
  while (va_arg (args, char*))
    n++;
  va_end (args);

  arg_array = alloca (sizeof(char*) * n);
  arg_array[0] = (char*) arg_1;
  va_start (args, arg_1);
  n=1;
  while ((a=va_arg (args, char*)) != NULL)
    arg_array[n++] = a;
  va_end (args);
  yc_cmdline_mutually_exclusive_v (one_required, n, arg_array);
}

void yc_cmdline_mutually_exclusive_v (bool     one_required,
                                       unsigned        n_excl,
                                       char          **excl)
{
  YcCmdlineArg **args = YC_NEW_ARRAY (n_excl, YcCmdlineArg*);
  ExclNode *node = YC_NEW (ExclNode);
  unsigned i;
  node->args = args;
  for (i = 0; i < n_excl; i++)
    if ((args[i] = try_option (excl[i])) == NULL)
      yc_die ("yc_cmdline_mutually_exclusive_v: bad option %s", excl[i]);
  node->n_args = n_excl;
  node->one_required = one_required;
  node->next = NULL;

  if (configuring->first_excl_node == NULL)
    configuring->first_excl_node = node;
  else
    configuring->last_excl_node->next = node;
  configuring->last_excl_node = node;
}

void yc_cmdline_permit_unknown_options (bool permit)
{
  configuring->permit_unknown_options = permit;
}
void yc_cmdline_permit_extra_arguments (bool permit)
{
  configuring->permit_extra_arguments = permit;
}
void yc_cmdline_set_argument_handler (YcCmdlineArgumentHandler handler)
{
  yc_assert (configuring->argument_handler == NULL);
  configuring->argument_handler = handler;
}

void yc_cmdline_process_args(int            *argc_inout,
                              char         ***argv_inout)
{
  char *error = NULL;
  if (!yc_cmdline_try_process_args (argc_inout, argv_inout, &error))
    yc_die ("error processing command-line arguments: %s", error);
}

static void
skip_or_swallow (int *argc_inout,
                 char ***argv_inout,
                 int *i_inout,
                 unsigned n)
{
  if (swallow_arguments)
    {
      memmove ((*argv_inout) + (*i_inout),
               (*argv_inout) + (*i_inout) + n,
               ((*argc_inout + 1) - (*i_inout + n)) * sizeof (char*));
      *argc_inout -= n;
    }
  else
    *i_inout += n;
}

static bool
check_mandatory_args_recursive (YcCmdlineArg *node,
                                char     **error)
{
  if ((node->flags & (YC_CMDLINE_MANDATORY|_YC_CMDLINE_OPTION_USED)) == YC_CMDLINE_MANDATORY)
    {
      set_error (error, "mandatory option --%s not supplied", node->option_name);
      return false;
    }
  return (node->left == NULL || check_mandatory_args_recursive (node->left, error))
      && (node->right == NULL || check_mandatory_args_recursive (node->right, error));
}

static bool
check_mutually_exclusive_args (YcCmdlineMode *mode,
                               char **error)
{
  ExclNode *at;
  for (at = mode->first_excl_node; at; at = at->next)
    {
      unsigned i;
      const char *got_arg_name = NULL;
      for (i = 0; i < at->n_args; i++)
        if (at->args[i]->flags & _YC_CMDLINE_OPTION_USED)
          {
            if (got_arg_name)
              {
                set_error (error, "'--%s' and '--%s' are mutually exclusive",
                               got_arg_name, at->args[i]->option_name);
                return false;
              }
            got_arg_name = at->args[i]->option_name;
          }
      if (got_arg_name == NULL && at->one_required)
        {
          YcBuffer buffer = YC_BUFFER_INIT;
          for (i = 0; i < at->n_args; i++)
            {
              yc_buffer_append_string (&buffer, " --");
              yc_buffer_append_string (&buffer, at->args[i]->option_name);
            }
          char *str = yc_buffer_empty_to_string (&buffer);
          set_error (error, "one of the following options is required:%s",
                         str);
          yc_free (str);
          return false;
        }
    }
  return true;
}

static void
print_option (YcCmdlineArg *arg)
{
  fprintf (stderr, "  ");
  if (arg->option_name)
    {
      if ((arg->flags & YC_CMDLINE_OPTIONAL) != 0 && arg->arg_description != NULL)
        fprintf (stderr, "--%s[=%s]", arg->option_name, arg->arg_description);
      else if (arg->arg_description)
        fprintf (stderr, "--%s=%s", arg->option_name, arg->arg_description);
      else
        fprintf (stderr, "--%s", arg->option_name);
    }
  if (arg->option_name && arg->c)
    fprintf (stderr, ", ");
  if (arg->c)
    {
      fprintf (stderr, "-%c", arg->c);
      if (arg->arg_description)
        fprintf (stderr, " %s", arg->arg_description);
    }
  fprintf (stderr, "\n        %s\n", arg->description);
}

static void
print_options_recursive (YcCmdlineArg *tree)
{
  if (tree == NULL)
    return;
  print_options_recursive (tree->left);
  print_option (tree);
  print_options_recursive (tree->right);
}

static void usage (YcCmdlineMode *mode,
                   const char *prog_name)
{
  const char *base_prog_name = strrchr (prog_name, '/');
  if (base_prog_name)
    base_prog_name++;
  else
    base_prog_name = prog_name;
  if (mode->short_desc)
    fprintf (stderr, "%s - %s\n\n",
             base_prog_name,
             mode->short_desc);
  fprintf (stderr, "usage: %s [OPTIONS] %s\n",
           base_prog_name, mode->non_option_arg_desc ? mode->non_option_arg_desc : "");
  if (mode->long_desc)
    {
      fprintf (stderr, "\n%s\n", mode->long_desc);
    }
  fprintf (stderr, "Options:\n");
  print_options_recursive (mode->arg_tree);
}

void
yc_cmdline_print_usage (void)
{
  usage(&toplevel_mode, cmdline_prgname);
}

static YcCmdlineMode *
find_child_mode (YcCmdlineMode *parent,
                 const char     *name)
{
  YcCmdlineMode *at;
  for (at = parent->first_child; at; at = at->next_sibling)
    {
      unsigned i;
      if (strcmp (at->mode_name, name) == 0)
        return at;
      for (i = 0; i < at->n_aliases; i++)
        if (strcmp (at->aliases[i], name) == 0)
          return at;
    }
  return NULL;
}

bool
yc_cmdline_try_process_args (int *argc_inout,
                              char ***argv_inout,
                              char **error)
{
  char **argv = *argv_inout;
  int i;
  YcCmdlineMode *mode = &toplevel_mode;
  YcCmdlineMode *submode;
  cmdline_prgname = argv[0];
  for (i = 1; i < *argc_inout; )
    {
      if (argv[i][0] == '-')
        {
          if (argv[i][1] == '-')
            {
              /* long option */
              const char *opt = argv[i] + 2;
              YcCmdlineArg *arg;
              YC_RBTREE_LOOKUP_COMPARATOR (CMDLINE_ARG_GET_TREE (mode), opt,
                                            COMPARE_STR_EQUAL_TO_ARG_NODE, arg);
              if (arg == NULL)
                {
                  if (mode->permit_unknown_options)
                    {
                      i++;
                      continue;
                    }
                  if (mode->permit_help && strcmp (opt, "help") == 0)
                    {
                      usage (mode, **argv_inout);
                      exit (1);
                    }
                  set_error (error, "bad option --%s", opt);
                  return false;
                }
              else
                {
                  const char *eq = strchr (opt, '=');
                  if (eq == NULL
                   && (arg->flags & YC_CMDLINE_TAKES_ARGUMENT)
                   && (arg->flags & YC_CMDLINE_OPTIONAL) == 0)
                    {
                      if (i + 1 == *argc_inout)
                        {
                          set_error (error, "option --%s requires argument", opt);
                          return false;
                        }
                      if (!arg->func (arg, argv[i+1], error))
                        {
                          yc_add_error_prefix (error, "processing --%s", opt);
                          return false;
                        }
                      arg->flags |= _YC_CMDLINE_OPTION_USED;
                      skip_or_swallow (argc_inout, argv_inout, &i, 2);
                      continue;
                    }
                  if (eq != NULL && !(arg->flags & YC_CMDLINE_TAKES_ARGUMENT))
                    {
                      set_error (error, "option --%s has argument: not allowed", opt);
                      return false;
                    }
                  if (!arg->func (arg, eq ? (eq + 1) : NULL, error))
                    {
                      yc_add_error_prefix (error, "processing --%s", opt);
                      return false;
                    }
                  skip_or_swallow (argc_inout, argv_inout, &i, 1);
                  arg->flags |= _YC_CMDLINE_OPTION_USED;
                }
            }
          else if (argv[i][1] == 0)
            {
              /* just '-':  cease processing options, if allowed */
              if (!mode->permit_extra_arguments)
                {
                  set_error (error, "got '-', but non-options are forbidding");
                  return false;
                }
              skip_or_swallow (argc_inout, argv_inout, &i, 1);
              break;
            }
          else
            {
              /* short option */
              if (argv[i][2] == 0)
                {
                  /* a single short-option may take arguments (as in argv[i+1]) */
                  YcCmdlineArg *arg;
                  if ((uint8_t)argv[i][1] < FIRST_SINGLE_CHAR_CMDLINE_ARG
                   || (uint8_t)argv[i][1] > LAST_SINGLE_CHAR_CMDLINE_ARG)
                    {
                      set_error (error, "invalid single-char option '%c'", argv[i][1]);
                      return false;
                    }
                  arg = mode->args_single_char[argv[i][1] - FIRST_SINGLE_CHAR_CMDLINE_ARG];
                  if (arg == NULL)
                    {
                      set_error (error, "invalid single-char option '%c'", argv[i][1]);
                      return false;
                    }
                  if (arg->flags & YC_CMDLINE_TAKES_ARGUMENT)
                    {
                      if (i + 1 == *argc_inout)
                        {
                          set_error (error, "option %s takes argument", argv[i]);
                          return false;
                        }
                      if (!arg->func (arg, argv[i+1], error))
                        {
                          yc_add_error_prefix (error, "parsing argument to %s", argv[i]);
                          return false;
                        }
                      skip_or_swallow (argc_inout, argv_inout, &i, 2);
                      arg->flags |= _YC_CMDLINE_OPTION_USED;
                    }
                  else
                    {
                      if (!arg->func (arg, NULL, error))
                        {
                          yc_add_error_prefix (error, "parsing argument to %s", argv[i]);
                          return false;
                        }
                      skip_or_swallow (argc_inout, argv_inout, &i, 1);
                      arg->flags |= _YC_CMDLINE_OPTION_USED;
                    }
                }
              else
                {
                  /* any number of no-argument short options */
                  unsigned ci;
                  for (ci = 1; argv[i][ci] != 0; ci++)
                    {
                      if (argv[i][ci] < FIRST_SINGLE_CHAR_CMDLINE_ARG
                       || argv[i][ci] > LAST_SINGLE_CHAR_CMDLINE_ARG
                       || mode->args_single_char[argv[i][ci] - FIRST_SINGLE_CHAR_CMDLINE_ARG] == NULL)
                        {
                          set_error (error, "invalid single-char option '%c'", argv[i][ci]);
                          return false;
                        }
                      if (mode->args_single_char[argv[i][ci] - FIRST_SINGLE_CHAR_CMDLINE_ARG]->flags & YC_CMDLINE_TAKES_ARGUMENT)
                        {
                          set_error (error, "multiple short-options combined, but '%c' take argument", argv[i][ci]);
                          return false;
                        }
                    }
                  for (ci = 1; argv[i][ci] != 0; ci++)
                    {
                      YcCmdlineArg *arg = mode->args_single_char[argv[i][ci] - FIRST_SINGLE_CHAR_CMDLINE_ARG];
                      if (!arg->func (arg, NULL, error))
                        {
                          yc_add_error_prefix (error, "processing -%c", argv[i][ci]);
                          return false;
                        }
                      arg->flags |= _YC_CMDLINE_OPTION_USED;
                    }
                  skip_or_swallow (argc_inout, argv_inout, &i, 1);
                }
            }
        }
      else if ((submode=find_child_mode (mode, argv[i])) != NULL)
        {
          mode = submode;
          yc_cmdline_mode_user_data = mode->user_data;
          if (mode->callback)
            mode->callback ();
          skip_or_swallow (argc_inout, argv_inout, &i, 1);
        }
      else if (mode->argument_handler)
        {
          if (!mode->argument_handler (argv[i], error))
            return false;
          skip_or_swallow (argc_inout, argv_inout, &i, 1);
        }
      else if (mode->permit_extra_arguments)
        {
          i++;
        }
      else
        {
          set_error (error, "unexpected command-line argument '%s'", argv[i]);
          return false;
        }
    }

  /* check that all mandatory arguments are used */
  for (submode = mode; submode; submode = submode->parent)
    {
      if (submode->arg_tree != NULL
       && !check_mandatory_args_recursive (submode->arg_tree, error))
        return false;
      if (!check_mutually_exclusive_args (submode, error))
        return false;
    }

  return true;
}

static void
_yc_cmdline_arg_tree_free_recursive (YcCmdlineArg *arg)
{
  if (arg == NULL)
    return;
  _yc_cmdline_arg_tree_free_recursive (arg->left);
  _yc_cmdline_arg_tree_free_recursive (arg->right);
  yc_free (arg);
}

static void _yc_cmdline_mode_clear (YcCmdlineMode *mode)
{
  while (mode->first_child)
    {
      YcCmdlineMode *kill = mode->first_child;
      mode->first_child = kill->next_sibling;
      _yc_cmdline_mode_clear (kill);
      yc_free (kill);
    }
  _yc_cmdline_arg_tree_free_recursive (mode->arg_tree);
}

void _yc_cmdline_cleanup (void)
{
  _yc_cmdline_mode_clear (&toplevel_mode);
}
