#include "test_utils.h"
#include "../include/filesystem.h"
#include "../include/paging.h"
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <string.h>

// Test result constants
#define TEST_PASSED 1
#define TEST_FAILED 0

// Global variables from filesystem.c needed for testing
extern FileSystemState fs_state;
extern unsigned char page_bitmap[TOTAL_PAGES / 8];
extern pthread_mutex_t mutex;

// Test statistics
typedef struct {
    int total;
    int passed;
    int failed;
} TestStats;

TestStats test_stats = {0};

// Forward declarations
void initialize_test_environment();
void cleanup_test_environment();

// Helper macros
#define ASSERT(condition, message) \
    do { \
        if (!(condition)) { \
            printf("\033[1;31mAssertion failed: %s\033[0m\n", message); \
            return TEST_FAILED; \
        } \
    } while (0)

// Test cases
int test_file_creation();
int test_directory_operations();
int test_file_operations();
int test_permissions();
int test_paging_system();
int test_concurrent_access();
int test_path_resolution();
int test_link_operations();
int test_backup_restore();
int test_special_commands();
int test_user_management();
int test_file_metadata();
int test_directory_listing();
int test_error_handling();

// Helper function to find file by name
File* find_file(const char* filename) {
    for (int i = 0; i < fs_state.directories[fs_state.current_directory].file_count; i++) {
        if (strcmp(fs_state.directories[fs_state.current_directory].files[i].filename, filename) == 0) {
            return &fs_state.directories[fs_state.current_directory].files[i];
        }
    }
    return NULL;
}

// Helper to count allocated pages
int count_allocated_pages() {
    int count = 0;
    for (int i = 0; i < TOTAL_PAGES; i++) {
        if (page_bitmap[i / 8] & (1 << (i % 8))) {
            count++;
        }
    }
    return count;
}

// Update the TEST macro to properly track statistics
#undef TEST
#define TEST(test_name) \
    do { \
        printf("\n\033[1;33m=== Running test: %s ===\033[0m\n", #test_name); \
        clock_t start = clock(); \
        int result = test_name(); \
        clock_t end = clock(); \
        double elapsed = (double)(end - start) / CLOCKS_PER_SEC; \
        test_stats.total++; \
        if (result) { \
            printf("\033[1;32m=== Test %s PASSED (%.3fs) ===\033[0m\n", #test_name, elapsed); \
            test_stats.passed++; \
        } else { \
            printf("\033[1;31m=== Test %s FAILED (%.3fs) ===\033[0m\n", #test_name, elapsed); \
            test_stats.failed++; \
        } \
    } while (0)

int main() {
    // Initialize test environment
    initialize_test_environment();

    printf("\n\033[1;34m=== Starting Test Suite ===\033[0m\n");
    
    // Run all tests
    TEST(test_file_creation);
    TEST(test_directory_operations);
    TEST(test_file_operations);
    TEST(test_permissions);
    TEST(test_paging_system);
    TEST(test_concurrent_access);
    TEST(test_path_resolution);
    TEST(test_link_operations);
    TEST(test_backup_restore);
    TEST(test_special_commands);
    TEST(test_user_management);
    TEST(test_file_metadata);
    TEST(test_directory_listing);
    TEST(test_error_handling);

    // Print summary
    printf("\n\033[1;34m=== Test Summary ===\033[0m\n");
    printf("\033[1;36mTotal Tests: %d\033[0m\n", test_stats.total);
    printf("\033[1;32mPassed: %d\033[0m\n", test_stats.passed);
    printf("\033[1;31mFailed: %d\033[0m\n", test_stats.failed);
    
    // Calculate and print score
    float score = test_stats.total > 0 ? (test_stats.passed * 100.0f) / test_stats.total : 0;
    printf("\033[1;35mScore: %.1f%%\033[0m\n", score);

    // Clean up
    cleanup_test_environment();
    return test_stats.failed > 0 ? 1 : 0;
}

// [Rest of the test functions remain the same, but with the following fixes:]
// 1. Ensure all ASSERT macros are properly closed with semicolons
// 2. Add proper includes (unistd.h for sleep(), time.h for time())
// 3. Fix the cleanup_test_environment() function:
void cleanup_test_environment() {
    // Clean up any remaining test files
    for (int i = 0; i < MAX_DIRECTORIES; i++) {
        if (strlen(fs_state.directories[i].dirname) > 0) {
            for (int j = 0; j < fs_state.directories[i].file_count; j++) {
                File* f = &fs_state.directories[i].files[j];
                if (f->content) free(f->content);
                if (f->page_table) free(f->page_table);
                if (f->link_target) free(f->link_target);
            }
            fs_state.directories[i].file_count = 0;
        }
    }
    
    // Destroy mutex
    pthread_mutex_destroy(&mutex);
}