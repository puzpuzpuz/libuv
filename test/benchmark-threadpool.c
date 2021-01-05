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

#include "uv.h"
#include "task.h"
#include <stdlib.h>
#include <unistd.h>

#define CONCURRENT_CALLS 1000
#define TOTAL_CALLS 100000
#define SLEEP_US 100

static uv_loop_t* loop;

static uv_work_t req[CONCURRENT_CALLS];
static int req_data[CONCURRENT_CALLS];

static int calls_initiated = 0;
static int calls_completed = 0;
static int64_t start_time;
static int64_t end_time;

static void task_initiate(uv_work_t* req);


static void process_task(uv_work_t *req) {
  int n = *(int *) req->data;
  usleep(n);
}

static void task_cb(uv_work_t *req, int status) {
  ASSERT(status == 0);
  calls_completed++;
  if (calls_initiated < TOTAL_CALLS) {
    task_initiate(req);
  }
}

static void task_initiate(uv_work_t* req) {
  int r;

  calls_initiated++;

  r = uv_queue_work(loop, req, process_task, task_cb);
  ASSERT(r == 0);
}

BENCHMARK_IMPL(threadpool) {
  int i;

  loop = uv_default_loop();

  uv_update_time(loop);
  start_time = uv_now(loop);

  for (i = 0; i < CONCURRENT_CALLS; i++) {
    req_data[i] = SLEEP_US;
    req[i].data = (void *) &req_data[i];
    task_initiate(&req[i]);
  }

  uv_run(loop, UV_RUN_DEFAULT);

  uv_update_time(loop);
  end_time = uv_now(loop);

  ASSERT(calls_initiated == TOTAL_CALLS);
  ASSERT(calls_completed == TOTAL_CALLS);

  fprintf(stderr, "threadpool: %.0f req/s\n",
          (double) calls_completed / (double) (end_time - start_time) * 1000.0);
  fflush(stderr);

  MAKE_VALGRIND_HAPPY();
  return 0;
}
