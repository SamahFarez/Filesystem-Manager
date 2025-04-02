#include "../include/filesystem.h"
#include "../include/scheduler.h"
#include "../include/commands.h"
#include "../include/globals.h"


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