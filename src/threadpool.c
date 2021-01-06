/* Copyright Joyent, Inc. and other Node contributors. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "uv-common.h"

#if !defined(_WIN32)
# include "unix/internal.h"
#endif

#include <stdlib.h>
// TODO does libuv use C11?
#include <stdatomic.h>

#define MAX_THREADPOOL_SIZE 1024
#define THREADPOOL_POST_SPINS 2

static uv_once_t once = UV_ONCE_INIT;
static unsigned int nthreads;
static QUEUE exit_message;
static atomic_uint post_n = 0;

typedef struct
{
  uv_thread_t thread;
  uv_cond_t cond;
  uv_mutex_t mutex;
  QUEUE queue;
} w_thread_t;

typedef struct
{
  uv_sem_t* sem;
  unsigned int n;
} w_thread_args_t;

static w_thread_t* w_threads;
static w_thread_t default_w_threads[4];



static void uv__cancelled(struct uv__work* w) {
  abort();
}


/* To avoid deadlock with uv_cancel() it's crucial that the worker
 * never holds the global mutex and the loop-local mutex at the same time.
 */
static void worker(void* arg) {
  unsigned int n, i;
  w_thread_t* wt;
  struct uv__work* w;
  QUEUE* q;

  n = ((w_thread_args_t*) arg)->n;
  uv_sem_post(((w_thread_args_t*) arg)->sem);
  arg = NULL;

  for (;;) {
    // work stealing
    for (i = 0; i < nthreads; ++i) {
      wt = w_threads + ((i + n) % nthreads);
      if (uv_mutex_trylock(&wt->mutex) == 0) {
        if (QUEUE_EMPTY(&wt->queue)) {
          uv_mutex_unlock(&wt->mutex);
          wt = NULL;
          continue;
        }
        break;
      } else {
        wt = NULL;
      }
    }

    // could not steal, so fallback to pessimistic mode
    if (wt == NULL) {
      wt = w_threads + n;
      uv_mutex_lock(&wt->mutex);
      while (QUEUE_EMPTY(&wt->queue)) {
        uv_cond_wait(&wt->cond, &wt->mutex);
      }
    }

    q = QUEUE_HEAD(&wt->queue);

    if (q == &exit_message)
      uv_cond_signal(&wt->cond);
    else {
      QUEUE_REMOVE(q);
      QUEUE_INIT(q);  /* Signal uv_cancel() that the work req is
                             executing. */
    }

    uv_mutex_unlock(&wt->mutex);

    if (q == &exit_message)
      break;

    w = QUEUE_DATA(q, struct uv__work, wq);
    w->work(w);

    w->work = NULL;  /* Signal uv_cancel() that the work req is done
                        executing. */
    w->wq_.state = w;
    mpscq_push(&w->loop->wq_, &w->wq_);
    uv_async_send(&w->loop->wq_async);
  }
}


static void post(QUEUE* q, enum uv__work_kind kind) {
  unsigned int n, i;
  w_thread_t* wt;

  n = atomic_fetch_add_explicit(&post_n, 1, memory_order_relaxed);
  // optimistic post mode
  for (i = 0; i < nthreads * THREADPOOL_POST_SPINS; ++i) {
    wt = w_threads + ((i + n) % nthreads);
    if (uv_mutex_trylock(&wt->mutex) == 0) {      
      break;
    } else {
      wt = NULL;
    }
  }

  // fallback to pessimistic mode
  if (wt == NULL) {
    wt = w_threads + (n % nthreads);
    uv_mutex_lock(&wt->mutex);
  }

  QUEUE_INSERT_TAIL(&wt->queue, q);
  uv_cond_signal(&wt->cond);
  uv_mutex_unlock(&wt->mutex);
}


void uv__threadpool_cleanup(void) {
#ifndef _WIN32
  w_thread_t* wt;

  if (nthreads == 0)
    return;

  // post exit message into all queues
  for (wt = w_threads; wt < w_threads + nthreads; wt++) {
    uv_mutex_lock(&wt->mutex);
    QUEUE_INSERT_TAIL(&wt->queue, &exit_message); // UV__WORK_CPU
    uv_cond_signal(&wt->cond);
    uv_mutex_unlock(&wt->mutex);
  }

  for (wt = w_threads; wt < w_threads + nthreads; wt++)
    if (uv_thread_join(&wt->thread))
      abort();

  for (wt = w_threads; wt < w_threads + nthreads; wt++) {
    uv_mutex_destroy(&wt->mutex);
    uv_cond_destroy(&wt->cond);
  }
  
  if (w_threads != default_w_threads)
    uv__free(w_threads);

  w_threads = NULL;
  nthreads = 0;
#endif
}


