#ifndef SCHEDULER_H
#define SCHEDULER_H

#include "filesystem.h"

extern Job job_queue[MAX_JOBS];
extern int front, rear, job_count;
extern pthread_mutex_t queue_lock;
extern pthread_cond_t job_available;
extern int running;

void *scheduler(void *arg);
void add_job(const char *command);
void print_queue(int current_job_index);
void cleanup();
void handle_signal(int sig);

#endif // SCHEDULER_H