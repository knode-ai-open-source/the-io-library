// SPDX-FileCopyrightText:  2019-2025 Andy Curtis <contactandyc@gmail.com>
// SPDX-FileCopyrightText: 2024-2025 Knode.ai
// SPDX-License-Identifier: Apache-2.0
#include "the-io-library/io_out.h"

#include "the-lz4-library/lz4.h"
#include "a-memory-library/aml_alloc.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

/* options for fixed output -- TODO */
void io_out_ext_options_fixed_compare(io_out_ext_options_t *h,
                                      io_fixed_compare_cb compare, void *arg);
void io_out_ext_options_fixed_sort(io_out_ext_options_t *h,
                                   io_fixed_sort_cb sort, void *arg);
void io_out_ext_options_fixed_reducer(io_out_ext_options_t *h,
                                      io_fixed_reducer_cb reducer, void *arg);

typedef bool (*io_out_write_cb)(io_out_t *h, const void *d, size_t len);

const int IO_OUT_NORMAL_TYPE = 0;
const int IO_OUT_PARTITIONED_TYPE = 1;
const int IO_OUT_SORTED_TYPE = 2;

struct io_out_s {
  int type;
  io_out_options_t options;
  io_out_write_cb write_record;

  int fd;
  bool fd_owner;
  char *filename;
  char *buffer;
  size_t buffer_pos;
  size_t buffer_size;

  // for lz4
  char *buffer2;
  size_t buffer_pos2;
  size_t buffer_size2;

  io_out_write_cb write_d;
  gzFile gz;

  lz4_t *lz4;

  unsigned char delimiter;
  uint32_t fixed;
};

static bool _write_to_gz(gzFile *fd, const char *p, size_t len) {
  ssize_t n;
  const char *ep = p + len;
  while (p < ep) {
    if (ep - p > 0x7FFFFFFFU)
      n = gzwrite(*fd, p, 0x7FFFFFFFU);
    else
      n = gzwrite(*fd, p, ep - p);
    if (n > 0)
      p += n;
    else {
      int gzerrno;
      gzerror(*fd, &gzerrno);
      if (gzerrno == Z_ERRNO && errno == ENOSPC) {
        time_t cur_time = time(NULL);
        fprintf(stderr, "%s ERROR DISK FULL %s\n", aml_file_line(),
                ctime(&cur_time));
      }
      gzclose(*fd);
      *fd = NULL;
      return false;
    }
  }
  return true;
}

static bool _write_to_fd(int *fd, const char *p, size_t len) {
  ssize_t n;
  const char *ep = p + len;
  while (p < ep) {
    if (ep - p > 0x7FFFFFFFU)
      n = write(*fd, p, 0x7FFFFFFFU);
    else
      n = write(*fd, p, ep - p);
    if (n > 0)
      p += n;
    else {
      if (n == -1 && errno == ENOSPC) {
        time_t cur_time = time(NULL);
        fprintf(stderr, "%s ERROR DISK FULL %s\n", aml_file_line(),
                ctime(&cur_time));
      }
      return false;
    }
  }
  return true;
}

static bool _write_to_lz4(io_out_t *h, const char *p, size_t len) {
start:;
  bool written = true;
  if (len) {
    char *wp = h->buffer2 + h->buffer_pos2;
    char *ep = h->buffer2 + h->buffer_size2;
    char *mp = wp + lz4_compress_bound(len) + 8;
    written = false;
    if (mp <= ep) {
      uint32_t n = lz4_compress_block(h->lz4, p, len, wp, mp - wp);
      wp += n;
      h->buffer_pos2 += n;
      if (wp < ep)
        return true;
      written = true;
    }
  }
  if (!_write_to_fd(&(h->fd), h->buffer2, h->buffer_pos2)) {
    if (h->fd_owner)
      close(h->fd);
    h->fd = -1;
    return false;
  }

  h->buffer_pos2 = 0;
  if (!written)
    goto start;
  return true;
}

static bool _io_out_write(io_out_t *h, const void *d, size_t len) {
  if (h->buffer_pos + len < h->buffer_size) {
    if (len) {
      memcpy(h->buffer + h->buffer_pos, d, len);
      h->buffer_pos += len;
    }
    if (len)
      return true;
    else {
      if (!_write_to_fd(&(h->fd), h->buffer, h->buffer_pos)) {
        if (h->fd_owner)
          close(h->fd);
        h->fd = -1;
        return false;
      }
      h->buffer_pos = 0;
      return true;
    }
  }
  size_t diff = h->buffer_size - h->buffer_pos;
  memcpy(h->buffer + h->buffer_pos, d, diff);
  h->buffer_pos += diff;
  if (!_write_to_fd(&(h->fd), h->buffer, h->buffer_pos)) {
    if (h->fd_owner)
      close(h->fd);
    h->fd = -1;
    return false;
  }
  char *p = (char *)d;
  p += diff;
  len -= diff;
  h->buffer_pos = 0;
  if (len >= h->buffer_size) {
    if (!_write_to_fd(&(h->fd), p, len)) {
      if (h->fd_owner)
        close(h->fd);
      h->fd = -1;

      return false;
    }
  } else {
    memcpy(h->buffer, p, len);
    h->buffer_pos = len;
  }
  return true;
}

static bool _io_out_write_lz4(io_out_t *h, const void *d, size_t len) {
  // printf("writing %lu\n", len);
  if (h->buffer_pos + len <= h->buffer_size) {
    if (len)
      memcpy(h->buffer + h->buffer_pos, d, len);
    h->buffer_pos += len;
    if (len)
      return true;
    else {
      if (!_write_to_lz4(h, h->buffer, h->buffer_pos))
        return false;
      h->buffer_pos = 0;
      char *wp = h->buffer2 + h->buffer_pos2;
      uint32_t n = lz4_finish(h->lz4, wp);
      h->buffer_pos2 += n;
      if (!_write_to_lz4(h, NULL, 0))
        return false;
      return true;
    }
  }
  size_t diff = h->buffer_size - h->buffer_pos;
  memcpy(h->buffer + h->buffer_pos, d, diff);
  h->buffer_pos += diff;
  if (!_write_to_lz4(h, h->buffer, h->buffer_pos))
    return false;
  char *p = (char *)d;
  p += diff;
  len -= diff;
  h->buffer_pos = 0;
  while (len >= h->buffer_size) {
    if (!_write_to_lz4(h, p, h->buffer_size))
      return false;
    len -= h->buffer_size;
    p += h->buffer_size;
  }
  if (len) {
    memcpy(h->buffer, p, len);
    h->buffer_pos = len;
  }
  return true;
}

