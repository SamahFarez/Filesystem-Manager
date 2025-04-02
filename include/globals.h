// globals.h
#ifndef GLOBALS_H
#define GLOBALS_H

#include <pthread.h>
#include "filesystem.h"  // For FileSystemState and Job definitions

// EXTERN DECLARATIONS (no initialization here)
extern FileSystemState fs_state;
extern pthread_mutex_t mutex;
extern Job job_queue[MAX_JOBS];
extern int front, rear, job_count;
extern pthread_mutex_t queue_lock;
extern pthread_cond_t job_available;
extern int running;
extern unsigned char page_bitmap[];  // No size or initialization here

#endif


