#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <pthread.h>
#include <errno.h>
#include <sys/file.h>
#include <termios.h>

#define FS_SIZE 1048576 // 1MB filesystem
#define BLOCK_SIZE 4096 // 4KB per block
#define MAX_FILES 100
#define MAX_FILENAME 32
#define MAX_INPUT_SIZE 128
#define MAX_USERS 10
#define MAX_USERNAME 32
#define MAX_PASSWORD 32

// Structure for file metadata
typedef struct {
    char name[MAX_FILENAME];
    int size;
    int block_index;
    int is_directory;
    mode_t permissions;
    uid_t owner;  // Add owner to the file (current user)
    char content[BLOCK_SIZE];       // Content of the file (only relevant if it's a regular file)
} FileEntry;

// Structure for the filesystem
typedef struct {
    FileEntry files[MAX_FILES];
    int free_blocks[FS_SIZE / BLOCK_SIZE];
} FileSystem;

FileSystem fs;
const char *fs_filename = "filesystem.img";

// Structure for user information
typedef struct {
    char username[MAX_USERNAME];
    char password[MAX_PASSWORD];
    uid_t user_id;
} User;

// Global user information
User users[MAX_USERS];
int user_count = 0;
uid_t current_user = -1;

pthread_mutex_t fs_lock = PTHREAD_MUTEX_INITIALIZER;  // Mutex for thread safety

// Flag to check if the filesystem is initialized
int fs_initialized = 0;

// Track current directory
int current_dir_index = 0; // Default to root directory

// Dummy users for login
void add_default_users() {
    // Predefined users (you can add more here)
    strncpy(users[user_count].username, "ikram", MAX_USERNAME);
    strncpy(users[user_count].password, "ikrampass", MAX_PASSWORD);
    users[user_count].user_id = user_count;
    user_count++;

    strncpy(users[user_count].username, "ines", MAX_USERNAME);
    strncpy(users[user_count].password, "inespass", MAX_PASSWORD);
    users[user_count].user_id = user_count;
    user_count++;


    strncpy(users[user_count].username, "ali", MAX_USERNAME);
    strncpy(users[user_count].password, "alipass", MAX_PASSWORD);
    users[user_count].user_id = user_count;
    user_count++;
}

// Function to lock the filesystem file (flock)
int lock_fs_file() {
    int fd = open(fs_filename, O_RDWR);
    if (fd == -1) {
        perror("Failed to open filesystem file");
        return -1;
    }
    if (flock(fd, LOCK_EX) == -1) {
        perror("Failed to lock filesystem file");
        close(fd);
        return -1;
    }
    return fd;
}

// Unlock the filesystem file
void unlock_fs_file(int fd) {
    if (flock(fd, LOCK_UN) == -1) {
        perror("Failed to unlock filesystem file");
    }
    close(fd);
}

// Initialize the filesystem
void init_fs() {
    // Check if the filesystem file already exists
    if (access(fs_filename, F_OK) == 0) {
        printf("Filesystem already initialized.\n");
        return;
    }

    // Create the filesystem file
    memset(&fs, 0, sizeof(FileSystem));
    for (int i = 0; i < (FS_SIZE / BLOCK_SIZE); i++) {
        fs.free_blocks[i] = 1; // All blocks are free initially
    }

    // Manually add default directories to the filesystem
    strncpy(fs.files[0].name, "home", MAX_FILENAME);         // /home directory
    fs.files[0].is_directory = 1;
    fs.files[0].permissions = 0755;
    fs.files[0].owner = current_user;  // Default owner

    strncpy(fs.files[1].name, "home/ikram", MAX_FILENAME);    // /home/ikram directory
    fs.files[1].is_directory = 1;
    fs.files[1].permissions = 0700;
    fs.files[1].owner = current_user;

    strncpy(fs.files[2].name, "etc", MAX_FILENAME);           // /etc directory
    fs.files[2].is_directory = 1;
    fs.files[2].permissions = 0755;
    fs.files[2].owner = current_user;

    strncpy(fs.files[3].name, "bin", MAX_FILENAME);           // /bin directory
    fs.files[3].is_directory = 1;
    fs.files[3].permissions = 0755;
    fs.files[3].owner = current_user;

    strncpy(fs.files[4].name, "tmp", MAX_FILENAME);           // /tmp directory
    fs.files[4].is_directory = 1;
    fs.files[4].permissions = 0777;
    fs.files[4].owner = current_user;

    // Create the filesystem file on disk
    FILE *fp = fopen(fs_filename, "wb");
    if (fp == NULL) {
        perror("Failed to initialize filesystem");
        return;
    }

    fwrite(&fs, sizeof(FileSystem), 1, fp);
    fclose(fp);

    fs_initialized = 1; // Mark the filesystem as initialized
    printf("Filesystem initialized with default directories.\n");
}


