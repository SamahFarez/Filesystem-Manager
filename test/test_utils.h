#ifndef TEST_UTILS_H
#define TEST_UTILS_H

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include "../include/filesystem.h"
#include "../include/paging.h"

// Test result codes
#define TEST_PASSED 1
#define TEST_FAILED 0

// Test assertion macros with color coding
#define ASSERT(condition, message) \
    do { \
        if (!(condition)) { \
            printf("\033[1;31mFAIL\033[0m: %s (%s:%d)\n", message, __FILE__, __LINE__); \
            return TEST_FAILED; \
        } else { \
            printf("\033[1;32mPASS\033[0m: %s\n", message); \
        } \
    } while (0)

#define TEST(test_name) \
    do { \
        printf("\n\033[1;33m=== Running test: %s ===\033[0m\n", #test_name); \
        clock_t start = clock(); \
        int result = test_name(); \
        clock_t end = clock(); \
        double elapsed = (double)(end - start) / CLOCKS_PER_SEC; \
        if (result) { \
            printf("\033[1;32m=== Test %s PASSED (%.3fs) ===\033[0m\n", #test_name, elapsed); \
        } else { \
            printf("\033[1;31m=== Test %s FAILED (%.3fs) ===\033[0m\n", #test_name, elapsed); \
        } \
    } while (0)

// Helper functions
void initialize_test_environment();
void cleanup_test_environment();

// Utility functions for testing
int verify_file_exists(const char* filename);
int verify_directory_exists(const char* dirname);
int verify_file_content(const char* filename, const char* expected_content);
int verify_file_permissions(const char* filename, int expected_perms);
int verify_file_owner(const char* filename, const char* expected_owner);
int verify_page_allocation(const char* filename, int expected_pages);

#endif // TEST_UTILS_H