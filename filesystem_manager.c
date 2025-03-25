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
int create_file(char *filename, int size, char *owner, int permissions);
int create_directory(char *dirname);
void delete_file(char *filename);
void list_files();
int write_to_file(const char *filename, const char *data, int append);
char *read_from_file(const char *filename, int bytes_to_read, int offset);
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
    // Clear the entire filesystem state first (avoid garbage data)
    memset(&fs_state, 0, sizeof(fs_state));
    memset(page_bitmap, 0, sizeof(page_bitmap));

    // Initialize root directory (ID 0)
    strcpy(fs_state.directories[0].dirname, "root");
    fs_state.directories[0].file_count = 0;
    fs_state.directories[0].parent_directory = -1; // Root has no parent
    fs_state.directories[0].creation_time = time(NULL);

    // Create a default home directory (ID 1)
    strcpy(fs_state.directories[1].dirname, "home");
    fs_state.directories[1].file_count = 0;
    fs_state.directories[1].parent_directory = 0; // Parent is root
    fs_state.directories[1].creation_time = time(NULL);

    // Set current directory to root
    fs_state.current_directory = 0;

    // Create default users
    strcpy(fs_state.users[0].username, "ikram");
    strcpy(fs_state.users[0].password, "ikrampass");
    strcpy(fs_state.users[1].username, "ines");
    strcpy(fs_state.users[1].password, "inespass");
    strcpy(fs_state.users[2].username, "ali");
    strcpy(fs_state.users[2].password, "alipass");

    // Create default files (readme.txt, notes.txt)
    PageTableEntry *file1_pages = malloc(sizeof(PageTableEntry));
    PageTableEntry *file2_pages = malloc(sizeof(PageTableEntry));

    if (!file1_pages || !file2_pages)
    {
        free(file1_pages);
        free(file2_pages);
        return;
    }

    // Allocate pages (physical page numbers 0 and 1)
    file1_pages[0].physical_page = 0;
    file1_pages[0].is_allocated = 1;
    file2_pages[0].physical_page = 1;
    file2_pages[0].is_allocated = 1;

    // Mark pages as used in bitmap
    page_bitmap[0 / 8] |= (1 << (0 % 8)); // Page 0
    page_bitmap[1 / 8] |= (1 << (1 % 8)); // Page 1

    // Create default files
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
        .page_table_size = 1};

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
        .page_table_size = 1};

    // Add files to root directory
    fs_state.directories[0].files[fs_state.directories[0].file_count++] = file1;
    fs_state.directories[0].files[fs_state.directories[0].file_count++] = file2;

    // Save the initial state
    save_state();
}

void save_state()
{
    FILE *fp = fopen(STORAGE_FILE, "wb");
    if (fp)
    {
        // Save main structure without dynamic content
        FileSystemState temp_state = fs_state;
        for (int i = 0; i < MAX_DIRECTORIES; i++)
        {
            for (int j = 0; j < temp_state.directories[i].file_count; j++)
            {
                temp_state.directories[i].files[j].content = NULL;
                temp_state.directories[i].files[j].page_table = NULL;
            }
        }
        fwrite(&temp_state, sizeof(temp_state), 1, fp);

        // Save page bitmap
        fwrite(page_bitmap, sizeof(page_bitmap), 1, fp);

        // Save file contents and page tables
        for (int i = 0; i < MAX_DIRECTORIES; i++)
        {
            for (int j = 0; j < fs_state.directories[i].file_count; j++)
            {
                File *file = &fs_state.directories[i].files[j];

                // Save content size and content
                fwrite(&file->content_size, sizeof(size_t), 1, fp);
                if (file->content && file->content_size > 0)
                {
                    fwrite(file->content, 1, file->content_size, fp);
                }

                // Save page table
                fwrite(&file->page_table_size, sizeof(int), 1, fp);
                if (file->page_table && file->page_table_size > 0)
                {
                    fwrite(file->page_table, sizeof(PageTableEntry), file->page_table_size, fp);
                }
            }
        }
        fclose(fp);
    }
}

