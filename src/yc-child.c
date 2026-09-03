/* SPDX-License-Identifier: 0BSD */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <uv.h>

#include "yc-alloc.h"
#include "yc-child.h"
#include "yc-common.h"

static void child_check_done      (YcChild *child);
static void container_signal_ready(YcChildContainer *container);

/* --- errors --- */

static void
set_error (YcChildCreateError *error,
           YcChildCreateErrorCode code,
           int sys_errno,
           const char *format,
           ...)
{
  va_list args;
  if (error == NULL)
    return;
  error->code = code;
  error->sys_errno = sys_errno;
  va_start (args, format);
  vsnprintf (error->message, sizeof (error->message), format, args);
  va_end (args);
}

/* --- string arrays --- */

/* The caller's argv/env commonly point into a line-buffer that is
   about to be reused, so take copies. */
static char **
dup_string_array (char **strs)
{
  size_t i, n = 0;
  char **rv;
  if (strs == NULL)
    return NULL;
  while (strs[n] != NULL)
    n++;
  rv = YC_NEW_ARRAY (n + 1, char *);
  for (i = 0; i < n; i++)
    rv[i] = yc_strdup (strs[i]);
  rv[n] = NULL;
  return rv;
}

static void
free_string_array (char **strs)
{
  size_t i;
  if (strs == NULL)
    return;
  for (i = 0; strs[i] != NULL; i++)
    yc_free (strs[i]);
  yc_free (strs);
}

/* --- file-descriptor plumbing --- */

static bool
set_nonblock_cloexec (int fd)
{
  int flags = fcntl (fd, F_GETFL);
  if (flags < 0 || fcntl (fd, F_SETFL, flags | O_NONBLOCK) < 0)
    return false;
  flags = fcntl (fd, F_GETFD);
  if (flags < 0 || fcntl (fd, F_SETFD, flags | FD_CLOEXEC) < 0)
    return false;
  return true;
}

/* 'child_reads'/'child_writes' are from the child's point of view.
   Our end is made non-blocking (the application does raw read()s and
   write()s on it) and close-on-exec: without CLOEXEC it would leak
   into every child spawned afterward, and then nobody would ever see
   EOF on it. */
static bool
open_pipe_pair (bool child_reads,
                bool child_writes,
                int *our_fd_out,
                int *child_fd_out)
{
  int fds[2];

  if (child_reads && child_writes)
    {
      if (socketpair (AF_UNIX, SOCK_STREAM, 0, fds) < 0)
        return false;
      *our_fd_out = fds[0];
      *child_fd_out = fds[1];
    }
  else
    {
      if (pipe (fds) < 0)
        return false;
      if (child_reads)
        {
          *child_fd_out = fds[0];
          *our_fd_out = fds[1];
        }
      else
        {
          *child_fd_out = fds[1];
          *our_fd_out = fds[0];
        }
    }

  /* O_NONBLOCK lives on the open file description, and the two ends of
     a pipe (or socketpair) are separate descriptions, so this does not
     affect the child. */
  if (!set_nonblock_cloexec (*our_fd_out))
    {
      int save = errno;
      close (*our_fd_out);
      close (*child_fd_out);
      errno = save;
      return false;
    }
  return true;
}

static int
open_flags_for_direction (bool in, bool out, bool creating, bool append)
{
  if (in && out)
    return O_RDWR | (creating ? O_CREAT : 0);
  else if (in)
    return O_RDONLY;
  else if (append)
    return O_WRONLY | (creating ? (O_CREAT | O_APPEND) : O_APPEND);
  else
    return O_WRONLY | (creating ? (O_CREAT | O_TRUNC) : 0);
}

/* --- polling one pipe --- */

static void child_fd_poll_cb (uv_poll_t *poll, int status, int events);

