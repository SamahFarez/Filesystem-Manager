#include "../include/commands.h"
#include "../include/filesystem.h"
#include "../include/globals.h"

void help()
{
    printf("\n" COLOR_GREEN "Mini UNIX-like File System Help" COLOR_RESET "\n");
    printf("===================================\n\n");

    printf(COLOR_YELLOW "File Operations:" COLOR_RESET "\n");
    printf("  chmod <mode> <file>      - Change permissions (e.g., 755)\n");
    printf("  close <file>             - Close file handle\n");
    printf("  create <file> <perms>    - Create file with octal permissions (e.g., 644)\n");
    printf("  delete <file>            - Delete a file\n");
    printf("  open <file>              - Open file handle\n");
    printf("  move <src> <dest> [newname] - Move file (optionally rename)\n");
    printf("  read <file> [off] [len]  - Read file (optional offset and length)\n");
    printf("  seek <file> <off> <whence> - Move file pointer (SET/CUR/END)\n");
    printf("  stat <file>              - Show file metadata\n");
    printf("  write [-a] <file> <data> - Write to file (-a to append)\n\n");

    printf(COLOR_YELLOW "Directory Operations:" COLOR_RESET "\n");
    printf("  cd <dir>                 - Change directory\n");
    printf("  copy <src> <dest>        - Copy file\n");
    printf("  create -d <dir>          - Create directory\n");
    printf("  delete -d <dir>          - Delete empty directory\n");
    printf("  dirinfo [dir]            - Show directory info\n");
    printf("  list                     - List directory contents\n");
    printf("  move -d <src> <dest> [newname] - Move directory (optionally rename)\n\n");
    printf("  pwd                      - Print working directory\n");
    printf("  tree [-i]                - Directory tree (-i shows inodes)\n\n");

    printf(COLOR_YELLOW "Link Operations:" COLOR_RESET "\n");
    printf("  ln <target> <link>       - Create hard link\n");
    printf("  ln -s <target> <link>    - Create symbolic link\n\n");

    printf(COLOR_YELLOW "System Operations:" COLOR_RESET "\n");
    printf("  backup [name]            - Create backup\n");
    printf("  format                   - Wipe filesystem (DANGER!)\n");
    printf("  help                     - This help message\n");
    printf("  quit                     - Exit the system\n");
    printf("  restore [name]           - Restore backup\n");
    printf("  showpages [file]         - Show page table info\n");

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
        if (strstr(command, "-d") != NULL)
        {
            // Handle directory move
            char src_dir[MAX_FILENAME], dest_dir[MAX_FILENAME], new_name[MAX_FILENAME] = {0};

            // Parse command - allow for optional new name
            int parsed = sscanf(command, "move -d %s %s %s", src_dir, dest_dir, new_name);

            if (parsed >= 2)
            {
                // If only 2 args, use source dirname as new name
                if (parsed == 2)
                {
                    // Extract just the directory name from src path
                    char *last_slash = strrchr(src_dir, '/');
                    if (last_slash)
                    {
                        strncpy(new_name, last_slash + 1, MAX_FILENAME - 1);
                    }
                    else
                    {
                        strncpy(new_name, src_dir, MAX_FILENAME - 1);
                    }
                }

                // Trim any trailing slashes
                char *end = dest_dir + strlen(dest_dir) - 1;
                while (end > dest_dir && (*end == '/' || *end == ' '))
                    *end-- = '\0';

                move_directory(src_dir, dest_dir, new_name[0] ? new_name : NULL);
            }
            else
            {
                printf(COLOR_RED "Usage: move -d <src_dir> <dest_dir> [newname]\n" COLOR_RESET);
            }
        }
        else
        {

            // Check if first argument is a directory
            char first_arg[MAX_FILENAME];
            if (sscanf(command, "move %s", first_arg) == 1)
            {
                int dir_idx = find_directory_from_path(first_arg);
                if (dir_idx != -1)
                {
                    printf(COLOR_RED "Error: Use '-d' flag for directory moves\n" COLOR_RESET);
                    printf(COLOR_RED "Usage: move -d <src_dir> <dest_dir> [newname]\n" COLOR_RESET);
                    free(job.command);
                    return;
                }
            }
            // Handle file move (existing code)
            char src_path[MAX_FILENAME], dest_path[MAX_FILENAME], new_name[MAX_FILENAME] = {0};

            char *token = strtok((char *)command + 5, " ");
            if (!token)
            {
                printf(COLOR_RED "Usage: move <src> <dest> [newname]\n" COLOR_RESET);
                return;
            }
            strncpy(src_path, token, MAX_FILENAME - 1);

            token = strtok(NULL, " ");
            if (!token)
            {
                printf(COLOR_RED "Usage: move <src> <dest> [newname]\n" COLOR_RESET);
                return;
            }
            strncpy(dest_path, token, MAX_FILENAME - 1);

            token = strtok(NULL, " ");
            if (token)
            {
                strncpy(new_name, token, MAX_FILENAME - 1);
            }

            // Trim any trailing slashes from dest_path
            char *end = dest_path + strlen(dest_path) - 1;
            while (end > dest_path && (*end == '/' || *end == ' '))
                *end-- = '\0';

            move_file_to_dir(src_path, dest_path, new_name[0] ? new_name : NULL);
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
        printf(COLOR_RED "Error: Unknown command '%s'\n" COLOR_RESET, command);
        printf(COLOR_YELLOW "Type 'help' for a list of available commands\n" COLOR_RESET);
    }
    free(job.command);
}