void load_state()
{
    printf("Attempting to load state...\n");
    FILE *fp = fopen(STORAGE_FILE, "rb");
    if (fp)
    {
        // Load main structure
        printf("Found existing filesystem.dat\n");
        if (fread(&fs_state, sizeof(fs_state), 1, fp) != 1)
        {
            printf("Error reading filesystem state, initializing new one\n");
            fclose(fp);
            initialize_directories();
            return;
        }

        // Load page bitmap
        if (fread(page_bitmap, sizeof(page_bitmap), 1, fp) != 1)
        {
            fclose(fp);
            initialize_directories();
            return;
        }

        // Load file contents and page tables
        for (int i = 0; i < MAX_DIRECTORIES; i++)
        {
            for (int j = 0; j < fs_state.directories[i].file_count; j++)
            {
                File *file = &fs_state.directories[i].files[j];

                // Load content
                if (fread(&file->content_size, sizeof(size_t), 1, fp) != 1)
                {
                    file->content = NULL;
                    file->content_size = 0;
                    continue;
                }

                if (file->content_size > 0)
                {
                    file->content = malloc(file->content_size);
                    if (!file->content)
                    {
                        printf("Error allocating memory for file content\n");
                        file->content_size = 0;
                        continue;
                    }
                    if (fread(file->content, 1, file->content_size, fp) != file->content_size)
                    {
                        free(file->content);
                        file->content = NULL;
                        file->content_size = 0;
                    }
                }

                // Load page table
                if (fread(&file->page_table_size, sizeof(int), 1, fp) != 1)
                {
                    file->page_table = NULL;
                    file->page_table_size = 0;
                    continue;
                }

                if (file->page_table_size > 0)
                {
                    file->page_table = malloc(file->page_table_size * sizeof(PageTableEntry));
                    if (!file->page_table)
                    {
                        file->page_table_size = 0;
                        continue;
                    }
                    if (fread(file->page_table, sizeof(PageTableEntry), file->page_table_size, fp) != file->page_table_size)
                    {
                        free(file->page_table);
                        file->page_table = NULL;
                        file->page_table_size = 0;
                    }
                }
            }
        }
        fclose(fp);
    }
    else
    {
        printf("No existing filesystem found, initializing new one\n");
        initialize_directories();
    }
}

int open_file(const char *filename)
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
        printf("Error: File not found\n");
        pthread_mutex_unlock(&mutex);
        return -1;
    }

    file->is_open = 1;
    file->open_count++;
    printf("File '%s' opened (count: %d)\n", filename, file->open_count);
    pthread_mutex_unlock(&mutex);
    return 0;
}

int close_file(const char *filename)
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
        printf("Error: File not found\n");
        pthread_mutex_unlock(&mutex);
        return -1;
    }

    if (file->open_count > 0)
    {
        file->open_count--;
        if (file->open_count == 0)
        {
            file->is_open = 0;
        }
        printf("File '%s' closed (count: %d)\n", filename, file->open_count);
    }
    else
    {
        printf("Error: File not open\n");
    }
    pthread_mutex_unlock(&mutex);
    return 0;
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

// Initialize paging system
void initialize_paging()
{
    memset(page_bitmap, 0, sizeof(page_bitmap));
}

// Allocate pages for a file
int allocate_pages(int pages_needed, PageTableEntry **page_table)
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

// Free pages for a file
void free_pages(File *file)
{
    for (int i = 0; i < file->page_table_size; i++)
    {
        int page_num = file->page_table[i].physical_page;
        page_bitmap[page_num / 8] &= ~(1 << (page_num % 8));
    }
    free(file->page_table);
}

