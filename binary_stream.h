#ifndef BINARY_STREAM_H
#define BINARY_STREAM_H

#include <pthread.h>
#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 収録側とSD書き込み側を分離する2面バッファ。
 * 収録側は active 面だけに追記し、満杯になった面を writer thread へ渡す。
 */
typedef struct binary_stream_s
{
  FILE *fp;
  unsigned char *buffers[2];
  size_t element_size;
  int capacity;
  int active_index;
  int active_count;
  int pending_index;
  int pending_count;
  int writing_index;
  int stopping;
  int failed;
  int thread_started;
  const char *label;
  pthread_t thread;
  pthread_mutex_t mutex;
  pthread_cond_t cond;
} binary_stream_t;

/* FILE* に対する非同期バイナリ書き込み用2面バッファを準備する。 */
int binary_stream_open(binary_stream_t *stream,
                       FILE *fp,
                       size_t element_size,
                       int capacity,
                       const char *label);

/* 1レコードを active 面へ追加する。満杯時は書き込み面へ切り替える。 */
int binary_stream_append(binary_stream_t *stream, const void *element);

/* active 面に残ったデータをSD書き込みへ渡し、書き込み完了を待つ。 */
int binary_stream_close(binary_stream_t *stream);

/* 現在収録側が使っている active 面の使用数を返す。 */
int binary_stream_count(const binary_stream_t *stream);

#ifdef __cplusplus
}
#endif

#endif /* BINARY_STREAM_H */
