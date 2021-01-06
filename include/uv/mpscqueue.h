/* Based on Dmitry Vyukov's MPSC queue algorithm:
 * http://www.1024cores.net/home/lock-free-algorithms/queues/non-intrusive-mpsc-node-based-queue
 */

struct mpscq_node_s {
    struct mpscq_node_s* volatile  next;
    void*                          state;
};
struct mpscq_node_s;
typedef struct mpscq_node_s mpscq_node_t;

typedef struct {
    mpscq_node_t* volatile  head;
    mpscq_node_t*           tail;
} mpscq_t;

void mpscq_create(mpscq_t* self, mpscq_node_t* stub);

void mpscq_push(mpscq_t* self, mpscq_node_t* n);

mpscq_node_t* mpscq_pop(mpscq_t* self);