int create_file(char *filename, int size, char *owner, int permissions)
{
    pthread_mutex_lock(&mutex);

    // Check if file exists
    for (int i = 0; i < fs_state.directories[fs_state.current_directory].file_count; i++)
    {
        if (strcmp(fs_state.directories[fs_state.current_directory].files[i].filename, filename) == 0)
        {
            printf("Error: File already exists\n");
            pthread_mutex_unlock(&mutex);
            return -1;
        }
    }

    // Validate size
    if (size <= 0)
    {
        printf("Error: Invalid file size\n");
        pthread_mutex_unlock(&mutex);
        return -1;
    }

    // Calculate needed pages (round up)
    int pages_needed = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    if (pages_needed <= 0)
        pages_needed = 1; // Minimum 1 page

    printf("Creating file of size %d, requiring %d pages\n", size, pages_needed); // Debug output

    // Allocate page table
    PageTableEntry *page_table = malloc(pages_needed * sizeof(PageTableEntry));
    if (!page_table)
    {
        printf("Error: Could not allocate page table\n");
        pthread_mutex_unlock(&mutex);
        return -1;
    }

    // Initialize page table
    for (int i = 0; i < pages_needed; i++)
    {
        page_table[i].physical_page = -1; // Mark as unallocated initially
        page_table[i].is_allocated = 0;
    }

    // Create the file structure
    File new_file;
    strncpy(new_file.filename, filename, MAX_FILENAME - 1);
    new_file.filename[MAX_FILENAME - 1] = '\0';
    new_file.size = size;
    strncpy(new_file.owner, owner, 19);
    new_file.owner[19] = '\0';
    new_file.permissions = permissions;
    new_file.creation_time = time(NULL);
    new_file.modification_time = new_file.creation_time;
    new_file.content_size = strlen("HELLO WORLD") + 1;
    new_file.content = strdup("HELLO WORLD");
    new_file.file_position = 0;
    new_file.page_table = page_table;
    new_file.page_table_size = pages_needed;

    // Try to allocate physical pages
    int allocation_success = 1;
    for (int i = 0; i < pages_needed; i++)
    {
        // Find a free page
        int page_found = 0;
        for (int j = 0; j < TOTAL_PAGES; j++)
        {
            if (!(page_bitmap[j / 8] & (1 << (j % 8))))
            {
                // Mark page as used
                page_bitmap[j / 8] |= (1 << (j % 8));
                page_table[i].physical_page = j;
                page_table[i].is_allocated = 1;
                page_found = 1;
                break;
            }
        }

        if (!page_found)
        {
            allocation_success = 0;
            break;
        }
    }

    if (!allocation_success)
    {
        // Clean up any allocated pages
        for (int i = 0; i < pages_needed; i++)
        {
            if (page_table[i].is_allocated)
            {
                int page_num = page_table[i].physical_page;
                page_bitmap[page_num / 8] &= ~(1 << (page_num % 8));
            }
        }
        free(page_table);
        printf("Error: Not enough memory pages available\n");
        pthread_mutex_unlock(&mutex);
        return -1;
    }

    // Add file to directory
    if (fs_state.directories[fs_state.current_directory].file_count >= MAX_FILES)
    {
        // Clean up pages if directory is full
        for (int i = 0; i < pages_needed; i++)
        {
            int page_num = page_table[i].physical_page;
            page_bitmap[page_num / 8] &= ~(1 << (page_num % 8));
        }
        free(page_table);
        printf("Error: Directory is full\n");
        pthread_mutex_unlock(&mutex);
        return -1;
    }

    fs_state.directories[fs_state.current_directory].files[fs_state.directories[fs_state.current_directory].file_count++] = new_file;

    save_state();
    printf(COLOR_GREEN "File '%s' created successfully with %d pages\n" COLOR_RESET, filename, pages_needed);
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
            // Free pages first
            for (int j = 0; j < fs_state.directories[fs_state.current_directory].files[i].page_table_size; j++)
            {
                if (fs_state.directories[fs_state.current_directory].files[i].page_table[j].is_allocated)
                {
                    int page_num = fs_state.directories[fs_state.current_directory].files[i].page_table[j].physical_page;
                    page_bitmap[page_num / 8] &= ~(1 << (page_num % 8));
                }
            }

            // Free page table
            free(fs_state.directories[fs_state.current_directory].files[i].page_table);

            // Free content if exists
            if (fs_state.directories[fs_state.current_directory].files[i].content)
            {
                free(fs_state.directories[fs_state.current_directory].files[i].content);
            }

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
        printf("Error: File '%s' not found\n", filename);
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
        printf("Error: File not found\n");
        pthread_mutex_unlock(&mutex);
        return -1;
    }

    // Check write permissions
    if ((file->permissions & 0222) == 0)
    { // Check if any write bits are set
        printf(COLOR_RED "Error: Permission denied (no write permission)\n" COLOR_RESET);
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
            printf("Error: Memory allocation failed\n");
            pthread_mutex_unlock(&mutex);
            return -1;
        }
        strcat(new_content, data);
    }
    else
    {
        // Overwrite mode
        new_content_size = data_len;
        new_content = strdup(data);
        if (!new_content)
        {
            printf("Error: Memory allocation failed\n");
            pthread_mutex_unlock(&mutex);
            return -1;
        }
    }

    printf(COLOR_GREEN "Wrote new content into file successfully\n" COLOR_RESET);

    // Check if we need more pages
    int pages_needed = (new_content_size + PAGE_SIZE - 1) / PAGE_SIZE;
    if (pages_needed > file->page_table_size)
    {
        PageTableEntry *new_table = realloc(file->page_table, pages_needed * sizeof(PageTableEntry));
        if (!new_table)
        {
            printf("Error: Could not expand page table\n");
            free(new_content);
            pthread_mutex_unlock(&mutex);
            return -1;
        }

        // Allocate new pages
        for (int i = file->page_table_size; i < pages_needed; i++)
        {
            int page = -1;
            for (int j = 0; j < TOTAL_PAGES; j++)
            {
                if (!(page_bitmap[j / 8] & (1 << (j % 8))))
                {
                    page_bitmap[j / 8] |= (1 << (j % 8));
                    new_table[i].physical_page = j;
                    new_table[i].is_allocated = 1;
                    page = j;
                    break;
                }
            }
            if (page == -1)
            {
                // Cleanup
                for (int k = file->page_table_size; k < i; k++)
                {
                    page_bitmap[new_table[k].physical_page / 8] &= ~(1 << (new_table[k].physical_page % 8));
                }
                free(new_content);
                pthread_mutex_unlock(&mutex);
                return -1;
            }
        }

        file->page_table = new_table;
        file->page_table_size = pages_needed;
    }

    // Update file content
    if (file->content)
        ;
    file->content = new_content;
    file->content_size = new_content_size;
    file->size = new_content_size;
    file->modification_time = time(NULL);

    save_state();
    pthread_mutex_unlock(&mutex);
    return data_len;
}

