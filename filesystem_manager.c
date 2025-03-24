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
#include <time.h>

#define MAX_JOBS 10
#define TIME_QUANTUM 2
#define BLOCK_SIZE 4
#define TOTAL_BLOCKS 262144 // 1MB / 4B
#define MAX_USERS 2
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
    int size;
    PageTableEntry *page_table; // Dynamic page table
    int page_table_size;        // Number of entries in page table
    int blocks_used;
    int start_block;
    char owner[20];
    int permissions;
    time_t creation_time;
    time_t modification_time;
    int content_size;  // Add this to track content size
    char *content;     // Pointer to actual content
    int file_position; // Add this for seek functionality
} File;

typedef struct
{
    char dirname[MAX_FILENAME];
    File files[MAX_FILES];
    int file_count;
    int parent_directory;
    time_t creation_time;
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
    int block_map[TOTAL_BLOCKS];
} FileSystemState;

FileSystemState fs_state;
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

Job job_queue[MAX_JOBS];
int front = 0, rear = 0, job_count = 0;
pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t job_available = PTHREAD_COND_INITIALIZER;
int running = 1;
unsigned char page_bitmap[TOTAL_PAGES / 8]; // 1 bit per page

// Function prototypes
void initialize_directories();
void save_state();
void load_state();
int allocate_blocks(int blocks_needed);
void free_blocks(int start_block, int blocks_used);
int create_file(char *filename, int size, char *owner, int permissions);
int create_directory(char *dirname);
void delete_file(char *filename);
void list_files();
int write_to_file(const char *filename, const char *data, int append);
char *read_from_file(const char *filename, int bytes_to_read);
int login();
int find_directory_index(const char *dirname);
void copy_file_to_dir(const char *filename, const char *dirname);
void move_file_to_dir(const char *filename, const char *dirname);
void change_directory(char *dirname);
void change_permissions(char *filename, int mode);
void print_file_info(const char *filename);
void create_hard_link(const char *source, const char *link);
void create_symbolic_link(const char *source, const char *link);
void help();
void execute_job(Job job);
void print_queue(int current_job_index);
void *scheduler(void *arg);
void add_job(const char *command);
void cleanup();
void handle_signal(int sig);

void initialize_directories()
{
    if (strlen(fs_state.directories[0].dirname) == 0)
    {
        memset(page_bitmap, 0, sizeof(page_bitmap));

        // Initialize root directory
        strcpy(fs_state.directories[0].dirname, "root");
        fs_state.directories[0].file_count = 0;
        fs_state.directories[0].parent_directory = -1;
        fs_state.directories[0].creation_time = time(NULL);

        // Create a default directory
        strcpy(fs_state.directories[1].dirname, "home");
        fs_state.directories[1].file_count = 0;
        fs_state.directories[1].parent_directory = 0;
        fs_state.directories[1].creation_time = time(NULL);

        fs_state.current_directory = 0;

        // Create default users
        strcpy(fs_state.users[0].username, "ikram");
        strcpy(fs_state.users[0].password, "ikrampass");
        strcpy(fs_state.users[1].username, "ines");
        strcpy(fs_state.users[1].password, "inespass");
        strcpy(fs_state.users[1].username, "ali");
        strcpy(fs_state.users[1].password, "alipass");

        // Create some default files
        // In your initialize_directories() function, replace the file creation with:

        // Create page tables for default files
        PageTableEntry *file1_pages = malloc(sizeof(PageTableEntry));
        PageTableEntry *file2_pages = malloc(sizeof(PageTableEntry));

        if (!file1_pages || !file2_pages)
        {
            // Handle allocation error
            free(file1_pages);
            free(file2_pages);
            return;
        }

        // Allocate pages (using physical page numbers 0 and 1 for simplicity)
        file1_pages[0].physical_page = 0;
        file1_pages[0].is_allocated = 1;
        file2_pages[0].physical_page = 1;
        file2_pages[0].is_allocated = 1;

        // Mark pages as used in bitmap
        page_bitmap[0 / 8] |= (1 << (0 % 8));
        page_bitmap[1 / 8] |= (1 << (1 % 8));

        // Create default files with paging
        File file1 = {
            .filename = "readme.txt",
            .size = 16,
            .owner = "root",
            .permissions = 0644,
            .creation_time = time(NULL),
            .modification_time = time(NULL),
            .content_size = strlen("HELLO WORLD") + 1,
            .content = strdup("HELLO WORLD"),
            .file_position = 0,
            .page_table = file1_pages,
            .page_table_size = 1 // Only needs 1 page (4KB) for small file
        };

        File file2 = {
            .filename = "notes.txt",
            .size = 8,
            .owner = "root",
            .permissions = 0600,
            .creation_time = time(NULL),
            .modification_time = time(NULL),
            .content_size = strlen("HELLO WORLD") + 1,
            .content = strdup("HELLO WORLD"),
            .file_position = 0,
            .page_table = file2_pages,
            .page_table_size = 1 // Only needs 1 page (4KB) for small file
        };

        // Add to directory
        fs_state.directories[0].files[fs_state.directories[0].file_count++] = file1;
        fs_state.directories[0].files[fs_state.directories[0].file_count++] = file2;

        // Mark blocks as used
        for (int i = 0; i < file1.blocks_used + file2.blocks_used; i++)
        {
            fs_state.block_map[i] = 1;
        }
    }
}

void save_state()
{
    FILE *fp = fopen(STORAGE_FILE, "wb");
    if (fp)
    {
        // Save main structure without content pointers
        size_t base_size = sizeof(fs_state) - sizeof(File) * MAX_DIRECTORIES * MAX_FILES;
        fwrite(&fs_state, base_size, 1, fp);

        // Save directory structures without file content
        for (int i = 0; i < MAX_DIRECTORIES; i++)
        {
            size_t dir_size = sizeof(Directory) - sizeof(File) * MAX_FILES;
            fwrite(&fs_state.directories[i], dir_size, 1, fp);

            // Save file metadata and content
            for (int j = 0; j < fs_state.directories[i].file_count; j++)
            {
                File *file = &fs_state.directories[i].files[j];
                size_t file_meta_size = sizeof(File) - sizeof(char *);
                fwrite(file, file_meta_size, 1, fp);

                // Save content with size prefix
                if (file->content && file->content_size > 0)
                {
                    fwrite(file->content, 1, file->content_size, fp);
                }
            }
        }

        // Save block map
        fwrite(fs_state.block_map, sizeof(fs_state.block_map), 1, fp);
        fclose(fp);
    }
}