static bool _io_out_write_gz(io_out_t *h, const void *d, size_t len) {
  if (h->buffer_pos + len < h->buffer_size) {
    memcpy(h->buffer + h->buffer_pos, d, len);
    h->buffer_pos += len;
    if (len)
      return true;
    else {
      if (!_write_to_gz(&(h->gz), h->buffer, h->buffer_pos))
        return false;
      h->buffer_pos = 0;
      return true;
    }
  }
  size_t diff = h->buffer_size - h->buffer_pos;
  memcpy(h->buffer + h->buffer_pos, d, diff);
  h->buffer_pos += diff;
  if (!_write_to_gz(&(h->gz), h->buffer, h->buffer_pos))
    return false;
  char *p = (char *)d;
  p += diff;
  len -= diff;
  h->buffer_pos = 0;
  while (len >= h->buffer_size) {
    if (!_write_to_gz(&(h->gz), p, h->buffer_size))
      return false;
    len -= h->buffer_size;
    p += h->buffer_size;
  }
  if (len) {
    memcpy(h->buffer, p, len);
    h->buffer_pos = len;
  }
  return true;
}

static io_out_t *_io_out_init_lz4(const char *filename, int fd, bool fd_owner,
                                  io_out_options_t *options) {
  bool append_mode = options->append_mode;
  if (append_mode) {
    abort(); // for now

    int fd = open(filename, O_RDONLY);
    char header[7];
    int n = read(fd, header, 7);
    close(fd);
    if (n == 0)
      append_mode = false;
    else if (n != 7)
      return NULL;
    lz4_header_t h;
    if (!lz4_check_header(&h, header, 7))
      return NULL;
    if (h.content_checksum) /* cannot append if content_checksum exists */
      return NULL;
    /* seek to last valid block - for now, disallow this?  maybe create
       a routine which will trim file based upon last valid record.  It
       should check blocksum if they exist within lz4 */
    return NULL; // for now
  }

  size_t buffer_size = options->buffer_size;
  lz4_t *lz4 =
      lz4_init(options->level, options->size, options->block_checksum,
                  options->content_checksum);
  uint32_t compressed_size = lz4_compressed_size(lz4);
  uint32_t block_size = lz4_block_size(lz4);

  if (buffer_size < compressed_size + block_size + 8)
    buffer_size = compressed_size + block_size + 8;

  int filename_length = filename ? strlen(filename) + 1 : 0;

  int extra = options->safe_mode ? (filename_length * 2) + 20 : 0;
  extra += options->write_ack_file ? 5 : 0;

  io_out_t *h = (io_out_t *)aml_malloc(sizeof(io_out_t) + buffer_size + 8 +
                                      filename_length + extra);
  memset(h, 0, sizeof(*h));
  h->fd = fd;
  h->lz4 = lz4;
  h->buffer = (char *)(h + 1);
  h->buffer2 = h->buffer + block_size;
  h->filename = filename_length ? h->buffer + buffer_size + 8 : NULL;
  if (h->filename) {
    strcpy(h->filename, filename);
    if (!io_make_path_valid(h->filename)) {
      aml_free(h);
      return NULL;
    }
  }
  h->buffer_size = block_size;
  h->buffer_size2 = buffer_size - block_size;
  char *tmp = h->filename;
  if (options->safe_mode) {
    tmp = tmp + strlen(h->filename) + 1;
    strcpy(tmp, h->filename);
    tmp[strlen(tmp) - 4] = 0;
    strcat(tmp, "-safe.lz4");
  }

  if (h->fd == -1)
    h->fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0777);
  uint32_t header_size = 0;
  const char *header = lz4_get_header(lz4, &header_size);

  memcpy(h->buffer2, header, header_size);
  h->buffer_pos2 = header_size;
  h->options = *options;
  h->write_d = _io_out_write_lz4;
  return h;
}

static io_out_t *_io_out_init_gz(const char *filename, int fd, bool fd_owner,
                                 io_out_options_t *options) {
  size_t buffer_size = options->buffer_size;
  bool append_mode = options->append_mode;

  if (buffer_size < (64 * 1024))
    buffer_size = 64 * 1024;

  int filename_length = filename ? strlen(filename) + 1 : 0;

  int extra = options->safe_mode ? (filename_length * 2) + 20 : 0;
  extra += options->write_ack_file ? 5 : 0;

  io_out_t *h = (io_out_t *)aml_malloc(sizeof(io_out_t) + buffer_size +
                                      filename_length + extra);
  memset(h, 0, sizeof(*h));
  h->fd = -1;
  h->buffer = (char *)(h + 1);

  h->filename = filename_length ? h->buffer + buffer_size : NULL;
  if (h->filename) {
    strcpy(h->filename, filename);
    if (!io_make_path_valid(h->filename)) {
      aml_free(h);
      return NULL;
    }
  }
  h->buffer_size = buffer_size;
  h->options = *options;
  char *tmp = h->filename;
  if (options->safe_mode) {
    tmp = tmp + strlen(h->filename) + 1;
    strcpy(tmp, h->filename);
    tmp[strlen(tmp) - 3] = 0;
    strcat(tmp, "-safe.gz");
  }

  char mode[3];
  mode[0] = append_mode ? 'a' : 'w';
  mode[1] = options->level + '0';
  mode[2] = 0;

  if (fd != -1)
    h->gz = gzdopen(fd, mode);
  else
    h->gz = gzopen(tmp, mode);
  h->write_d = _io_out_write_gz;
  return h;
}