char *read_from_file(const char *filename, int bytes_to_read, int offset)
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
        printf("Error: File not found\n");
        pthread_mutex_unlock(&mutex);
        return NULL;
    }

    // Check read permissions
    if ((file->permissions & 0444) == 0)
    { // Check if any read bits are set
        printf(COLOR_RED "Error: Permission denied (no read permission)\n" COLOR_RESET);
        pthread_mutex_unlock(&mutex);
        return NULL;
    }

    // Check if file has content
    if (!file->content || file->content_size <= 0)
    {
        printf("File is empty\n");
        pthread_mutex_unlock(&mutex);
        return strdup(""); // Return empty string
    }

    // Validate offset
    if (offset < 0)
    {
        offset = 0;
    }
    else if (offset > file->content_size)
    {
        offset = file->content_size;
    }

    // Calculate how many bytes we can actually read
    int remaining_bytes = file->content_size - offset;
    int read_bytes = (bytes_to_read <= 0) ? remaining_bytes : (bytes_to_read < remaining_bytes) ? bytes_to_read
                                                                                                : remaining_bytes;

    // Allocate buffer
    char *buffer = malloc(read_bytes + 1);
    if (!buffer)
    {
        printf("Error: Memory allocation failed\n");
        pthread_mutex_unlock(&mutex);
        return NULL;
    }

    // Copy data from specified offset
    memcpy(buffer, file->content + offset, read_bytes);
    buffer[read_bytes] = '\0';

    // Update modification time
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
            // Ensure we only change permission bits (last 9 bits)
            fs_state.directories[fs_state.current_directory].files[i].permissions = mode & 0777;
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
            printf("Owner: %s\n", f->owner);
            printf("Permissions: %04o ", f->permissions);

            // Show permission interpretation
            printf("(%c%c%c%c%c%c%c%c%c)\n",
                   (f->permissions & 0400) ? 'r' : '-',
                   (f->permissions & 0200) ? 'w' : '-',
                   (f->permissions & 0100) ? 'x' : '-',
                   (f->permissions & 0040) ? 'r' : '-',
                   (f->permissions & 0020) ? 'w' : '-',
                   (f->permissions & 0010) ? 'x' : '-',
                   (f->permissions & 0004) ? 'r' : '-',
                   (f->permissions & 0002) ? 'w' : '-',
                   (f->permissions & 0001) ? 'x' : '-');

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

    // Create the copy
    File new_file = *src_file;
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