void load_state()
{
    printf("Attempting to load state...\n");
    FILE *fp = fopen(STORAGE_FILE, "rb");
    if (fp)
    {
        printf("Found existing filesystem.dat\n");
        // Load base structure
        size_t base_size = sizeof(fs_state) - sizeof(File) * MAX_DIRECTORIES * MAX_FILES;
        if (fread(&fs_state, base_size, 1, fp) != 1)
        {
            printf("Error reading base structure, initializing new one\n");
            fclose(fp);
            initialize_directories();
            return;
        }

        // Load directories
        for (int i = 0; i < MAX_DIRECTORIES; i++)
        {
            size_t dir_size = sizeof(Directory) - sizeof(File) * MAX_FILES;
            if (fread(&fs_state.directories[i], dir_size, 1, fp) != 1)
            {
                printf("Error reading directory %d, initializing new one\n", i);
                fclose(fp);
                initialize_directories();
                return;
            }

            // Load files
            for (int j = 0; j < fs_state.directories[i].file_count; j++)
            {
                File *file = &fs_state.directories[i].files[j];
                size_t file_meta_size = sizeof(File) - sizeof(char *);
                if (fread(file, file_meta_size, 1, fp) != 1)
                {
                    printf("Error reading file %d in directory %d\n", j, i);
                    break;
                }

                // Load content
                if (file->content_size > 0)
                {
                    file->content = malloc(file->content_size);
                    if (fread(file->content, 1, file->content_size, fp) != file->content_size)
                    {
                        printf("Error reading file content\n");
                        free(file->content);
                        file->content = NULL;
                    }
                }
                else
                {
                    file->content = NULL;
                }
            }
        }

        // Load block map
        if (fread(fs_state.block_map, sizeof(fs_state.block_map), 1, fp) != 1)
        {
            printf("Error reading block map\n");
        }
        fclose(fp);
    }
    else
    {
        printf("No existing filesystem found, initializing new one\n");
        initialize_directories();
    }
}

// Add this to your file system code
void print_page_table(const char *filename)
{
    pthread_mutex_lock(&mutex);

    File *file = NULL;
    for (int i = 0; i < fs_state.directories[fs_state.current_directory].file_count; i++)
    {
        if (strcmp(fs_state.directories[fs_state.current_directory].files[i].filename, filename) == 0)
        {
            file = &fs_state.directories[fs_state.current_directory].files[i];
            break;
        }
    }

    if (!file)
    {
        printf("File not found: %s\n", filename);
        pthread_mutex_unlock(&mutex);
        return;
    }

    printf("\nPage Table for %s (Size: %d bytes, Pages: %d):\n",
           filename, file->size, file->page_table_size);
    printf("----------------------------------------\n");
    printf("Page | Physical Page | Status\n");
    printf("-----|---------------|--------\n");

    for (int i = 0; i < file->page_table_size; i++)
    {
        printf("%4d | %13d | %s\n",
               i,
               file->page_table[i].physical_page,
               file->page_table[i].is_allocated ? "Allocated" : "Free");
    }

    pthread_mutex_unlock(&mutex);
}

void print_page_bitmap()
{
    pthread_mutex_lock(&mutex);

    printf("\nPage Allocation Bitmap:\n");
    printf("----------------------\n");

    for (int i = 0; i < TOTAL_PAGES; i++)
    {
        if (i % 64 == 0)
            printf("\n%04d: ", i);
        printf("%c", (page_bitmap[i / 8] & (1 << (i % 8))) ? 'X' : '.');
    }

    printf("\n\nX = Allocated, . = Free\n");
    pthread_mutex_unlock(&mutex);
}

int allocate_blocks(int blocks_needed)
{
    int start_block = -1;
    int consecutive_blocks = 0;
    for (int i = 0; i < TOTAL_BLOCKS; i++)
    {
        if (fs_state.block_map[i] == 0)
        {
            if (start_block == -1)
                start_block = i;
            consecutive_blocks++;
            if (consecutive_blocks == blocks_needed)
            {
                for (int j = start_block; j < start_block + blocks_needed; j++)
                {
                    fs_state.block_map[j] = 1;
                }
                return start_block;
            }
        }
        else
        {
            start_block = -1;
            consecutive_blocks = 0;
        }
    }
    return -1;
}

void free_blocks(int start_block, int blocks_used)
{
    for (int i = start_block; i < start_block + blocks_used; i++)
    {
        fs_state.block_map[i] = 0;
    }
}

int file_seek(File *file, int offset, int whence)
{
    int new_position;

    switch (whence)
    {
    case SEEK_SET: // From beginning
        new_position = offset;
        break;
    case SEEK_CUR: // From current position
        new_position = file->file_position + offset;
        break;
    case SEEK_END: // From end
        new_position = file->content_size + offset;
        break;
    default:
        return -1; // Invalid whence
    }

    // Validate position
    if (new_position < 0)
        new_position = 0;
    if (new_position > file->content_size)
        new_position = file->content_size;

    file->file_position = new_position;
    return new_position;
}

void initialize_paging()
{
    memset(page_bitmap, 0, sizeof(page_bitmap));
}

// Helper function to allocate pages
static int allocate_pages(int pages_needed, PageTableEntry **page_table)
{
    *page_table = malloc(pages_needed * sizeof(PageTableEntry));
    if (*page_table == NULL)
    {
        return -1;
    }

    for (int i = 0; i < pages_needed; i++)
    {
        int page = -1;
        for (int j = 0; j < TOTAL_PAGES; j++)
        {
            if (!(page_bitmap[j / 8] & (1 << (j % 8))))
            {
                page_bitmap[j / 8] |= (1 << (j % 8));
                (*page_table)[i].physical_page = j;
                (*page_table)[i].is_allocated = 1;
                page = j;
                break;
            }
        }
        if (page == -1)
        {
            // Cleanup already allocated pages
            for (int k = 0; k < i; k++)
            {
                page_bitmap[(*page_table)[k].physical_page / 8] &= ~(1 << ((*page_table)[k].physical_page % 8));
            }
            free(*page_table);
            return -1;
        }
    }
    return 0;
}

void free_page(int page_num)
{
    if (page_num >= 0 && page_num < TOTAL_PAGES)
    {
        page_bitmap[page_num / 8] &= ~(1 << (page_num % 8));
    }
}

// Updated create_file function
int create_file(char *filename, int size, char *owner, int permissions)
{
    pthread_mutex_lock(&mutex);

    // Check if file already exists
    for (int i = 0; i < fs_state.directories[fs_state.current_directory].file_count; i++)
    {
        if (strcmp(fs_state.directories[fs_state.current_directory].files[i].filename, filename) == 0)
        {
            printf(COLOR_RED "Error: File '%s' already exists\n" COLOR_RESET, filename);
            pthread_mutex_unlock(&mutex);
            return -1;
        }
    }

    // Calculate needed pages
    int pages_needed = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    PageTableEntry *page_table = NULL;

    if (allocate_pages(pages_needed, &page_table) == -1)
    {
        printf(COLOR_RED "Error: Not enough disk space\n" COLOR_RESET);
        pthread_mutex_unlock(&mutex);
        return -1;
    }

    // Create default content
    const char *default_content = "HELLO WORLD";
    int content_size = strlen(default_content) + 1;

    File new_file;
    strcpy(new_file.filename, filename);
    new_file.size = size;
    strcpy(new_file.owner, owner);
    new_file.permissions = permissions;
    new_file.creation_time = time(NULL);
    new_file.modification_time = new_file.creation_time;
    new_file.content_size = content_size;
    new_file.content = strdup(default_content);
    new_file.file_position = 0;
    new_file.page_table = page_table;
    new_file.page_table_size = pages_needed;

    if (!new_file.content)
    {
        printf(COLOR_RED "Error: Memory allocation failed for file content\n" COLOR_RESET);
        for (int i = 0; i < pages_needed; i++)
        {
            page_bitmap[page_table[i].physical_page / 8] &= ~(1 << (page_table[i].physical_page % 8));
        }
        free(page_table);
        pthread_mutex_unlock(&mutex);
        return -1;
    }

    // Add to directory
    fs_state.directories[fs_state.current_directory].files[fs_state.directories[fs_state.current_directory].file_count++] = new_file;

    save_state();
    printf(COLOR_GREEN "File '%s' created successfully with permissions %04o\n" COLOR_RESET,
           filename, permissions);
    printf("Default content added: \"%s\"\n", default_content);
    pthread_mutex_unlock(&mutex);
    return 0;
}

