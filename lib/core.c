#include "strm.h"
#include <pthread.h>

struct strm_thread {
  pthread_t th;
  strm_queue *queue;
} *threads;

static int thread_max;
static int pipeline_count = 0;
static pthread_mutex_t pipeline_mutex;
static pthread_cond_t pipeline_cond;

#include <assert.h>

static void task_init();

static void
task_push(int tid, strm_stream *s, strm_func func, void *data)
{
  int i;

  assert(threads != NULL);
  if (s->tid < 0) {
    if (tid >= 0) {
      s->tid = (tid + 1) % thread_max;
    }
    else {
      int n = 0;
      int max = 0;

      for (i=0; i<thread_max; i++) {
        int size = strm_queue_size(threads[i].queue);
        if (size == 0) break;
        if (size > max) {
          max = size;
          n = i;
        }
      }
      if (i == thread_max) {
        s->tid = n;
      }
      else {
        s->tid = i;
      }
    }
  }
  strm_queue_push(threads[s->tid].queue, s, func, data);
}

void
strm_task_push(strm_stream *s, strm_func func, void *data)
{
  assert(threads != NULL);
  task_push(s->tid, s, func, data);
}

void
strm_emit(strm_stream *strm, void *data, strm_func func)
{
  strm_stream *d = strm->dst;

  while (d) {
    task_push(strm->tid, d, d->start_func, data);
    d = d->nextd;
  }
  if (func) {
    strm_task_push(strm, func, NULL);
  }
}

int
strm_connect(strm_stream *src, strm_stream *dst)
{
  strm_stream *s;

  assert(dst->mode != strm_task_prod);
  s = src->dst;
  if (s) {
    while (s->nextd) {
      s = s->nextd;
    }
    s->nextd = dst;
  }
  else {
    src->dst = dst;
  }

  if (src->mode == strm_task_prod) {
    task_init();
    pipeline_count++;
    strm_task_push(src, src->start_func, NULL);
  }
  return 1;
}

int cpu_count();
void strm_init_io_loop();
strm_stream *strm_io_deque();
int strm_io_waiting();

static int
thread_count()
{
  char *e = getenv("STRM_THREAD_MAX");
  int n;

  if (e) {
    n = atoi(e);
    if (n > 0) return n;
  }
  return cpu_count();
}

static void
task_ping()
{
  pthread_mutex_lock(&pipeline_mutex);
  pthread_cond_signal(&pipeline_cond);
  pthread_mutex_unlock(&pipeline_mutex);
}

static void*
task_loop(void *data)
{
  struct strm_thread *th = (struct strm_thread*)data;

  for (;;) {
    strm_queue_exec(th->queue);
    if (pipeline_count == 0 && !strm_queue_p(th->queue)) {
      task_ping();
    }
  }
  return NULL;
}

static void
task_init()
{
  int i;

  if (threads) return;

  strm_init_io_loop();

  pthread_mutex_init(&pipeline_mutex, NULL);
  pthread_cond_init(&pipeline_cond, NULL);

  thread_max = thread_count();
  threads = malloc(sizeof(struct strm_thread)*thread_max);
  for (i=0; i<thread_max; i++) {
    threads[i].queue = strm_queue_alloc();
    pthread_create(&threads[i].th, NULL, task_loop, &threads[i]);
  }
}

int
strm_loop()
{
  task_init();
  for (;;) {
    pthread_mutex_lock(&pipeline_mutex);
    pthread_cond_wait(&pipeline_cond, &pipeline_mutex);
    pthread_mutex_unlock(&pipeline_mutex);
    if (pipeline_count == 0) {
      int i;

      for (i=0; i<thread_max; i++) {
        if (strm_queue_size(threads[i].queue) > 0)
          break;
      }
      if (i == thread_max) break;
    }
  }
  return 1;
}

strm_stream*
strm_alloc_stream(strm_task_mode mode, strm_func start_func, strm_func close_func, void *data)
{
  strm_stream *s = malloc(sizeof(strm_stream));
  s->tid = -1;                  /* -1 means uninitialized */
  s->mode = mode;
  s->start_func = start_func;
  s->close_func = close_func;
  s->data = data;
  s->dst = NULL;
  s->nextd = NULL;
  s->flags = 0;

  return s;
}

void
pipeline_finish(strm_stream *strm, void *data)
{
  pthread_mutex_lock(&pipeline_mutex);
  pipeline_count--;
  if (pipeline_count == 0) {
    pthread_cond_signal(&pipeline_cond);
  }
  pthread_mutex_unlock(&pipeline_mutex);
}

void
strm_close(strm_stream *strm)
{
  if (strm->close_func) {
    (*strm->close_func)(strm, NULL);
  }
  strm_stream *d = strm->dst;

  while (d) {
    strm_task_push(d, (strm_func)strm_close, NULL);
    d = d->nextd;
  }
  if (strm->mode == strm_task_prod) {
    strm_task_push(strm, pipeline_finish, NULL);
  }
}