void create_hard_link(const char *source, const char *link) {
    pthread_mutex_lock(&mutex);
    
    // 1. Find source file
    File *src_file = NULL;
    for (int i = 0; i < fs_state.directories[fs_state.current_directory].file_count; i++) {
        if (strcmp(fs_state.directories[fs_state.current_directory].files[i].filename, source) == 0) {
            src_file = &fs_state.directories[fs_state.current_directory].files[i];
            break;
        }
    }

    if (!src_file) {
        printf("Error: Source file '%s' not found\n", source);
        pthread_mutex_unlock(&mutex);
        return;
    }

    // 2. Create new link (SHARE the same content pointer)
    File new_link;
    memcpy(&new_link, src_file, sizeof(File)); // Copy all metadata
    strcpy(new_link.filename, link); // Only change the filename
    new_link.creation_time = time(NULL); // Update timestamp

    // 3. Add to directory (now both files point to the SAME content)
    if (fs_state.directories[fs_state.current_directory].file_count >= MAX_FILES) {
        printf("Error: Directory full\n");
        pthread_mutex_unlock(&mutex);
        return;
    }

    fs_state.directories[fs_state.current_directory].files[fs_state.directories[fs_state.current_directory].file_count++] = new_link;

    printf("Created hard link '%s' -> '%s'\n", link, source);
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

    strcpy(symlink.owner, fs_state.users[0].username); // Use current user
    symlink.permissions = 0777;                        // Common permission for symlinks
    symlink.creation_time = time(NULL);
    symlink.modification_time = symlink.creation_time;

    fs_state.directories[fs_state.current_directory].files[fs_state.directories[fs_state.current_directory].file_count++] = symlink;

    symlink.content = strdup(source);
    symlink.content_size = strlen(source) + 1;

    save_state();
    printf(COLOR_GREEN "Created symbolic link '%s' -> '%s'\n" COLOR_RESET, link, source);
    pthread_mutex_unlock(&mutex);
}

void format_filesystem()
{
    pthread_mutex_lock(&mutex);
    printf(COLOR_RED "WARNING: This will erase ALL data! Continue? [y/N] " COLOR_RESET);
    char response[10];
    fgets(response, sizeof(response), stdin);

    if (tolower(response[0]) == 'y')
    {
        // Wipe the storage file
        FILE *fp = fopen(STORAGE_FILE, "wb");
        if (fp)
            fclose(fp);

        // Reinitialize everything
        initialize_directories();
        initialize_paging();
        printf(COLOR_GREEN "File system formatted successfully\n" COLOR_RESET);
    }
    else
    {
        printf("Format cancelled\n");
    }
    pthread_mutex_unlock(&mutex);
}

void backup_filesystem(const char *backup_name)
{
    pthread_mutex_lock(&mutex);
    char backup_file[256];
    snprintf(backup_file, sizeof(backup_file), "%s.bak", backup_name);

    FILE *src = fopen(STORAGE_FILE, "rb");
    FILE *dst = fopen(backup_file, "wb");

    if (!src || !dst)
    {
        printf("Error creating backup\n");
        if (src)
            fclose(src);
        if (dst)
            fclose(dst);
        pthread_mutex_unlock(&mutex);
        return;
    }

    char buffer[4096];
    size_t bytes;
    while ((bytes = fread(buffer, 1, sizeof(buffer), src)))
    {
        fwrite(buffer, 1, bytes, dst);
    }

    fclose(src);
    fclose(dst);
    printf("Backup created: %s\n", backup_file);
    pthread_mutex_unlock(&mutex);
}