int create_directory(char *dirname)
{
    pthread_mutex_lock(&mutex);

    // Check if directory already exists
    for (int i = 0; i < MAX_DIRECTORIES; i++)
    {
        if (strcmp(fs_state.directories[i].dirname, dirname) == 0)
        {
            printf(COLOR_RED "Error: Directory '%s' already exists\n" COLOR_RESET, dirname);
            pthread_mutex_unlock(&mutex);
            return -1;
        }
    }

    for (int i = 0; i < MAX_DIRECTORIES; i++)
    {
        if (strlen(fs_state.directories[i].dirname) == 0)
        {
            strcpy(fs_state.directories[i].dirname, dirname);
            fs_state.directories[i].file_count = 0;
            fs_state.directories[i].parent_directory = fs_state.current_directory;
            fs_state.directories[i].creation_time = time(NULL);

            save_state();
            printf(COLOR_GREEN "Directory '%s' created successfully\n" COLOR_RESET, dirname);
            pthread_mutex_unlock(&mutex);
            return 0;
        }
    }

    printf(COLOR_RED "Error: Maximum number of directories reached\n" COLOR_RESET);
    pthread_mutex_unlock(&mutex);
    return -1;
}

char *get_current_working_directory()
{
    pthread_mutex_lock(&mutex);

    static char path[1024] = {0};
    int current = fs_state.current_directory;
    int parent = fs_state.directories[current].parent_directory;

    // Start from current directory and work backwards to root
    char *ptr = path + sizeof(path) - 1;
    *ptr = '\0';

    while (current != -1)
    {
        const char *dirname = fs_state.directories[current].dirname;
        size_t len = strlen(dirname);

        ptr -= len;
        memcpy(ptr, dirname, len);

        if (parent != -1)
        {
            *--ptr = '/';
        }

        current = parent;
        if (current != -1)
        {
            parent = fs_state.directories[current].parent_directory;
        }
    }

    // If we're at root, make sure we have a leading slash
    if (*ptr != '/')
    {
        *--ptr = '/';
    }

    pthread_mutex_unlock(&mutex);
    return ptr;
}

void delete_file(char *filename)
{
    pthread_mutex_lock(&mutex);
    int found = 0;
    for (int i = 0; i < fs_state.directories[fs_state.current_directory].file_count; i++)
    {
        if (strcmp(fs_state.directories[fs_state.current_directory].files[i].filename, filename) == 0)
        {
            // Free content memory
            if (fs_state.directories[fs_state.current_directory].files[i].content)
            {
                free(fs_state.directories[fs_state.current_directory].files[i].content);
            }

            // Free all pages used by this file
            for (int j = 0; j < fs_state.directories[fs_state.current_directory].files[i].page_table_size; j++)
            {
                int page_num = fs_state.directories[fs_state.current_directory].files[i].page_table[j].physical_page;
                page_bitmap[page_num / 8] &= ~(1 << (page_num % 8));
            }

            // Free the page table
            free(fs_state.directories[fs_state.current_directory].files[i].page_table);

            // Shift remaining files
            for (int j = i; j < fs_state.directories[fs_state.current_directory].file_count - 1; j++)
            {
                fs_state.directories[fs_state.current_directory].files[j] =
                    fs_state.directories[fs_state.current_directory].files[j + 1];
            }
            fs_state.directories[fs_state.current_directory].file_count--;
            found = 1;
            break;
        }
    }

    if (found)
    {
        save_state();
        printf(COLOR_GREEN "File '%s' deleted successfully\n" COLOR_RESET, filename);
    }
    else
    {
        printf(COLOR_RED "Error: File '%s' not found\n" COLOR_RESET, filename);
    }
    pthread_mutex_unlock(&mutex);
}

void delete_directory(const char *dirname)
{
    pthread_mutex_lock(&mutex);

    int dir_index = -1;
    int parent_dir = fs_state.current_directory;

    // Find the directory
    for (int i = 0; i < MAX_DIRECTORIES; i++)
    {
        if (strcmp(fs_state.directories[i].dirname, dirname) == 0 &&
            fs_state.directories[i].parent_directory == parent_dir)
        {
            dir_index = i;
            break;
        }
    }

    if (dir_index == -1)
    {
        printf(COLOR_RED "Error: Directory '%s' not found\n" COLOR_RESET, dirname);
        pthread_mutex_unlock(&mutex);
        return;
    }

    // Don't allow deleting root directory
    if (dir_index == 0)
    {
        printf(COLOR_RED "Error: Cannot delete root directory\n" COLOR_RESET);
        pthread_mutex_unlock(&mutex);
        return;
    }

    // Don't allow deleting current directory
    if (dir_index == fs_state.current_directory)
    {
        printf(COLOR_RED "Error: Cannot delete current directory\n" COLOR_RESET);
        pthread_mutex_unlock(&mutex);
        return;
    }

    // Check if directory is empty or has subdirectories
    int needs_confirmation = 0;
    if (fs_state.directories[dir_index].file_count > 0)
    {
        needs_confirmation = 1;
        printf(COLOR_RED "Warning: Directory '%s' is not empty (%d files)\n" COLOR_RESET,
               dirname, fs_state.directories[dir_index].file_count);
    }

    // Check for subdirectories
    for (int i = 0; i < MAX_DIRECTORIES; i++)
    {
        if (fs_state.directories[i].parent_directory == dir_index &&
            strlen(fs_state.directories[i].dirname) > 0)
        {
            needs_confirmation = 1;
            printf(COLOR_RED "Warning: Directory '%s' contains subdirectories\n" COLOR_RESET, dirname);
            break;
        }
    }

    // Ask for confirmation if not empty
    if (needs_confirmation)
    {
        printf(COLOR_RED "Are you sure you want to delete '%s' and all its contents? [Y/n] " COLOR_RESET, dirname);
        fflush(stdout);

        char response[10];
        if (fgets(response, sizeof(response), stdin) == NULL)
        {
            pthread_mutex_unlock(&mutex);
            return;
        }

        // Check response (default to 'Y' if empty)
        if (response[0] != '\n' && tolower(response[0]) != 'y')
        {
            printf("Deletion cancelled\n");
            pthread_mutex_unlock(&mutex);
            return;
        }
    }

    // Delete all files in the directory first
    for (int i = 0; i < fs_state.directories[dir_index].file_count; i++)
    {
        File *file = &fs_state.directories[dir_index].files[i];
        if (file->content)
        {
            free(file->content);
        }
        free_blocks(file->start_block, file->blocks_used);
    }
    fs_state.directories[dir_index].file_count = 0;

    // Delete any subdirectories (recursive)
    for (int i = 0; i < MAX_DIRECTORIES; i++)
    {
        if (fs_state.directories[i].parent_directory == dir_index &&
            strlen(fs_state.directories[i].dirname) > 0)
        {
            // Temporarily unlock mutex for recursive call
            pthread_mutex_unlock(&mutex);
            delete_directory(fs_state.directories[i].dirname);
            pthread_mutex_lock(&mutex);
        }
    }

    // Clear the directory entry
    memset(fs_state.directories[dir_index].dirname, 0, MAX_FILENAME);
    fs_state.directories[dir_index].parent_directory = -1;

    save_state();
    printf(COLOR_GREEN "Directory '%s' deleted successfully\n" COLOR_RESET, dirname);
    pthread_mutex_unlock(&mutex);
}

