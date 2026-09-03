/* SPDX-License-Identifier: 0BSD */
/* Spawning and supervising child processes.
 *
 * A YcChildContainer owns a libuv event-loop and runs at most
 * 'max_children' processes at a time.  Whenever a slot is free, the
 * container invokes ready_to_spawn(); the application either calls
 * yc_child_new() to fill the slot, or returns without spawning to say
 * that its supply of work is exhausted.  When no children remain and
 * nobody wants to spawn, the loop unwinds and all_done() is invoked.
 *
 * Pipes to the child are exposed as raw non-blocking file-descriptors:
 * the io() callback says "this fd is ready", and the application does
 * its own read()/write().  The application must close each pipe (with
 * yc_child_fd_close()) when it is finished with it; a child is not
 * 'done' until it has exited AND all its pipes are closed.
 */

#ifndef YC_CHILD_H_
#define YC_CHILD_H_

#include <stdbool.h>
#include <stddef.h>
#include <uv.h>

typedef struct YcChild YcChild;
typedef struct YcChildFd YcChildFd;
typedef struct YcChildContainer YcChildContainer;

typedef enum {
  YC_CHILD_STATUS_RUNNING,
  YC_CHILD_STATUS_EXITED,
  YC_CHILD_STATUS_KILLED
} YcChildStatus;

typedef enum
{
  YC_CHILD_FD_MODE_NULL,            /* /dev/null (the default) */
  YC_CHILD_FD_MODE_PIPE,            /* pipe (or socketpair) back to us */
  YC_CHILD_FD_MODE_INHERIT,         /* our fd of the same number */
  YC_CHILD_FD_MODE_FILE,            /* 'filename', opened for us */
  YC_CHILD_FD_MODE_DUP              /* a second name for the child's dup_fd */
} YcChildFdMode;

typedef enum
{
  YC_CHILD_FD_EVENT_READABLE = (1<<0),
  YC_CHILD_FD_EVENT_WRITABLE = (1<<1),
  YC_CHILD_FD_EVENT_HANGUP   = (1<<2)
} YcChildFdEvents;

/* 'in' and 'out' are from the CHILD's point of view: 'in' means the
 * child reads from this fd (so we write to it), 'out' means the child
 * writes to it (so we read).  Both set, with MODE_PIPE, gives a
 * socketpair.  For fds 0/1/2, leaving both false picks the obvious
 * direction (0 => in, 1 and 2 => out).
 *
 * MODE_DUP is '2>&1': the child's fd becomes a second name for its own
 * 'dup_fd', so the two share one file offset.  It is resolved against
 * dup_fd's FINAL setting, which is why the shell parser refuses to
 * translate '2>&1 >log' (see yc-shell.h).
 */
typedef struct YcChildCreateFdInfo {
  bool in, out;
  YcChildFdMode mode;
  const char *filename;             /* MODE_FILE only */
  bool append;                      /* MODE_FILE + out: O_APPEND ('>>') */
  int dup_fd;                       /* MODE_DUP only */
} YcChildCreateFdInfo;

typedef struct YcChildCreateOtherFdInfo {
  YcChildCreateFdInfo info;
  int fd;                           /* fd number in the child; must be >= 3 */
} YcChildCreateOtherFdInfo;

/* One end of one of the child's file-descriptors.  Everything up to
 * 'revents' is public and read-only to the application.
 */
struct YcChildFd {
  int child_fd;                     /* fd number as the child sees it */
  int pipe_fd;                      /* our end, or -1 if not a pipe / closed */
  YcChildFdMode mode;
  int dup_fd;                       /* MODE_DUP only */
  bool in, out;
  YcChildFdEvents revents;          /* what io() is being called about */

  /*< private >*/
  YcChild *child;
  YcChildFdEvents watch;
  uv_poll_t poll;
  bool poll_inited;
  bool poll_started;
  int closing_fd;
};

typedef struct YcChildCallbacks
{
  /* 'pipe_fd' is ready; see child->fds[]/other_fds[] .revents for
   * which direction.  Do a single non-blocking read()/write() here. */
  void (*io)     (YcChild *child,
                  int child_fd,
                  int pipe_fd);

  /* 'pipe_fd' has been closed; it is no longer valid. */
  void (*closed) (YcChild *child,
                  int child_fd,
                  int pipe_fd);

  /* The child has exited and all its pipes are closed.  'child' is
   * freed after this returns, unless container->max_done_children
   * says to retain it. */
  void (*done)   (YcChild *child);
} YcChildCallbacks;