static void init_threads(void) {
  unsigned int i;
  const char* val;
  w_thread_t* wt;
  uv_sem_t sem;
  w_thread_args_t* args;

  nthreads = ARRAY_SIZE(default_w_threads);
  val = getenv("UV_THREADPOOL_SIZE");
  if (val != NULL)
    nthreads = atoi(val);
  if (nthreads == 0)
    nthreads = 1;
  if (nthreads > MAX_THREADPOOL_SIZE)
    nthreads = MAX_THREADPOOL_SIZE;

  w_threads = default_w_threads;
  if (nthreads > ARRAY_SIZE(default_w_threads)) {
    w_threads = uv__malloc(nthreads * sizeof(w_threads[0]));
    if (w_threads == NULL) {
      nthreads = ARRAY_SIZE(default_w_threads);
      w_threads = default_w_threads;
    }
  }

  for (wt = w_threads; wt < w_threads + nthreads; wt++) {
    if (uv_cond_init(&wt->cond))
      abort();
    if (uv_mutex_init(&wt->mutex))
      abort();
    QUEUE_INIT(&wt->queue);
  }

  if (uv_sem_init(&sem, 0))
    abort();

  args = uv__malloc(nthreads * sizeof(w_thread_args_t));
  for (i = 0; i < nthreads; i++) {
    (args + i)->sem = &sem;
    (args + i)->n = i;
    wt = w_threads + i;
    if (uv_thread_create(&wt->thread, worker, args + i))
      abort();
  }

  for (i = 0; i < nthreads; i++)
    uv_sem_wait(&sem);

  uv__free(args);
  uv_sem_destroy(&sem);  
}


#ifndef _WIN32
static void reset_once(void) {
  uv_once_t child_once = UV_ONCE_INIT;
  memcpy(&once, &child_once, sizeof(child_once));
}
#endif


static void init_once(void) {
#ifndef _WIN32
  /* Re-initialize the threadpool after fork.
   * Note that this discards the global mutex and condition as well
   * as the work queue.
   */
  if (pthread_atfork(NULL, NULL, &reset_once))
    abort();
#endif
  init_threads();
}


void uv__work_submit(uv_loop_t* loop,
                     struct uv__work* w,
                     enum uv__work_kind kind,
                     void (*work)(struct uv__work* w),
                     void (*done)(struct uv__work* w, int status)) {
  uv_once(&once, init_once);
  w->loop = loop;
  w->work = work;
  w->done = done;
  post(&w->wq, kind);
}


static int uv__work_cancel(uv_loop_t* loop, uv_req_t* req, struct uv__work* w) {
  // int cancelled;

  // TODO fix cancellation

  // uv_mutex_lock(&mutex);
  // uv_mutex_lock(&w->loop->wq_mutex);

  // cancelled = !QUEUE_EMPTY(&w->wq) && w->work != NULL;
  // if (cancelled)
  //   QUEUE_REMOVE(&w->wq);

  // uv_mutex_unlock(&w->loop->wq_mutex);
  // uv_mutex_unlock(&mutex);

  // if (!cancelled)
  //   return UV_EBUSY;

  w->work = uv__cancelled;
  // uv_mutex_lock(&loop->wq_mutex);
  // QUEUE_INSERT_TAIL(&loop->wq, &w->wq);
  // uv_async_send(&loop->wq_async);
  // uv_mutex_unlock(&loop->wq_mutex);

  return 0;
}


void uv__work_done(uv_async_t* handle) {
  struct uv__work* w;
  uv_loop_t* loop;
  mpscq_node_t* node;
  int err;

  loop = container_of(handle, uv_loop_t, wq_async);
  node = mpscq_pop(&loop->wq_);
  // uv_mutex_lock(&loop->wq_mutex);
  // QUEUE_MOVE(&loop->wq, &wq);
  // uv_mutex_unlock(&loop->wq_mutex);

  while (node != NULL) {
    w = (struct uv__work*) node->state;
    err = (w->work == uv__cancelled) ? UV_ECANCELED : 0;
    w->done(w, err);

    node = mpscq_pop(&loop->wq_);
  }
}


static void uv__queue_work(struct uv__work* w) {
  uv_work_t* req = container_of(w, uv_work_t, work_req);

  req->work_cb(req);
}


static void uv__queue_done(struct uv__work* w, int err) {
  uv_work_t* req;

  req = container_of(w, uv_work_t, work_req);
  uv__req_unregister(req->loop, req);

  if (req->after_work_cb == NULL)
    return;

  req->after_work_cb(req, err);
}


int uv_queue_work(uv_loop_t* loop,
                  uv_work_t* req,
                  uv_work_cb work_cb,
                  uv_after_work_cb after_work_cb) {
  if (work_cb == NULL)
    return UV_EINVAL;

  uv__req_init(loop, req, UV_WORK);
  req->work_cb = work_cb;
  req->after_work_cb = after_work_cb;
  uv__work_submit(loop,
                  &req->work_req,
                  UV__WORK_CPU,
                  uv__queue_work,
                  uv__queue_done);
  return 0;
}


int uv_cancel(uv_req_t* req) {
  struct uv__work* wreq;
  uv_loop_t* loop;

  switch (req->type) {
  case UV_FS:
    loop =  ((uv_fs_t*) req)->loop;
    wreq = &((uv_fs_t*) req)->work_req;
    break;
  case UV_GETADDRINFO:
    loop =  ((uv_getaddrinfo_t*) req)->loop;
    wreq = &((uv_getaddrinfo_t*) req)->work_req;
    break;
  case UV_GETNAMEINFO:
    loop = ((uv_getnameinfo_t*) req)->loop;
    wreq = &((uv_getnameinfo_t*) req)->work_req;
    break;
  case UV_RANDOM:
    loop = ((uv_random_t*) req)->loop;
    wreq = &((uv_random_t*) req)->work_req;
    break;
  case UV_WORK:
    loop =  ((uv_work_t*) req)->loop;
    wreq = &((uv_work_t*) req)->work_req;
    break;
  default:
    return UV_EINVAL;
  }

  return uv__work_cancel(loop, req, wreq);
}
