
typedef struct YcInputEntry {
  YcInputStream *owner;
  char **values;
} YcInputEntry;

typedef struct YcInputStream {
  size_t          n_columns;
  char          **column_names;
  YcInputEntry *(*next)   (YcInputStream *stream);
  void          (*destroy)(YcInputStream *stream);
} YcInputStream;

typedef enum {
  YC_INPUT_FORMAT_LINE_BY_LINE,
  YC_INPUT_FORMAT_NUL_SEPARATED,
  YC_INPUT_FORMAT_CSV,
  YC_INPUT_FORMAT_TSV,
} YcInputFormatType;

typedef struct {
  YcInputFormatType type;
  const char *comment_prefix;  // NULL for no comments
} YcInputFormat;

typedef enum {
  YC_INPUT_SOURCE_FD,
  YC_INPUT_SOURCE_FILE,
  YC_INPUT_SOURCE_COUNT
} YcInputSourceType;

typedef struct {
  YcInputSourceType type;
  union {
    struct {
      const char *filename;
      YcInputFormat format;
    } file;
    struct {
      int fd;
      YcInputFormat format;
    } fd;
    struct {
      int64_t start, end, step;
    } count;
  } info;
} YcInputSource;

YcInputStream *yc_input_source_make_stream (YcInputSource *source);