static io_out_t *_io_out_init(const char *filename, int fd, bool fd_owner,
                              io_out_options_t *options) {
  size_t buffer_size = options->buffer_size;
  bool append_mode = options->append_mode;

  int filename_length = filename ? strlen(filename) + 1 : 0;

  int extra = options->safe_mode ? (filename_length * 2) + 20 : 0;
  extra += options->write_ack_file ? 5 : 0;
  io_out_t *h = (io_out_t *)aml_malloc(sizeof(io_out_t) + buffer_size +
                                      filename_length + extra);
  memset(h, 0, sizeof(*h));
  h->buffer = (char *)(h + 1);
  h->filename = filename_length ? h->buffer + buffer_size : NULL;
  if (h->filename) {
    strcpy(h->filename, filename);
    if (!io_make_path_valid(h->filename)) {
      aml_free(h);
      return NULL;
    }
  }
  h->options = *options;
  h->buffer_size = buffer_size;
  char *tmp = h->filename;
  if (options->safe_mode) {
    tmp = tmp + strlen(h->filename) + 1;
    strcpy(tmp, h->filename);
    strcat(tmp, "-safe");
  }

  if (fd != -1)
    h->fd = fd;
  else if (append_mode)
    h->fd = open(tmp, O_WRONLY | O_CREAT | O_APPEND, 0777);
  else {
    h->fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0777);
    if (h->fd == -1) {
      perror("Unable to open file\n");
    }
  }
  h->write_d = _io_out_write;
  return h;
}

void io_out_options_init(io_out_options_t *h) {
  memset(h, 0, sizeof(*h));

  h->buffer_size = 64 * 1024;
  h->append_mode = false;
  h->safe_mode = false;
  h->write_ack_file = false;
  h->level = 1;
  h->size = s64kb;
  h->block_checksum = false;
  h->content_checksum = false;
  h->abort_on_error = false;
  h->format = 0;
  h->lz4 = false;
  h->gz = false;
}

void io_out_options_buffer_size(io_out_options_t *h, size_t buffer_size) {
  h->buffer_size = buffer_size;
}

void io_out_options_format(io_out_options_t *h, io_format_t format) {
  h->format = format;
}

void io_out_options_abort_on_error(io_out_options_t *h) {
  h->abort_on_error = true;
}

void io_out_options_append_mode(io_out_options_t *h) { h->append_mode = true; }

void io_out_options_safe_mode(io_out_options_t *h) { h->safe_mode = true; }

void io_out_options_write_ack_file(io_out_options_t *h) {
  h->write_ack_file = true;
}

void io_out_options_gz(io_out_options_t *h, int level) {
  h->gz = true;
  h->level = level;
}

void io_out_options_lz4(io_out_options_t *h, int level,
                        lz4_block_size_t size, bool block_checksum,
                        bool content_checksum) {
  h->lz4 = true;
  h->level = level;
  h->size = size;
  h->block_checksum = block_checksum;
  h->content_checksum = content_checksum;
}

void io_out_ext_options_init(io_out_ext_options_t *h) {
  memset(h, 0, sizeof(*h));
  // h->lz4_tmp = false;
  h->lz4_tmp = true;
}

void io_out_ext_options_sort_while_partitioning(io_out_ext_options_t *h) {
  h->sort_while_partitioning = true;
}

void io_out_ext_options_num_sort_threads(io_out_ext_options_t *h,
                                         size_t num_sort_threads) {
  h->num_sort_threads = num_sort_threads;
}

void io_out_ext_options_sort_before_partitioning(io_out_ext_options_t *h) {
  h->sort_before_partitioning = true;
}

void io_out_ext_options_use_extra_thread(io_out_ext_options_t *h) {
  h->use_extra_thread = true;
}

void io_out_ext_options_dont_compress_tmp(io_out_ext_options_t *h) {
  h->lz4_tmp = false;
}

/* options for creating a partitioned output */
void io_out_ext_options_partition(io_out_ext_options_t *h,
                                  io_partition_cb part, void *arg) {
  h->partition = part;
  h->partition_arg = arg;
}

void io_out_ext_options_num_partitions(io_out_ext_options_t *h,
                                       size_t num_partitions) {
  h->num_partitions = num_partitions;
}

/* options for sorting the output */
void io_out_ext_options_compare(io_out_ext_options_t *h,
                                io_compare_cb compare, void *arg) {
  h->compare = compare;
  h->compare_arg = arg;
}

void io_out_ext_options_intermediate_group_size(io_out_ext_options_t *h,
                                                size_t num_per_group) {
  h->num_per_group = num_per_group;
}

void io_out_ext_options_intermediate_compare(io_out_ext_options_t *h,
                                             io_compare_cb compare,
                                             void *arg) {
  h->int_compare = compare;
  h->int_compare_arg = arg;
}

/* set the reducer */
void io_out_ext_options_reducer(io_out_ext_options_t *h,
                                io_reducer_cb reducer, void *arg) {
  h->reducer = reducer;
  h->reducer_arg = arg;
}

void io_out_ext_options_intermediate_reducer(io_out_ext_options_t *h,
                                             io_reducer_cb reducer,
                                             void *arg) {
  h->int_reducer = reducer;
  h->int_reducer_arg = arg;
}

/* options for fixed output */
void io_out_ext_options_fixed_reducer(io_out_ext_options_t *h,
                                      io_fixed_reducer_cb reducer,
                                      void *arg) {
  h->fixed_reducer = reducer;
  h->fixed_reducer_arg = arg;
}

void io_out_ext_options_fixed_compare(io_out_ext_options_t *h,
                                      io_fixed_compare_cb compare,
                                      void *arg) {
  h->fixed_compare = compare;
  h->fixed_compare_arg = arg;
}

void io_out_ext_options_fixed_sort(io_out_ext_options_t *h,
                                   io_fixed_sort_cb sort, void *arg) {
  h->fixed_sort = sort;
  h->fixed_sort_arg = arg;
}

bool _io_out_write_prefix(io_out_t *h, const void *d, size_t len) {
  uint32_t length = len;
  if (!io_out_write(h, &length, sizeof(length)) || !io_out_write(h, d, length))
    return false;
  return true;
}

bool io_out_write_prefix(io_out_t *h, const void *d, size_t len) {
  if (h->type)
    return false;
  return _io_out_write_prefix(h, d, len);
}

static bool _io_out_write_delimiter(io_out_t *h, const void *d, size_t len) {
  if (!io_out_write(h, d, len) ||
      !io_out_write(h, &h->delimiter, sizeof(h->delimiter)))
    return false;
  return true;
}

static bool _io_out_write_fixed(io_out_t *h, const void *d, size_t len) {
  if (len != h->fixed)
    abort();
  return io_out_write(h, d, len);
}