void list_files()
{
    pthread_mutex_lock(&mutex);

    // Count actual existing directories
    int dir_count = 0;
    for (int i = 0; i < MAX_DIRECTORIES; i++)
    {
        if (strlen(fs_state.directories[i].dirname) > 0)
        {
            dir_count++;
        }
    }

    printf("\nCurrent directory: %d (%s)\n", fs_state.current_directory,
           fs_state.directories[fs_state.current_directory].dirname);
    printf("Existing directories: %d\n", dir_count); // Changed from "Total directories"
    printf("File count: %d\n", fs_state.directories[fs_state.current_directory].file_count);

    printf("\nContents of directory '%s':\n", fs_state.directories[fs_state.current_directory].dirname);
    printf("--------------------------------\n");

    // List directories
    printf("[Directories]\n");
    for (int i = 0; i < MAX_DIRECTORIES; i++)
    {
        if (i != fs_state.current_directory &&
            fs_state.directories[i].parent_directory == fs_state.current_directory &&
            strlen(fs_state.directories[i].dirname) > 0)
        {
            printf("  %s/\n", fs_state.directories[i].dirname);
        }
    }

    // List files
    printf("\n[Files]\n");
    for (int i = 0; i < fs_state.directories[fs_state.current_directory].file_count; i++)
    {
        File *f = &fs_state.directories[fs_state.current_directory].files[i];
        printf("  %-15s %6d bytes  %04o  %s\n",
               f->filename, f->size, f->permissions, f->owner);
    }
    printf("--------------------------------\n");
    pthread_mutex_unlock(&mutex);
}
int write_to_file(const char *filename, const char *data, int append)
{
    pthread_mutex_lock(&mutex);

    File *file = NULL;
    // Find the file
    for (int i = 0; i < fs_state.directories[fs_state.current_directory].file_count; i++)
    {
        if (strcmp(fs_state.directories[fs_state.current_directory].files[i].filename, filename) == 0)
        {
            file = &fs_state.directories[fs_state.current_directory].files[i];
            break;
        }
    }

    if (!file)
    {
        printf(COLOR_RED "Error: File '%s' not found\n" COLOR_RESET, filename);
        pthread_mutex_unlock(&mutex);
        return -1;
    }

    // Check write permissions
    if ((file->permissions & 0222) == 0)
    {
        printf(COLOR_RED "Error: No write permissions for file '%s'\n" COLOR_RESET, filename);
        pthread_mutex_unlock(&mutex);
        return -1;
    }

    int data_len = strlen(data);
    int new_content_size;
    char *new_content;

    if (append)
    {
        // Append mode
        new_content_size = file->content_size + data_len;
        new_content = realloc(file->content, new_content_size + 1);
        if (!new_content)
        {
            printf(COLOR_RED "Error: Memory allocation failed\n" COLOR_RESET);
            pthread_mutex_unlock(&mutex);
            return -1;
        }
        strcat(new_content, data);
    }
    else
    {
        // Overwrite mode (ignore seek position)
        new_content_size = data_len;
        new_content = strdup(data);
        if (!new_content)
        {
            printf(COLOR_RED "Error: Memory allocation failed\n" COLOR_RESET);
            pthread_mutex_unlock(&mutex);
            return -1;
        }
        // Reset position on overwrite
        file->file_position = 0;
    }

    // Update file properties
    if (file->content)
        free(file->content);
    file->content = new_content;
    file->content_size = new_content_size;
    file->size = new_content_size; // For compatibility
    file->modification_time = time(NULL);

    // Update block usage if needed
    int blocks_needed = (new_content_size / BLOCK_SIZE) + 1;
    if (blocks_needed != file->blocks_used)
    {
        free_blocks(file->start_block, file->blocks_used);
        file->start_block = allocate_blocks(blocks_needed);
        if (file->start_block == -1)
        {
            printf(COLOR_RED "Error: Not enough space for file expansion\n" COLOR_RESET);
            pthread_mutex_unlock(&mutex);
            return -1;
        }
        file->blocks_used = blocks_needed;
    }

    save_state();
    printf(COLOR_GREEN "Wrote %d bytes to '%s' (new size: %d bytes)\n" COLOR_RESET,
           data_len, filename, new_content_size);

    pthread_mutex_unlock(&mutex);
    return data_len;
}

char *read_from_file(const char *filename, int bytes_to_read)
{
    pthread_mutex_lock(&mutex);

    File *file = NULL;
    // Find the file
    for (int i = 0; i < fs_state.directories[fs_state.current_directory].file_count; i++)
    {
        if (strcmp(fs_state.directories[fs_state.current_directory].files[i].filename, filename) == 0)
        {
            file = &fs_state.directories[fs_state.current_directory].files[i];
            break;
        }
    }

    if (!file)
    {
        printf(COLOR_RED "Error: File '%s' not found\n" COLOR_RESET, filename);
        pthread_mutex_unlock(&mutex);
        return NULL;
    }

    // Check read permissions
    if ((file->permissions & 0444) == 0)
    {
        printf(COLOR_RED "Error: No read permissions for file '%s'\n" COLOR_RESET, filename);
        pthread_mutex_unlock(&mutex);
        return NULL;
    }

    // Handle EOF
    if (file->file_position >= file->content_size)
    {
        pthread_mutex_unlock(&mutex);
        return strdup(""); // Return empty string at EOF
    }

    // Calculate how many bytes we can actually read
    int remaining_bytes = file->content_size - file->file_position;
    int read_bytes = (bytes_to_read <= 0) ? remaining_bytes : (bytes_to_read < remaining_bytes) ? bytes_to_read
                                                                                                : remaining_bytes;

    // Calculate which pages we need to read from
    int start_page = file->file_position / PAGE_SIZE;
    int end_page = (file->file_position + read_bytes - 1) / PAGE_SIZE;
    int start_offset = file->file_position % PAGE_SIZE;

    // Allocate buffer and copy data
    char *buffer = malloc(read_bytes + 1);
    if (!buffer)
    {
        printf(COLOR_RED "Error: Memory allocation failed\n" COLOR_RESET);
        pthread_mutex_unlock(&mutex);
        return NULL;
    }

    char *current_pos = buffer;
    int bytes_remaining = read_bytes;

    for (int i = start_page; i <= end_page && bytes_remaining > 0; i++)
    {
        if (i >= file->page_table_size || !file->page_table[i].is_allocated)
        {
            break;
        }

        int bytes_in_page = PAGE_SIZE - start_offset;
        if (bytes_in_page > bytes_remaining)
        {
            bytes_in_page = bytes_remaining;
        }

        // In a real implementation, you would read from disk here
        // For this simulation, we'll just read from the content buffer
        memcpy(current_pos, file->content + (i * PAGE_SIZE) + start_offset, bytes_in_page);

        current_pos += bytes_in_page;
        bytes_remaining -= bytes_in_page;
        start_offset = 0; // For subsequent pages, start at beginning
    }

    buffer[read_bytes] = '\0'; // Null-terminate

    // Update position
    file->file_position += read_bytes;
    file->modification_time = time(NULL);

    pthread_mutex_unlock(&mutex);
    return buffer;
}