static void
child_fd_apply_watch (YcChildFd *cfd)
{
  int events = 0;

  if (!cfd->poll_inited || cfd->pipe_fd < 0)
    return;
  if (cfd->watch & YC_CHILD_FD_EVENT_READABLE)
    events |= UV_READABLE;
  if (cfd->watch & YC_CHILD_FD_EVENT_WRITABLE)
    events |= UV_WRITABLE;

  if (events == 0)
    {
      if (cfd->poll_started)
        {
          uv_poll_stop (&cfd->poll);
          cfd->poll_started = false;
        }
    }
  else
    {
      uv_poll_start (&cfd->poll, events | UV_DISCONNECT, child_fd_poll_cb);
      cfd->poll_started = true;
    }
}

static void
child_fd_poll_closed_cb (uv_handle_t *handle)
{
  YcChildFd *cfd = handle->data;
  YcChild *child = cfd->child;

  /* uv_poll_t never owns the fd, so it is ours to close -- but only
     once the handle is really gone. */
  if (cfd->closing_fd >= 0)
    {
      close (cfd->closing_fd);
      cfd->closing_fd = -1;
    }
  child->n_uv_handles--;
  child_check_done (child);
}

static void
child_fd_poll_cb (uv_poll_t *poll, int status, int events)
{
  YcChildFd *cfd = poll->data;
  YcChild *child = cfd->child;
  int child_fd = cfd->child_fd;

  if (status < 0)
    {
      /* Nothing useful an application could do with this fd; treat it
         as a hangup. */
      yc_child_fd_close (child, child_fd);
      return;
    }

  cfd->revents = 0;
  if (events & UV_READABLE)
    cfd->revents |= YC_CHILD_FD_EVENT_READABLE;
  if (events & UV_WRITABLE)
    cfd->revents |= YC_CHILD_FD_EVENT_WRITABLE;
  if (events & UV_DISCONNECT)
    cfd->revents |= YC_CHILD_FD_EVENT_HANGUP;

  if ((cfd->revents & (YC_CHILD_FD_EVENT_READABLE
                       | YC_CHILD_FD_EVENT_WRITABLE)) != 0
   && child->callbacks.io != NULL)
    child->callbacks.io (child, child_fd, cfd->pipe_fd);

  /* io() is allowed to have closed the fd.
     A hangup on an fd we are READING is not a reason to stop: the pipe
     can still hold buffered data that the child wrote before it went
     away, and closing here would silently discard it.  The real end of
     a readable fd is read() returning 0, which only the application
     can see.  A write-only fd has nothing to drain, so for that one a
     hangup is final. */
  if (cfd->pipe_fd >= 0
   && (cfd->revents & YC_CHILD_FD_EVENT_HANGUP) != 0
   && (cfd->watch & YC_CHILD_FD_EVENT_READABLE) == 0)
    yc_child_fd_close (child, child_fd);
}

/* --- per-fd setup, before spawning --- */

/* Fills in 'cfd' and 'stdio', and sets *dup_fd_out to a descriptor
   that we must close once uv_spawn() has dup2()ed it into the child
   (or -1 if there is nothing to close). */