bool io_out_write_delimiter(io_out_t *h, const void *d, size_t len,
                            char delim) {
  if (h->type)
    return false;
  if (!io_out_write(h, d, len) || !io_out_write(h, &delim, sizeof(delim)))
    return false;
  return true;
}

io_out_t *_io_out_init_(const char *filename, int fd, bool fd_owner,
                        io_out_options_t *options) {
  io_out_options_t opts;
  if (!options) {
    options = &opts;
    io_out_options_init(options);
  }

  if (!filename && fd == -1)
    abort();
  if (fd != -1 && options->append_mode)
    abort();
  if (options->safe_mode && options->append_mode) /* not a valid combination */
    abort();
  if (fd != -1 && (options->safe_mode || options->write_ack_file))
    abort();

  io_out_t *h;
  if ((!filename && options->lz4) || io_extension(filename, "lz4"))
    h = _io_out_init_lz4(filename, fd, fd_owner, options);
  else if ((!filename && options->gz) || io_extension(filename, "gz"))
    h = _io_out_init_gz(filename, fd, fd_owner, options);
  else
    h = _io_out_init(filename, fd, fd_owner, options);

  if (h) {
    if (options->format < 0) {
      int delim = (-options->format) - 1;
      if(delim >= 256) // for output, csv format is expected to be done before write_record
        delim -= 256;
      h->delimiter = delim;
      h->write_record = _io_out_write_delimiter;
    } else if (options->format > 0) {
      h->fixed = options->format;
      h->write_record = _io_out_write_fixed;
    } else
      h->write_record = _io_out_write_prefix;
  } else if (options->abort_on_error)
    abort();
  return h;
}

io_out_t *io_out_init(const char *filename, io_out_options_t *options) {
  return _io_out_init_(filename, -1, true, options);
}

io_out_t *io_out_init_with_fd(int fd, bool fd_owner,
                              io_out_options_t *options) {
  return _io_out_init_(NULL, fd, fd_owner, options);
}

bool io_out_write_record(io_out_t *h, const void *d, size_t len) {
  return h->write_record(h, d, len);
}

bool io_out_write(io_out_t *h, const void *d, size_t len) {
  if (h->type)
    return false;
  if (!len)
    return true;
  if (h->write_d) {
    if (!h->write_d(h, d, len)) {
      h->write_d = NULL;
      if (h->options.abort_on_error)
        abort();
      return false;
    }
    return true;
  }
  if (h->options.abort_on_error)
    abort();
  return false;
}

static bool io_out_flush(io_out_t *h) {
  if (h->write_d) {
    if (!h->write_d(h, NULL, 0)) {
      h->write_d = NULL;
      if (h->options.abort_on_error)
        abort();
      return false;
    }
    return true;
  }
  if (h->options.abort_on_error)
    abort();
  return false;
}

static void io_out_ext_destroy(io_out_t *hp);

void _io_out_destroy(io_out_t *h) {
  io_out_flush(h);
  if (h->fd > -1 && h->fd_owner) {
    close(h->fd);
    h->fd = -1;
  }

  if (h->lz4) {
    lz4_destroy(h->lz4);
    h->lz4 = NULL;
  }
  if (h->gz) {
    gzclose(h->gz);
    h->gz = NULL;
  }
}

void remove_out(io_out_t *h) {
  if (h->options.safe_mode)
    remove(h->filename + strlen(h->filename) + 1);
  else
    remove(h->filename);

  aml_free(h);
}

io_in_t *io_out_normal_in(io_out_t *h) {
  _io_out_destroy(h);
  char *filename = h->filename;
  if (!filename)
    return NULL;

  if (h->options.safe_mode)
    filename = filename + strlen(filename) + 1;

  io_in_options_t opts;
  io_in_options_init(&opts);
  io_in_options_buffer_size(&opts, h->buffer_size);
  io_in_options_format(&opts, h->options.format);
  io_in_t *in = io_in_init(filename, &opts);
  if (in)
    io_in_destroy_out(in, h, remove_out);
  return in;
}

void io_out_destroy(io_out_t *h) {
  if (h->type != IO_OUT_NORMAL_TYPE) {
    io_out_ext_destroy(h);
    return;
  }

  _io_out_destroy(h);

  if (h->options.safe_mode)
    rename(h->filename + strlen(h->filename) + 1, h->filename);

  if (h->options.write_ack_file) {
    strcat(h->filename, ".ack");
    FILE *out = fopen(h->filename, "wb");
    fclose(out);
  }

  aml_free(h);
}

/** io_out_ext functionality **/
static void suffix_filename_with_id(char *dest, size_t dest_len, const char *filename, size_t id,
                                    const char *extra, bool use_lz4) {
  strcpy(dest, filename);
  dest_len -= strlen(dest);
  dest += strlen(dest);

  if (io_extension(filename, "lz4"))
    snprintf(dest - 4, dest_len + 4, "%s%s_%lu.lz4", extra ? "_" : "",
            extra ? extra : "", id);
  else if (io_extension(filename, "gz")) {
    if (use_lz4)
      snprintf(dest - 3, dest_len + 3, "%s%s_%lu.lz4", extra ? "_" : "",
              extra ? extra : "", id);
    else
      snprintf(dest - 3, dest_len + 3, "%s%s_%lu.gz", extra ? "_" : "",
              extra ? extra : "", id);
  } else {
    if (use_lz4)
      snprintf(dest, dest_len, "%s%s_%lu.lz4", extra ? "_" : "",
              extra ? extra : "", id);
    else
      snprintf(dest, dest_len, "%s%s_%lu", extra ? "_" : "",
               extra ? extra : "", id);
  }
}

/* used to create a partitioned filename */
void io_out_partition_filename(char *dest, const char *filename, size_t id) {
  suffix_filename_with_id(dest, strlen(filename) + 20, filename, id, NULL, false);
}

/** io_out_partitioned_t **/
typedef struct {
  int type;
  io_out_options_t options;
  io_out_write_cb write_record;

  char *filename;

  io_out_ext_options_t ext_options;

  io_out_options_t part_options;
  io_out_ext_options_t ext_part_options;

  io_in_options_t in_options;

  io_out_t **partitions;
  size_t num_partitions;
  io_partition_cb partition;
  void *partition_arg;

  size_t *tasks;
  size_t *taskp;
  size_t *taskep;
  pthread_mutex_t mutex;
} io_out_partitioned_t;

