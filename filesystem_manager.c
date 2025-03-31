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
int create_file(char *path, int permissions);
int create_directory(char *dirname);
void delete_file(char *path);
void list_files();
int write_to_file(const char *path, const char *data, int append);
char *read_from_file(const char *path, int bytes_to_read, int offset);
int login();
int find_directory_index(const char *dirname);
void copy_file_to_dir(const char *path, const char *dirname);
void move_file_to_dir(const char *path, const char *dirname);
void change_directory(char *dirname);
void change_permissions(char *path, int mode);
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
void defragment_filesystem();

// ====================== FILE OPERATION HELPERS ======================


// ====================== PATH RESOLUTION HELPERS ======================

// Helper to split path into directory and filename components
void split_path(const char *path, char **dir, char **file) {
    char *last_slash = strrchr(path, '/');
    if (last_slash) {
        *dir = strndup(path, last_slash - path);
        *file = strdup(last_slash + 1);
    } else {
        *dir = strdup(".");
        *file = strdup(path);
    }
}

// Helper to find directory index from path (absolute or relative)
int find_directory_from_path(const char *path) {
    if (strcmp(path, ".") == 0) return fs_state.current_directory;
    if (strcmp(path, "..") == 0) {
        return (fs_state.directories[fs_state.current_directory].parent_directory != -1) ? 
               fs_state.directories[fs_state.current_directory].parent_directory : 
               fs_state.current_directory;
    }

    int current_dir = (path[0] == '/') ? 0 : fs_state.current_directory;
    char *path_copy = strdup(path + (path[0] == '/'));
    char *token = strtok(path_copy, "/");

    while (token) {
        // Handle special directory components
        if (strcmp(token, ".") == 0) {
            token = strtok(NULL, "/");
            continue;
        }
        if (strcmp(token, "..") == 0) {
            if (fs_state.directories[current_dir].parent_directory != -1) {
                current_dir = fs_state.directories[current_dir].parent_directory;
            }
            token = strtok(NULL, "/");
            continue;
        }

        // Look for matching subdirectory
        int found = -1;
        for (int i = 0; i < MAX_DIRECTORIES; i++) {
            if (strcmp(fs_state.directories[i].dirname, token) == 0 &&
                fs_state.directories[i].parent_directory == current_dir) {
                found = i;
                break;
            }
        }
        if (found == -1) {
            free(path_copy);
            return -1;
        }
        current_dir = found;
        token = strtok(NULL, "/");
    }
    free(path_copy);
    return current_dir;
}

// Helper to find a file in a directory
File* find_file_in_dir(int dir_idx, const char *filename) {
    if (dir_idx < 0 || dir_idx >= MAX_DIRECTORIES) return NULL;
    
    for (int i = 0; i < fs_state.directories[dir_idx].file_count; i++) {
        if (strcmp(fs_state.directories[dir_idx].files[i].filename, filename) == 0) {
            return &fs_state.directories[dir_idx].files[i];
        }
    }
    return NULL;
}

// Helper to check file permissions
int check_file_permissions(File *file, int required_perms) {
    if (!file) return 0;
    
    // Check owner first (simplified - in real system you'd check user/group)
    if (strcmp(file->owner, fs_state.users[0].username) == 0) {
        return (file->permissions & (required_perms << 6));
    }
    // For simplicity, we just check "other" permissions here
    return (file->permissions & required_perms);
}

// Helper to resolve a file path to its actual file (handles symlinks)
File* resolve_file_path(const char *path, int *dir_idx, char **filename) {
    char *dir_path = NULL;
    split_path(path, &dir_path, filename);
    
    *dir_idx = find_directory_from_path(dir_path);
    if (*dir_idx == -1) {
        free(dir_path);
        free(*filename);
        return NULL;
    }

    File *file = find_file_in_dir(*dir_idx, *filename);
    if (!file) {
        free(dir_path);
        free(*filename);
        return NULL;
    }

    // Special handling for deletion commands - don't follow symlinks
    if (strstr(path, "delete") != NULL && file->is_symlink) {
        free(dir_path);
        return file; // Return the symlink itself for deletion
    }

    // Handle symlink resolution for non-deletion operations
    if (file->is_symlink && file->link_target) {
        free(dir_path);
        free(*filename);
        return resolve_file_path(file->link_target, dir_idx, filename);
    }

    free(dir_path);
    return file;
}


void free_pages(File *file) {
    if (!file || !file->page_table) return;
    
    for (int i = 0; i < file->page_table_size; i++) {
        int page_num = file->page_table[i].physical_page;
        page_bitmap[page_num / 8] &= ~(1 << (page_num % 8));
    }
    file->page_table_size = 0;
}












void initialize_directories()
{
    // Clear the entire filesystem state first (avoid garbage data)
    memset(&fs_state, 0, sizeof(fs_state));
    memset(page_bitmap, 0, sizeof(page_bitmap));

    // Initialize root directory (ID 0)
    strcpy(fs_state.directories[0].dirname, "/");
    fs_state.directories[0].file_count = 0;
    fs_state.directories[0].parent_directory = -1; // Root has no parent
    fs_state.directories[0].creation_time = time(NULL);
    fs_state.directories[0].inode = 1743450750;  // Root always gets inode 1

    // Create a default home directory (ID 1)
    strcpy(fs_state.directories[1].dirname, "home");
    fs_state.directories[1].file_count = 0;
    fs_state.directories[1].parent_directory = 0; // Parent is root
    fs_state.directories[1].creation_time = time(NULL);
    fs_state.directories[1].inode = 1743450785;  // Home directory gets inode 2

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

    // Create default files with inode numbers
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
        .page_table_size = 1,
        .inode = 1743450767,  // readme.txt gets inode 3
        .ref_count = 1,
        .is_symlink = 0,
        .link_target = NULL
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
        .page_table_size = 1,
        .inode = 17434757870,  // notes.txt gets inode 4
        .ref_count = 1,
        .is_symlink = 0,
        .link_target = NULL
    };

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

int open_file(const char *filename) {
    pthread_mutex_lock(&mutex);
    
    char *dir_path = NULL, *file_name = NULL;
    int dir_idx = -1;
    File *file = resolve_file_path(filename, &dir_idx, &file_name);
    
    if (!file) {
        printf(COLOR_RED "Error: File not found\n" COLOR_RESET);
        pthread_mutex_unlock(&mutex);
        return -1;
    }

    file->is_open = 1;
    file->open_count++;
    printf("File '%s' opened (count: %d)\n", filename, file->open_count);
    
    free(dir_path);
    free(file_name);
    pthread_mutex_unlock(&mutex);
    return 0;
}