static bool
child_fd_init (YcChild *child,
               YcChildFd *cfd,
               int child_fd,
               const YcChildCreateFdInfo *info,
               uv_stdio_container_t *stdio,
               int *dup_fd_out,
               YcChildCreateError *error)
{
  bool in = info->in, out = info->out;
  int fd;

  if (!in && !out)
    {
      if (child_fd == 0)
        in = true;
      else if (child_fd == 1 || child_fd == 2)
        out = true;
      else
        {
          set_error (error, YC_CHILD_CREATE_ERROR_BAD_ARGUMENTS, 0,
                     "fd %d: neither 'in' nor 'out' was given", child_fd);
          return false;
        }
    }

  cfd->child_fd = child_fd;
  cfd->pipe_fd = -1;
  cfd->closing_fd = -1;
  cfd->mode = info->mode;
  cfd->dup_fd = info->dup_fd;
  cfd->in = in;
  cfd->out = out;
  cfd->child = child;
  *dup_fd_out = -1;

  switch (info->mode)
    {
    case YC_CHILD_FD_MODE_DUP:
      /* Filled in by resolve_dup_fds() once every other fd is settled. */
      stdio->flags = UV_IGNORE;
      return true;

    case YC_CHILD_FD_MODE_INHERIT:
      stdio->flags = UV_INHERIT_FD;
      stdio->data.fd = child_fd;
      return true;

    case YC_CHILD_FD_MODE_NULL:
      fd = open ("/dev/null",
                 open_flags_for_direction (in, out, false, false) | O_CLOEXEC);
      if (fd < 0)
        {
          set_error (error, YC_CHILD_CREATE_ERROR_OPEN_FAILED, errno,
                     "fd %d: opening /dev/null: %s",
                     child_fd, strerror (errno));
          return false;
        }
      stdio->flags = UV_INHERIT_FD;
      stdio->data.fd = fd;
      *dup_fd_out = fd;
      return true;

    case YC_CHILD_FD_MODE_FILE:
      if (info->filename == NULL)
        {
          set_error (error, YC_CHILD_CREATE_ERROR_BAD_ARGUMENTS, 0,
                     "fd %d: MODE_FILE without a filename", child_fd);
          return false;
        }
      fd = open (info->filename,
                 open_flags_for_direction (in, out, true, info->append)
                 | O_CLOEXEC,
                 0666);
      if (fd < 0)
        {
          set_error (error, YC_CHILD_CREATE_ERROR_OPEN_FAILED, errno,
                     "fd %d: opening %s: %s",
                     child_fd, info->filename, strerror (errno));
          return false;
        }
      stdio->flags = UV_INHERIT_FD;
      stdio->data.fd = fd;
      *dup_fd_out = fd;
      return true;

    case YC_CHILD_FD_MODE_PIPE:
      /* We create the pipe ourselves and hand the far end to libuv as
         a plain inherited fd; UV_CREATE_PIPE would give us a
         uv_stream_t, but this API hands raw fds to the application. */
      {
        int our_fd, their_fd;
        if (!open_pipe_pair (in, out, &our_fd, &their_fd))
          {
            set_error (error, YC_CHILD_CREATE_ERROR_PIPE_FAILED, errno,
                       "fd %d: creating pipe: %s", child_fd, strerror (errno));
            return false;
          }
        cfd->pipe_fd = our_fd;
        cfd->watch = (in ? YC_CHILD_FD_EVENT_WRITABLE : 0)
                   | (out ? YC_CHILD_FD_EVENT_READABLE : 0);
        stdio->flags = UV_INHERIT_FD;
        stdio->data.fd = their_fd;
        *dup_fd_out = their_fd;
        return true;
      }
    }

  set_error (error, YC_CHILD_CREATE_ERROR_BAD_ARGUMENTS, 0,
             "fd %d: bad mode %u", child_fd, (unsigned) info->mode);
  return false;
}

/* '2>&1': point the child's fd at the same descriptor another of its
   fds already uses, so the two share one file offset -- which is why
   this cannot be done by re-opening the same filename.  Runs after
   every other fd is set up, since the source may come later in the
   array (or be a dup itself). */
static bool
resolve_dup_fds (YcChild *child,
                 uv_stdio_container_t *stdio,
                 size_t stdio_count,
                 YcChildCreateError *error)
{
  size_t n_fds = 3 + child->n_other_fds;
  size_t i;

  for (i = 0; i < n_fds; i++)
    {
      YcChildFd *cfd = i < 3 ? &child->fds[i] : &child->other_fds[i - 3];
      int src = cfd->dup_fd;
      size_t hops;

      if (cfd->mode != YC_CHILD_FD_MODE_DUP)
        continue;

      /* Follow a chain of dups, refusing to go round in circles. */
      for (hops = 0; ; hops++)
        {
          YcChildFd *src_cfd;
          if (src < 0 || (size_t) src >= stdio_count
           || (src_cfd = yc_child_get_fd (child, src)) == NULL)
            {
              set_error (error, YC_CHILD_CREATE_ERROR_BAD_ARGUMENTS, 0,
                         "fd %d: dup of fd %d, which is not configured",
                         cfd->child_fd, src);
              return false;
            }
          if (src_cfd->mode != YC_CHILD_FD_MODE_DUP)
            break;
          if (hops > n_fds)
            {
              set_error (error, YC_CHILD_CREATE_ERROR_BAD_ARGUMENTS, 0,
                         "fd %d: circular dup", cfd->child_fd);
              return false;
            }
          src = src_cfd->dup_fd;
        }

      /* Both entries name one descriptor, which we close exactly once
         after uv_spawn(): only the source recorded it in dup_fds[]. */
      stdio[cfd->child_fd] = stdio[src];
    }
  return true;
}