bool write_partitioned_record(io_out_t *hp, const void *d, size_t len) {
  io_out_partitioned_t *h = (io_out_partitioned_t *)hp;

  io_record_t r;
  r.length = len;
  r.record = (char *)d;
  r.tag = 0;

  size_t partition = h->partition(&r, h->num_partitions, h->partition_arg);
  if (partition >= h->num_partitions)
    return false;

  io_out_t *o = h->partitions[partition];
  return o->write_record(o, d, len);
}

io_out_t *io_out_partitioned_init(const char *filename,
                                  io_out_options_t *options,
                                  io_out_ext_options_t *ext_options) {
  if (ext_options->num_partitions == 0) {
    io_partition_cb partition = ext_options->partition;
    ext_options->partition = NULL;
    io_out_t *r = io_out_ext_init(filename, options, ext_options);
    ext_options->partition = partition;
    return r;
  } else if (ext_options->num_partitions == 1) {
    if (!filename)
      abort();
    // give suffix to filename
    size_t tmp_name_len = strlen(filename) + 40;
    char *tmp_name = (char *)aml_malloc(tmp_name_len);
    io_partition_cb partition = ext_options->partition;
    ext_options->partition = NULL;
    suffix_filename_with_id(tmp_name, tmp_name_len, filename, 0, NULL, false);
    io_out_t *r = io_out_ext_init(tmp_name, options, ext_options);
    ext_options->partition = partition;
    aml_free(tmp_name);
    return r;
  } else {
    if (!filename)
      abort();

    io_out_partitioned_t *h = (io_out_partitioned_t *)aml_malloc(
        sizeof(io_out_partitioned_t) + strlen(filename) + 1 +
        (sizeof(io_out_t *) * ext_options->num_partitions));
    memset(h, 0, sizeof(*h));
    h->options = *options;
    h->part_options = *options;
    h->ext_options = *ext_options;
    h->ext_part_options = *ext_options;
    h->partitions = (io_out_t **)(h + 1);
    h->num_partitions = ext_options->num_partitions;
    h->filename = (char *)(h->partitions + ext_options->num_partitions);
    strcpy(h->filename, filename);
    h->partition = ext_options->partition;
    h->partition_arg = ext_options->partition_arg;

    h->part_options.buffer_size = options->buffer_size / h->num_partitions;
    h->ext_part_options.partition = NULL;

    if (!h->ext_options.sort_while_partitioning) {
      io_out_options_format(&(h->part_options), io_prefix());
      h->part_options.write_ack_file = false;
    }

    size_t tmp_name_len = strlen(filename) + 40;
    char *tmp_name = (char *)aml_malloc(tmp_name_len);
    for (size_t i = 0; i < h->num_partitions; i++) {
      // printf("%s\n", tmp_name);
      if (h->ext_options.sort_while_partitioning || !h->ext_options.compare) {
        suffix_filename_with_id(tmp_name, tmp_name_len, filename, i, NULL, false);
        h->partitions[i] = io_out_ext_init(tmp_name, &(h->part_options),
                                           &(h->ext_part_options));
      } else {
        suffix_filename_with_id(tmp_name, tmp_name_len, filename, i, "unsorted",
                                h->ext_options.lz4_tmp);
        h->partitions[i] = io_out_init(tmp_name, &(h->part_options));
      }
    }
    h->write_record = write_partitioned_record;
    aml_free(tmp_name);
    h->type = IO_OUT_PARTITIONED_TYPE;
    return (io_out_t *)h;
  }
}

void *sort_partitions(void *arg) {
  io_out_partitioned_t *h = (io_out_partitioned_t *)arg;
  char *filename = h->filename;
  size_t tmp_name_len = strlen(filename) + 40;
  char *tmp_name = (char *)aml_malloc(tmp_name_len);

  while (true) {
    pthread_mutex_lock(&h->mutex);
    size_t *tp = h->taskp;
    h->taskp++;
    pthread_mutex_unlock(&h->mutex);
    if (tp >= h->taskep)
      break;

    suffix_filename_with_id(tmp_name, tmp_name_len, filename, *tp, "unsorted",
                            h->ext_options.lz4_tmp);
    io_in_t *in = io_in_init(tmp_name, &(h->in_options));
    suffix_filename_with_id(tmp_name, tmp_name_len, filename, *tp, NULL, false);
    io_out_t *out =
        io_out_ext_init(tmp_name, &(h->part_options), &(h->ext_part_options));
    io_record_t *r;
    while ((r = io_in_advance(in)) != NULL)
      io_out_write_record(out, r->record, r->length);
    io_out_destroy(out);
    io_in_destroy(in);
  }
  aml_free(tmp_name);
  return NULL;
}

void _io_out_partitioned_destroy(io_out_t *hp) {
  io_out_partitioned_t *h = (io_out_partitioned_t *)hp;
  for (size_t i = 0; i < h->num_partitions; i++) {
    io_out_destroy(h->partitions[i]);
  }
  if (!h->ext_options.sort_while_partitioning && h->ext_options.compare) {
    /*  buffer_size memory, num_threads, input, output - prefer input
       because OS will buffer output.
      */
    size_t num_threads = h->ext_options.num_sort_threads;
    if (num_threads < 1)
      num_threads = 1;
    if (num_threads > h->num_partitions)
      num_threads = h->num_partitions;

    size_t buffer_size = h->options.buffer_size / (num_threads * 2);

    io_out_options_buffer_size(&(h->part_options), buffer_size);
    io_out_options_format(&(h->part_options), h->options.format);
    h->ext_part_options.use_extra_thread = false;
    io_in_options_init(&(h->in_options));
    io_in_options_buffer_size(&(h->in_options), buffer_size);
    io_in_options_format(&(h->in_options), io_prefix());

    h->tasks = (size_t *)aml_malloc(sizeof(size_t) * h->num_partitions);
    h->taskp = h->tasks;
    h->taskep = h->tasks + h->num_partitions;
    for (size_t i = 0; i < h->num_partitions; i++)
      h->tasks[i] = i;

    pthread_mutex_init(&h->mutex, NULL);
    pthread_t *threads =
        (pthread_t *)aml_malloc(sizeof(pthread_t) * num_threads);
    for (size_t i = 0; i < num_threads; i++)
      pthread_create(threads + i, NULL, sort_partitions, h);
    for (size_t i = 0; i < num_threads; i++)
      pthread_join(threads[i], NULL);
    pthread_mutex_destroy(&h->mutex);
    aml_free(h->tasks);
    aml_free(threads);
    char *filename = h->filename;
    size_t tmp_name_len = strlen(h->filename) + 40;
    char *tmp_name = (char *)aml_malloc(tmp_name_len);
    for (size_t i = 0; i < h->num_partitions; i++) {
      suffix_filename_with_id(tmp_name, tmp_name_len, filename, i, "unsorted",
                              h->ext_options.lz4_tmp);
      remove(tmp_name);
    }
    aml_free(tmp_name);
  }
}