void change_permissions(char *filename, int mode)
{
    pthread_mutex_lock(&mutex);
    int found = 0;
    for (int i = 0; i < fs_state.directories[fs_state.current_directory].file_count; i++)
    {
        if (strcmp(fs_state.directories[fs_state.current_directory].files[i].filename, filename) == 0)
        {
            fs_state.directories[fs_state.current_directory].files[i].permissions = mode;
            fs_state.directories[fs_state.current_directory].files[i].modification_time = time(NULL);
            found = 1;
            break;
        }
    }

    if (found)
    {
        save_state();
        printf(COLOR_GREEN "Permissions of '%s' changed to %04o\n" COLOR_RESET, filename, mode);
    }
    else
    {
        printf(COLOR_RED "Error: File '%s' not found\n" COLOR_RESET, filename);
    }
    pthread_mutex_unlock(&mutex);
}

void print_file_info(const char *filename)
{
    pthread_mutex_lock(&mutex);
    int found = 0;
    for (int i = 0; i < fs_state.directories[fs_state.current_directory].file_count; i++)
    {
        if (strcmp(fs_state.directories[fs_state.current_directory].files[i].filename, filename) == 0)
        {
            File *f = &fs_state.directories[fs_state.current_directory].files[i];
            printf("\nFile: %s\n", f->filename);
            printf("Size: %d bytes\n", f->size);
            printf("Blocks used: %d (starting at block %d)\n", f->blocks_used, f->start_block);
            printf("Owner: %s\n", f->owner);
            printf("Permissions: %04o\n", f->permissions);
            printf("Created: %s", ctime(&f->creation_time));
            printf("Modified: %s", ctime(&f->modification_time));
            found = 1;
            break;
        }
    }

    if (!found)
    {
        printf(COLOR_RED "Error: File '%s' not found\n" COLOR_RESET, filename);
    }
    pthread_mutex_unlock(&mutex);
}

void change_directory(char *dirname)
{
    // First get current directory info WITHOUT locking
    int current_dir = fs_state.current_directory;
    int parent_dir = fs_state.directories[current_dir].parent_directory;
    char current_dirname[MAX_FILENAME];
    strcpy(current_dirname, fs_state.directories[current_dir].dirname);

    // Now lock only for the actual directory change
    pthread_mutex_lock(&mutex);

    // Handle special cases
    if (strcmp(dirname, ".") == 0)
    {
        pthread_mutex_unlock(&mutex);
        return;
    }

    if (strcmp(dirname, "..") == 0)
    {
        if (parent_dir != -1)
        {
            fs_state.current_directory = parent_dir;
            printf(COLOR_GREEN "Changed to parent directory\n" COLOR_RESET);
        }
        else
        {
            printf(COLOR_RED "Already at root directory\n" COLOR_RESET);
        }
        pthread_mutex_unlock(&mutex);
        return;
    }

    // Handle absolute paths
    if (dirname[0] == '/')
    {
        int target_dir = 0; // Start at root
        char *path = dirname + 1;
        char *token = strtok(path, "/");

        while (token != NULL)
        {
            int found = -1;
            for (int i = 0; i < MAX_DIRECTORIES; i++)
            {
                if (strcmp(fs_state.directories[i].dirname, token) == 0 &&
                    fs_state.directories[i].parent_directory == target_dir)
                {
                    found = i;
                    break;
                }
            }

            if (found == -1)
            {
                printf(COLOR_RED "Directory not found: %s\n" COLOR_RESET, token);
                pthread_mutex_unlock(&mutex);
                return;
            }
            target_dir = found;
            token = strtok(NULL, "/");
        }

        fs_state.current_directory = target_dir;
        pthread_mutex_unlock(&mutex);
        return;
    }

    // Handle relative paths
    for (int i = 0; i < MAX_DIRECTORIES; i++)
    {
        if (strcmp(fs_state.directories[i].dirname, dirname) == 0 &&
            fs_state.directories[i].parent_directory == current_dir)
        {
            fs_state.current_directory = i;
            pthread_mutex_unlock(&mutex);
            return;
        }
    }

    printf(COLOR_RED "Directory not found: %s\n" COLOR_RESET, dirname);
    pthread_mutex_unlock(&mutex);
}

void copy_file_to_dir(const char *filename, const char *dirname)
{
    pthread_mutex_lock(&mutex);

    // Find source file
    File *src_file = NULL;
    for (int i = 0; i < fs_state.directories[fs_state.current_directory].file_count; i++)
    {
        if (strcmp(fs_state.directories[fs_state.current_directory].files[i].filename, filename) == 0)
        {
            src_file = &fs_state.directories[fs_state.current_directory].files[i];
            break;
        }
    }

    if (!src_file)
    {
        printf(COLOR_RED "Error: File '%s' not found\n" COLOR_RESET, filename);
        pthread_mutex_unlock(&mutex);
        return;
    }

    // Find target directory
    int target_dir = -1;
    for (int i = 0; i < MAX_DIRECTORIES; i++)
    {
        if (strcmp(fs_state.directories[i].dirname, dirname) == 0)
        {
            target_dir = i;
            break;
        }
    }

    if (target_dir == -1)
    {
        printf(COLOR_RED "Error: Directory '%s' not found\n" COLOR_RESET, dirname);
        pthread_mutex_unlock(&mutex);
        return;
    }

    // Check if file exists in target
    for (int i = 0; i < fs_state.directories[target_dir].file_count; i++)
    {
        if (strcmp(fs_state.directories[target_dir].files[i].filename, filename) == 0)
        {
            printf(COLOR_RED "Error: File already exists in target directory\n" COLOR_RESET);
            pthread_mutex_unlock(&mutex);
            return;
        }
    }

    // Check space in target
    if (fs_state.directories[target_dir].file_count >= MAX_FILES)
    {
        printf(COLOR_RED "Error: Target directory full\n" COLOR_RESET);
        pthread_mutex_unlock(&mutex);
        return;
    }

    // Allocate new blocks for the copy
    int new_start_block = allocate_blocks(src_file->blocks_used);
    if (new_start_block == -1)
    {
        printf(COLOR_RED "Error: Not enough space for copy\n" COLOR_RESET);
        pthread_mutex_unlock(&mutex);
        return;
    }

    // Create the copy
    File new_file = *src_file;
    new_file.start_block = new_start_block;
    new_file.creation_time = time(NULL);
    new_file.modification_time = new_file.creation_time;

    fs_state.directories[target_dir].files[fs_state.directories[target_dir].file_count++] = new_file;

    save_state();
    printf(COLOR_GREEN "Copied '%s' to directory '%s'\n" COLOR_RESET, filename, dirname);
    pthread_mutex_unlock(&mutex);
}

