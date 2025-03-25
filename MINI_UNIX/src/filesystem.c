#include "../include/filesystem.h"
#include "../include/paging.h"

FileSystemState fs_state;
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
unsigned char page_bitmap[TOTAL_PAGES / 8];


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


void initialize_directories()
{
    if (strlen(fs_state.directories[0].dirname) == 0)
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
        strcpy(fs_state.users[0].username, "ikram");
        strcpy(fs_state.users[0].password, "ikrampass");
        strcpy(fs_state.users[1].username, "ines");
        strcpy(fs_state.users[1].password, "inespass");
        strcpy(fs_state.users[1].username, "ali");
        strcpy(fs_state.users[1].password, "alipass");

        // Create some default files
        memset(page_bitmap, 0, sizeof(page_bitmap));

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
        fclose(fp);
    }
    else
    {
        printf("No existing filesystem found, initializing new one\n");
        initialize_directories();
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


int create_file(char *filename, int size, char *owner, int permissions) {
    pthread_mutex_lock(&mutex);

    // Check if file exists
    for (int i = 0; i < fs_state.directories[fs_state.current_directory].file_count; i++) {
        if (strcmp(fs_state.directories[fs_state.current_directory].files[i].filename, filename) == 0) {
            printf("Error: File already exists\n");
            pthread_mutex_unlock(&mutex);
            return -1;
        }
    }

    // Validate size
    if (size <= 0) {
        printf("Error: Invalid file size\n");
        pthread_mutex_unlock(&mutex);
        return -1;
    }

    // Calculate needed pages (round up)
    int pages_needed = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    if (pages_needed <= 0) pages_needed = 1;  // Minimum 1 page

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
void delete_file(char *filename) {
    pthread_mutex_lock(&mutex);
    int found = 0;
    for (int i = 0; i < fs_state.directories[fs_state.current_directory].file_count; i++) {
        if (strcmp(fs_state.directories[fs_state.current_directory].files[i].filename, filename) == 0) {
            // Free pages first
            for (int j = 0; j < fs_state.directories[fs_state.current_directory].files[i].page_table_size; j++) {
                if (fs_state.directories[fs_state.current_directory].files[i].page_table[j].is_allocated) {
                    int page_num = fs_state.directories[fs_state.current_directory].files[i].page_table[j].physical_page;
                    page_bitmap[page_num / 8] &= ~(1 << (page_num % 8));
                }
            }
            
            // Free page table
            free(fs_state.directories[fs_state.current_directory].files[i].page_table);
            
            // Free content if exists
            if (fs_state.directories[fs_state.current_directory].files[i].content) {
                free(fs_state.directories[fs_state.current_directory].files[i].content);
            }
            
            // Shift remaining files
            for (int j = i; j < fs_state.directories[fs_state.current_directory].file_count - 1; j++) {
                fs_state.directories[fs_state.current_directory].files[j] = 
                    fs_state.directories[fs_state.current_directory].files[j + 1];
            }
            fs_state.directories[fs_state.current_directory].file_count--;
            found = 1;
            break;
        }
    }
    
    if (found) {
        save_state();
        printf(COLOR_GREEN "File '%s' deleted successfully\n" COLOR_RESET, filename);
    } else {
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


char *read_from_file(const char *filename, int bytes_to_read, int offset) {
    pthread_mutex_lock(&mutex);
    
    File *file = NULL;
    // Find the file
    for (int i = 0; i < fs_state.directories[fs_state.current_directory].file_count; i++) {
        if (strcmp(fs_state.directories[fs_state.current_directory].files[i].filename, filename) == 0) {
            file = &fs_state.directories[fs_state.current_directory].files[i];
            break;
        }
    }

    if (!file) {
        printf("Error: File not found\n");
        pthread_mutex_unlock(&mutex);
        return NULL;
    }

    // Check read permissions
    if ((file->permissions & 0444) == 0) {  // Check if any read bits are set
        printf(COLOR_RED "Error: Permission denied (no read permission)\n" COLOR_RESET);
        pthread_mutex_unlock(&mutex);
        return NULL;
    }

    // Validate offset
    if (offset < 0) {
        offset = 0;
    } else if (offset > file->content_size) {
        offset = file->content_size;
    }

    // Calculate how many bytes we can actually read
    int remaining_bytes = file->content_size - offset;
    int read_bytes = (bytes_to_read <= 0) ? remaining_bytes : 
                    (bytes_to_read < remaining_bytes) ? bytes_to_read : remaining_bytes;

    // Allocate buffer
    char *buffer = malloc(read_bytes + 1);
    if (!buffer) {
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

void change_permissions(char *filename, int mode) {
    pthread_mutex_lock(&mutex);
    int found = 0;
    for (int i = 0; i < fs_state.directories[fs_state.current_directory].file_count; i++) {
        if (strcmp(fs_state.directories[fs_state.current_directory].files[i].filename, filename) == 0) {
            // Ensure we only change permission bits (last 9 bits)
            fs_state.directories[fs_state.current_directory].files[i].permissions = mode & 0777;
            fs_state.directories[fs_state.current_directory].files[i].modification_time = time(NULL);
            found = 1;
            break;
        }
    }

    if (found) {
        save_state();
        printf(COLOR_GREEN "Permissions of '%s' changed to %04o\n" COLOR_RESET, filename, mode);
    } else {
        printf(COLOR_RED "Error: File '%s' not found\n" COLOR_RESET, filename);
    }
    pthread_mutex_unlock(&mutex);
}

void print_file_info(const char *filename) {
    pthread_mutex_lock(&mutex);
    int found = 0;
    for (int i = 0; i < fs_state.directories[fs_state.current_directory].file_count; i++) {
        if (strcmp(fs_state.directories[fs_state.current_directory].files[i].filename, filename) == 0) {
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

    if (!found) {
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