io_in_t *io_out_partitioned_in(io_out_t *hp) {
  _io_out_partitioned_destroy(hp);
  /*  TODO:
      1. either a list of files or a compared set with heap */
  // for now return NULL as this would be unusual case
  aml_free(hp);
  return NULL;
}

void io_out_partitioned_destroy(io_out_t *hp) {
  _io_out_partitioned_destroy(hp);
  aml_free(hp);
}

typedef struct {
  char *buffer;
  char *bp;
  char *ep;
  size_t num_records;
  size_t size;
} io_out_buffer_t;

const int EXTRA_IN = 0;
const int EXTRA_FILENAME = 1;
const int EXTRA_FILE_TO_REMOVE = EXTRA_FILENAME;
const int EXTRA_ACK_FILE = EXTRA_FILENAME | 2;

typedef struct extra_s {
  int type;
  void *p;
  struct extra_s *next;
} extra_t;

typedef struct {
  int type;
  io_out_options_t options;
  io_out_write_cb write_record;

  io_in_options_t file_options;

  char *filename;
  char *suffix;

  char *tmp_filename;

  io_out_buffer_t buf1, buf2;
  io_out_buffer_t *b, *b2;

  size_t num_written;
  size_t num_group_written;

  bool thread_started;
  pthread_t thread;
  bool out_in_called;
  extra_t *extras;

  int tag;

  io_out_ext_options_t ext_options;
  io_out_ext_options_t partition_options;
} io_out_sorted_t;

bool write_sorted_record(io_out_t *hp, const void *d, size_t len);

static void _extra_add(io_out_t *hp, void *p, int type) {
  io_out_sorted_t *h = (io_out_sorted_t *)hp;
  extra_t *extra;
  if (type & EXTRA_FILENAME) {
    char *f = (char *)p;
    extra = (extra_t *)aml_malloc(sizeof(extra_t) + strlen(f) + 1);
    extra->p = (void *)(extra + 1);
    strcpy((char *)extra->p, f);
  } else {
    extra = (extra_t *)aml_malloc(sizeof(extra_t));
    extra->p = p;
  }
  extra->type = type;
  extra->next = h->extras;
  h->extras = extra;
}

void io_out_sorted_add_in(io_out_t *hp, io_in_t *in) {
  _extra_add(hp, in, EXTRA_IN);
}

void io_out_sorted_add_file_to_remove(io_out_t *hp, const char *filename) {
  _extra_add(hp, (void *)filename, EXTRA_FILE_TO_REMOVE);
}

void io_out_sorted_add_ack_file(io_out_t *hp, const char *filename) {
  _extra_add(hp, (void *)filename, EXTRA_ACK_FILE);
}

static void tmp_filename(char *dest, const char *filename, uint32_t n,
                         const char *suffix) {
  snprintf(dest, strlen(filename) + 24 + strlen(suffix), "%s_%u_tmp%s", filename, n, suffix);
}

static void group_tmp_filename(char *dest, const char *filename, uint32_t n,
                               const char *suffix) {
  snprintf(dest, strlen(filename) + strlen(suffix) + 30, "%s_%u_gtmp%s", filename, n, suffix);
}

static inline void clear_buffer(io_out_buffer_t *b) {
  b->bp = b->buffer;
  b->ep = b->bp + b->size;
  b->num_records = 0;
}

static inline void init_buffer(io_out_buffer_t *b, size_t buffer_size) {
  b->buffer = (char *)aml_zalloc(buffer_size);
  b->size = buffer_size;
  clear_buffer(b);
}

static io_in_t *_in_from_buffer(io_out_sorted_t *h, io_out_buffer_t *b) {
  if (!b->num_records)
    return NULL;

  io_record_t *r = (io_record_t *)b->buffer;
  uint32_t num_r = b->num_records;
  io_sort_records(r, num_r, h->ext_options.int_compare,
                     h->ext_options.int_compare_arg);

  clear_buffer(b);
  return io_in_records_init(r, num_r, &(h->file_options));
}

io_out_t *io_out_sorted_init(const char *filename, io_out_options_t *options,
                             io_out_ext_options_t *ext_options) {
  io_out_options_t opts;
  if (!options) {
    options = &opts;
    io_out_options_init(options);
  }
  size_t buffer_size = options->buffer_size;
  io_out_sorted_t *h = (io_out_sorted_t *)aml_zalloc(
      sizeof(io_out_sorted_t) + (strlen(filename) * 3) + 100);
  h->filename = (char *)(h + 1);
  strcpy(h->filename, filename);
  h->type = IO_OUT_SORTED_TYPE;

  h->out_in_called = false;

  h->tmp_filename = h->filename + strlen(filename) + 1;
  if (io_extension(filename, "lz4")) {
    h->filename[strlen(filename) - 4] = 0;
    h->suffix = (char *)".lz4";
  } else if (io_extension(filename, "gz")) {
    h->suffix = (char *)".gz";
    h->filename[strlen(filename) - 3] = 0;
  }

  h->thread_started = false;

  h->ext_options = *ext_options;
  h->partition_options = *ext_options;
  h->partition_options.compare = NULL;
  h->options = *options;

  io_in_options_init(&(h->file_options));
  if (ext_options->int_reducer)
    io_in_options_reducer(&(h->file_options), ext_options->int_compare,
                          ext_options->int_compare_arg,
                          ext_options->int_reducer,
                          ext_options->int_reducer_arg);

  if (ext_options->use_extra_thread) {
    buffer_size /= 2;
    init_buffer(&h->buf1, buffer_size);
    init_buffer(&h->buf2, buffer_size);
    h->b = &(h->buf1);
    h->b2 = &(h->buf2);
  } else {
    init_buffer(&h->buf1, buffer_size);
    h->b = &(h->buf1);
    h->b2 = &(h->buf1);
  }
  h->write_record = write_sorted_record;
  return (io_out_t *)h;
}

