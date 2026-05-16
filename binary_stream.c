#include "binary_stream.h"

#include "binary_file.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BINARY_STREAM_NO_BUFFER (-1)

/*
 * active 面以外で、writer thread が使っていない空き面を探す。
 * 呼び出し元は stream->mutex を保持している前提。
 */
static int binary_stream_find_free_locked(binary_stream_t *stream)
{
  int i;

  for (i = 0; i < 2; i++)
    {
      if (i != stream->active_index &&
          i != stream->pending_index &&
          i != stream->writing_index)
        {
          return i;
        }
    }

  return BINARY_STREAM_NO_BUFFER;
}

/*
 * active 面を writer thread へ渡す。
 * wait_free が0の場合、空き面がなければ待たずにエラーとして返す。
 */
static int binary_stream_submit_active(binary_stream_t *stream, int wait_free)
{
  int next_index;

  if (stream->active_count == 0)
    {
      return 0;
    }

  pthread_mutex_lock(&stream->mutex);
  if (stream->failed)
    {
      pthread_mutex_unlock(&stream->mutex);
      return -1;
    }

  while ((next_index = binary_stream_find_free_locked(stream)) < 0)
    {
      if (!wait_free)
        {
          stream->failed = 1;
          printf("ERROR: %s writer could not keep up.\n", stream->label);
          pthread_mutex_unlock(&stream->mutex);
          return -1;
        }

      pthread_cond_wait(&stream->cond, &stream->mutex);
      if (stream->failed)
        {
          pthread_mutex_unlock(&stream->mutex);
          return -1;
        }
    }

  stream->pending_index = stream->active_index;
  stream->pending_count = stream->active_count;
  stream->active_index = next_index;
  stream->active_count = 0;
  pthread_cond_signal(&stream->cond);
  pthread_mutex_unlock(&stream->mutex);

  return 0;
}

/* writer thread 本体。pending 面を受け取り、FILE* へ順番に書き込む。 */
static void *binary_stream_writer_main(void *arg)
{
  binary_stream_t *stream = (binary_stream_t *)arg;

  while (1)
    {
      int index;
      int count;
      int write_failed = 0;

      pthread_mutex_lock(&stream->mutex);
      while (stream->pending_index == BINARY_STREAM_NO_BUFFER &&
             !stream->stopping)
        {
          pthread_cond_wait(&stream->cond, &stream->mutex);
        }

      if (stream->pending_index == BINARY_STREAM_NO_BUFFER &&
          stream->stopping)
        {
          pthread_mutex_unlock(&stream->mutex);
          break;
        }

      index = stream->pending_index;
      count = stream->pending_count;
      stream->pending_index = BINARY_STREAM_NO_BUFFER;
      stream->pending_count = 0;
      stream->writing_index = index;
      pthread_mutex_unlock(&stream->mutex);

      write_failed = binary_file_write(stream->fp,
                                       stream->buffers[index],
                                       stream->element_size,
                                       count,
                                       stream->label);
      if (!write_failed && fflush(stream->fp) != 0)
        {
          printf("ERROR: Failed to flush %s stream. %d\n",
                 stream->label, errno);
          write_failed = 1;
        }

      pthread_mutex_lock(&stream->mutex);
      stream->writing_index = BINARY_STREAM_NO_BUFFER;
      if (write_failed)
        {
          stream->failed = 1;
        }
      pthread_cond_broadcast(&stream->cond);
      pthread_mutex_unlock(&stream->mutex);
    }

  return NULL;
}

