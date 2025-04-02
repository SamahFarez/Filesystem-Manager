#ifndef SCHEDULER_H
#define SCHEDULER_H

#include "filesystem.h"


void execute_job(Job job);
void* scheduler(void *arg);
void add_job(const char *command);
void cleanup();
void handle_signal(int sig);

#endif // SCHEDULER_H