static inline void wait_on_thread(io_out_sorted_t *h) {
  if (h->thread_started) {
    pthread_join(h->thread, NULL);
    h->thread_started = false;
  }
}

io_out_t *get_next_tmp(io_out_sorted_t *h, bool tmp_only) {
  const char *suffix = h->ext_options.lz4_tmp ? ".lz4" : "";
  if (!tmp_only && h->ext_options.num_per_group) {
    group_tmp_filename(h->tmp_filename, h->filename, h->num_group_written,
                       suffix);
    h->num_group_written++;
  } else {
    tmp_filename(h->tmp_filename, h->filename, h->num_written, suffix);
    h->num_written++;
  }
  // allow output buffer to be supplied to io_out_options...
  // allow input buffer to be supplied as well
  io_out_options_t options;
  io_out_options_init(&options);
  io_out_options_format(&options, io_prefix());
  /* reuse the same buffer? */
  io_out_options_buffer_size(&options, 10 * 1024 * 1024);
  return io_out_init(h->tmp_filename, &options);
}

void check_for_merge(io_out_sorted_t *h) {
  if (!h->ext_options.num_per_group ||
      h->num_group_written < h->ext_options.num_per_group)
    return;

  io_out_t *out = get_next_tmp(h, true);

  io_in_options_t opts;
  io_in_options_init(&opts);
  io_in_options_format(&opts, io_prefix());
  io_in_t *in =
      io_in_ext_init(h->ext_options.compare, h->ext_options.compare_arg, &opts);
  if (h->ext_options.reducer)
    io_in_ext_reducer(in, h->ext_options.reducer, h->ext_options.reducer_arg);

  const char *suffix = h->ext_options.lz4_tmp ? ".lz4" : "";
  for (size_t i = 0; i < h->num_group_written; i++) {
    group_tmp_filename(h->tmp_filename, h->filename, i, suffix);
    io_in_ext_add(in, io_in_init(h->tmp_filename, &opts), 0);
  }
  io_record_t *r;
  while ((r = io_in_advance(in)) != NULL)
    io_out_write_record(out, r->record, r->length);

  io_out_destroy(out);
  io_in_destroy(in);
  h->num_group_written = 0;
}

void *write_sorted_thread(void *arg) {
  io_out_sorted_t *h = (io_out_sorted_t *)arg;
  io_in_t *in = _in_from_buffer(h, h->b2);
  io_out_t *out = get_next_tmp(h, false);
  io_record_t *r;
  while ((r = io_in_advance(in)) != NULL)
    io_out_write_record(out, r->record, r->length);
  io_in_destroy(in);
  io_out_destroy(out);

  if (h->ext_options.num_per_group)
    check_for_merge(h);
  return NULL;
}

void write_sorted(io_out_sorted_t *h) {
  if (h->b->bp == h->b->buffer)
    return;
  wait_on_thread(h);
  if (h->ext_options.use_extra_thread) {
    io_out_buffer_t *tmp = h->b;
    h->b = h->b2;
    h->b2 = tmp;

    h->thread_started = true;
    pthread_create(&h->thread, NULL, write_sorted_thread, h);
  } else
    write_sorted_thread(h);
}

void io_out_tag(io_out_t *hp, int tag) {
  io_out_sorted_t *h = (io_out_sorted_t *)hp;
  if (h->type != IO_OUT_SORTED_TYPE)
    return;

  h->tag = tag;
}

io_in_t *_io_out_sorted_in(io_out_t *hp) {
  io_out_sorted_t *h = (io_out_sorted_t *)hp;
  if (h->type != IO_OUT_SORTED_TYPE)
    return NULL;

  if (h->out_in_called)
    return NULL;

  h->out_in_called = true;

  if (!h->num_written && !h->num_group_written) {
    if (&(h->buf1) == h->b) {
      if (h->buf2.buffer) {
        aml_free(h->buf2.buffer);
        h->buf2.buffer = NULL;
      }
    } else {
      if (h->buf1.buffer) {
        aml_free(h->buf1.buffer);
        h->buf1.buffer = NULL;
      }
    }
    return _in_from_buffer(h, h->b);
  }

  if (h->b->num_records) {
    wait_on_thread(h);
    if (h->ext_options.use_extra_thread) {
      io_out_buffer_t *tmp = h->b;
      h->b = h->b2;
      h->b2 = tmp;
    }
    if (h->ext_options.num_per_group)
      h->ext_options.num_per_group =
          h->num_group_written ? h->num_group_written : 1;
    write_sorted_thread(h);
  }

  if (h->buf1.buffer) {
    aml_free(h->buf1.buffer);
    h->buf1.buffer = NULL;
  }
  if (h->buf2.buffer) {
    aml_free(h->buf2.buffer);
    h->buf2.buffer = NULL;
  }

  io_in_options_t opts;
  io_in_options_init(&opts);
  io_in_options_buffer_size(&opts, h->buf1.size / 10);
  io_in_options_format(&opts, io_prefix());
  io_in_t *in =
      io_in_ext_init(h->ext_options.compare, h->ext_options.compare_arg, &opts);
  if (h->ext_options.reducer)
    io_in_ext_reducer(in, h->ext_options.reducer, h->ext_options.reducer_arg);

  const char *suffix = h->ext_options.lz4_tmp ? ".lz4" : "";
  // printf("%s num_written: %lu\n", h->filename, h->num_written);
  for (size_t i = 0; i < h->num_written; i++) {
    tmp_filename(h->tmp_filename, h->filename, i, suffix);
    io_in_ext_add(in, io_in_init(h->tmp_filename, &opts), i);
  }
  return in;
}

io_in_t *io_out_in(io_out_t *hp) {
  io_in_t *in = NULL;
  if (hp->type == IO_OUT_SORTED_TYPE) {
    in = _io_out_sorted_in(hp);
    if (in)
      io_in_destroy_out(in, hp, NULL);
    else
      io_out_destroy(hp);
  } else if (hp->type == IO_OUT_PARTITIONED_TYPE)
    in = io_out_partitioned_in(hp);
  else if (hp->type == IO_OUT_NORMAL_TYPE)
    in = io_out_normal_in(hp);
  return in;
}