/* --- child lifetime --- */

static void
child_free (YcChild *child)
{
  yc_free (child->other_fds);
  yc_free (child->program);
  free_string_array (child->argv);
  free_string_array (child->env);
  yc_free (child);
}

/* Drop every pipe we hold.  Goes through yc_child_fd_close() so that a
   live uv_poll_t is unwound before its fd is closed -- closing a
   polled fd behind libuv's back is undefined. */
static void
child_close_all_fds (YcChild *child)
{
  size_t i;
  for (i = 0; i < 3; i++)
    yc_child_fd_close (child, child->fds[i].child_fd);
  for (i = 0; i < child->n_other_fds; i++)
    yc_child_fd_close (child, child->other_fds[i].child_fd);
}

static void
container_retain_done_child (YcChildContainer *container, YcChild *child)
{
  if (container->max_done_children == 0)
    {
      child_free (child);
      return;
    }

  while (container->n_done_children >= container->max_done_children)
    {
      child_free (container->done_children[0]);
      container->n_done_children--;
      memmove (container->done_children,
               container->done_children + 1,
               sizeof (YcChild *) * container->n_done_children);
    }

  if (container->n_done_children == container->done_children_alloced)
    {
      container->done_children_alloced = container->max_done_children;
      container->done_children = YC_RENEW (YcChild *,
                                           container->done_children,
                                           container->done_children_alloced);
    }
  container->done_children[container->n_done_children++] = child;
}

/* Called every time one of the child's uv handles goes away.  A child
   is finished when it has exited AND we have let go of every pipe:
   a process that exits while we still have unread output is not done. */
static void
child_check_done (YcChild *child)
{
  YcChildContainer *container = child->container;
  size_t i;

  if (child->done_emitted || !child->process_exited || child->n_uv_handles > 0)
    return;
  child->done_emitted = true;

  if (child->callbacks.done != NULL)
    child->callbacks.done (child);

  i = child->container_index;
  container->children[i] = container->children[container->n_children - 1];
  container->children[i]->container_index = i;
  container->n_children--;

  container_retain_done_child (container, child);
  container_signal_ready (container);
}

static void
child_process_closed_cb (uv_handle_t *handle)
{
  YcChild *child = handle->data;
  child->n_uv_handles--;
  child_check_done (child);
}

static void
child_exit_cb (uv_process_t *process, int64_t exit_status, int term_signal)
{
  YcChild *child = process->data;

  child->process_exited = true;
  if (term_signal != 0)
    {
      child->status = YC_CHILD_STATUS_KILLED;
      child->status_value = term_signal;
    }
  else
    {
      child->status = YC_CHILD_STATUS_EXITED;
      child->status_value = (int) exit_status;
    }

  uv_close ((uv_handle_t *) process, child_process_closed_cb);
}

/* --- public: per-fd --- */

YcChildFd *
yc_child_get_fd (YcChild *child, int child_fd)
{
  size_t i;
  if (child_fd >= 0 && child_fd < 3)
    return &child->fds[child_fd];
  for (i = 0; i < child->n_other_fds; i++)
    if (child->other_fds[i].child_fd == child_fd)
      return &child->other_fds[i];
  return NULL;
}

