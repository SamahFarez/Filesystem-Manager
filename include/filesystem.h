#ifndef FILESYSTEM_H
#define FILESYSTEM_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <pthread.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <ctype.h>
#include <time.h>

#define MAX_JOBS 10
#define BLOCK_SIZE 4
#define TOTAL_BLOCKS 262144 // 1MB / 4B
#define MAX_USERS 3
#define MAX_FILES 100
#define MAX_FILENAME 50
#define MAX_DIRECTORIES 10
#define STORAGE_FILE "filesystem.dat"

// ANSI color codes
#define COLOR_YELLOW "\033[1;33m"
#define COLOR_BLUE "\033[1;34m"
#define COLOR_GREEN "\033[1;32m"
#define COLOR_RED "\033[1;31m"
#define COLOR_RESET "\033[0m"

#define SEEK_SET 0 // Seek from beginning of file
#define SEEK_CUR 1 // Seek from current position
#define SEEK_END 2 // Seek from end of file

#define PAGE_SIZE 4096 // 4KB pages
#define TOTAL_PAGES (TOTAL_BLOCKS * BLOCK_SIZE / PAGE_SIZE)

typedef struct {
    int physical_page; // Physical page number
    int is_allocated;  // Allocation status
} PageTableEntry;

typedef struct {
    char filename[MAX_FILENAME];
    int size;
    PageTableEntry *page_table; // Dynamic page table
    int page_table_size;        // Number of entries in page table
    char owner[20];
    int permissions;
    time_t creation_time;
    time_t modification_time;
    int content_size;
    char *content;
    int file_position;
    int is_open;
    int open_count;
} File;

typedef struct {
    char dirname[MAX_FILENAME];
    File files[MAX_FILES];
    int file_count;
    int parent_directory;
    time_t creation_time;
} Directory;

typedef struct {
    char username[20];
    char password[20];
} User;

typedef struct {
    User users[MAX_USERS];
    Directory directories[MAX_DIRECTORIES];
    int current_directory;
} FileSystemState;

// Global variables
extern FileSystemState fs_state;
extern pthread_mutex_t mutex;
extern unsigned char page_bitmap[TOTAL_PAGES / 8];

// Function declarations
void initialize_directories();
void save_state();
void load_state();
int create_file(char *filename, int size, char *owner, int permissions);
int create_directory(char *dirname);
void delete_file(char *filename);
void delete_directory(const char *dirname);
void list_files();
int write_to_file(const char *filename, const char *data, int append);
char *read_from_file(const char *filename, int bytes_to_read, int offset);
int file_seek(File *file, int offset, int whence);
void change_permissions(char *filename, int mode);
void print_file_info(const char *filename);
void change_directory(char *dirname);
void copy_file_to_dir(const char *filename, const char *dirname);
void move_file_to_dir(const char *filename, const char *dirname);
void create_hard_link(const char *source, const char *link);
void create_symbolic_link(const char *source, const char *link);
void format_filesystem();
void backup_filesystem(const char *backup_name);
void restore_filesystem(const char *backup_name);
void show_directory_info(const char *dirname);
char *get_current_working_directory();
int login();

// Paging functions
void initialize_paging();
int allocate_pages(int pages_needed, PageTableEntry **page_table);
void free_pages(File *file);
void print_page_table(const char *filename);
void print_page_bitmap();

#endif // FILESYSTEM_H