struct YcChild {
  YcChildContainer *container;
  uv_process_t process;
  int pid;
  void *user_data;

  YcChildFd fds[3];                 /* stdin, stdout, stderr */
  size_t n_other_fds;
  YcChildFd *other_fds;

  YcChildStatus status;
  int status_value;                 /* exit status, or signal if KILLED */

  /*< private >*/
  YcChildCallbacks callbacks;
  char *program;
  char **argv;
  char **env;
  size_t n_uv_handles;              /* live handles, incl. 'process' */
  bool process_exited;
  bool done_emitted;
  size_t container_index;
};

typedef struct YcChildCreateInfo {
  char **env;                       /* NULL-terminated, or NULL to inherit */
  char **argv;                      /* NULL-terminated; argv[0] is argv[0] */
  const char *program;              /* NULL means use argv[0] */

  YcChildCreateFdInfo fd_infos[3];  /* stdin, stdout, stderr */
  size_t n_other_fd_infos;
  YcChildCreateOtherFdInfo *other_fd_infos;

  YcChildCallbacks *callbacks;      /* copied, need not outlive the call */
  void *user_data;
} YcChildCreateInfo;

typedef enum {
  YC_CHILD_CREATE_ERROR_NONE = 0,
  YC_CHILD_CREATE_ERROR_BAD_ARGUMENTS,
  YC_CHILD_CREATE_ERROR_CONTAINER_FULL,
  YC_CHILD_CREATE_ERROR_PIPE_FAILED,
  YC_CHILD_CREATE_ERROR_OPEN_FAILED,
  YC_CHILD_CREATE_ERROR_SPAWN_FAILED
} YcChildCreateErrorCode;

/* By-value so that callers can keep it on the stack and ignore
 * cleanup; 'message' is always NUL-terminated. */
typedef struct YcChildCreateError {
  YcChildCreateErrorCode code;
  int sys_errno;                    /* errno, or a negative uv error code */
  char message[256];
} YcChildCreateError;

typedef struct YcChildContainerCallbacks
{
  /* There is room for another child.  Call yc_child_new(), or return
   * without spawning if there is no more work.  Called repeatedly
   * while it keeps spawning and slots remain. */
  void (*ready_to_spawn)(YcChildContainer *container);

  void (*all_done)(YcChildContainer *container);
  void (*destroy)(YcChildContainer *container);
} YcChildContainerCallbacks;

struct YcChildContainer
{
  size_t max_children;
  size_t n_children;
  YcChild **children;

  /* Finished children, oldest first, retained for inspection.
   * max_done_children defaults to 0, meaning children are freed as
   * soon as done() returns. */
  size_t n_done_children;
  YcChild **done_children;
  size_t max_done_children;

  void *container_data;
  YcChildContainerCallbacks callbacks;

  /*< private >*/
  uv_loop_t *loop;
  uv_loop_t own_loop;
  bool owns_loop;
  uv_idle_t ready_idle;
  bool ready_idle_started;
  bool running;
  size_t children_alloced;
  size_t done_children_alloced;
};

typedef struct YcChildContainerCreationInfo
{
  size_t max_children;              /* 0 is treated as 1 */
  YcChildContainerCallbacks callbacks;
  void *container_data;
  uv_loop_t *loop;                  /* NULL to get a private loop */
} YcChildContainerCreationInfo;

YcChildContainer *yc_child_container_new     (YcChildContainerCreationInfo *);

/* Runs the loop until every child is done and ready_to_spawn() declines
 * to start another.  Primes the pump itself, so spawning before the
 * call is optional. */
void              yc_child_container_run     (YcChildContainer *container);

void              yc_child_container_destroy (YcChildContainer *container);

// *error_out is only initialized if return value is NULL.
YcChild          *yc_child_new           (YcChildContainer *container,
                                          YcChildCreateInfo *create_info,
                                          YcChildCreateError *error_out);

/* NULL if the child has no such fd. */
YcChildFd        *yc_child_get_fd        (YcChild *child,
                                          int      child_fd);

/* Which events io() should be called for.  Pipes to the child start
 * watched for their natural direction; drop WRITABLE once you have
 * nothing to write, or the loop will spin. */
void              yc_child_fd_watch      (YcChild *child,
                                          int      child_fd,
                                          YcChildFdEvents events);

/* Close our end; invokes the closed() callback.  Idempotent. */
void              yc_child_fd_close      (YcChild *child,
                                          int      child_fd);

void              yc_child_kill          (YcChild *child,
                                          int      signum);

#endif
