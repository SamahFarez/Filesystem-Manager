#include "../include/commands.h"


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