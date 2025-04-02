#ifndef TEST_UTILS_H
#define TEST_UTILS_H

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <pthread.h>
#include "../include/filesystem.h"
#include "../include/paging.h"

// Test result codes
#define TEST_PASSED 1
#define TEST_FAILED 0

// ANSI color codes for test output
#define COLOR_RED     "\033[1;31m"
#define COLOR_GREEN   "\033[1;32m"
#define COLOR_YELLOW  "\033[1;33m"
#define COLOR_BLUE    "\033[1;34m"
#define COLOR_MAGENTA "\033[1;35m"
#define COLOR_CYAN    "\033[1;36m"
#define COLOR_RESET   "\033[0m"

// Test assertion macros with detailed output
#define ASSERT(condition, message) \
    do { \
        if (!(condition)) { \
            printf(COLOR_RED "FAIL" COLOR_RESET ": %s\n\tFile: %s\n\tLine: %d\n", \
                  message, __FILE__, __LINE__); \
            return TEST_FAILED; \
        } else { \
            printf(COLOR_GREEN "PASS" COLOR_RESET ": %s\n", message); \
        } \
    } while (0)

#define ASSERT_MSG(condition, message, ...) \
    do { \
        if (!(condition)) { \
            printf(COLOR_RED "FAIL" COLOR_RESET ": "); \
            printf(message, ##__VA_ARGS__); \
            printf("\n\tFile: %s\n\tLine: %d\n", __FILE__, __LINE__); \
            return TEST_FAILED; \
        } \
    } while (0)

// Test execution macro with timing
#define TEST(test_name) \
    do { \
        printf("\n" COLOR_YELLOW "=== Running test: %s ===" COLOR_RESET "\n", #test_name); \
        clock_t start = clock(); \
        int result = test_name(); \
        clock_t end = clock(); \
        double elapsed = (double)(end - start) / CLOCKS_PER_SEC; \
        if (result) { \
            printf(COLOR_GREEN "=== Test %s PASSED (%.3fs) ===" COLOR_RESET "\n", \
                  #test_name, elapsed); \
        } else { \
            printf(COLOR_RED "=== Test %s FAILED (%.3fs) ===" COLOR_RESET "\n", \
                  #test_name, elapsed); \
        } \
    } while (0)

// Test statistics structure
typedef struct {
    int total;
    int passed;
    int failed;
    double total_time;
} TestStats;

// Global test statistics
extern TestStats test_stats;

// Test suite setup/teardown
void initialize_test_environment();
void cleanup_test_environment();
void reset_test_environment();

// Filesystem verification utilities
int verify_file_exists(const char* filename);
int verify_directory_exists(const char* dirname);
int verify_file_content(const char* filename, const char* expected_content);
int verify_file_permissions(const char* filename, int expected_perms);
int verify_file_owner(const char* filename, const char* expected_owner);
int verify_file_inode(const char* filename, ino_t expected_inode);
int verify_file_size(const char* filename, size_t expected_size);
int verify_file_open_count(const char* filename, int expected_count);

// Paging system verification
int verify_page_allocation(const char* filename, int expected_pages);
int verify_page_bitmap_consistency();
int count_allocated_pages();

// Path resolution utilities
int resolve_test_path(const char* path);
char* get_test_absolute_path(const char* path);

// Directory verification
int verify_file_in_directory(const char* dirname, const char* filename);
int verify_directory_empty(const char* dirname);
int verify_directory_file_count(const char* dirname, int expected_count);

// Link verification
int verify_hard_link(const char* file1, const char* file2);
int verify_symbolic_link(const char* link, const char* target);

// User and permission utilities
int verify_user_exists(const char* username);
int verify_permission_check(const char* filename, int required_perms, int should_pass);

// Test file generation
void generate_test_file(const char* filename, size_t size);
void generate_large_test_file(const char* filename, size_t size_in_mb);

// Test directory generation
void generate_test_directory(const char* dirname);
void generate_deep_directory_structure(int depth);

// Benchmark utilities
void start_benchmark(const char* name);
void end_benchmark(const char* name);

// Concurrency utilities
void run_concurrent_test(void* (*test_func)(void*), int thread_count);

#endif // TEST_UTILS_H