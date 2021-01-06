/* Based on Dmitry Vyukov's MPSC queue algorithm:
 * http://www.1024cores.net/home/lock-free-algorithms/queues/non-intrusive-mpsc-node-based-queue
 */
#include <stdatomic.h>
#include "uv-common.h"

void mpscq_create(mpscq_t* self, mpscq_node_t* stub)
{
    stub->next = 0;
    self->head = stub;
    self->tail = stub;
}

void mpscq_push(mpscq_t* self, mpscq_node_t* n)
{
    n->next = 0;
    // serialization-point wrt producers, acquire-release
    mpscq_node_t* prev = atomic_exchange(&self->head, n);
    // serialization-point wrt consumer, release
    prev->next = n;
}

mpscq_node_t* mpscq_pop(mpscq_t* self)
{
    mpscq_node_t* tail = self->tail;
    // serialization-point wrt producers, acquire
    mpscq_node_t* next = tail->next;
    if (next)
    {
        self->tail = next;
        tail->state = next->state;
        return tail;
    }
    return 0;
}
