#include "../include/globals.h"
#include "../include/paging.h"  // For TOTAL_PAGES definition

// ACTUAL DEFINITIONS (with initialization)
FileSystemState fs_state;
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

Job job_queue[MAX_JOBS];
int front = 0;
int rear = 0;
int job_count = 0;
pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t job_available = PTHREAD_COND_INITIALIZER;
int running = 1;
unsigned char page_bitmap[TOTAL_PAGES / 8] = {0};  // Initialization happens here