void move_file_to_dir(const char *filename, const char *dirname)
{
    pthread_mutex_lock(&mutex);

    // Find source file index
    int file_index = -1;
    for (int i = 0; i < fs_state.directories[fs_state.current_directory].file_count; i++)
    {
        if (strcmp(fs_state.directories[fs_state.current_directory].files[i].filename, filename) == 0)
        {
            file_index = i;
            break;
        }
    }

    if (file_index == -1)
    {
        printf(COLOR_RED "Error: File '%s' not found\n" COLOR_RESET, filename);
        pthread_mutex_unlock(&mutex);
        return;
    }

    // Find target directory
    int target_dir = -1;
    for (int i = 0; i < MAX_DIRECTORIES; i++)
    {
        if (strcmp(fs_state.directories[i].dirname, dirname) == 0)
        {
            target_dir = i;
            break;
        }
    }

    if (target_dir == -1)
    {
        printf(COLOR_RED "Error: Directory '%s' not found\n" COLOR_RESET, dirname);
        pthread_mutex_unlock(&mutex);
        return;
    }

    // Check if target has space
    if (fs_state.directories[target_dir].file_count >= MAX_FILES)
    {
        printf(COLOR_RED "Error: Target directory full\n" COLOR_RESET);
        pthread_mutex_unlock(&mutex);
        return;
    }

    // Check if file exists in target
    for (int i = 0; i < fs_state.directories[target_dir].file_count; i++)
    {
        if (strcmp(fs_state.directories[target_dir].files[i].filename, filename) == 0)
        {
            printf(COLOR_RED "Error: File already exists in target directory\n" COLOR_RESET);
            pthread_mutex_unlock(&mutex);
            return;
        }
    }

    // Perform the move
    File file_to_move = fs_state.directories[fs_state.current_directory].files[file_index];
    fs_state.directories[target_dir].files[fs_state.directories[target_dir].file_count++] = file_to_move;

    // Remove from source
    for (int i = file_index; i < fs_state.directories[fs_state.current_directory].file_count - 1; i++)
    {
        fs_state.directories[fs_state.current_directory].files[i] =
            fs_state.directories[fs_state.current_directory].files[i + 1];
    }
    fs_state.directories[fs_state.current_directory].file_count--;

    save_state();
    printf(COLOR_GREEN "Moved '%s' to directory '%s'\n" COLOR_RESET, filename, dirname);
    pthread_mutex_unlock(&mutex);
}

void create_hard_link(const char *source, const char *link)
{
    pthread_mutex_lock(&mutex);

    // Find source file
    File *src_file = NULL;
    for (int i = 0; i < fs_state.directories[fs_state.current_directory].file_count; i++)
    {
        if (strcmp(fs_state.directories[fs_state.current_directory].files[i].filename, source) == 0)
        {
            src_file = &fs_state.directories[fs_state.current_directory].files[i];
            break;
        }
    }

    if (!src_file)
    {
        printf(COLOR_RED "Error: Source file '%s' not found\n" COLOR_RESET, source);
        pthread_mutex_unlock(&mutex);
        return;
    }

    // Check if link name already exists
    for (int i = 0; i < fs_state.directories[fs_state.current_directory].file_count; i++)
    {
        if (strcmp(fs_state.directories[fs_state.current_directory].files[i].filename, link) == 0)
        {
            printf(COLOR_RED "Error: Link name '%s' already exists\n" COLOR_RESET, link);
            pthread_mutex_unlock(&mutex);
            return;
        }
    }

    // Create the hard link (just another file entry pointing to same blocks)
    if (fs_state.directories[fs_state.current_directory].file_count >= MAX_FILES)
    {
        printf(COLOR_RED "Error: Directory full\n" COLOR_RESET);
        pthread_mutex_unlock(&mutex);
        return;
    }

    File new_link = *src_file;
    strcpy(new_link.filename, link);
    new_link.creation_time = time(NULL);

    fs_state.directories[fs_state.current_directory].files[fs_state.directories[fs_state.current_directory].file_count++] = new_link;

    save_state();
    printf(COLOR_GREEN "Created hard link '%s' -> '%s'\n" COLOR_RESET, link, source);
    pthread_mutex_unlock(&mutex);
}

void create_symbolic_link(const char *source, const char *link)
{
    pthread_mutex_lock(&mutex);

    // Check if source exists (we won't actually verify it points to anything valid)
    int source_exists = 0;
    for (int i = 0; i < fs_state.directories[fs_state.current_directory].file_count; i++)
    {
        if (strcmp(fs_state.directories[fs_state.current_directory].files[i].filename, source) == 0)
        {
            source_exists = 1;
            break;
        }
    }

    if (!source_exists)
    {
        printf(COLOR_RED "Warning: Source file '%s' not found (creating dangling symlink)\n" COLOR_RESET, source);
    }

    // Check if link name already exists
    for (int i = 0; i < fs_state.directories[fs_state.current_directory].file_count; i++)
    {
        if (strcmp(fs_state.directories[fs_state.current_directory].files[i].filename, link) == 0)
        {
            printf(COLOR_RED "Error: Link name '%s' already exists\n" COLOR_RESET, link);
            pthread_mutex_unlock(&mutex);
            return;
        }
    }

    // Create the symbolic link (special file that just contains the path)
    if (fs_state.directories[fs_state.current_directory].file_count >= MAX_FILES)
    {
        printf(COLOR_RED "Error: Directory full\n" COLOR_RESET);
        pthread_mutex_unlock(&mutex);
        return;
    }

    File symlink;
    strcpy(symlink.filename, link);
    symlink.size = strlen(source);
    symlink.blocks_used = 1;
    symlink.start_block = allocate_blocks(1);
    if (symlink.start_block == -1)
    {
        printf(COLOR_RED "Error: Not enough space for symlink\n" COLOR_RESET);
        pthread_mutex_unlock(&mutex);
        return;
    }
    strcpy(symlink.owner, fs_state.users[0].username); // Use current user
    symlink.permissions = 0777;                        // Common permission for symlinks
    symlink.creation_time = time(NULL);
    symlink.modification_time = symlink.creation_time;

    fs_state.directories[fs_state.current_directory].files[fs_state.directories[fs_state.current_directory].file_count++] = symlink;

    save_state();
    printf(COLOR_GREEN "Created symbolic link '%s' -> '%s'\n" COLOR_RESET, link, source);
    pthread_mutex_unlock(&mutex);
}

void help()
{
    printf("\nAvailable commands:\n");
    printf("-------------------\n");

    // File operations
    printf("File Operations:\n");
    printf("  create <filename> <size> <permissions> - Create new file (permissions in octal)\n");
    printf("  delete <filename>                      - Delete a file\n");
    printf("  delete -d <dirname>                   - Delete an empty directory\n");
    printf("  write [-a] <file> <data>              - Write data to file (-a for append)\n");
    printf("  read <file> [bytes]                   - Read content from file (optional byte count)\n");
    printf("  seek <file> <offset> <SET|CUR|END>    - Move read position in file\n");
    printf("  chmod <mode> <file>                   - Change file permissions\n");
    printf("  stat <file>                           - Show file information\n");

    // Directory operations
    printf("\nDirectory Operations:\n");
    printf("  create -d <dirname>                   - Create new directory\n");
    printf("  list                                  - List directory contents\n");
    printf("  pwd                                   - Print current directory path\n");
    printf("  copy <file> <directory>               - Copy file to another directory\n");
    printf("  move <file> <directory>               - Move file to another directory\n");

    printf("\Changing Directory:\n");
    printf("  cd <dirname>       - Change directory (default: /home)\n");
    printf("  cd /               - Go to root directory\n");
    printf("  cd ..              - Go to parent directory\n");
    printf("  cd dir             - Go to subdirectory 'dir'\n");
    printf("  cd /path/to/dir    - Go to absolute path\n");

    // Link operations
    printf("\nLink Operations:\n");
    printf("  ln <source> <link>                    - Create hard link\n");
    printf("  ln -s <source> <link>                 - Create symbolic link\n");

    // System operations
    printf("\nSystem Operations:\n");
    printf("  help                                  - Show this help message\n");
    printf("  quit                                  - Exit the terminal\n");

    printf("\nSeek Position Examples:\n");
    printf("  seek file.txt 10 SET    - Move to position 10 from start\n");
    printf("  seek file.txt 5 CUR     - Move 5 bytes forward\n");
    printf("  seek file.txt -3 END    - Move 3 bytes before end\n");

    printf("\nPermission Examples:\n");
    printf("  644 - Owner: read/write, Others: read\n");
    printf("  755 - Owner: all, Others: read/execute\n");
    printf("  600 - Owner: read/write, Others: none\n");

    printf("\n");
}