int close_file(const char *filename) {
    pthread_mutex_lock(&mutex);
    
    char *dir_path = NULL, *file_name = NULL;
    int dir_idx = -1;
    File *file = resolve_file_path(filename, &dir_idx, &file_name);
    
    if (!file) {
        printf("Error: File not found\n");
        pthread_mutex_unlock(&mutex);
        return -1;
    }

    if (file->open_count > 0) {
        file->open_count--;
        if (file->open_count == 0) {
            file->is_open = 0;
        }
        printf("File '%s' closed (count: %d)\n", filename, file->open_count);
    } else {
        printf("Error: File not open\n");
    }
    
    free(dir_path);
    free(file_name);
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
        printf(COLOR_RED "File not found: %s\n" COLOR_RESET, filename);
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

int create_file(char *path, int permissions)
{
    pthread_mutex_lock(&mutex);

    // Split path into directory and filename
    char *dir_path = NULL, *filename = NULL;
    split_path(path, &dir_path, &filename);

    // Find target directory
    int dir_idx = find_directory_from_path(dir_path);
    if (dir_idx == -1)
    {
        printf(COLOR_RED "Error: Directory not found: %s\n" COLOR_RESET, dir_path);
        free(dir_path);
        free(filename);
        pthread_mutex_unlock(&mutex);
        return -1;
    }

    // Check if file exists
    for (int i = 0; i < fs_state.directories[dir_idx].file_count; i++)
    {
        if (strcmp(fs_state.directories[dir_idx].files[i].filename, filename) == 0)
        {
            printf(COLOR_RED "Error: File already exists: %s\n" COLOR_RESET, path);
            free(dir_path);
            free(filename);
            pthread_mutex_unlock(&mutex);
            return -1;
        }
    }

    // Create new file with default content
    File new_file;
    memset(&new_file, 0, sizeof(File));

    strncpy(new_file.filename, filename, MAX_FILENAME - 1);
    strcpy(new_file.owner, fs_state.users[0].username); // Current user
    new_file.permissions = permissions & 0777;
    new_file.creation_time = time(NULL);
    new_file.modification_time = new_file.creation_time;
    new_file.ref_count = 1;
    new_file.inode = (ino_t)(time(NULL) + rand() + (long)&new_file); // More unique inode


    // Set default content
    const char *default_content = "HELLO WORLD";
    new_file.content = strdup(default_content);
    new_file.content_size = strlen(default_content) + 1;
    new_file.size = new_file.content_size; // Automatic size calculation

    // Allocate pages based on content size
    int pages_needed = (new_file.content_size + PAGE_SIZE - 1) / PAGE_SIZE;
    if (allocate_pages(pages_needed, &new_file.page_table) != 0)
    {
        printf(COLOR_RED "Error: Not enough space\n" COLOR_RESET);
        free(new_file.content);
        free(dir_path);
        free(filename);
        pthread_mutex_unlock(&mutex);
        return -1;
    }
    new_file.page_table_size = pages_needed;

    // Add to directory
    if (fs_state.directories[dir_idx].file_count >= MAX_FILES)
    {
        printf(COLOR_RED "Error: Directory full\n" COLOR_RESET);
        free_pages(&new_file);
        free(new_file.content);
        free(dir_path);
        free(filename);
        pthread_mutex_unlock(&mutex);
        return -1;
    }

    fs_state.directories[dir_idx].files[fs_state.directories[dir_idx].file_count++] = new_file;

    save_state();
    printf(COLOR_GREEN "Created file %s (size: %d bytes, inode: %lu)\n" COLOR_RESET,
           path, new_file.size, new_file.inode);

    free(dir_path);
    free(filename);
    pthread_mutex_unlock(&mutex);
    return 0;
}

int create_directory(char *path)
{
    pthread_mutex_lock(&mutex);

    // Split path into parent directory and new directory name
    char *parent_path = NULL, *dirname = NULL;
    split_path(path, &parent_path, &dirname);

    // Validate directory name
    if (strlen(dirname) == 0 || strlen(dirname) >= MAX_FILENAME)
    {
        printf(COLOR_RED "Error: Invalid directory name\n" COLOR_RESET);
        free(parent_path);
        free(dirname);
        pthread_mutex_unlock(&mutex);
        return -1;
    }

    // Find parent directory
    int parent_dir_idx = find_directory_from_path(parent_path);
    if (parent_dir_idx == -1)
    {
        printf(COLOR_RED "Error: Parent directory not found: %s\n" COLOR_RESET, parent_path);
        free(parent_path);
        free(dirname);
        pthread_mutex_unlock(&mutex);
        return -1;
    }

    // Check if directory already exists
    for (int i = 0; i < MAX_DIRECTORIES; i++)
    {
        if (fs_state.directories[i].parent_directory == parent_dir_idx &&
            strcmp(fs_state.directories[i].dirname, dirname) == 0)
        {
            printf(COLOR_RED "Error: Directory already exists: %s\n" COLOR_RESET, path);
            free(parent_path);
            free(dirname);
            pthread_mutex_unlock(&mutex);
            return -1;
        }
    }

    // Find empty slot for new directory
    int new_dir_idx = -1;
    for (int i = 0; i < MAX_DIRECTORIES; i++)
    {
        if (strlen(fs_state.directories[i].dirname) == 0)
        {
            new_dir_idx = i;
            break;
        }
    }

    if (new_dir_idx == -1)
    {
        printf(COLOR_RED "Error: Maximum number of directories reached\n" COLOR_RESET);
        free(parent_path);
        free(dirname);
        pthread_mutex_unlock(&mutex);
        return -1;
    }

    // Create new directory
    Directory new_dir;
    memset(&new_dir, 0, sizeof(Directory));

    strncpy(new_dir.dirname, dirname, MAX_FILENAME - 1);
    new_dir.parent_directory = parent_dir_idx;
    new_dir.creation_time = time(NULL);
    new_dir.inode = (ino_t)(time(NULL) + rand() + (long)&new_dir); // More unique inode

    // Initialize directory contents
    new_dir.file_count = 0;
    for (int i = 0; i < MAX_FILES; i++)
    {
        memset(&new_dir.files[i], 0, sizeof(File));
    }

    // Add to filesystem
    fs_state.directories[new_dir_idx] = new_dir;

    save_state();
    printf(COLOR_GREEN "Created directory %s (inode: %lu)\n" COLOR_RESET,
           path, new_dir.inode);

    free(parent_path);
    free(dirname);
    pthread_mutex_unlock(&mutex);
    return 0;
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

void delete_file(char *path) {
    pthread_mutex_lock(&mutex);
    
    char *dir_path = NULL, *filename = NULL;
    int dir_idx = -1;
    
    // Special handling for symbolic links - we want to delete the link itself
    File *file = NULL;
    Directory *dir = NULL;
    
    // First try to find the file without following symlinks
    split_path(path, &dir_path, &filename);
    dir_idx = find_directory_from_path(dir_path);
    
    if (dir_idx == -1) {
        printf(COLOR_RED "Error: Directory not found: %s\n" COLOR_RESET, dir_path);
        goto cleanup;
    }
    
    dir = &fs_state.directories[dir_idx];
    
    // Find the file in the directory without resolving symlinks
    for (int i = 0; i < dir->file_count; i++) {
        if (strcmp(dir->files[i].filename, filename) == 0) {
            file = &dir->files[i];
            break;
        }
    }
    
    if (!file) {
        printf(COLOR_RED "Error: File not found: %s\n" COLOR_RESET, path);
        goto cleanup;
    }

    if (file->is_symlink) {
        printf(COLOR_BLUE "Deleting symbolic link (inode: %lu): %s -> %s\n" COLOR_RESET,
               file->inode, path, file->link_target ? file->link_target : "(null)");
        
        // Only delete the link, not the target
        if (file->link_target) {
            free(file->link_target);
            file->link_target = NULL;
        }
        
        // Remove from directory
        int file_idx = file - dir->files;
        for (int i = file_idx; i < dir->file_count - 1; i++) {
            dir->files[i] = dir->files[i + 1];
        }
        
        // Clear the last entry and decrement count
        memset(&dir->files[dir->file_count - 1], 0, sizeof(File));
        dir->file_count--;

        save_state();
        printf(COLOR_GREEN "Successfully deleted symbolic link: %s\n" COLOR_RESET, path);
    } 
    else {
        // Handle regular file or hard link deletion
        printf(COLOR_BLUE "Deleting %s (inode: %lu): %s\n" COLOR_RESET,
               (file->ref_count > 1) ? "hard link" : "file",
               file->inode, path);

        if (--file->ref_count <= 0) {
            free_pages(file);
            if (file->content) {
                free(file->content);
                file->content = NULL;
            }
            if (file->page_table) {
                free(file->page_table);
                file->page_table = NULL;
            }
            if (file->link_target) {
                free(file->link_target);
                file->link_target = NULL;
            }
        }

        // Remove from directory
        int file_idx = file - dir->files;
        for (int i = file_idx; i < dir->file_count - 1; i++) {
            dir->files[i] = dir->files[i + 1];
        }
        
        memset(&dir->files[dir->file_count - 1], 0, sizeof(File));
        dir->file_count--;

        save_state();
        printf(COLOR_GREEN "Successfully deleted: %s\n" COLOR_RESET, path);
    }

cleanup:
    free(dir_path);
    free(filename);
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

void list_files() {
    pthread_mutex_lock(&mutex);

    // Count actual existing directories
    int dir_count = 0;
    for (int i = 0; i < MAX_DIRECTORIES; i++) {
        if (strlen(fs_state.directories[i].dirname) > 0) {
            dir_count++;
        }
    }

    printf("\nCurrent directory: %d (%s)\n", fs_state.current_directory,
           fs_state.directories[fs_state.current_directory].dirname);
    printf("Existing directories: %d\n", dir_count);
    printf("File count: %d\n", fs_state.directories[fs_state.current_directory].file_count);

    printf("\nContents of directory '%s':\n", fs_state.directories[fs_state.current_directory].dirname);
    printf("--------------------------------\n");

    // List directories
    printf("[Directories]\n");
    for (int i = 0; i < MAX_DIRECTORIES; i++) {
        if (i != fs_state.current_directory &&
            fs_state.directories[i].parent_directory == fs_state.current_directory &&
            strlen(fs_state.directories[i].dirname) > 0) {
            printf("  %s/\n", fs_state.directories[i].dirname);
        }
    }

    // List files - only show entries with non-empty filenames
    printf("\n[Files]\n");
    Directory *dir = &fs_state.directories[fs_state.current_directory];
    for (int i = 0; i < dir->file_count; i++) {
        if (strlen(dir->files[i].filename) == 0) continue;
        
        File *f = &dir->files[i];
        printf("  %-15s %6d bytes  %04o  %s",
               f->filename, f->size, f->permissions, f->owner);
        
        if (f->is_symlink) {
            printf(" -> %s", f->link_target ? f->link_target : "(null)");
        }
        printf("\n");
    }
    printf("--------------------------------\n");
    pthread_mutex_unlock(&mutex);
}



int write_to_file(const char *path, const char *data, int append) {
    pthread_mutex_lock(&mutex);
    
    char *dir_path = NULL, *filename = NULL;
    int dir_idx = -1;
    File *file = resolve_file_path(path, &dir_idx, &filename);
    
    if (!file) {
        printf(COLOR_RED "Error: File not found\n" COLOR_RESET);
        pthread_mutex_unlock(&mutex);
        return -1;
    }

    if (!check_file_permissions(file, 2)) { // 2 = write permission
        printf(COLOR_RED "Error: Permission denied\n" COLOR_RESET);
        free(dir_path);
        free(filename);
        pthread_mutex_unlock(&mutex);
        return -1;
    }

    int data_len = strlen(data);
    int new_content_size;
    char *new_content;

    if (append) {
        // Append mode
        new_content_size = file->content_size + data_len;
        new_content = realloc(file->content, new_content_size + 1);
        if (!new_content) {
            printf(COLOR_RED "Error: Memory allocation failed\n" COLOR_RESET);
            free(dir_path);
            free(filename);
            pthread_mutex_unlock(&mutex);
            return -1;
        }
        strcat(new_content, data);
    } else {
        // Overwrite mode
        new_content_size = data_len;
        new_content = strdup(data);
        if (!new_content) {
            printf(COLOR_RED "Error: Memory allocation failed\n" COLOR_RESET);
            free(dir_path);
            free(filename);
            pthread_mutex_unlock(&mutex);
            return -1;
        }
        if (file->content) {
            free(file->content);
        }
    }

    // Check if we need more pages
    int pages_needed = (new_content_size + PAGE_SIZE - 1) / PAGE_SIZE;
    if (pages_needed > file->page_table_size) {
        PageTableEntry *new_table = realloc(file->page_table, pages_needed * sizeof(PageTableEntry));
        if (!new_table) {
            printf(COLOR_RED "Error: Could not expand page table\n" COLOR_RESET);
            free(new_content);
            free(dir_path);
            free(filename);
            pthread_mutex_unlock(&mutex);
            return -1;
        }

        // Initialize new page table entries
        for (int i = file->page_table_size; i < pages_needed; i++) {
            new_table[i].physical_page = -1;
            new_table[i].is_allocated = 0;
        }

        // Allocate new pages
        for (int i = file->page_table_size; i < pages_needed; i++) {
            int page_found = 0;
            for (int j = 0; j < TOTAL_PAGES; j++) {
                if (!(page_bitmap[j / 8] & (1 << (j % 8)))) {
                    page_bitmap[j / 8] |= (1 << (j % 8));
                    new_table[i].physical_page = j;
                    new_table[i].is_allocated = 1;
                    page_found = 1;
                    break;
                }
            }
            if (!page_found) {
                // Cleanup already allocated pages
                for (int k = file->page_table_size; k < i; k++) {
                    page_bitmap[new_table[k].physical_page / 8] &= ~(1 << (new_table[k].physical_page % 8));
                }
                free(new_content);
                free(new_table);
                free(dir_path);
                free(filename);
                pthread_mutex_unlock(&mutex);
                return -1;
            }
        }

        file->page_table = new_table;
        file->page_table_size = pages_needed;
    }

    // Update file metadata
    file->content = new_content;
    file->content_size = new_content_size;
    file->size = new_content_size;
    file->modification_time = time(NULL);

    save_state();
    printf(COLOR_GREEN "Successfully wrote %d bytes to %s (new size: %d bytes)\n" COLOR_RESET,
           data_len, path, new_content_size);

    free(dir_path);
    free(filename);
    pthread_mutex_unlock(&mutex);
    return data_len;
}


char *read_file_content(File *file, int bytes_to_read, int offset)
{
    // Helper function that actually reads content (without path resolution)
    if (!file->content || file->content_size <= 0)
    {
        return strdup("");
    }

    if (offset < 0)
        offset = 0;
    if (offset > file->content_size)
        offset = file->content_size;

    int remaining = file->content_size - offset;
    int read_bytes = (bytes_to_read <= 0) ? remaining : (bytes_to_read < remaining) ? bytes_to_read
                                                                                    : remaining;

    char *buffer = malloc(read_bytes + 1);
    if (!buffer)
    {
        printf(COLOR_RED "Error: Memory allocation failed\n" COLOR_RESET);
        return NULL;
    }

    memcpy(buffer, file->content + offset, read_bytes);
    buffer[read_bytes] = '\0';
    return buffer;
}

char *read_from_file(const char *path, int bytes_to_read, int offset) {
    pthread_mutex_lock(&mutex);
    
    char *dir_path = NULL, *filename = NULL;
    int dir_idx = -1;
    File *file = resolve_file_path(path, &dir_idx, &filename);
    
    if (!file) {
        printf(COLOR_RED "Error: File not found\n" COLOR_RESET);
        pthread_mutex_unlock(&mutex);
        return NULL;
    }

    if (!check_file_permissions(file, 4)) { // 4 = read permission
        printf(COLOR_RED "Error: Permission denied\n" COLOR_RESET);
        free(dir_path);
        free(filename);
        pthread_mutex_unlock(&mutex);
        return NULL;
    }

    // Read the actual content
    char *buffer = NULL;
    if (file->content && file->content_size > 0) {
        if (offset < 0) offset = 0;
        if (offset > file->content_size) offset = file->content_size;
        
        int remaining = file->content_size - offset;
        int read_bytes = (bytes_to_read <= 0) ? remaining : 
                        (bytes_to_read < remaining) ? bytes_to_read : remaining;

        buffer = malloc(read_bytes + 1);
        if (buffer) {
            memcpy(buffer, file->content + offset, read_bytes);
            buffer[read_bytes] = '\0';
        }
    } else {
        buffer = strdup("");
    }

    // Update access time
    file->modification_time = time(NULL);

    free(dir_path);
    free(filename);
    pthread_mutex_unlock(&mutex);
    return buffer;
}

void change_directory(char *path)
{
    pthread_mutex_lock(&mutex);

    // Handle special cases
    if (path == NULL || strcmp(path, "") == 0)
    {
        // If no argument, go to home directory
        path = "/";
    }
    else if (strcmp(path, "~") == 0)
    {
        // Handle home directory shortcut
        path = "/";
    }

    // Handle absolute paths
    if (path[0] == '/')
    {
        int current_dir = 0;                // Start at root
        char *path_copy = strdup(path + 1); // Skip leading slash
        char *token = strtok(path_copy, "/");

        while (token != NULL)
        {
            int found = -1;
            for (int i = 0; i < MAX_DIRECTORIES; i++)
            {
                if (strcmp(fs_state.directories[i].dirname, token) == 0 &&
                    fs_state.directories[i].parent_directory == current_dir)
                {
                    found = i;
                    break;
                }
            }

            if (found == -1)
            {
                printf(COLOR_RED "Directory not found: %s\n" COLOR_RESET, path);
                free(path_copy);
                pthread_mutex_unlock(&mutex);
                return;
            }
            current_dir = found;
            token = strtok(NULL, "/");
        }

        fs_state.current_directory = current_dir;
        free(path_copy);
        printf("Changed to directory: %s\n", fs_state.directories[current_dir].dirname);
        pthread_mutex_unlock(&mutex);
        return;
    }

    // Handle relative paths
    int current_dir = fs_state.current_directory;
    char *path_copy = strdup(path);
    char *token = strtok(path_copy, "/");

    while (token != NULL)
    {
        // Handle special directory components
        if (strcmp(token, ".") == 0)
        {
            // Do nothing, stay in current directory
            token = strtok(NULL, "/");
            continue;
        }
        else if (strcmp(token, "..") == 0)
        {
            // Move to parent directory if possible
            if (fs_state.directories[current_dir].parent_directory != -1)
            {
                current_dir = fs_state.directories[current_dir].parent_directory;
            }
            token = strtok(NULL, "/");
            continue;
        }

        // Look for matching subdirectory
        int found = -1;
        for (int i = 0; i < MAX_DIRECTORIES; i++)
        {
            if (strcmp(fs_state.directories[i].dirname, token) == 0 &&
                fs_state.directories[i].parent_directory == current_dir)
            {
                found = i;
                break;
            }
        }

        if (found == -1)
        {
            printf(COLOR_RED "Directory not found: %s\n" COLOR_RESET, path);
            free(path_copy);
            pthread_mutex_unlock(&mutex);
            return;
        }
        current_dir = found;
        token = strtok(NULL, "/");
    }

    fs_state.current_directory = current_dir;
    printf("Changed to directory: %s\n", fs_state.directories[current_dir].dirname);
    free(path_copy);
    pthread_mutex_unlock(&mutex);
}

// Helper function to resolve paths (returns directory index or -1 if not found)
int resolve_path(const char *path)
{
    // Handle absolute paths
    if (path[0] == '/')
    {
        int current_dir = 0;                // Start at root
        char *path_copy = strdup(path + 1); // Skip leading slash
        char *token = strtok(path_copy, "/");

        while (token)
        {
            int found = -1;
            for (int i = 0; i < MAX_DIRECTORIES; i++)
            {
                if (strcmp(fs_state.directories[i].dirname, token) == 0 &&
                    fs_state.directories[i].parent_directory == current_dir)
                {
                    found = i;
                    break;
                }
            }
            if (found == -1)
            {
                free(path_copy);
                return -1;
            }
            current_dir = found;
            token = strtok(NULL, "/");
        }
        free(path_copy);
        return current_dir;
    }

    // Handle relative paths
    char *path_copy = strdup(path);
    char *token = strtok(path_copy, "/");
    int current_dir = fs_state.current_directory;

    while (token)
    {
        // Handle special cases
        if (strcmp(token, ".") == 0)
        {
            token = strtok(NULL, "/");
            continue;
        }
        if (strcmp(token, "..") == 0)
        {
            if (fs_state.directories[current_dir].parent_directory != -1)
            {
                current_dir = fs_state.directories[current_dir].parent_directory;
            }
            token = strtok(NULL, "/");
            continue;
        }

        int found = -1;
        for (int i = 0; i < MAX_DIRECTORIES; i++)
        {
            if (strcmp(fs_state.directories[i].dirname, token) == 0 &&
                fs_state.directories[i].parent_directory == current_dir)
            {
                found = i;
                break;
            }
        }
        if (found == -1)
        {
            free(path_copy);
            return -1;
        }
        current_dir = found;
        token = strtok(NULL, "/");
    }
    free(path_copy);
    return current_dir;
}

void copy_file_to_dir(const char *src_path, const char *dest_dir_path) {
    pthread_mutex_lock(&mutex);
    
    // Resolve source file
    char *src_dir_path = NULL, *src_filename = NULL;
    int src_dir_idx = -1;
    File *src_file = resolve_file_path(src_path, &src_dir_idx, &src_filename);
    
    if (!src_file) {
        printf(COLOR_RED "Error: Source file not found: %s\n" COLOR_RESET, src_path);
        goto cleanup;
    }

    // Resolve destination directory
    int dest_dir_idx = find_directory_from_path(dest_dir_path);
    if (dest_dir_idx == -1) {
        printf(COLOR_RED "Error: Destination directory not found: %s\n" COLOR_RESET, dest_dir_path);
        goto cleanup;
    }

    // Check if file already exists in destination
    for (int i = 0; i < fs_state.directories[dest_dir_idx].file_count; i++) {
        if (strcmp(fs_state.directories[dest_dir_idx].files[i].filename, src_filename) == 0) {
            printf(COLOR_RED "Error: File already exists in destination directory\n" COLOR_RESET);
            goto cleanup;
        }
    }

    // Check space in destination
    if (fs_state.directories[dest_dir_idx].file_count >= MAX_FILES) {
        printf(COLOR_RED "Error: Destination directory is full\n" COLOR_RESET);
        goto cleanup;
    }

    // Create the copy with a new inode
    File new_file = *src_file;
    new_file.inode = (ino_t)(time(NULL) + rand() + (long)&new_file); // New unique inode
    new_file.creation_time = time(NULL);
    new_file.modification_time = new_file.creation_time;
    
    // For copies, we don't share content - make a deep copy
    if (new_file.content) {
        new_file.content = strdup(src_file->content);
        if (!new_file.content) {
            printf(COLOR_RED "Error: Failed to copy file content\n" COLOR_RESET);
            goto cleanup;
        }
    }
    
    // Copy page table if it exists
    if (new_file.page_table && new_file.page_table_size > 0) {
        new_file.page_table = malloc(new_file.page_table_size * sizeof(PageTableEntry));
        if (!new_file.page_table) {
            printf(COLOR_RED "Error: Failed to copy page table\n" COLOR_RESET);
            if (new_file.content) free(new_file.content);
            goto cleanup;
        }
        memcpy(new_file.page_table, src_file->page_table, 
               new_file.page_table_size * sizeof(PageTableEntry));
    }
    
    // For copies, reset ref_count to 1 (it's a new independent file)
    new_file.ref_count = 1;
    
    // Add to destination directory
    fs_state.directories[dest_dir_idx].files[fs_state.directories[dest_dir_idx].file_count++] = new_file;

    save_state();
    printf(COLOR_GREEN "Copied '%s' to '%s/%s' (new inode: %lu)\n" COLOR_RESET, 
           src_path, fs_state.directories[dest_dir_idx].dirname, src_filename, new_file.inode);

cleanup:
    free(src_dir_path);
    free(src_filename);
    pthread_mutex_unlock(&mutex);
}


void move_file_to_dir(const char *src_path, const char *dest_dir_path) {
    pthread_mutex_lock(&mutex);
    
    // Resolve source file
    char *src_dir_path = NULL, *src_filename = NULL;
    int src_dir_idx = -1;
    File *src_file = resolve_file_path(src_path, &src_dir_idx, &src_filename);
    
    if (!src_file) {
        printf(COLOR_RED "Error: Source file not found: %s\n" COLOR_RESET, src_path);
        goto cleanup;
    }

    // Resolve destination directory
    int dest_dir_idx = find_directory_from_path(dest_dir_path);
    if (dest_dir_idx == -1) {
        printf(COLOR_RED "Error: Destination directory not found: %s\n" COLOR_RESET, dest_dir_path);
        goto cleanup;
    }

    // Check if file already exists in destination
    for (int i = 0; i < fs_state.directories[dest_dir_idx].file_count; i++) {
        if (strcmp(fs_state.directories[dest_dir_idx].files[i].filename, src_filename) == 0) {
            printf(COLOR_RED "Error: File already exists in destination directory\n" COLOR_RESET);
            goto cleanup;
        }
    }

    // Check space in destination
    if (fs_state.directories[dest_dir_idx].file_count >= MAX_FILES) {
        printf(COLOR_RED "Error: Destination directory is full\n" COLOR_RESET);
        goto cleanup;
    }

    // Find source file index
    int src_file_idx = -1;
    for (int i = 0; i < fs_state.directories[src_dir_idx].file_count; i++) {
        if (&fs_state.directories[src_dir_idx].files[i] == src_file) {
            src_file_idx = i;
            break;
        }
    }

    if (src_file_idx == -1) {
        printf(COLOR_RED "Error: Could not locate file in source directory\n" COLOR_RESET);
        goto cleanup;
    }

    // Move to destination directory
    fs_state.directories[dest_dir_idx].files[fs_state.directories[dest_dir_idx].file_count++] = *src_file;

    // Remove from source directory
    for (int i = src_file_idx; i < fs_state.directories[src_dir_idx].file_count - 1; i++) {
        fs_state.directories[src_dir_idx].files[i] = fs_state.directories[src_dir_idx].files[i + 1];
    }
    fs_state.directories[src_dir_idx].file_count--;

    save_state();
    printf(COLOR_GREEN "Moved '%s' to '%s/%s'\n" COLOR_RESET, 
           src_path, fs_state.directories[dest_dir_idx].dirname, src_filename);

cleanup:
    free(src_dir_path);
    free(src_filename);
    pthread_mutex_unlock(&mutex);
}

void print_file_info(const char *path) {
    pthread_mutex_lock(&mutex);
    
    char *dir_path = NULL, *filename = NULL;
    int dir_idx = -1;
    File *file = resolve_file_path(path, &dir_idx, &filename);
    
    if (!file) {
        printf(COLOR_RED "Error: File not found: %s\n" COLOR_RESET, path);
        pthread_mutex_unlock(&mutex);
        return;
    }

    printf("\nFile: %s\n", file->filename);
    printf("Path: %s/%s\n", fs_state.directories[dir_idx].dirname, file->filename);
    printf("Size: %d bytes\n", file->size);
    printf("Owner: %s\n", file->owner);
    printf("Permissions: %04o ", file->permissions);

    // Show permission interpretation
    printf("(%c%c%c%c%c%c%c%c%c)\n",
           (file->permissions & 0400) ? 'r' : '-',
           (file->permissions & 0200) ? 'w' : '-',
           (file->permissions & 0100) ? 'x' : '-',
           (file->permissions & 0040) ? 'r' : '-',
           (file->permissions & 0020) ? 'w' : '-',
           (file->permissions & 0010) ? 'x' : '-',
           (file->permissions & 0004) ? 'r' : '-',
           (file->permissions & 0002) ? 'w' : '-',
           (file->permissions & 0001) ? 'x' : '-');

    printf("Type: %s\n", file->is_symlink ? "Symbolic link" : 
                        (file->ref_count > 1) ? "Hard link" : "Regular file");
    
    if (file->is_symlink) {
        printf("Link target: %s\n", file->link_target ? file->link_target : "(null)");
    } else if (file->ref_count > 1) {
        printf("Link count: %d\n", file->ref_count);
    }
    
    printf("Inode: %lu\n", file->inode);
    printf("Created: %s", ctime(&file->creation_time));
    printf("Modified: %s", ctime(&file->modification_time));
    printf("Open count: %d\n", file->open_count);
    printf("Pages allocated: %d\n", file->page_table_size);

    free(dir_path);
    free(filename);
    pthread_mutex_unlock(&mutex);
}

void change_permissions(char *path, int mode) {
    pthread_mutex_lock(&mutex);
    
    char *dir_path = NULL, *filename = NULL;
    int dir_idx = -1;
    File *file = resolve_file_path(path, &dir_idx, &filename);
    
    if (!file) {
        printf(COLOR_RED "Error: File not found: %s\n" COLOR_RESET, path);
        goto cleanup;
    }

    // Ensure we only change permission bits (last 9 bits)
    file->permissions = mode & 0777;
    file->modification_time = time(NULL);

    save_state();
    printf(COLOR_GREEN "Permissions of '%s' changed to %04o\n" COLOR_RESET, path, mode);

cleanup:
    free(dir_path);
    free(filename);
    pthread_mutex_unlock(&mutex);
}


void create_hard_link(const char *source_path, const char *link_path)
{
    pthread_mutex_lock(&mutex);

    // Split paths
    char *src_dir = NULL, *src_file = NULL;
    split_path(source_path, &src_dir, &src_file);
    char *link_dir = NULL, *link_file = NULL;
    split_path(link_path, &link_dir, &link_file);

    // Find source directory
    int src_dir_idx = find_directory_from_path(src_dir);
    if (src_dir_idx == -1)
    {
        printf(COLOR_RED "Error: Source directory not found: %s\n" COLOR_RESET, src_dir);
        goto cleanup;
    }

    // Find source file
    File *src_file_ptr = NULL;
    for (int i = 0; i < fs_state.directories[src_dir_idx].file_count; i++)
    {
        if (strcmp(fs_state.directories[src_dir_idx].files[i].filename, src_file) == 0)
        {
            src_file_ptr = &fs_state.directories[src_dir_idx].files[i];
            break;
        }
    }

    if (!src_file_ptr)
    {
        printf(COLOR_RED "Error: Source file not found: %s\n" COLOR_RESET, source_path);
        goto cleanup;
    }

    // Find link directory
    int link_dir_idx = find_directory_from_path(link_dir);
    if (link_dir_idx == -1)
    {
        printf(COLOR_RED "Error: Link directory not found: %s\n" COLOR_RESET, link_dir);
        goto cleanup;
    }

    // Check for existing link
    for (int i = 0; i < fs_state.directories[link_dir_idx].file_count; i++)
    {
        if (strcmp(fs_state.directories[link_dir_idx].files[i].filename, link_file) == 0)
        {
            printf(COLOR_RED "Error: Link already exists: %s\n" COLOR_RESET, link_path);
            goto cleanup;
        }
    }

    // Create hard link (shares all data with original)
    File new_link = *src_file_ptr;
    strncpy(new_link.filename, link_file, MAX_FILENAME - 1);
    new_link.filename[MAX_FILENAME - 1] = '\0';
    new_link.creation_time = time(NULL);

    // Share the same content and page table
    new_link.content = src_file_ptr->content;
    new_link.page_table = src_file_ptr->page_table;
    new_link.is_symlink = 0;
    new_link.link_target = NULL;

    // Add to directory
    if (fs_state.directories[link_dir_idx].file_count >= MAX_FILES)
    {
        printf(COLOR_RED "Error: Directory is full\n" COLOR_RESET);
        goto cleanup;
    }

    fs_state.directories[link_dir_idx].files[fs_state.directories[link_dir_idx].file_count++] = new_link;

    save_state();
    printf(COLOR_GREEN "Created hard link: %s -> %s\n" COLOR_RESET, link_path, source_path);

cleanup:
    free(src_dir);
    free(src_file);
    free(link_dir);
    free(link_file);
    pthread_mutex_unlock(&mutex);
}

// Add this new function implementation
void defragment_filesystem()
{
    printf("Running defragmentation...\n");

    // Create a list of all allocated pages across all files
    int total_pages_used = 0;
    for (int d = 0; d < MAX_DIRECTORIES; d++)
    {
        if (strlen(fs_state.directories[d].dirname))
        {
            for (int f = 0; f < fs_state.directories[d].file_count; f++)
            {
                File *file = &fs_state.directories[d].files[f];
                total_pages_used += file->page_table_size;
            }
        }
    }

    // If we're using less than 90% of pages, no need to defragment
    if (total_pages_used < (TOTAL_PAGES * 0.9))
    {
        printf("Defragmentation not needed (fragmentation level is low)\n");
        return;
    }

    // Compact pages by moving files to lower-numbered pages
    int next_free_page = 0;
    for (int d = 0; d < MAX_DIRECTORIES; d++)
    {
        if (strlen(fs_state.directories[d].dirname))
        {
            for (int f = 0; f < fs_state.directories[d].file_count; f++)
            {
                File *file = &fs_state.directories[d].files[f];

                for (int p = 0; p < file->page_table_size; p++)
                {
                    int old_page = file->page_table[p].physical_page;

                    if (old_page > next_free_page)
                    {
                        // Move this page to the next free spot
                        file->page_table[p].physical_page = next_free_page;

                        // Update bitmap
                        page_bitmap[old_page / 8] &= ~(1 << (old_page % 8));
                        page_bitmap[next_free_page / 8] |= (1 << (next_free_page % 8));
                    }

                    next_free_page++;
                }
            }
        }
    }

    printf("Defragmentation completed. %d pages compacted.\n", total_pages_used);
}

void create_symbolic_link(const char *source, const char *link_path) {
    pthread_mutex_lock(&mutex);

    // Split paths
    char *link_dir = NULL, *link_file = NULL;
    split_path(link_path, &link_dir, &link_file);

    // Find link directory
    int link_dir_idx = find_directory_from_path(link_dir);
    if (link_dir_idx == -1) {
        printf(COLOR_RED "Error: Link directory not found: %s\n" COLOR_RESET, link_dir);
        goto cleanup;
    }

    // Check for existing link
    for (int i = 0; i < fs_state.directories[link_dir_idx].file_count; i++) {
        if (strcmp(fs_state.directories[link_dir_idx].files[i].filename, link_file) == 0) {
            printf(COLOR_RED "Error: Link already exists: %s\n" COLOR_RESET, link_path);
            goto cleanup;
        }
    }

    // Create symbolic link with unique inode
    File symlink;
    memset(&symlink, 0, sizeof(File));

    strncpy(symlink.filename, link_file, MAX_FILENAME - 1);
    symlink.filename[MAX_FILENAME - 1] = '\0';
    symlink.size = strlen(source); // Size of link content (path string)
    strcpy(symlink.owner, fs_state.users[0].username);
    symlink.permissions = 0777; // Default permissions for symlinks
    symlink.creation_time = time(NULL);
    symlink.modification_time = symlink.creation_time;
    symlink.is_symlink = 1;
    symlink.link_target = strdup(source);
    symlink.inode = (ino_t)(time(NULL) + rand()); // Unique inode
    symlink.ref_count = 1;
    symlink.content = NULL; // Symlinks don't have content
    symlink.content_size = 0;
    symlink.page_table = NULL; // No pages needed for symlinks
    symlink.page_table_size = 0;

    if (!symlink.link_target) {
        printf(COLOR_RED "Error: Failed to allocate memory for symlink target\n" COLOR_RESET);
        goto cleanup;
    }

    // Add to directory
    if (fs_state.directories[link_dir_idx].file_count >= MAX_FILES) {
        printf(COLOR_RED "Error: Directory is full\n" COLOR_RESET);
        free(symlink.link_target);
        goto cleanup;
    }

    fs_state.directories[link_dir_idx].files[fs_state.directories[link_dir_idx].file_count++] = symlink;

    save_state();
    printf(COLOR_GREEN "Created symbolic link: %s -> %s (inode: %lu)\n" COLOR_RESET, 
           link_path, source, symlink.inode);

cleanup:
    free(link_dir);
    free(link_file);
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

void backup_filesystem(const char *backup_name) {
    pthread_mutex_lock(&mutex);
    
    char backup_file[256];
    snprintf(backup_file, sizeof(backup_file), "%s.bak", backup_name);

    // Check if backup file already exists
    if (access(backup_file, F_OK) == 0) {
        printf(COLOR_YELLOW "Warning: Backup file '%s' already exists!\n" COLOR_RESET, backup_file);
        printf(COLOR_RED "This operation will overwrite it. Continue? [y/N] " COLOR_RESET);
        
        char response[10];
        if (fgets(response, sizeof(response), stdin) == NULL || 
            (response[0] != 'y' && response[0] != 'Y')) {
            printf(COLOR_BLUE "Backup cancelled\n" COLOR_RESET);
            pthread_mutex_unlock(&mutex);
            return;
        }
    }

    FILE *src = fopen(STORAGE_FILE, "rb");
    if (!src) {
        printf(COLOR_RED "Error: Could not open source file '%s' for reading\n" COLOR_RESET, STORAGE_FILE);
        pthread_mutex_unlock(&mutex);
        return;
    }

    FILE *dst = fopen(backup_file, "wb");
    if (!dst) {
        printf(COLOR_RED "Error: Could not open backup file '%s' for writing\n" COLOR_RESET, backup_file);
        fclose(src);
        pthread_mutex_unlock(&mutex);
        return;
    }

    char buffer[4096];
    size_t bytes;
    int error_occurred = 0;
    
    while ((bytes = fread(buffer, 1, sizeof(buffer), src)) > 0) {
        if (fwrite(buffer, 1, bytes, dst) != bytes) {
            printf(COLOR_RED "Error: Failed to write to backup file\n" COLOR_RESET);
            error_occurred = 1;
            break;
        }
    }

    if (ferror(src)) {
        printf(COLOR_RED "Error: Failed to read from source file\n" COLOR_RESET);
        error_occurred = 1;
    }

    fclose(src);
    fclose(dst);

    if (error_occurred) {
        // Delete the partial backup file if there was an error
        remove(backup_file);
        printf(COLOR_RED "Backup failed - no files were changed\n" COLOR_RESET);
    } else {
        printf(COLOR_GREEN "Backup successfully created: %s\n" COLOR_RESET, backup_file);
    }

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

void show_directory_info(const char *dirname)
{
    pthread_mutex_lock(&mutex);

    // First gather all needed information while holding the lock
    int target_dir = fs_state.current_directory;
    char dirname_copy[MAX_FILENAME] = {0};
    char path[1024] = {0};

    if (dirname == NULL || strcmp(dirname, ".") == 0)
    {
        // Use current directory
        strncpy(dirname_copy, fs_state.directories[target_dir].dirname, MAX_FILENAME - 1);
    }
    else
    {
        // Find the target directory
        int found = 0;
        for (int i = 0; i < MAX_DIRECTORIES; i++)
        {
            if (strcmp(fs_state.directories[i].dirname, dirname) == 0 &&
                fs_state.directories[i].parent_directory == fs_state.current_directory)
            {
                target_dir = i;
                strncpy(dirname_copy, dirname, MAX_FILENAME - 1);
                found = 1;
                break;
            }
        }
        if (!found)
        {
            pthread_mutex_unlock(&mutex);
            printf(COLOR_RED "Directory not found\n" COLOR_RESET);
            return;
        }
    }

    // Build path without calling pwd (to avoid deadlock)
    int current = target_dir;
    char *ptr = path + sizeof(path) - 1;
    *ptr = '\0';

    while (current != -1)
    {
        const char *name = fs_state.directories[current].dirname;
        size_t len = strlen(name);

        ptr -= len;
        memcpy(ptr, name, len);

        if (fs_state.directories[current].parent_directory != -1)
        {
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

void print_tree_recursive(int dir_idx, int depth, int show_inodes) {
    Directory *dir = &fs_state.directories[dir_idx];

    // Print current directory
    for (int i = 0; i < depth; i++) {
        printf("│   ");
    }
    if (show_inodes) {
        printf("├── [%lu] " COLOR_BLUE "%s" COLOR_RESET "\n", dir->inode, dir->dirname);
    } else {
        printf("├── " COLOR_BLUE "%s" COLOR_RESET "\n", dir->dirname);
    }

    // Print files in this directory - skip empty entries
    for (int i = 0; i < dir->file_count; i++) {
        if (strlen(dir->files[i].filename) == 0) continue;
        
        File *file = &dir->files[i];
        for (int j = 0; j < depth + 1; j++) {
            printf("│   ");
        }

        if (show_inodes) {
            if (file->is_symlink) {
                printf("├── [%lu] " COLOR_YELLOW "%s" COLOR_RESET " -> %s\n",
                       file->inode, file->filename, 
                       file->link_target ? file->link_target : "(null)");
            } else if (file->ref_count > 1) {
                printf("├── [%lu] " COLOR_GREEN "%s" COLOR_RESET "\n",
                       file->inode, file->filename);
            } else {
                printf("├── [%lu] %s\n", file->inode, file->filename);
            }
        } else {
            if (file->is_symlink) {
                printf("├── " COLOR_YELLOW "%s" COLOR_RESET " -> %s\n",
                       file->filename,
                       file->link_target ? file->link_target : "(null)");
            } else if (file->ref_count > 1) {
                printf("├── " COLOR_GREEN "%s" COLOR_RESET "\n", file->filename);
            } else {
                printf("├── %s\n", file->filename);
            }
        }
    }

    // Recursively print subdirectories
    for (int i = 0; i < MAX_DIRECTORIES; i++) {
        if (fs_state.directories[i].parent_directory == dir_idx &&
            strlen(fs_state.directories[i].dirname) > 0) {
            print_tree_recursive(i, depth + 1, show_inodes);
        }
    }
}


void tree_command(int show_inodes)
{
    pthread_mutex_lock(&mutex);

    printf(".\n");
    print_tree_recursive(fs_state.current_directory, 0, show_inodes);

    pthread_mutex_unlock(&mutex);
}

void help() {
    printf("\n" COLOR_GREEN "Mini UNIX-like File System Help" COLOR_RESET "\n");
    printf("===================================\n\n");
    
    printf(COLOR_YELLOW "File Operations:" COLOR_RESET "\n");
    printf("  create <file> <perms>    - Create file with octal permissions (e.g., 644)\n");
    printf("  delete <file>            - Delete a file\n");
    printf("  read <file> [off] [len]  - Read file (optional offset and length)\n");
    printf("  write [-a] <file> <data> - Write to file (-a to append)\n");
    printf("  stat <file>              - Show file metadata\n");
    printf("  chmod <mode> <file>      - Change permissions (e.g., 755)\n");
    printf("  open/close <file>        - Open/close file handles\n");
    printf("  seek <file> <off> <whence> - Move file pointer (SET/CUR/END)\n\n");
    
    printf(COLOR_YELLOW "Directory Operations:" COLOR_RESET "\n");
    printf("  create -d <dir>          - Create directory\n");
    printf("  delete -d <dir>          - Delete empty directory\n");
    printf("  cd <dir>                 - Change directory\n");
    printf("  list                     - List directory contents\n");
    printf("  pwd                      - Print working directory\n");
    printf("  copy <src> <dest>        - Copy file\n");
    printf("  move <src> <dest>        - Move file\n");
    printf("  dirinfo [dir]            - Show directory info\n");
    printf("  tree [-i]                - Directory tree (-i shows inodes)\n\n");
    
    printf(COLOR_YELLOW "Link Operations:" COLOR_RESET "\n");
    printf("  ln <target> <link>       - Create hard link\n");
    printf("  ln -s <target> <link>    - Create symbolic link\n\n");
    
    printf(COLOR_YELLOW "System Operations:" COLOR_RESET "\n");
    printf("  format                   - Wipe filesystem (DANGER!)\n");
    printf("  backup [name]            - Create backup\n");
    printf("  restore [name]           - Restore backup\n");
    printf("  showpages [file]         - Show page table info\n");
    printf("  help                     - This help message\n");
    printf("  quit                     - Exit the system\n\n");
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
            int permissions;
            if (sscanf(command, "create %s %o", filename, &permissions) == 2)
            {
                create_file(filename, permissions);
            }
            else
            {
                printf(COLOR_RED "Usage: create <filename> <permissions>\n" COLOR_RESET);
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
                printf(COLOR_RED "File not found\n" COLOR_RESET);
            }
        }
        else
        {
            printf("Usage: seek <filename> <offset> <SET|CUR|END>\n");
        }
    }
    else if (strcmp(command, "tree") == 0)
    {
        // Basic tree command without inodes
        tree_command(0);
    }
    else if (strcmp(command, "tree -i") == 0)
    {
        // Tree command with inodes
        tree_command(1);
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
            change_directory("/");
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
