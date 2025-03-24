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
#define COLOR_QUEUE "\033[1;33m"
#define COLOR_BLUE "\033[1;34m"
#define COLOR_GREEN "\033[1;32m"
#define COLOR_RED "\033[1;31m"
#define COLOR_RESET "\033[0m"

typedef struct
{
    char *command;
} Job;

typedef struct
{
    char filename[MAX_FILENAME];
    int size;
    int blocks_used;
    int start_block;
    char owner[20];
    int permissions;
    time_t creation_time;
    time_t modification_time;
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
    strcpy(fs_state.users[0].username, "user1");
    strcpy(fs_state.users[0].password, "pass1");
    strcpy(fs_state.users[1].username, "user2");
    strcpy(fs_state.users[1].password, "pass2");

    // Create some default files
    File file1 = {"readme.txt", 16, 4, 0, "user1", 0644, time(NULL), time(NULL)};
    File file2 = {"notes.txt", 8, 2, 4, "user2", 0600, time(NULL), time(NULL)};
    fs_state.directories[0].files[fs_state.directories[0].file_count++] = file1;
    fs_state.directories[0].files[fs_state.directories[0].file_count++] = file2;

    // Mark blocks as used
    for (int i = 0; i < file1.blocks_used + file2.blocks_used; i++)
    {
        fs_state.block_map[i] = 1;
    }
}

void save_state()
{
    FILE *fp = fopen(STORAGE_FILE, "wb");
    if (fp)
    {
        fwrite(&fs_state, sizeof(FileSystemState), 1, fp);
        fclose(fp);
    }
}

void load_state()
{
    FILE *fp = fopen(STORAGE_FILE, "rb");
    if (fp)
    {
        fread(&fs_state, sizeof(FileSystemState), 1, fp);
        fclose(fp);
    }
    else
    {
        initialize_directories();
    }
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

    int blocks_needed = size / BLOCK_SIZE + (size % BLOCK_SIZE != 0);
    int start_block = allocate_blocks(blocks_needed);
    if (start_block == -1)
    {
        printf(COLOR_RED "Error: Not enough disk space\n" COLOR_RESET);
        pthread_mutex_unlock(&mutex);
        return -1;
    }

    File new_file;
    strcpy(new_file.filename, filename);
    new_file.size = size;
    new_file.blocks_used = blocks_needed;
    new_file.start_block = start_block;
    strcpy(new_file.owner, owner);
    new_file.permissions = permissions;
    new_file.creation_time = time(NULL);
    new_file.modification_time = new_file.creation_time;

    fs_state.directories[fs_state.current_directory].files[fs_state.directories[fs_state.current_directory].file_count++] = new_file;

    save_state();
    printf(COLOR_GREEN "File '%s' created successfully with permissions %04o\n" COLOR_RESET,
           filename, permissions);
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

void delete_file(char *filename)
{
    pthread_mutex_lock(&mutex);
    int found = 0;
    for (int i = 0; i < fs_state.directories[fs_state.current_directory].file_count; i++)
    {
        if (strcmp(fs_state.directories[fs_state.current_directory].files[i].filename, filename) == 0)
        {
            free_blocks(fs_state.directories[fs_state.current_directory].files[i].start_block,
                        fs_state.directories[fs_state.current_directory].files[i].blocks_used);

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

void list_files()
{
    pthread_mutex_lock(&mutex);
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
    pthread_mutex_lock(&mutex);
    int found = -1;

    if (strcmp(dirname, "..") == 0)
    {
        if (fs_state.directories[fs_state.current_directory].parent_directory != -1)
        {
            found = fs_state.directories[fs_state.current_directory].parent_directory;
        }
    }
    else
    {
        for (int i = 0; i < MAX_DIRECTORIES; i++)
        {
            if (strcmp(fs_state.directories[i].dirname, dirname) == 0 &&
                fs_state.directories[i].parent_directory == fs_state.current_directory)
            {
                found = i;
                break;
            }
        }
    }

    if (found != -1)
    {
        fs_state.current_directory = found;
        printf(COLOR_GREEN "Changed directory to '%s'\n" COLOR_RESET,
               fs_state.directories[fs_state.current_directory].dirname);
    }
    else
    {
        printf(COLOR_RED "Error: Directory '%s' not found\n" COLOR_RESET, dirname);
    }
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
    printf("create <filename> <size> <permissions> - Create a new file (permissions in octal, e.g., 644)\n");
    printf("create -d <dirname>                    - Create a new directory\n");
    printf("delete <filename>                      - Delete a file\n");
    printf("copy <file> <directory>                - Copy a file to another directory\n");
    printf("move <file> <directory>                - Move a file to another directory\n");
    printf("ln -s <source> <link>                  - Create symbolic link\n");
    printf("ln <source> <link>                     - Create hard link\n");
    printf("chmod <mode> <file>                    - Change file permissions\n");
    printf("stat <file>                            - Show file information\n");
    printf("list                                   - List directory contents\n");
    printf("cd <dirname>                           - Change directory\n");
    printf("help                                   - Show this help message\n");
    printf("quit                                   - Exit the terminal\n\n");
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
    else if (strncmp(command, "cd", 2) == 0)
    {
        char dirname[MAX_FILENAME];
        if (sscanf(command, "cd %s", dirname) == 1)
        {
            change_directory(dirname);
        }
        else
        {
            printf(COLOR_RED "Usage: cd <dirname>\n" COLOR_RESET);
        }
    }
    else if (strcmp(command, "help") == 0)
    {
        help();
    }
    else if (strncmp(command, "delete", 6) == 0)
    {
        char filename[MAX_FILENAME];
        if (sscanf(command, "delete %s", filename) == 1)
        {
            delete_file(filename);
        }
        else
        {
            printf(COLOR_RED "Usage: delete <filename>\n" COLOR_RESET);
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
    printf(COLOR_QUEUE "Command Queue:\n" COLOR_RESET);
    if (job_count == 0)
    {
        printf(COLOR_QUEUE "  [Empty]\n" COLOR_RESET);
        return;
    }

    for (int i = 0; i < job_count; i++)
    {
        int idx = (front + i) % MAX_JOBS;
        if (i == current_job_index)
        {
            printf(COLOR_QUEUE "  > %s (Running)\n" COLOR_RESET, job_queue[idx].command);
        }
        else
        {
            printf(COLOR_QUEUE "  - %s\n" COLOR_RESET, job_queue[idx].command);
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

        if (!fgets(input, sizeof(input), stdin)) {
            break;
        }

        input[strcspn(input, "\n")] = 0;

        if (strcmp(input, "quit") == 0) {
            handle_signal(SIGINT);
        }
        else if (strchr(input, '|') != NULL) {
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