void execute_job(Job job)
{
    char command[256];
    strcpy(command, job.command);

    if (strncmp(command, "create", 6) == 0)
    {
        if (strstr(command, "-d") != NULL)
        {
            char dirname[MAX_FILENAME];
            if (sscanf(command, "create -d %s", dirname) == 1)
            {
                create_directory(dirname);
            }
            else
            {
                printf(COLOR_RED "Usage: create -d <dirname>\n" COLOR_RESET);
            }
        }
        else
        {
            char filename[MAX_FILENAME];
            int size, permissions;
            if (sscanf(command, "create %s %d %o", filename, &size, &permissions) == 3)
            {
                create_file(filename, size, fs_state.users[0].username, permissions);
            }
            else
            {
                printf(COLOR_RED "Usage: create <filename> <size> <permissions>\n" COLOR_RESET);
                printf(COLOR_RED "Example: create myfile.txt 1024 644\n" COLOR_RESET);
            }
        }
    }
    else if (strcmp(command, "list") == 0)
    {
        list_files();
    }
    else if (strcmp(command, "pwd") == 0)
    {
        printf("%s\n", get_current_working_directory());
    }
    else if (strncmp(command, "seek", 4) == 0)
    {
        char filename[MAX_FILENAME];
        int offset;
        char whence_str[10];

        if (sscanf(command, "seek %s %d %s", filename, &offset, whence_str) == 3)
        {
            int whence;
            if (strcmp(whence_str, "SET") == 0)
                whence = SEEK_SET;
            else if (strcmp(whence_str, "CUR") == 0)
                whence = SEEK_CUR;
            else if (strcmp(whence_str, "END") == 0)
                whence = SEEK_END;
            else
            {
                printf("Invalid whence. Use SET, CUR or END\n");
                return;
            }

            // Find the file
            File *file = NULL;
            for (int i = 0; i < fs_state.directories[fs_state.current_directory].file_count; i++)
            {
                if (strcmp(fs_state.directories[fs_state.current_directory].files[i].filename, filename) == 0)
                {
                    file = &fs_state.directories[fs_state.current_directory].files[i];
                    break;
                }
            }

            if (file)
            {
                int new_pos = file_seek(file, offset, whence);
                if (new_pos != -1)
                {
                    printf("Position set to %d in file '%s'\n", new_pos, filename);
                }
                else
                {
                    printf("Invalid seek position\n");
                }
            }
            else
            {
                printf("File not found\n");
            }
        }
        else
        {
            printf("Usage: seek <filename> <offset> <SET|CUR|END>\n");
        }
    }
    else if (strncmp(command, "cd", 2) == 0)
    {
        char dirname[MAX_FILENAME];
        if (sscanf(command, "cd %s", dirname) == 1)
        {
            change_directory(dirname);
        }
        else
        {
            // If no argument given, go to home directory
            change_directory("/root");
        }
    }
    else if (strcmp(command, "help") == 0)
    {
        help();
    }
    else if (strncmp(command, "write", 5) == 0)
    {
        char filename[MAX_FILENAME], data[256];
        int append = 0;

        // Check for append flag
        if (strstr(command, "-a") != NULL)
        {
            if (sscanf(command, "write -a %s %[^\n]", filename, data) == 2)
            {
                append = 1;
            }
            else
            {
                printf(COLOR_RED "Usage: write [-a] <filename> <data>\n" COLOR_RESET);
                free(job.command);
                return;
            }
        }
        else
        {
            if (sscanf(command, "write %s %[^\n]", filename, data) != 2)
            {
                printf(COLOR_RED "Usage: write [-a] <filename> <data>\n" COLOR_RESET);
                free(job.command);
                return;
            }
        }

        write_to_file(filename, data, append);
    }
    else if (strncmp(command, "read", 4) == 0)
    {
        char filename[MAX_FILENAME];
        int bytes_to_read = -1; // Default: read to end of file

        // Parse either "read <filename>" or "read <filename> <bytes>"
        if (sscanf(command, "read %s %d", filename, &bytes_to_read) >= 1)
        {
            char *content = read_from_file(filename, bytes_to_read);
            if (content)
            {
                printf("File content [%d bytes]: %s\n", (int)strlen(content), content);
                free(content);

                // Show current position after read
                pthread_mutex_lock(&mutex);
                for (int i = 0; i < fs_state.directories[fs_state.current_directory].file_count; i++)
                {
                    if (strcmp(fs_state.directories[fs_state.current_directory].files[i].filename, filename) == 0)
                    {
                        printf("Current position: %d/%d\n",
                               fs_state.directories[fs_state.current_directory].files[i].file_position,
                               fs_state.directories[fs_state.current_directory].files[i].content_size);
                        printf(COLOR_YELLOW "To change position use seek command. \n" COLOR_YELLOW);

                        break;
                    }
                }
                pthread_mutex_unlock(&mutex);
            }
        }
        else
        {
            printf(COLOR_RED "Usage: read <filename> [bytes]\n" COLOR_RESET);
            printf(COLOR_RED "Examples:\n" COLOR_RESET);
            printf(COLOR_RED "  read file.txt       - Read entire file\n" COLOR_RESET);
            printf(COLOR_RED "  read file.txt 10    - Read next 10 bytes\n" COLOR_RESET);
            printf(COLOR_RED "  Use 'seek' command to change position first\n" COLOR_RESET);
        }
    }
    else if (strncmp(command, "delete", 6) == 0)
    {
        if (strstr(command, "-d") != NULL)
        {
            char dirname[MAX_FILENAME];
            if (sscanf(command, "delete -d %s", dirname) == 1)
            {
                delete_directory(dirname);
            }
            else
            {
                printf(COLOR_RED "Usage: delete -d <dirname>\n" COLOR_RESET);
            }
        }
        else
        {
            char filename[MAX_FILENAME];
            if (sscanf(command, "delete %s", filename) == 1)
            {
                delete_file(filename);
            }
            else
            {
                printf(COLOR_RED "Usage: delete <filename> OR delete -d <dirname>\n" COLOR_RESET);
            }
        }
    }
    else if (strncmp(command, "copy", 4) == 0)
    {
        char filename[MAX_FILENAME], dirname[MAX_FILENAME];
        if (sscanf(command, "copy %s %s", filename, dirname) == 2)
        {
            copy_file_to_dir(filename, dirname);
        }
        else
        {
            printf(COLOR_RED "Usage: copy <filename> <directory>\n" COLOR_RESET);
        }
    }
    else if (strncmp(command, "move", 4) == 0)
    {
        char filename[MAX_FILENAME], dirname[MAX_FILENAME];
        if (sscanf(command, "move %s %s", filename, dirname) == 2)
        {
            move_file_to_dir(filename, dirname);
        }
        else
        {
            printf(COLOR_RED "Usage: move <filename> <directory>\n" COLOR_RESET);
        }
    }
    else if (strncmp(command, "chmod", 5) == 0)
    {
        char filename[MAX_FILENAME];
        int mode;
        if (sscanf(command, "chmod %o %s", &mode, filename) == 2)
        {
            change_permissions(filename, mode);
        }
        else
        {
            printf(COLOR_RED "Usage: chmod <mode> <filename>\n" COLOR_RESET);
            printf(COLOR_RED "Example: chmod 755 script.sh\n" COLOR_RESET);
        }
    }
    else if (strncmp(command, "stat", 4) == 0)
    {
        char filename[MAX_FILENAME];
        if (sscanf(command, "stat %s", filename) == 1)
        {
            print_file_info(filename);
        }
        else
        {
            printf(COLOR_RED "Usage: stat <filename>\n" COLOR_RESET);
        }
    }
    // Add this to your execute_job function
    else if (strncmp(command, "showpages", 9) == 0)
    {
        char filename[MAX_FILENAME];
        if (sscanf(command, "showpages %s", filename) == 1)
        {
            print_page_table(filename);
        }
        else
        {
            print_page_bitmap();
        }
    }
    else if (strncmp(command, "ln", 2) == 0)
    {
        if (strstr(command, "-s") != NULL)
        {
            char source[MAX_FILENAME], link[MAX_FILENAME];
            if (sscanf(command, "ln -s %s %s", source, link) == 2)
            {
                create_symbolic_link(source, link);
            }
            else
            {
                printf(COLOR_RED "Usage: ln -s <source> <link>\n" COLOR_RESET);
            }
        }
        else
        {
            char source[MAX_FILENAME], link[MAX_FILENAME];
            if (sscanf(command, "ln %s %s", source, link) == 2)
            {
                create_hard_link(source, link);
            }
            else
            {
                printf(COLOR_RED "Usage: ln <source> <link>\n" COLOR_RESET);
            }
        }
    }
    else if (strcmp(command, "quit") == 0)
    {
        handle_signal(SIGINT);
    }
    else
    {
        // Handle external commands
        pid_t pid = fork();
        if (pid == 0)
        {
            char *args[] = {"/bin/sh", "-c", command, NULL};
            execvp(args[0], args);
            perror("execvp failed");
            exit(1);
        }
        else if (pid < 0)
        {
            perror("fork failed");
        }
        else
        {
            // Parent waits for child to complete
            waitpid(pid, NULL, 0);
        }
    }
    free(job.command);
}