void
yc_child_fd_watch (YcChild *child, int child_fd, YcChildFdEvents events)
{
  YcChildFd *cfd = yc_child_get_fd (child, child_fd);
  if (cfd == NULL || cfd->pipe_fd < 0)
    return;
  cfd->watch = events & (YC_CHILD_FD_EVENT_READABLE
                         | YC_CHILD_FD_EVENT_WRITABLE);
  child_fd_apply_watch (cfd);
}

void
yc_child_fd_close (YcChild *child, int child_fd)
{
  YcChildFd *cfd = yc_child_get_fd (child, child_fd);
  int pipe_fd;

  if (cfd == NULL || cfd->pipe_fd < 0)
    return;

  pipe_fd = cfd->pipe_fd;
  cfd->pipe_fd = -1;
  cfd->watch = 0;

  if (cfd->poll_inited)
    {
      if (cfd->poll_started)
        {
          uv_poll_stop (&cfd->poll);
          cfd->poll_started = false;
        }
      cfd->poll_inited = false;
      cfd->closing_fd = pipe_fd;
      uv_close ((uv_handle_t *) &cfd->poll, child_fd_poll_closed_cb);
    }
  else
    close (pipe_fd);

  /* 'pipe_fd' is passed for identification only: it is closed, or
     about to be. */
  if (child->callbacks.closed != NULL)
    child->callbacks.closed (child, child_fd, pipe_fd);

  if (!cfd->poll_inited && cfd->closing_fd < 0)
    child_check_done (child);
}

void
yc_child_kill (YcChild *child, int signum)
{
  if (!child->process_exited)
    uv_process_kill (&child->process, signum);
}

/* --- public: the container --- */

static void
container_ready_idle_cb (uv_idle_t *idle)
{
  YcChildContainer *container = idle->data;

  uv_idle_stop (idle);
  container->ready_idle_started = false;

  /* Keep offering slots until the application stops taking them. */
  while (container->n_children < container->max_children)
    {
      size_t before = container->n_children;
      container->callbacks.ready_to_spawn (container);
      if (container->n_children == before)
        break;
    }
}

/* Deferred to an idle callback rather than called inline: we get here
   from the middle of tearing a child down, and yc_child_new() should
   not run until the container's bookkeeping has settled. */
static void
container_signal_ready (YcChildContainer *container)
{
  if (container->callbacks.ready_to_spawn == NULL
   || container->ready_idle_started
   || container->n_children >= container->max_children)
    return;
  uv_idle_start (&container->ready_idle, container_ready_idle_cb);
  container->ready_idle_started = true;
}

YcChildContainer *
yc_child_container_new (YcChildContainerCreationInfo *info)
{
  YcChildContainer *container = YC_NEW0 (YcChildContainer);

  container->max_children = info->max_children == 0 ? 1 : info->max_children;
  container->callbacks = info->callbacks;
  container->container_data = info->container_data;

  if (info->loop != NULL)
    container->loop = info->loop;
  else
    {
      int uv_err = uv_loop_init (&container->own_loop);
      if (uv_err != 0)
        yc_die ("uv_loop_init failed: %s", uv_strerror (uv_err));
      container->loop = &container->own_loop;
      container->owns_loop = true;
    }

  container->children_alloced = container->max_children;
  container->children = YC_NEW0_ARRAY (container->children_alloced, YcChild *);

  uv_idle_init (container->loop, &container->ready_idle);
  container->ready_idle.data = container;

  return container;
}

void
yc_child_container_run (YcChildContainer *container)
{
  container->running = true;
  container_signal_ready (container);

  /* Returns once no handles are left: every child has been reaped and
     ready_to_spawn() has declined to start another. */
  uv_run (container->loop, UV_RUN_DEFAULT);
  container->running = false;

  if (container->n_children == 0 && container->callbacks.all_done != NULL)
    container->callbacks.all_done (container);
}

