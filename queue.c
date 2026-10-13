#include "queue.h"

void queue_init(work_queue_t *queue) {
    queue->head = 0;
    queue->tail = 0;
    queue->count = 0;

    pthread_mutex_init(&queue->mutex, NULL);
    pthread_cond_init(&queue->not_empty, NULL);
    pthread_cond_init(&queue->not_full, NULL);
}

void queue_push(work_queue_t *queue, work_item_t item) {
    pthread_mutex_lock(&queue->mutex);

    while (queue->count == QUEUE_CAPACITY) {
        pthread_cond_wait(&queue->not_full, &queue->mutex);
    }

    queue->items[queue->tail] = item;
    queue->tail = (queue->tail + 1) % QUEUE_CAPACITY;
    queue->count++;

    pthread_cond_signal(&queue->not_empty);
    pthread_mutex_unlock(&queue->mutex);
}

work_item_t queue_pop(work_queue_t *queue) {
    pthread_mutex_lock(&queue->mutex);

    while (queue->count == 0) {
        pthread_cond_wait(&queue->not_empty, &queue->mutex);
    }

    work_item_t item = queue->items[queue->head];
    queue->head = (queue->head + 1) % QUEUE_CAPACITY;
    queue->count--;

    pthread_cond_signal(&queue->not_full);
    pthread_mutex_unlock(&queue->mutex);

    return item;
}
