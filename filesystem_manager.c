#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <pthread.h>
#include <signal.h>

#define MAX_JOBS 10
#define TIME_QUANTUM 2 // Round Robin time slice in seconds
#define BLOCK_SIZE 4
#define TOTAL_BLOCKS 262144 // 1MB / 4B
#define MAX_USERS 2
#define MAX_FILES 100
#define MAX_FILENAME 50
#define MAX_DIRECTORIES 10

// ANSI color codes
#define COLOR_QUEUE "\033[1;33m" // Yellow
#define COLOR_RESET "\033[0m"

#define COLOR_BLUE "\033[1;34m" // Bright blue
#define COLOR_RESET "\033[0m"   // Reset to default color

typedef struct
{
    char *command;
} Job;

typedef struct
{
    char filename[MAX_FILENAME];
    int size;
    int blocks_used;
    char owner[20];
    int permissions; // Now stores Unix-style permissions (e.g., 755)
} File;

typedef struct
{
    char dirname[MAX_FILENAME];
    File files[MAX_FILES];
    int file_count;
    int parent_directory; // Index of the parent directory (-1 for root)
} Directory;

typedef struct
{
    char username[20];
    char password[20];
} User;

User users[MAX_USERS] = {
    {"user1", "pass1"},
    {"user2", "pass2"}};

Directory directories[MAX_DIRECTORIES];
int current_directory = 0;
int block_map[TOTAL_BLOCKS] = {0};
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

Job job_queue[MAX_JOBS];
int front = 0, rear = 0, job_count = 0;
pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t job_available = PTHREAD_COND_INITIALIZER;
int running = 1;

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

void initialize_directories()
{
    // Initialize root directory
    strcpy(directories[0].dirname, "root");
    directories[0].file_count = 0;

    // Create a default directory
    strcpy(directories[1].dirname, "home");
    directories[1].file_count = 0;

    // Create some default files in the root directory
    File file1 = {"readme.txt", 16, 4, "user1", 1};
    File file2 = {"notes.txt", 8, 2, "user2", 0};
    directories[0].files[directories[0].file_count++] = file1;
    directories[0].files[directories[0].file_count++] = file2;

    // Mark blocks as used for default files
    for (int i = 0; i < file1.blocks_used; i++)
        block_map[i] = 1;
    for (int i = file1.blocks_used; i < file1.blocks_used + file2.blocks_used; i++)
        block_map[i] = 1;
}

