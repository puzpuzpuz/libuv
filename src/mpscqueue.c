/* Based on Dmitry Vyukov's MPSC queue algorithm:
 * http://www.1024cores.net/home/lock-free-algorithms/queues/intrusive-mpsc-node-based-queue
 * 
 * TODO consider:
 * http://www.1024cores.net/home/lock-free-algorithms/queues/non-intrusive-mpsc-node-based-queue
 */
#include <stdatomic.h>
#include "uv-common.h"

void mpscq_create(mpscq_t* self) {
  self->head = &self->stub;
  self->tail = &self->stub;
  self->stub.next = NULL;
}

void mpscq_push(mpscq_t* self, mpscq_node_t* n) {
  n->next = NULL;
  mpscq_node_t* prev = atomic_exchange(&self->head, n);
  // (*)
  prev->next = n;
}

mpscq_node_t* mpscq_pop(mpscq_t* self) {
  mpscq_node_t* tail = self->tail;
  mpscq_node_t* next = tail->next;
  if (tail == &self->stub) {
    if (NULL == next)
      return NULL;
    self->tail = next;
    tail = next;
    next = next->next;
  }
  if (next) {
    self->tail = next;
    return tail;
  }
  mpscq_node_t* head = self->head;
  if (tail != head)
    return NULL;
  mpscq_push(self, &self->stub);
  next = tail->next;
  if (next) {
    self->tail = next;
    return tail;
  }
  return NULL;
}
