/* SPDX-License-Identifier: 0BSD */
/* invariant:  if a buffer.size==0, then first_frag/last_frag == NULL.
   corollary:  if a buffer.size==0, then the buffer is using no memory. */

typedef struct _YcBuffer YcBuffer;
typedef struct _YcBufferFragment YcBufferFragment;

typedef void (*YcDestroyNotify) (void *);

#include <stdbool.h>


struct _YcBufferFragment
{
  YcBufferFragment    *next;
  uint8_t              *buf;
  unsigned              buf_max_size;	/* allocation size of buf */
  unsigned              buf_start;	/* offset in buf of valid data */
  unsigned              buf_length;	/* length of valid data in buf; != 0 */
  
  bool           is_foreign;
  YcDestroyNotify      destroy;
  void                 *destroy_data;
};

struct _YcBuffer
{
  unsigned              size;

  YcBufferFragment    *first_frag;
  YcBufferFragment    *last_frag;
};

#define YC_BUFFER_INIT		{ 0, NULL, NULL }


void     yc_buffer_init                (YcBuffer       *buffer);

unsigned yc_buffer_read                (YcBuffer    *buffer,
                                         unsigned      max_length,
                                         void         *data);
unsigned yc_buffer_peek                (const YcBuffer* buffer,
                                         unsigned      max_length,
                                         void         *data);
int      yc_buffer_discard             (YcBuffer    *buffer,
                                         unsigned      max_discard);
char    *yc_buffer_read_line           (YcBuffer    *buffer);

char    *yc_buffer_parse_string0       (YcBuffer    *buffer);
                        /* Returns first char of buffer, or -1. */
int      yc_buffer_peek_byte           (const YcBuffer *buffer);
int      yc_buffer_read_byte           (YcBuffer    *buffer);

/* 
 * Appending to the buffer.
 */
void     yc_buffer_append              (YcBuffer    *buffer, 
                                         unsigned      length,
                                         const void   *data);

static inline void yc_buffer_append_small (YcBuffer    *buffer, 
                                         unsigned      length,
                                         const void   *data);
void     yc_buffer_append_string       (YcBuffer    *buffer, 
                                         const char   *string);
static inline void yc_buffer_append_byte(YcBuffer    *buffer, 
                                         uint8_t       byte);
void     yc_buffer_append_repeated_byte(YcBuffer    *buffer, 
                                         size_t        count,
                                         uint8_t       byte);
#define yc_buffer_append_zeros(buffer, count) \
  yc_buffer_append_repeated_byte ((buffer), 0, (count))


void     yc_buffer_append_string0      (YcBuffer    *buffer,
                                         const char   *string);

void     yc_buffer_append_foreign      (YcBuffer    *buffer,
					 unsigned      length,
                                         const void   *data,
					 YcDestroyNotify destroy,
					 void         *destroy_data);

void     yc_buffer_printf              (YcBuffer    *buffer,
					 const char   *format,
					 ...);
void     yc_buffer_vprintf             (YcBuffer    *buffer,
                                         const char   *format,
                                         va_list       args);

uint8_t  yc_buffer_get_last_byte       (YcBuffer    *buffer);
uint8_t  yc_buffer_get_byte_at         (YcBuffer    *buffer,
                                         size_t        idx);


/* --- appending data that will be filled in later --- */
typedef struct {
  YcBuffer *buffer;
  YcBufferFragment *fragment;
  unsigned offset;
  unsigned length;
} YcBufferPlaceholder;

void     yc_buffer_append_placeholder  (YcBuffer    *buffer,
                                         unsigned      length,
                                         YcBufferPlaceholder *out);
void     yc_buffer_placeholder_set     (YcBufferPlaceholder *placeholder,
                                         const void       *data);

/* --- buffer-to-buffer transfers --- */
/* Take all the contents from src and append
 * them to dst, leaving src empty.
 */
size_t   yc_buffer_transfer            (YcBuffer    *dst,
                                         YcBuffer    *src);

/* Like `transfer', but only transfers some of the data. */
size_t   yc_buffer_transfer_max        (YcBuffer    *dst,
                                         YcBuffer    *src,
					 size_t        max_transfer);

size_t   yc_buffer_append_buffer       (YcBuffer    *dst,
                                         const YcBuffer *src);
size_t   yc_buffer_append_buffer_max   (YcBuffer    *dst,
                                         const YcBuffer *src,
					 size_t        max_transfer);

/* --- deallocating memory used by buffer --- */

/* This deallocates memory used by the buffer-- you are responsible
 * for the allocation and deallocation of the YcBuffer itself. */
void     yc_buffer_clear               (YcBuffer    *to_destroy);

/* Same as calling clear/init */
void     yc_buffer_reset               (YcBuffer    *to_reset);

/* Return a string and clear the buffer. */
char *yc_buffer_empty_to_string (YcBuffer *buffer);

/* --- iterating through the buffer --- */
/* 'frag_offset_out' is the offset of the returned fragment in the whole
   buffer. */
YcBufferFragment *yc_buffer_find_fragment (YcBuffer   *buffer,
                                             unsigned     offset,
                                             unsigned    *frag_offset_out);

/* Free all unused buffer fragments. */
void     _yc_buffer_cleanup_recycling_bin ();


/* misc */
int yc_buffer_index_of(YcBuffer *buffer, char char_to_find);

unsigned yc_buffer_fragment_peek (YcBufferFragment *fragment,
                                   unsigned           offset,
                                   unsigned           length,
                                   void              *buf);
unsigned yc_buffer_fragment_read (YcBufferFragment**fragment_inout,
                                   unsigned          *offset_inout,
                                   unsigned           length,
                                   void              *buf);
bool yc_buffer_fragment_advance (YcBufferFragment **frag_inout,
                                         unsigned           *offset_inout,
                                         unsigned            skip);

/* HACKS */
/* NOTE: the buffer is INVALID after this call, since no empty
   fragments are allowed.  You MUST deal with this if you do 
   not actually add data to the buffer */
void yc_buffer_append_empty_fragment (YcBuffer *buffer);

void yc_buffer_maybe_remove_empty_fragment (YcBuffer *buffer);

/* a way to delete the fragment from yc_buffer_append_empty_fragment() */
void yc_buffer_fragment_free (YcBufferFragment *fragment);


//
// Inline Functions
//

static inline void yc_buffer_append_small(YcBuffer    *buffer, 
                                         unsigned      length,
                                         const void   *data)
{
  YcBufferFragment *f = buffer->last_frag;
  if (f != NULL
   && !f->is_foreign
   && f->buf_start + f->buf_length + length <= f->buf_max_size)
    {
      uint8_t *dst = f->buf + (f->buf_start + f->buf_length);
      const uint8_t *src = data;
      f->buf_length += length;
      buffer->size += length;
      while (length--)
        *dst++ = *src++;
    }
  else
    yc_buffer_append (buffer, length, data);
}
static inline void yc_buffer_append_byte(YcBuffer    *buffer, 
                                            uint8_t       byte)
{
  YcBufferFragment *f = buffer->last_frag;
  if (f != NULL
   && !f->is_foreign
   && f->buf_start + f->buf_length < f->buf_max_size)
    {
      f->buf[f->buf_start + f->buf_length] = byte;
      ++(f->buf_length);
      buffer->size += 1;
    }
  else
    yc_buffer_append (buffer, 1, &byte);
}