int binary_stream_open(binary_stream_t *stream,
                       FILE *fp,
                       size_t element_size,
                       int capacity,
                       const char *label)
{
  int ret;

  memset(stream, 0, sizeof(*stream));
  stream->fp = fp;
  stream->element_size = element_size;
  stream->capacity = capacity;
  stream->active_index = 0;
  stream->pending_index = BINARY_STREAM_NO_BUFFER;
  stream->writing_index = BINARY_STREAM_NO_BUFFER;
  stream->label = label;

  stream->buffers[0] = (unsigned char *)malloc(element_size * capacity);
  stream->buffers[1] = (unsigned char *)malloc(element_size * capacity);
  if (stream->buffers[0] == NULL || stream->buffers[1] == NULL)
    {
      printf("ERROR: %s stream buffer allocation failed.\n", label);
      free(stream->buffers[0]);
      free(stream->buffers[1]);
      memset(stream, 0, sizeof(*stream));
      return 1;
    }

  ret = pthread_mutex_init(&stream->mutex, NULL);
  if (ret != 0)
    {
      printf("ERROR: %s stream mutex initialization failed. %d\n", label, ret);
      free(stream->buffers[0]);
      free(stream->buffers[1]);
      memset(stream, 0, sizeof(*stream));
      return 1;
    }

  ret = pthread_cond_init(&stream->cond, NULL);
  if (ret != 0)
    {
      printf("ERROR: %s stream cond initialization failed. %d\n", label, ret);
      pthread_mutex_destroy(&stream->mutex);
      free(stream->buffers[0]);
      free(stream->buffers[1]);
      memset(stream, 0, sizeof(*stream));
      return 1;
    }

  ret = pthread_create(&stream->thread, NULL, binary_stream_writer_main, stream);
  if (ret != 0)
    {
      printf("ERROR: %s stream writer thread creation failed. %d\n", label, ret);
      pthread_cond_destroy(&stream->cond);
      pthread_mutex_destroy(&stream->mutex);
      free(stream->buffers[0]);
      free(stream->buffers[1]);
      memset(stream, 0, sizeof(*stream));
      return 1;
    }

  stream->thread_started = 1;
  return 0;
}

int binary_stream_append(binary_stream_t *stream, const void *element)
{
  int failed;

  pthread_mutex_lock(&stream->mutex);
  failed = stream->failed;
  pthread_mutex_unlock(&stream->mutex);

  if (failed)
    {
      return -1;
    }

  if (stream->active_count >= stream->capacity &&
      binary_stream_submit_active(stream, 0))
    {
      return -1;
    }

  memcpy(stream->buffers[stream->active_index] +
         stream->element_size * stream->active_count,
         element,
         stream->element_size);
  stream->active_count++;
  return 0;
}

int binary_stream_close(binary_stream_t *stream)
{
  int failed;

  if (stream->buffers[0] == NULL && stream->buffers[1] == NULL)
    {
      return stream->failed;
    }

  pthread_mutex_lock(&stream->mutex);
  failed = stream->failed;
  pthread_mutex_unlock(&stream->mutex);

  if (!failed)
    {
      (void)binary_stream_submit_active(stream, 1);
    }

  pthread_mutex_lock(&stream->mutex);
  while (stream->pending_index != BINARY_STREAM_NO_BUFFER ||
         stream->writing_index != BINARY_STREAM_NO_BUFFER)
    {
      pthread_cond_wait(&stream->cond, &stream->mutex);
    }
  stream->stopping = 1;
  pthread_cond_signal(&stream->cond);
  pthread_mutex_unlock(&stream->mutex);

  if (stream->thread_started)
    {
      pthread_join(stream->thread, NULL);
    }

  if (fflush(stream->fp) != 0)
    {
      printf("ERROR: Failed to flush %s stream. %d\n", stream->label, errno);
      stream->failed = 1;
    }

  failed = stream->failed;
  pthread_cond_destroy(&stream->cond);
  pthread_mutex_destroy(&stream->mutex);
  free(stream->buffers[0]);
  free(stream->buffers[1]);
  stream->buffers[0] = NULL;
  stream->buffers[1] = NULL;
  return failed;
}

int binary_stream_count(const binary_stream_t *stream)
{
  return stream->active_count;
}
