#ifndef QUEUE_H
#define QUEUE_H

#include <pthread.h>
#include <stddef.h>

#define QUEUE_CAPACITY 128

typedef struct {
    int client_fd;
} work_item_t;

typedef struct {
    work_item_t items[QUEUE_CAPACITY];
    size_t head;
    size_t tail;
    size_t count;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} work_queue_t;

void queue_init(work_queue_t *queue);
void queue_push(work_queue_t *queue, work_item_t item);
work_item_t queue_pop(work_queue_t *queue);

#endif
