

typedef enum {
  YC_CHILD_STATUS_RUNNING,
  YC_CHILD_STATUS_EXITED,
  YC_CHILD_STATUS_KILLED
} YcChildStatus;


typedef struct YcChild {
  uv_process_t process;
  int fds[3];
  size_t n_other_fds;
  YcChildFd *other_fds;

  YcChildStatus status;
  int status_value;
} YcChild;

typedef enum
{
  YC_CHILD_FD_MODE_NULL,
  YC_CHILD_FD_MODE_PIPE,
  YC_CHILD_FD_MODE_INHERIT,
  YC_CHILD_FD_MODE_FILE
} YcChildFdMode;

typedef struct YcChildCreateFdInfo {
  bool in, out;
  YcChildFdMode mode;
  const char *filename;
} YcChildCreateFdInfo;

typedef struct YcChildCreateOtherFdInfo {
  YcChildCreateFdInfo info;
  int fd;
} YcChildCreateOtherFdInfo;

typedef struct YcChildCallbacks
{
  void (*io)     (YcChild *child,
                  int child_fd,
                  int pipe_fd);
  void (*closed) (YcChild *child,
                  int child_fd,
                  int pipe_fd);
  void (*done)   (YcChild *child);
} YcChildCallbacks;


typedef struct YcChildCreateInfo {
  char **env;
  char **argv;
  char *program;

  YcChildCreateFdInfo fd_infos[3];  // stdin, stdout, stderr
  size_t other_fd_infos;
  YcChildCreateOtherFdInfo *other_fd_infos;

  YcChildCallbacks *callbacks;
  void *user_data;
} YcChildCreateInfo;

typedef struct YcChildContainer
{
  size_t max_children;
  size_t n_children;
  YcChild **children;

  size_t n_done_children;
  YcChild **done_children;
  size_t max_done_children;

  void *container_data;
  YcChildContainerCallbacks callbacks;
} YcChildContainer;


typedef struct YcChildContainerCallbacks
{
  void (*ready_to_spawn)(YcChildContainer *container);
  void (*all_done)(YcChildContainer *container);
  void (*destroy)(YcChildContainer *container);
} YcChildContainerCallbacks;



typedef struct YcChildContainerCreationInfo
{
  size_t max_children;
  YcChildContainerCallbacks callbacks;
  void *container_data;
} YcChildContainerCreationInfo;

YcChildContainer *yc_child_container_new (YcChildContainerCreationInfo *);


#if 0
typedef enum {
  YC_CHILD_ARG_STRATEGY_SIMPLE,
  YC_CHILD_ARG_STRATEGY_SPAWN_SHELL
} YcChildArgStrategy;

typedef struct YcChildFactory {
  uv_loop_t *loop;

  YcChildArgStrategy arg_strategy;

  // These values are set by 'setup'.
  char **env;
  char **argv;
  char *program;
} YcChildFactory;

typedef struct YcChildFactoryOptions {
  uv_loop_t *loop;
  YcChildArgStrategy arg_strategy;
  char *template_cmdline;
} YcChildFactoryOptions;

YcChildFactory *yc_child_factory_new (YcChildFactoryOptions *options);

void yc_child_factory_setup_input (YcChildFactory *factory,
                                   YcInputEntry   *entry);

YcChild *yc_child_factory_make_child(YcChildFactory *factory);
#endif
