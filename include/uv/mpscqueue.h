/* Based on Dmitry Vyukov's MPSC queue algorithm
 */

struct mpscq_node_s {
  struct mpscq_node_s* volatile next;
  void* state;
};
struct mpscq_node_s;
typedef struct mpscq_node_s mpscq_node_t;

typedef struct {
  mpscq_node_t* volatile head;
  mpscq_node_t* tail;
  mpscq_node_t stub;
} mpscq_t;

void mpscq_create(mpscq_t* self);

void mpscq_push(mpscq_t* self, mpscq_node_t* n);

mpscq_node_t* mpscq_pop(mpscq_t* self);