void print_queue(int current_job_index)
{
    printf(COLOR_YELLOW "Command Queue:\n" COLOR_RESET);
    if (job_count == 0)
    {
        printf(COLOR_YELLOW "  [Empty]\n" COLOR_RESET);
        return;
    }

    for (int i = 0; i < job_count; i++)
    {
        int idx = (front + i) % MAX_JOBS;
        if (i == current_job_index)
        {
            printf(COLOR_YELLOW "  > %s (Running)\n" COLOR_RESET, job_queue[idx].command);
        }
        else
        {
            printf(COLOR_YELLOW "  - %s\n" COLOR_RESET, job_queue[idx].command);
        }
    }
}

void *scheduler(void *arg)
{
    while (running)
    {
        pthread_mutex_lock(&queue_lock);
        while (job_count == 0 && running)
        {
            pthread_cond_wait(&job_available, &queue_lock);
        }
        if (!running)
        {
            pthread_mutex_unlock(&queue_lock);
            break;
        }

        print_queue(0);

        Job job = job_queue[front];
        front = (front + 1) % MAX_JOBS;
        job_count--;
        pthread_mutex_unlock(&queue_lock);

        execute_job(job);
    }
    return NULL;
}

void add_job(const char *command)
{
    pthread_mutex_lock(&queue_lock);
    if (job_count < MAX_JOBS)
    {
        job_queue[rear].command = strdup(command);
        rear = (rear + 1) % MAX_JOBS;
        job_count++;
        pthread_cond_signal(&job_available);
    }
    else
    {
        printf(COLOR_RED "Job queue is full!\n" COLOR_RESET);
    }
    pthread_mutex_unlock(&queue_lock);
}

void cleanup()
{
    pthread_mutex_lock(&queue_lock);
    running = 0;
    pthread_cond_signal(&job_available);
    while (job_count > 0)
    {
        free(job_queue[front].command);
        front = (front + 1) % MAX_JOBS;
        job_count--;
    }
    pthread_mutex_unlock(&queue_lock);
}

void handle_signal(int sig)
{
    printf("\nShutting down...\n");
    cleanup();
    exit(0);
}

int login()
{
    char username[20], password[20];
    printf("Username: ");
    fgets(username, sizeof(username), stdin);
    username[strcspn(username, "\n")] = 0;

    printf("Password: ");
    fgets(password, sizeof(password), stdin);
    password[strcspn(password, "\n")] = 0;

    for (int i = 0; i < MAX_USERS; i++)
    {
        if (strcmp(fs_state.users[i].username, username) == 0 &&
            strcmp(fs_state.users[i].password, password) == 0)
        {
            return i;
        }
    }
    return -1;
}

int main()
{
    signal(SIGINT, handle_signal);
    pthread_t scheduler_thread;
    pthread_create(&scheduler_thread, NULL, scheduler, NULL);

    load_state(); // Load previous state or initialize

    int user_index = login();
    if (user_index == -1)
    {
        printf(COLOR_RED "Login failed\n" COLOR_RESET);
        return 1;
    }

    printf(COLOR_GREEN "\nWelcome to the Mini UNIX-like File System!\n" COLOR_RESET);
    printf("Type 'help' for a list of commands\n\n");

    char input[256];
    while (1)
    {
        printf(COLOR_BLUE "%s@%s> " COLOR_RESET,
               fs_state.users[user_index].username,
               fs_state.directories[fs_state.current_directory].dirname);
        fflush(stdout);

        if (!fgets(input, sizeof(input), stdin))
        {
            break;
        }

        input[strcspn(input, "\n")] = 0;

        if (strcmp(input, "quit") == 0)
        {
            handle_signal(SIGINT);
        }
        else if (strchr(input, '|') != NULL)
        {
            // Handle piped commands
            char *token = strtok(input, "|");
            while (token != NULL)
            {
                while (*token == ' ')
                    token++;
                char *end = token + strlen(token) - 1;
                while (end > token && *end == ' ')
                    end--;
                *(end + 1) = '\0';
                add_job(token);
                token = strtok(NULL, "|");
            }
        }
        else
        {
            // For single commands, execute immediately without queue
            Job job;
            job.command = strdup(input);
            execute_job(job);
        }
    }
    return 0;
}