void
yc_child_container_destroy (YcChildContainer *container)
{
  size_t i;

  if (container->callbacks.destroy != NULL)
    container->callbacks.destroy (container);

  /* Before draining the loop below, or the idle callback fires and
     asks the application for more work mid-teardown. */
  container->callbacks.ready_to_spawn = NULL;
  if (container->ready_idle_started)
    {
      uv_idle_stop (&container->ready_idle);
      container->ready_idle_started = false;
    }

  /* Children still running: kill them, drop our pipes so they cannot
     block on us, and spin the loop until they have all been reaped.
     No application callbacks fire during teardown. */
  if (container->n_children > 0)
    {
      for (i = 0; i < container->n_children; i++)
        {
          YcChild *child = container->children[i];
          memset (&child->callbacks, 0, sizeof (child->callbacks));
          yc_child_kill (child, SIGKILL);
          child_close_all_fds (child);
        }
      while (container->n_children > 0)
        uv_run (container->loop, UV_RUN_ONCE);
    }

  uv_close ((uv_handle_t *) &container->ready_idle, NULL);

  for (i = 0; i < container->n_done_children; i++)
    child_free (container->done_children[i]);
  yc_free (container->done_children);
  yc_free (container->children);

  if (container->owns_loop)
    {
      /* Let the pending close callbacks run before closing the loop. */
      while (uv_run (container->loop, UV_RUN_NOWAIT) != 0)
        ;
      uv_loop_close (container->loop);
    }

  yc_free (container);
}

/* --- public: spawning --- */