void restore_filesystem(const char *backup_name)
{
    pthread_mutex_lock(&mutex);
    char backup_file[256];
    snprintf(backup_file, sizeof(backup_file), "%s.bak", backup_name);

    printf(COLOR_RED "WARNING: This will overwrite current filesystem! Continue? [y/N] " COLOR_RESET);
    char response[10];
    fgets(response, sizeof(response), stdin);

    if (tolower(response[0]) != 'y')
    {
        printf("Restore cancelled\n");
        pthread_mutex_unlock(&mutex);
        return;
    }

    FILE *src = fopen(backup_file, "rb");
    FILE *dst = fopen(STORAGE_FILE, "wb");

    if (!src || !dst)
    {
        printf("Error restoring backup\n");
        if (src)
            fclose(src);
        if (dst)
            fclose(dst);
        pthread_mutex_unlock(&mutex);
        return;
    }

    char buffer[4096];
    size_t bytes;
    while ((bytes = fread(buffer, 1, sizeof(buffer), src)))
    {
        fwrite(buffer, 1, bytes, dst);
    }

    fclose(src);
    fclose(dst);

    // Reload the restored state
    load_state();
    printf("Filesystem restored from: %s\n", backup_file);
    pthread_mutex_unlock(&mutex);
}

void show_directory_info(const char *dirname) {
    pthread_mutex_lock(&mutex);
    
    // First gather all needed information while holding the lock
    int target_dir = fs_state.current_directory;
    char dirname_copy[MAX_FILENAME] = {0};
    char path[1024] = {0};
    
    if (dirname == NULL || strcmp(dirname, ".") == 0) {
        // Use current directory
        strncpy(dirname_copy, fs_state.directories[target_dir].dirname, MAX_FILENAME-1);
    } else {
        // Find the target directory
        int found = 0;
        for (int i = 0; i < MAX_DIRECTORIES; i++) {
            if (strcmp(fs_state.directories[i].dirname, dirname) == 0 &&
                fs_state.directories[i].parent_directory == fs_state.current_directory) {
                target_dir = i;
                strncpy(dirname_copy, dirname, MAX_FILENAME-1);
                found = 1;
                break;
            }
        }
        if (!found) {
            pthread_mutex_unlock(&mutex);
            printf("Directory not found\n");
            return;
        }
    }
    
    // Build path without calling pwd (to avoid deadlock)
    int current = target_dir;
    char *ptr = path + sizeof(path) - 1;
    *ptr = '\0';
    
    while (current != -1) {
        const char *name = fs_state.directories[current].dirname;
        size_t len = strlen(name);
        
        ptr -= len;
        memcpy(ptr, name, len);
        
        if (fs_state.directories[current].parent_directory != -1) {
            *--ptr = '/';
        }
        
        current = fs_state.directories[current].parent_directory;
    }
    
    Directory *dir = &fs_state.directories[target_dir];
    
    // Now we can release the lock before printing
    pthread_mutex_unlock(&mutex);
    
    // Print the information
    printf("\nDirectory: %s\n", dirname_copy);
    printf("Path: %s\n", ptr);
    printf("Files: %d\n", dir->file_count);
    printf("Created: %s", ctime(&dir->creation_time));
}