int allocate_blocks(int blocks_needed)
{
    int start_block = -1;
    int consecutive_blocks = 0;
    for (int i = 0; i < TOTAL_BLOCKS; i++)
    {
        if (block_map[i] == 0)
        {
            if (start_block == -1)
                start_block = i;
            consecutive_blocks++;
            if (consecutive_blocks == blocks_needed)
            {
                for (int j = start_block; j < start_block + blocks_needed; j++)
                {
                    block_map[j] = 1;
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
        block_map[i] = 0;
    }
}

int create_file(char *filename, int size, char *owner, int permissions)
{
    // Validate permission number (000-777)
    if (permissions < 0 || permissions > 0777)
    {
        printf("Error: Permissions must be between 000 and 777\n");
        return -1;
    }

    pthread_mutex_lock(&mutex);
    int blocks_needed = size / BLOCK_SIZE + (size % BLOCK_SIZE != 0);
    int start_block = allocate_blocks(blocks_needed);
    if (start_block == -1)
    {
        pthread_mutex_unlock(&mutex);
        printf("Error: Not enough disk space\n");
        return -1; // Not enough space
    }

    File new_file;
    strcpy(new_file.filename, filename);
    new_file.size = size;
    new_file.blocks_used = blocks_needed;
    strcpy(new_file.owner, owner);
    new_file.permissions = permissions;
    directories[current_directory].files[directories[current_directory].file_count++] = new_file;

    pthread_mutex_unlock(&mutex);
    return 0;
}

int create_directory(char *dirname)
{
    pthread_mutex_lock(&mutex);
    for (int i = 0; i < MAX_DIRECTORIES; i++)
    {
        if (strlen(directories[i].dirname) == 0)
        { // Find an empty slot
            strcpy(directories[i].dirname, dirname);
            directories[i].file_count = 0;
            directories[i].parent_directory = current_directory; // Set parent directory
            pthread_mutex_unlock(&mutex);
            return 0; // Directory created successfully
        }
    }
    pthread_mutex_unlock(&mutex);
    return -1; // No space for new directory
}

void delete_file(char *filename)
{
    pthread_mutex_lock(&mutex);
    for (int i = 0; i < directories[current_directory].file_count; i++)
    {
        if (strcmp(directories[current_directory].files[i].filename, filename) == 0)
        {
            free_blocks(i, directories[current_directory].files[i].blocks_used);
            for (int j = i; j < directories[current_directory].file_count - 1; j++)
            {
                directories[current_directory].files[j] = directories[current_directory].files[j + 1];
            }
            directories[current_directory].file_count--;
            break;
        }
    }
    pthread_mutex_unlock(&mutex);
}

void list_files()
{
    printf("Contents of directory '%s':\n", directories[current_directory].dirname);

    // List directories inside the current directory
    for (int i = 0; i < MAX_DIRECTORIES; i++)
    {
        if (i != current_directory && directories[i].parent_directory == current_directory && strlen(directories[i].dirname) > 0)
        {
            printf("[D] %s\n", directories[i].dirname);
        }
    }

    // List files in the current directory
    for (int i = 0; i < directories[current_directory].file_count; i++)
    {
        printf("[F] %s (Size: %d, Owner: %s, Permissions: %04o)\n",
               directories[current_directory].files[i].filename,
               directories[current_directory].files[i].size,
               directories[current_directory].files[i].owner,
               directories[current_directory].files[i].permissions);
    }
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
        if (strcmp(users[i].username, username) == 0 && strcmp(users[i].password, password) == 0)
        {
            return i;
        }
    }
    return -1;
}

// Helper function to find a directory by name
int find_directory_index(const char *dirname)
{
    for (int i = 0; i < MAX_DIRECTORIES; i++)
    {
        if (strcmp(directories[i].dirname, dirname) == 0)
        {
            return i;
        }
    }
    return -1;
}

// Copy file to another directory
void copy_file_to_dir(const char *filename, const char *dirname)
{
    pthread_mutex_lock(&mutex);

    // Find source file
    File *src_file = NULL;
    for (int i = 0; i < directories[current_directory].file_count; i++)
    {
        if (strcmp(directories[current_directory].files[i].filename, filename) == 0)
        {
            src_file = &directories[current_directory].files[i];
            break;
        }
    }

    if (!src_file)
    {
        printf("Error: File '%s' not found\n", filename);
        pthread_mutex_unlock(&mutex);
        return;
    }

    // Find target directory
    int target_dir = find_directory_index(dirname);
    if (target_dir == -1)
    {
        printf("Error: Directory '%s' not found\n", dirname);
        pthread_mutex_unlock(&mutex);
        return;
    }

    // Check if file already exists in target
    for (int i = 0; i < directories[target_dir].file_count; i++)
    {
        if (strcmp(directories[target_dir].files[i].filename, filename) == 0)
        {
            printf("Error: File already exists in target directory\n");
            pthread_mutex_unlock(&mutex);
            return;
        }
    }

    // Create copy in target directory
    if (directories[target_dir].file_count >= MAX_FILES)
    {
        printf("Error: Target directory full\n");
        pthread_mutex_unlock(&mutex);
        return;
    }

    File new_file = *src_file; // Copy all file attributes
    directories[target_dir].files[directories[target_dir].file_count++] = new_file;

    printf("Copied '%s' to directory '%s'\n", filename, dirname);
    pthread_mutex_unlock(&mutex);
}

// Move file to another directory
void move_file_to_dir(const char *filename, const char *dirname)
{
    pthread_mutex_lock(&mutex);

    // Find source file index
    int file_index = -1;
    for (int i = 0; i < directories[current_directory].file_count; i++)
    {
        if (strcmp(directories[current_directory].files[i].filename, filename) == 0)
        {
            file_index = i;
            break;
        }
    }

    if (file_index == -1)
    {
        printf("Error: File '%s' not found\n", filename);
        pthread_mutex_unlock(&mutex);
        return;
    }

    // Find target directory
    int target_dir = find_directory_index(dirname);
    if (target_dir == -1)
    {
        printf("Error: Directory '%s' not found\n", dirname);
        pthread_mutex_unlock(&mutex);
        return;
    }

    // Check if target has space
    if (directories[target_dir].file_count >= MAX_FILES)
    {
        printf("Error: Target directory full\n");
        pthread_mutex_unlock(&mutex);
        return;
    }

    // Check if file exists in target
    for (int i = 0; i < directories[target_dir].file_count; i++)
    {
        if (strcmp(directories[target_dir].files[i].filename, filename) == 0)
        {
            printf("Error: File already exists in target directory\n");
            pthread_mutex_unlock(&mutex);
            return;
        }
    }

    // Perform the move (copy + delete)
    File file_to_move = directories[current_directory].files[file_index];
    directories[target_dir].files[directories[target_dir].file_count++] = file_to_move;

    // Remove from source
    for (int i = file_index; i < directories[current_directory].file_count - 1; i++)
    {
        directories[current_directory].files[i] = directories[current_directory].files[i + 1];
    }
    directories[current_directory].file_count--;

    printf("Moved '%s' to directory '%s'\n", filename, dirname);
    pthread_mutex_unlock(&mutex);
}

void change_directory(char *dirname)
{
    int dir_index = find_directory_index(dirname);
    if (dir_index != -1)
    {
        current_directory = dir_index;
        printf("Changed directory to '%s'\n", dirname);
    }
    else
    {
        printf("Directory '%s' not found\n", dirname);
    }
}

void help()
{
    printf("Available commands:\n");
    printf("create <filename> <size> <permissions> - Create a new file\n");
    printf("create -d <dirname> - Create a new directory\n");
    printf("delete <filename> - Delete a file\n");
    printf("copy <source> <destination> - Copy a file\n");
    printf("move <source> <destination> - Move a file\n");
    printf("list - List files in the current directory\n");
    printf("cd <dirname> - Change directory\n");
    printf("help - Show this help message\n");
    printf("quit - Exit the terminal\n");
}

void execute_job(Job job)
{
    char command[256];
    strcpy(command, job.command);

    // Only show queue if there are multiple commands
    if (job_count > 0)
    {
        printf("\n");
        // print_queue(0);
    }

    if (strncmp(command, "create", 6) == 0)
    {
        // Handle file or directory creation
        if (strstr(command, "-d") != NULL)
        {
            // Directory creation
            char dirname[MAX_FILENAME];
            sscanf(command, "create -d %s", dirname);
            if (create_directory(dirname) == 0)
            {
                printf("Directory '%s' created successfully\n", dirname);
            }
            else
            {
                printf("Error: Could not create directory\n");
            }
        }
        else
        {
            // File creation
            char filename[MAX_FILENAME];
            int size, permissions;
            if (sscanf(command, "create %s %d %o", filename, &size, &permissions) == 3)
            {
                if (create_file(filename, size, users[0].username, permissions) == 0)
                {
                    printf("File '%s' created successfully with permissions %04o\n",
                           filename, permissions);
                }
            }
            else
            {
                printf("Usage: create <filename> <size> <permissions>\n");
                printf("Example: create myfile.txt 1024 644\n");
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
        sscanf(command, "cd %s", dirname);
        change_directory(dirname);
    }
    else if (strcmp(command, "help") == 0)
    {
        help();
    }
    else if (strcmp(command, "delete") == 0)
    {
        char filename[MAX_FILENAME];
        sscanf(command, "delete %s", filename);
        delete_file(filename);
        printf("File '%s' deleted\n", filename);
    }
    // Add to execute_job function:
    else if (strncmp(command, "copy", 4) == 0)
    {
        char filename[MAX_FILENAME], dirname[MAX_FILENAME];
        if (sscanf(command, "copy %s %s", filename, dirname) == 2)
        {
            copy_file_to_dir(filename, dirname);
        }
        else
        {
            printf("Usage: copy <filename> <directory>\n");
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
            printf("Usage: move <filename> <directory>\n");
        }
    }
    else
    {
        // Handle external commands without waiting
        pid_t pid = fork();
        if (pid == 0)
        { // Child process
            char *args[] = {"/bin/sh", "-c", command, NULL};
            execvp(args[0], args);
            perror("execvp failed");
            exit(1);
        }
        else if (pid < 0)
        {
            perror("fork failed");
        }
        // Parent continues without waiting
    }
    free(job.command); // Free allocated memory
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
        printf("Job queue is full!\n");
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
    printf("\nShutting down scheduler...\n");
    cleanup();
    exit(0);
}

int main()
{
    signal(SIGINT, handle_signal);
    pthread_t scheduler_thread;
    pthread_create(&scheduler_thread, NULL, scheduler, NULL);

    initialize_directories();
    int user_index = login();
    if (user_index == -1)
    {
        printf("Login failed\n");
        return 1;
    }

    char input[256];
    while (1)
    {

        // Modified prompt:
        printf(COLOR_BLUE "%s@%s> " COLOR_RESET,
               users[user_index].username,
               directories[current_directory].dirname);
        fflush(stdout); // Ensure prompt is displayed

        if (!fgets(input, sizeof(input), stdin))
        {
            break; // Handle EOF (Ctrl+D)
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