YcChild *
yc_child_new (YcChildContainer *container,
              YcChildCreateInfo *create_info,
              YcChildCreateError *error_out)
{
  YcChild *child;
  uv_stdio_container_t *stdio = NULL;
  int *dup_fds = NULL;
  size_t n_other = create_info->n_other_fd_infos;
  size_t stdio_count = 3;
  size_t i, j;
  uv_process_options_t options;
  int uv_err;

  if (create_info->argv == NULL || create_info->argv[0] == NULL)
    {
      set_error (error_out, YC_CHILD_CREATE_ERROR_BAD_ARGUMENTS, 0,
                 "argv must be non-empty");
      return NULL;
    }
  if (container->n_children >= container->max_children)
    {
      set_error (error_out, YC_CHILD_CREATE_ERROR_CONTAINER_FULL, 0,
                 "container already has its maximum of %u children",
                 (unsigned) container->max_children);
      return NULL;
    }

  for (i = 0; i < n_other; i++)
    {
      int fd = create_info->other_fd_infos[i].fd;
      if (fd < 3)
        {
          set_error (error_out, YC_CHILD_CREATE_ERROR_BAD_ARGUMENTS, 0,
                     "other_fd_infos[%u]: fd %d must be >= 3 "
                     "(use fd_infos[] for stdin/stdout/stderr)",
                     (unsigned) i, fd);
          return NULL;
        }
      for (j = 0; j < i; j++)
        if (create_info->other_fd_infos[j].fd == fd)
          {
            set_error (error_out, YC_CHILD_CREATE_ERROR_BAD_ARGUMENTS, 0,
                       "fd %d given twice", fd);
            return NULL;
          }
      if ((size_t) fd + 1 > stdio_count)
        stdio_count = (size_t) fd + 1;
    }

  child = YC_NEW0 (YcChild);
  child->container = container;
  child->user_data = create_info->user_data;
  child->status = YC_CHILD_STATUS_RUNNING;
  if (create_info->callbacks != NULL)
    child->callbacks = *create_info->callbacks;
  child->n_other_fds = n_other;
  child->other_fds = n_other == 0 ? NULL : YC_NEW0_ARRAY (n_other, YcChildFd);

  /* Any gaps below stdio_count stay UV_IGNORE, which closes them in
     the child. */
  stdio = YC_NEW0_ARRAY (stdio_count, uv_stdio_container_t);
  for (i = 0; i < stdio_count; i++)
    stdio[i].flags = UV_IGNORE;

  dup_fds = YC_NEW_ARRAY (3 + n_other, int);
  for (i = 0; i < 3 + n_other; i++)
    dup_fds[i] = -1;

  /* Identify every fd and mark it 'no pipe' before anything can fail:
     a zeroed YcChildFd would otherwise claim to own fd 0, and the
     unwind path would close our stdin. */
  for (i = 0; i < 3 + n_other; i++)
    {
      YcChildFd *cfd = i < 3 ? &child->fds[i] : &child->other_fds[i - 3];
      cfd->child_fd = i < 3
                    ? (int) i
                    : create_info->other_fd_infos[i - 3].fd;
      cfd->pipe_fd = -1;
      cfd->closing_fd = -1;
      cfd->child = child;
    }

  for (i = 0; i < 3; i++)
    if (!child_fd_init (child, &child->fds[i], (int) i,
                        &create_info->fd_infos[i], &stdio[i],
                        &dup_fds[i], error_out))
      goto fail;
  for (i = 0; i < n_other; i++)
    {
      YcChildCreateOtherFdInfo *oi = &create_info->other_fd_infos[i];
      if (!child_fd_init (child, &child->other_fds[i], oi->fd,
                          &oi->info, &stdio[oi->fd],
                          &dup_fds[3 + i], error_out))
        goto fail;
    }

  if (!resolve_dup_fds (child, stdio, stdio_count, error_out))
    goto fail;

  child->program = yc_strdup (create_info->program != NULL
                              ? create_info->program
                              : create_info->argv[0]);
  child->argv = dup_string_array (create_info->argv);
  child->env = dup_string_array (create_info->env);

  memset (&options, 0, sizeof (options));
  options.exit_cb = child_exit_cb;
  options.file = child->program;
  options.args = child->argv;
  options.env = child->env;              /* NULL inherits our environment */
  options.stdio = stdio;
  options.stdio_count = (int) stdio_count;

  child->process.data = child;
  uv_err = uv_spawn (container->loop, &child->process, &options);
  if (uv_err != 0)
    {
      set_error (error_out, YC_CHILD_CREATE_ERROR_SPAWN_FAILED, uv_err,
                 "spawning %s: %s", child->program, uv_strerror (uv_err));
      goto fail;
    }
  child->pid = child->process.pid;
  child->n_uv_handles = 1;

  /* uv_spawn() has dup2()ed these into the child; our copies are dead
     weight, and holding the far end of a pipe open would defeat EOF. */
  for (i = 0; i < 3 + n_other; i++)
    if (dup_fds[i] >= 0)
      close (dup_fds[i]);
  yc_free (dup_fds);
  yc_free (stdio);

  for (i = 0; i < 3 + n_other; i++)
    {
      YcChildFd *cfd = i < 3 ? &child->fds[i] : &child->other_fds[i - 3];
      if (cfd->pipe_fd < 0)
        continue;
      if (uv_poll_init (container->loop, &cfd->poll, cfd->pipe_fd) != 0)
        {
          /* The child is already running, so there is no failing the
             call any more; give up on this one fd. */
          close (cfd->pipe_fd);
          cfd->pipe_fd = -1;
          continue;
        }
      cfd->poll.data = cfd;
      cfd->poll_inited = true;
      child->n_uv_handles++;
      child_fd_apply_watch (cfd);
    }

  if (container->n_children == container->children_alloced)
    {
      container->children_alloced = container->max_children > container->n_children
                                  ? container->max_children
                                  : container->n_children + 1;
      container->children = YC_RENEW (YcChild *, container->children,
                                      container->children_alloced);
    }
  child->container_index = container->n_children;
  container->children[container->n_children++] = child;

  return child;

fail:
  if (dup_fds != NULL)
    {
      for (i = 0; i < 3 + n_other; i++)
        if (dup_fds[i] >= 0)
          close (dup_fds[i]);
      yc_free (dup_fds);
    }
  yc_free (stdio);
  /* The caller gets NULL and never saw this child, so it must not see
     closed() for it either. */
  memset (&child->callbacks, 0, sizeof (child->callbacks));
  child_close_all_fds (child);
  child_free (child);
  return NULL;
}