/*
TODO: Add support for fixed length records.  These records don't need the
io_record_t record array and can simply be sorted in place.

bool write_fixed_sorted_record(io_out_t *hp, const void *d, size_t len) {
  io_out_sorted_t *h = (io_out_sorted_t *)hp;
  if (len != h->fixed)
    abort();

  char *bp = h->b->bp;
  if (bp + len > h->b->ep) {
    write_sorted(h);
    bp = h->b->bp;
  }

  memcpy(bp, d, len);
  bp += len;
  h->b->bp = bp;
  h->b->num_records++;
  return true;
}
*/

bool write_one_record(io_out_sorted_t *h, const void *d, size_t len) {
   wait_on_thread(h);
   io_out_t *out = get_next_tmp(h, false);
   io_out_write_record(out, d, len);
   io_out_destroy(out);
   if (h->ext_options.num_per_group)
     check_for_merge(h);
   return true;
}

bool write_sorted_record(io_out_t *hp, const void *d, size_t len) {
  if (len > 0xffffffffU)
    return false;
  io_out_sorted_t *h = (io_out_sorted_t *)hp;

  size_t length = len + sizeof(io_record_t) + 5;
  char *bp = h->b->bp;
  if (bp + length > h->b->ep) {
    write_sorted(h);
    bp = h->b->bp;
    if (bp + length > h->b->ep)
      return write_one_record(h, d, len);
  }

  /* Write data to the end of the buffer and the records to the beginning.
     This has the effect of keeping the records in the original order and
     makes effective use of the buffer from both ends.  Later, the records
     will be sorted.  The data is written with a zero terminator to make it
     easy for string comparison functions.
  */
  char *ep = h->b->ep;
  ep--;
  *ep = 0;
  ep -= len;
  memcpy(ep, d, len);

  io_record_t *r = (io_record_t *)bp;
  r->record = ep;
  r->length = len;
  r->tag = h->tag;
  bp += sizeof(*r);

  h->b->bp = bp;
  h->b->ep = ep;
  h->b->num_records++;

  return true;
}

void io_out_ext_remove_tmp_files(char *tmp, const char *filename,
                                 bool lz4_tmp) {
  const char *suffix = lz4_tmp ? ".lz4" : "";
  uint32_t skipped = 0;
  for (uint32_t i = 0; skipped < 4; i++) {
    tmp_filename(tmp, filename, i, suffix);
    if (io_file_exists(tmp))
      remove(tmp);
    else
      skipped++;
  }
  skipped = 0;
  for (uint32_t i = 0; skipped < 4; i++) {
    group_tmp_filename(tmp, filename, i, suffix);
    if (io_file_exists(tmp))
      remove(tmp);
    else
      skipped++;
  }
}

static void remove_extras(io_out_sorted_t *h) {
  extra_t *extra = h->extras;
  while (extra) {
    if (extra->type == EXTRA_FILE_TO_REMOVE)
      remove((char *)extra->p);
    extra = extra->next;
  }
}

static void touch_extras(io_out_sorted_t *h) {
  extra_t *extra = h->extras;
  while (extra) {
    if (extra->type == EXTRA_ACK_FILE) {
      FILE *out = fopen((char *)extra->p, "wb");
      fclose(out);
    }
    extra = extra->next;
  }
}

static void destroy_extra_ins(io_out_sorted_t *h) {
  extra_t *extra = h->extras;
  while (extra) {
    if (extra->type == EXTRA_IN) {
      io_in_t *in = (io_in_t *)extra->p;
      if (in)
        io_in_destroy(in);
      extra->p = NULL;
    }
    extra = extra->next;
  }
}

void io_out_sorted_destroy(io_out_t *hp) {
  io_out_sorted_t *h = (io_out_sorted_t *)hp;
  io_in_t *in = _io_out_sorted_in(hp);
  if (in) {
    size_t tmp_len = strlen(h->filename);
    if(h->suffix)
        tmp_len += strlen(h->suffix);
    snprintf(h->tmp_filename, tmp_len + 1, "%s%s", h->filename, h->suffix ? h->suffix : "");
    io_out_t *out = io_out_ext_init(h->tmp_filename, &(h->options),
                                    &(h->partition_options));
    io_record_t *r;
    while ((r = io_in_advance(in)) != NULL)
      io_out_write_record(out, r->record, r->length);
    io_out_destroy(out);
    io_in_destroy(in);
  }
  if (h->buf1.buffer) {
    aml_free(h->buf1.buffer);
    h->buf1.buffer = NULL;
  }
  if (h->buf2.buffer) {
    aml_free(h->buf2.buffer);
    h->buf2.buffer = NULL;
  }
  io_out_ext_remove_tmp_files(h->tmp_filename, h->filename,
                              h->ext_options.lz4_tmp);
  destroy_extra_ins(h);
  remove_extras(h);
  touch_extras(h);

  extra_t *extra = h->extras;
  while (extra) {
    extra_t *next = extra->next;
    aml_free(extra);
    extra = next;
  }

  aml_free(h);
}

static void io_out_ext_destroy(io_out_t *hp) {
  if (hp->type == IO_OUT_PARTITIONED_TYPE)
    io_out_partitioned_destroy(hp);
  else if (hp->type == IO_OUT_SORTED_TYPE)
    io_out_sorted_destroy(hp);
  else
    abort();
}

io_out_t *io_out_ext_init(const char *filename, io_out_options_t *options,
                          io_out_ext_options_t *ext_options) {
  io_out_ext_options_t eopts;
  if (!ext_options)
    io_out_ext_options_init(&eopts);
  else
    eopts = *ext_options;

  if (!eopts.int_compare) {
    eopts.int_compare = eopts.compare;
    eopts.int_compare_arg = eopts.compare_arg;
  }
  if (!eopts.int_reducer) {
    eopts.int_reducer = eopts.reducer;
    eopts.int_reducer_arg = eopts.reducer_arg;
  }

  ext_options = &eopts;

  if (ext_options->partition && !ext_options->sort_before_partitioning)
    return io_out_partitioned_init(filename, options, ext_options);
  else if (ext_options->compare)
    return io_out_sorted_init(filename, options, ext_options);
  else if (ext_options->partition)
    return io_out_partitioned_init(filename, options, ext_options);
  return io_out_init(filename, options);
}