void help()
{
    printf("\nAvailable commands:\n");
    printf("-------------------\n");

    // File operations
    printf("File Operations:\n");
    printf("  create <filename> <size> <permissions>  - Create new file (permissions in octal)\n");
    printf("  delete <filename>                       - Delete a file\n");
    printf("  delete -d <dirname>                     - Delete an empty directory\n");
    printf("  write [-a] <file> <data>                - Write data to file (-a for append)\n");
    printf("  read <file> [bytes]                     - Read content from file (optional byte count)\n");
    printf("  seek <file> <offset> <SET|CUR|END>      - Move read position in file\n");
    printf("  chmod <permissions> <file               - Change file permissions\n");
    printf("  stat <file>                             - Show file information\n");


    // Directory operations
    printf("\nDirectory Operations:\n");
    printf("  create -d <dirname>                     - Create new directory\n");
    printf("  cd <dirname>                            - Change directory (default: /home)\n");
    printf("  list                                    - List directory contents\n");
    printf("  pwd                                     - Print current directory path\n");
    printf("  copy <file> <directory>                 - Copy file to another directory\n");
    printf("  move <file> <directory>                 - Move file to another directory\n");
    printf("  dirinfo [dirname]                       - Show directory information (default: current dir)\n");
    // Link operations
    printf("\nLink Operations:\n");
    printf("  ln <source> <link>                      - Create hard link\n");
    printf("  ln -s <source> <link>                   - Create symbolic link\n");

    // System operations
    printf("\nSystem Operations:\n");
    printf("  format                                  - Format the filesystem (WARNING: erases all data)\n");
    printf("  open <file>                             - Open a file\n");
    printf("  close <file>                            - Close a file\n");
    printf("  backup [name]                           - Create backup\n");
    printf("  restore [name]                          - Restore from backup\n");
    printf("  concurrency_test                        - Run concurrency test\n");
    printf("  dirinfo [dirname]                       - Show directory information\n");    printf("  showages                              - Show paging bitmap\n");
    printf("  help                                    - Show this help message\n");
    printf("  quit                                    - Exit the terminal\n");

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
    else if (strncmp(command, "backup", 6) == 0)
    {
        char name[256] = "default";
        sscanf(command, "backup %255s", name);
        backup_filesystem(name);
    }
    else if (strncmp(command, "restore", 7) == 0)
    {
        char name[256] = "default";
        sscanf(command, "restore %255s", name);
        restore_filesystem(name);
    }
    else if (strcmp(command, "format") == 0)
    {
        format_filesystem();
    }
    else if (strncmp(command, "dirinfo", 7) == 0)
    {
        char dirname[MAX_FILENAME] = {0};
        sscanf(command, "dirinfo %s", dirname);
        show_directory_info(strlen(dirname) > 0 ? dirname : NULL);
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
    else if (strncmp(command, "open", 4) == 0)
    {
        char filename[MAX_FILENAME];
        if (sscanf(command, "open %s", filename) == 1)
        {
            open_file(filename);
        }
        else
        {
            printf("Usage: open <filename>\n");
        }
    }
    else if (strncmp(command, "close", 5) == 0)
    {
        char filename[MAX_FILENAME];
        if (sscanf(command, "close %s", filename) == 1)
        {
            close_file(filename);
        }
        else
        {
            printf("Usage: close <filename>\n");
        }
    }
    else if (strncmp(command, "read", 4) == 0)
    {
        char filename[MAX_FILENAME];
        int bytes_to_read = -1; // Default: read to end of file
        int offset = 0;         // Default: read from beginning

        // Parse either "read <filename>", "read <filename> <bytes>", or "read <filename> <offset> <bytes>"
        if (sscanf(command, "read %s %d %d", filename, &offset, &bytes_to_read) >= 1)
        {
            char *content = read_from_file(filename, bytes_to_read, offset);
            if (content)
            {
                printf("File content [%d bytes]: %s\n", (int)strlen(content), content);
                free(content);
            }
        }
        else
        {
            printf(COLOR_RED "Usage: read <filename> [offset] [bytes]\n" COLOR_RESET);
            printf(COLOR_RED "Examples:\n" COLOR_RESET);
            printf(COLOR_RED "  read file.txt         - Read entire file\n" COLOR_RESET);
            printf(COLOR_RED "  read file.txt 10      - Read first 10 bytes\n" COLOR_RESET);
            printf(COLOR_RED "  read file.txt 5 10    - Read 10 bytes starting from offset 5\n" COLOR_RESET);
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
