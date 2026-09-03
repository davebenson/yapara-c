

typedef enum {
  YC_EVENT_INIT,
  YC_EVENT_CHILD_STARTED,
  YC_EVENT_CHILD_OUTPUT,
  YC_EVENT_CHILD_OPTIONS_CHANGED,
  YC_EVENT_CHILD_ENDED,
  YC_EVENT_END
} YcEventType;


#define YC_EVENT_HEADER_SIZE 24

#define YC_EVENT_HEADER_MAGIC 0xf23ca891



// Header:
//    YC_EVENT_HEADER_MAGIC little-endian (4 bytes)
//    0 0 0 -- reserved
//    YcEventType (1 byte)
//    payload length (8 bytes)
//    unixtime micros
typedef struct YcEventHeader {
  YcEventType type;
  uint64_t payload_length;
  uint64_t unixtime_micros;
} YcEventHeader;

typedef struct YcEventData {
  YcEventType type;
  uint64_t unixtime_micros;
  union {
    struct {
      uint64_t child_index;
      uint32_t child_pid;
    } child_started;
    struct {
      uint64_t child_index;
      uint32_t fd;      // 1 for stdout; 2 for stderr
      size_t len;
      const uint8_t *data;
    } child_output;
  } info;
  const void *data;
};

