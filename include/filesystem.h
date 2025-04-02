
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

typedef struct
{
    char *command;
} Job;

#define PAGE_SIZE 4096 // 4KB pages
#define TOTAL_PAGES (TOTAL_BLOCKS * BLOCK_SIZE / PAGE_SIZE)

typedef struct
{
    int physical_page; // Physical page number
    int is_allocated;  // Allocation status
} PageTableEntry;

typedef struct
{
    char filename[MAX_FILENAME];
    int size; // Will be calculated automatically
    PageTableEntry *page_table;
    int page_table_size;
    char owner[20];
    int permissions;
    time_t creation_time;
    time_t modification_time;
    int content_size;
    char *content;
    int file_position;
    int is_open;
    int open_count;
    int is_symlink;    // 1 if this is a symbolic link
    char *link_target; // Target path for symlinks
    int ref_count;     // For hard link reference counting
    ino_t inode;       // Unique inode number
} File;

typedef struct
{
    char dirname[MAX_FILENAME];
    File files[MAX_FILES];
    int file_count;
    int parent_directory;
    time_t creation_time;
    ino_t inode; // Add this for directories
} Directory;

typedef struct
{
    char username[20];
    char password[20];
} User;

typedef struct
{
    User users[MAX_USERS];
    Directory directories[MAX_DIRECTORIES];
    int current_directory;
} FileSystemState;

// Path resolution helpers
void split_path(const char *path, char **dir, char **file);
int find_directory_from_path(const char *path);
File* find_file_in_dir(int dir_idx, const char *filename);
File* resolve_file_path(const char *path, int *dir_idx, char **filename);
int resolve_path(const char *path);

// File operations
int open_file(const char *filename);
int close_file(const char *filename);
int file_seek(File *file, int offset, int whence);
int create_file(char *path, int permissions);
int create_directory(char *path);
char* get_current_working_directory();
void delete_file(char *path);
void delete_directory(const char *dirname);
void list_files();
int write_to_file(const char *path, const char *data, int append);
char* read_from_file(const char *path, int bytes_to_read, int offset);
void change_permissions(char *path, int mode);
void print_file_info(const char *path);
void copy_file_to_dir(const char *src_path, const char *dest_dir_path);
void change_directory(char *dirname);
void move_file_to_dir(const char *path, const char *dest_dir_path, const char *new_name);
void move_directory(const char *src_path, const char *dest_path, const char *new_name);

// Link operations
void create_hard_link(const char *source, const char *link);
void create_symbolic_link(const char *source, const char *link);

// System operations
void defragment_filesystem();
void format_filesystem();
void backup_filesystem(const char *backup_name);
void restore_filesystem(const char *backup_name);
void show_directory_info(const char *dirname);
void tree_command(int show_inodes);

// Initialization and state management
void initialize_directories();
void save_state();
void load_state();
int login();

// Helper functions
int check_file_permissions(File *file, int required_perms);


#endif // FILESYSTEM_H