void load_fs() {
    FILE *fp = fopen(fs_filename, "rb");
    if (fp) {
        fread(&fs, sizeof(FileSystem), 1, fp);
        fclose(fp);
        fs_initialized = 1;  // Mark filesystem as initialized
    }
}

// Save filesystem to disk
void save_fs() {
    int fd = lock_fs_file();
    if (fd == -1) return;

    FILE *fp = fopen(fs_filename, "wb");
    fwrite(&fs, sizeof(FileSystem), 1, fp);
    fclose(fp);

    unlock_fs_file(fd);
}

int find_file(const char *name) {
    for (int i = 0; i < MAX_FILES; i++) {
        if (strcmp(fs.files[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

void copy_file(const char *src, const char *dest) {
    int src_index = find_file(src);
    if (src_index == -1) {
        printf("Source file not found.\n");
        return;
    }
    
    int dest_index = find_file(dest);
    if (dest_index != -1) {
        printf("Destination file already exists.\n");
        return;
    }
    
    for (int i = 0; i < MAX_FILES; i++) {
        if (fs.files[i].name[0] == '\0') {
            strncpy(fs.files[i].name, dest, MAX_FILENAME - 1);
            fs.files[i].size = fs.files[src_index].size;
            fs.files[i].is_directory = fs.files[src_index].is_directory;
            fs.files[i].permissions = fs.files[src_index].permissions;
            fs.files[i].owner = fs.files[src_index].owner;
            strncpy(fs.files[i].content, fs.files[src_index].content, BLOCK_SIZE);
            save_fs();
            printf("File copied successfully.\n");
            return;
        }
    }
    printf("No space to copy file.\n");
}

void move_file(const char *src, const char *dest) {
    int src_index = find_file(src);
    if (src_index == -1) {
        printf("Source file not found.\n");
        return;
    }
    
    int dest_index = find_file(dest);
    if (dest_index != -1) {
        printf("Destination file already exists.\n");
        return;
    }
    
    strncpy(fs.files[src_index].name, dest, MAX_FILENAME - 1);
    save_fs();
    printf("File moved successfully.\n");
}


// User authentication: prompt for username and password
int authenticate_user() {
    char username[MAX_USERNAME];
    char password[MAX_PASSWORD];
    int attempts = 0;

    struct termios oldt, newt;

    while (attempts < 3) {
        printf("Enter username: ");
        fgets(username, sizeof(username), stdin);
        username[strcspn(username, "\n")] = 0;

        // Disable password echo
        tcgetattr(STDIN_FILENO, &oldt);
        newt = oldt;
        newt.c_lflag &= ~ECHO;
        tcsetattr(STDIN_FILENO, TCSANOW, &newt);

        printf("Enter password: ");
        fgets(password, sizeof(password), stdin);
        password[strcspn(password, "\n")] = 0;

        // Restore terminal settings
        tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
        printf("\n");

        for (int i = 0; i < user_count; i++) {
            if (strcmp(users[i].username, username) == 0 && strcmp(users[i].password, password) == 0) {
                current_user = users[i].user_id;
                printf("User '%s' logged in successfully!\n", username);
                return 1;
            }
        }

        attempts++;
        printf("Authentication failed. Attempts left: %d\n", 3 - attempts);
    }

    printf("Too many failed attempts. Exiting.\n");
    exit(EXIT_FAILURE);  // Exit program if login fails
}

// Check if the current user has permission to access the file
int has_permission(FileEntry *file, mode_t permission) {
    if (file->owner != current_user) {
        return 0; // Only the owner has permission
    }
    return (file->permissions & permission) == permission;
}

void quit() {
    printf("Exiting...\n");
    exit(EXIT_SUCCESS);
}

// Create a file or directory
void create_file(const char *name, int is_directory, mode_t permissions) {
    int fd = open(fs_filename, O_RDWR);
    if (fd == -1) {
        perror("Failed to open filesystem");
        return;
    }

    flock(fd, LOCK_EX);

    for (int i = 0; i < MAX_FILES; i++) {
        if (fs.files[i].name[0] == '\0') {
            strncpy(fs.files[i].name, name, MAX_FILENAME - 1);
            fs.files[i].name[MAX_FILENAME - 1] = '\0';
            fs.files[i].size = 0;
            fs.files[i].is_directory = is_directory;
            fs.files[i].permissions = permissions;
            fs.files[i].owner = current_user;
            fs.files[i].block_index = -1;
            strcpy(fs.files[i].content, "Hello World!");
            FILE *fp = fopen(fs_filename, "wb");
            if (fp) {
                fwrite(&fs, sizeof(FileSystem), 1, fp);
                fclose(fp);
            }
            printf("%s '%s' created with permissions %o.\n", is_directory ? "Directory" : "File", name, permissions);
            flock(fd, LOCK_UN);
            close(fd);
            return;
        }
    }

    flock(fd, LOCK_UN);
    close(fd);
    printf("Error: Max file limit reached.\n");
}


void list_files() {
    if (!fs_initialized) {
        printf("Error: Filesystem is not initialized. Please run the 'init' command first.\n");
        return;
    }

    if (current_user == -1 || current_user >= user_count) {  // Check if the user is valid
        printf("Error: No user logged in or invalid user.\n");
        return;
    }

    // Reload the filesystem from disk to get the most up-to-date state
    load_fs();

    int fd = lock_fs_file();
    if (fd == -1) return; // Ensure the lock is obtained

    printf("Files in the filesystem:\n");
    printf(".  ..  (Home directory for user %s)\n", users[current_user].username);

    for (int i = 0; i < MAX_FILES; i++) {
        if (fs.files[i].name[0] != '\0') { // Ensure valid file entry
            FileEntry *file = &fs.files[i];
            printf("[%s] %-20s (Permissions: %o, Owner: %d)\n",
                   file->is_directory ? "DIR" : "FILE", file->name, file->permissions, file->owner);
        }
    }

    unlock_fs_file(fd); // Always unlock after reading
}

// Function to modify file content
void modify_file(const char *name, const char *new_content) {
    int fd = open(fs_filename, O_RDWR);
    if (fd == -1) {
        perror("Failed to open filesystem");
        return;
    }

    flock(fd, LOCK_EX);

    // Search for the file in the filesystem
    int file_found = 0;
    for (int i = 0; i < MAX_FILES; i++) {
        if (strcmp(fs.files[i].name, name) == 0) {
            file_found = 1;
            if (fs.files[i].is_directory) {
                printf("%s is a directory, not a regular file.\n", name);
            } else if (has_permission(&fs.files[i], W_OK)) {
                strncpy(fs.files[i].content, new_content, BLOCK_SIZE - 1);
                fs.files[i].content[BLOCK_SIZE - 1] = '\0'; // Ensure null termination
                printf("File '%s' modified. New content: %s\n", name, fs.files[i].content);
                FILE *fp = fopen(fs_filename, "wb");
                if (fp) {
                    fwrite(&fs, sizeof(FileSystem), 1, fp);
                    fclose(fp);
                }
            } else {
                printf("Permission denied to modify the file.\n");
            }
            break;
        }
    }

    if (!file_found) {
        printf("File '%s' not found.\n", name);
    }

    flock(fd, LOCK_UN);
    close(fd);
}

// Function to read file content
void read_file(const char *name) {
    int fd = open(fs_filename, O_RDWR);
    if (fd == -1) {
        perror("Failed to open filesystem");
        return;
    }

    flock(fd, LOCK_EX);

    // Search for the file in the filesystem
    int file_found = 0;
    for (int i = 0; i < MAX_FILES; i++) {
        if (strcmp(fs.files[i].name, name) == 0) {
            file_found = 1;
            if (fs.files[i].is_directory) {
                printf("%s is a directory, not a regular file.\n", name);
            } else if (has_permission(&fs.files[i], R_OK)) {
                printf("File content: %s\n", fs.files[i].content);
            } else {
                printf("Permission denied to read the file.\n");
            }
            break;
        }
    }

    if (!file_found) {
        printf("File '%s' not found.\n", name);
    }

    flock(fd, LOCK_UN);
    close(fd);
}

// Delete a file or directory
void delete_file(const char *name) {
    int fd = open(fs_filename, O_RDWR);
    if (fd == -1) {
        perror("Failed to open filesystem");
        return;
    }

    flock(fd, LOCK_EX);

    for (int i = 0; i < MAX_FILES; i++) {
        if (strcmp(fs.files[i].name, name) == 0) {
            fs.files[i].name[0] = '\0';
            fs.files[i].block_index = -1;
            FILE *fp = fopen(fs_filename, "wb");
            if (fp) {
                fwrite(&fs, sizeof(FileSystem), 1, fp);
                fclose(fp);
            }
            printf("'%s' deleted successfully.\n", name);
            flock(fd, LOCK_UN);
            close(fd);
            return;
        }
    }

    flock(fd, LOCK_UN);
    close(fd);
    printf("Error: File not found.\n");
}

// Change directory
void cd(const char *dir) {
    int found = 0;
    for (int i = 0; i < MAX_FILES; i++) {
        if (fs.files[i].name[0] != '\0' && fs.files[i].is_directory &&
            strcmp(fs.files[i].name, dir) == 0) {
            current_dir_index = i; // Change to the specified directory
            found = 1;
            break;
        }
    }

    if (!found) {
        printf("Directory not found. Available directories:\n");
        for (int i = 0; i < MAX_FILES; i++) {
            if (fs.files[i].name[0] != '\0' && fs.files[i].is_directory) {
                printf("%s\n", fs.files[i].name);
            }
        }
    }
}

/// Change the permissions of an existing file
void chmod_file(const char *name, mode_t new_permissions) {
    int fd = open(fs_filename, O_RDWR);
    if (fd == -1) {
        perror("Failed to open filesystem");
        return;
    }

    flock(fd, LOCK_EX);

    // Search for the file in the filesystem
    int file_found = 0;
    for (int i = 0; i < MAX_FILES; i++) {
        if (strcmp(fs.files[i].name, name) == 0) {
            file_found = 1;

            // Check if the current user has permission to change the file's permissions
            if (fs.files[i].owner != current_user) {
                printf("Error: You do not have permission to modify this file's permissions.\n");
                flock(fd, LOCK_UN);
                close(fd);
                return;
            }

            // Update the file's permissions
            fs.files[i].permissions = new_permissions;

            // Save the updated filesystem to the file after permission change
            FILE *fp = fopen(fs_filename, "wb");
            if (fp) {
                fwrite(&fs, sizeof(FileSystem), 1, fp);
                fclose(fp);
            }

            printf("Permissions of '%s' changed to %o.\n", name, new_permissions);

            flock(fd, LOCK_UN);
            close(fd);
            return;
        }
    }

    // If the file was not found in the filesystem
    if (!file_found) {
        printf("Error: File '%s' not found.\n", name);
    }

    flock(fd, LOCK_UN);
    close(fd);
}


void helplist() {
    printf("Available commands:\n");
    printf("  cd <directory_name>                   - Change current directory\n");
    printf("  create <filename> <permissions> [-d]  - Create a file or directory\n");
    printf("  list                                  - List all files in the filesystem\n");
    printf("  delete <filename>                     - Delete a file or directory\n");
    printf("  chmod <filename> <permissions>        - Change file permissions\n");
    printf("  read <filename>                       - Read file content\n");
    printf("  modify <filename> <new_content>       - Modify file content\n");
    printf("  quit                                  - Exit the shell\n");
}

// Function to display help for specific commands
void help(const char *command) {
    if (strcmp(command, "create") == 0) {
        printf("Usage: create <filename> <permissions> [-f|-d]\n");
        printf("  -f: Create a file\n");
        printf("  -d: Create a directory\n");
    } 
    else if (strcmp(command, "list") == 0) {
        printf("Usage: list\n");
        printf("  List all files in the filesystem\n");
    }
    else if (strcmp(command, "delete") == 0) {
        printf("Usage: delete <filename>\n");
        printf("  Delete a file or directory\n");
    }
    else if (strcmp(command, "chmod") == 0) {
        printf("Usage: chmod <filename> <permissions>\n");
        printf("  Change file permissions\n");
    }
    else {
        printf("Invalid command. Available commands:\n");
        helplist();

        }
}



void execute_command(char *command) {
    char *token = strtok(command, " ");
    if (token == NULL) return;

    // Handle the 'help' command
    if (strcmp(token, "help") == 0) {
        char *arg = strtok(NULL, " ");
        if (arg) {
            help(arg); // Show help for a specific command
        } else {
            helplist(); // Show the general list of commands
        }
    }
    else if (strcmp(token, "list") == 0) {
        char *arg = strtok(NULL, " ");
        if (arg && strcmp(arg, "-h") == 0) {
            help("list");
        } else {
            list_files();
        }
    } 
    else if (strcmp(token, "create") == 0) {
        char *filename = strtok(NULL, " ");
        char *perm_str = strtok(NULL, " ");
        int is_directory = 0;
        if (filename && perm_str) {
            char *flag;
            while ((flag = strtok(NULL, " ")) != NULL) {
                if (strcmp(flag, "-d") == 0) is_directory = 1;
            }
            create_file(filename, is_directory, strtol(perm_str, NULL, 8));
        } else {
            help("create");
        }
    }
    else if (strcmp(token, "delete") == 0) {
        char *filename = strtok(NULL, " ");
        if (filename) {
            delete_file(filename);
        } else {
            help("delete");
        }
    }
    else if (strcmp(token, "cd") == 0) {
        char *dir = strtok(NULL, " ");
        if (dir) {
            cd(dir);
        } else {
            printf("Usage: cd <directory>\n");
        }
    }
    else if (strcmp(token, "chmod") == 0) {
        char *filename = strtok(NULL, " ");
        char *permissions = strtok(NULL, " ");
        if (filename && permissions) {
            chmod_file(filename, strtol(permissions, NULL, 8));
        } else {
            help("chmod");
        }
    }
    else if (strcmp(token, "modify") == 0) {
        // Handle modify command
        char *filename = strtok(NULL, " ");
        char *new_content = strtok(NULL, "");
        if (filename && new_content) {
            modify_file(filename, new_content); // Modify the file with new content
        } else {
            printf("Usage: modify <filename> <new_content>\n");
        }
    }
    else if (strcmp(token, "read") == 0) {
        // Handle read command
        char *filename = strtok(NULL, " ");
        if (filename) {
            read_file(filename); // Read the content of the specified file
        } else {
            printf("Usage: read <filename>\n");
        }
    }
    else if (strcmp(token, "quit") == 0) {
        quit();
    }

    else if (strcmp(token, "copy") == 0) {
        char *src = strtok(NULL, " ");
        char *dest = strtok(NULL, " ");
        if (src && dest) {
            copy_file(src, dest);
        } else {
            printf("Usage: copy <source> <destination>\n");
        }
    }
    else if (strcmp(token, "move") == 0) {
        char *src = strtok(NULL, " ");
        char *dest = strtok(NULL, " ");
        if (src && dest) {
            move_file(src, dest);
        } else {
            printf("Usage: move <source> <destination>\n");
        }
    }
    else {
        printf("Invalid command. Type help to see all available commands.\n");
    }
}


int main() {
    add_default_users();

    if (authenticate_user() == 0) return 0;

    if (access(fs_filename, F_OK) == 0) {
        load_fs();
    } else {
        init_fs();
    }

    char command[MAX_INPUT_SIZE];
    while (1) {
        printf("\033[0;32m%s> \033[0m", users[current_user].username);
        fgets(command, MAX_INPUT_SIZE, stdin);
        command[strcspn(command, "\n")] = 0;  // Remove newline
        execute_command(command);
    }

    return 0;
}

