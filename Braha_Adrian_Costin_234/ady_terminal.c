#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <termios.h>
#include <signal.h>
#include <sys/types.h>

#define MAX_CMD_LENGTH 1024
#define MAX_ARGS 100
#define MAX_PIPE_CMDS 10
#define MAX_HISTORY 50

char *history[MAX_HISTORY];
int history_count = 0;
int history_position = -1;

pid_t background_pids[MAX_HISTORY];
int background_count = 0;

void add_to_history(const char *command) {
    if (history_count < MAX_HISTORY) {
        history[history_count++] = strdup(command);
    } else {
        free(history[0]);
        memmove(&history[0], &history[1], (MAX_HISTORY - 1) * sizeof(char *));
        history[MAX_HISTORY - 1] = strdup(command);
    }
}

void print_command(const char *command) {
     printf("\r\033[1;31mass:(%s)> \033[0m %s \b", getcwd(NULL,0),command);
    fflush(stdout);
}

void capture_input(char *input) {
    struct termios oldt, newt;
    tcgetattr(STDIN_FILENO, &oldt);
    newt = oldt;
    newt.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);

    int pos = 0;
    input[0] = '\0';
    char c;

    while (1) {
        c = getchar();
        if (c == '\033') {
            getchar();    
            switch (getchar()) {
                case 'A':
                    if (history_count > 0 && history_position > 0) {
                        history_position--;
                        strcpy(input, history[history_position]);
                        print_command(input);
                    } else if (history_position == -1 && history_count > 0) {
                        history_position = history_count - 1;
                        strcpy(input, history[history_position]);
                        print_command(input);
                    }
                    break;
                case 'B':
                    if (history_position >= 0 && history_position < history_count - 1) {
                        history_position++;
                        strcpy(input, history[history_position]);
                        print_command(input);
                    } else if (history_position == history_count - 1) {
                        history_position = -1;
                        input[0] = '\0';
                        print_command("");
                    }
                    break;
            }
        } else if (c == '\n') {
            printf("\n");
            break;
        } else if (c == 127) {
            if (pos > 0) {
                input[--pos] = '\0';
                printf("\r\033[1;31mass:(%s)> \033[0m %s \b", getcwd(NULL,0),input);
                fflush(stdout);
            }
        } else {
            input[pos++] = c;
            input[pos] = '\0';
            printf("%c", c);
            fflush(stdout);
        }
    }

    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
}

char** parse_input(char *input) {
    char **args = malloc(MAX_ARGS * sizeof(char *));
    char *token = strtok(input, " \t\n");

    int i = 0;
    while (token != NULL) {
        args[i++] = token;
        token = strtok(NULL, " \t\n");
    }
    args[i] = NULL;
    return args;
}

int execute_builtin(char **args) {
    if (args[0] == NULL) return 0;

    if (strcmp(args[0], "cd") == 0) {
        if (args[1] == NULL) {
            fprintf(stderr, "cd: missing argument\n");
        } else if (chdir(args[1]) != 0) {
            perror("cd");
        }
        return 1;
    } else if (strcmp(args[0], "history") == 0) {
        for (int i = 0; i < history_count; i++) {
            printf("%d: %s\n", i + 1, history[i]);
        }
        return 1;
    } else if (strcmp(args[0], "jobs") == 0) {
        if (background_count > 0) {
            for (int i = 0; i < background_count; i++) {
                printf("[%d] %d\n", i + 1, background_pids[i]);
            }
        } else {
            printf("No background jobs\n");
        }
        return 1;
    } else if (strcmp(args[0], "fg") == 0) {
        if (background_count > 0) {
            pid_t pid = background_pids[background_count - 1];
            kill(pid, SIGCONT);
            waitpid(pid, NULL, 0);
            background_count--;
        } else {
            printf("No background job to bring to the foreground\n");
        }
        return 1;
    } else if (strcmp(args[0], "bg") == 0) {
        if (background_count > 0) {
            pid_t pid = background_pids[background_count - 1];
            kill(pid, SIGCONT);
        } else {
            printf("No background job to resume\n");
        }
        return 1;
    }

    return 0;
}

void execute_command(char **args) {
    if (execvp(args[0], args) == -1) {
        perror("execvp");
    }
    exit(1);
}

void execute_pipe_commands(char *input) {
    char *commands[MAX_PIPE_CMDS];
    char *cmd = strtok(input, "|");

    int cmd_count = 0;
    while (cmd != NULL && cmd_count < MAX_PIPE_CMDS) {
        commands[cmd_count++] = cmd;
        cmd = strtok(NULL, "|");
    }

    int fd[2], in_fd = 0;

    for (int i = 0; i < cmd_count; i++) {
        pipe(fd);

        pid_t pid = fork();
        if (pid == 0) {
            if (i < cmd_count - 1) {
                dup2(fd[1], STDOUT_FILENO);
            }
            if (i > 0) {
                dup2(in_fd, STDIN_FILENO);
            }

            close(fd[0]);
            close(fd[1]);

            char **args = parse_input(commands[i]);
            execute_command(args);
        } else {
            wait(NULL);
            close(fd[1]);
            in_fd = fd[0];
        }
    }
}

int execute_logic_commands(char *input) {
    char *cmd1, *cmd2;
    int status;
    
    cmd1 = strtok(input, "&&");
    if (cmd1 != NULL) {
        cmd2 = strtok(NULL, "&&");
        if (cmd2 != NULL) {
            pid_t pid = fork();
            if (pid == 0) {
                char **args1 = parse_input(cmd1);
                if (execvp(args1[0], args1) == -1) {
                    perror("execvp");
                    exit(1);
                }
            } else {
                waitpid(pid, &status, 0);
                if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
                    pid_t pid2 = fork();
                    if (pid2 == 0) {
                        char **args2 = parse_input(cmd2);
                        if (execvp(args2[0], args2) == -1) {
                            perror("execvp");
                            exit(1);
                        }
                    } else {
                        waitpid(pid2, &status, 0);
                    }
                }
            }
        } else {
            pid_t pid = fork();
            if (pid == 0) {
                char **args1 = parse_input(cmd1);
                if (execvp(args1[0], args1) == -1) {
                    perror("execvp");
                    exit(1);
                }
            } else {
                waitpid(pid, &status, 0);
            }
        }
    } else if (strstr(input, "||") != NULL) {
        cmd1 = strtok(input, "||");
        cmd2 = strtok(NULL, "||");

        if (cmd2 != NULL) {
            pid_t pid = fork();
            if (pid == 0) {
                char **args1 = parse_input(cmd1);
                if (execvp(args1[0], args1) == -1) {
                    perror("execvp");
                    exit(1);
                }
            } else {
                waitpid(pid, &status, 0);
                if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
                    pid_t pid2 = fork();
                    if (pid2 == 0) {
                        char **args2 = parse_input(cmd2);
                        if (execvp(args2[0], args2) == -1) {
                            perror("execvp");
                            exit(1);
                        }
                    } else {
                        waitpid(pid2, &status, 0);
                    }
                }
            }
        }
    }

    return 0;
}

void print_prompt() {
    printf("\033[1;31mass:(%s)> \033[0m", getcwd(NULL, 0));
}

int main() {
    printf("\n=================================================\n");
    printf("\n               ady's secure shell\n");
    printf("\n=================================================\n");
    char input[MAX_CMD_LENGTH];
    while (1) {
        print_prompt();
        fflush(stdout);
        capture_input(input);

        if (strlen(input) == 0) {
            continue;
        }

        add_to_history(input);
        history_position = -1;

        if (strchr(input, '|')) {
            execute_pipe_commands(input);
            continue;
        }

        if (strchr(input, '&') || strchr(input, '|')) {
            execute_logic_commands(input);
            continue;
        }

        int background = 0;
        if (input[strlen(input) - 2] == '&') {
            background = 1;
            input[strlen(input) - 2] = '\0';
        }

        char **args = parse_input(input);

        if (execute_builtin(args)) {
            free(args);
            continue;
        }

        pid_t pid = fork();
        if (pid == -1) {
            perror("fork");
            exit(1);
        } else if (pid == 0) {
            execute_command(args);
        } else {
            if (background) {
                background_pids[background_count++] = pid;
                printf("Background job started (pid: %d)\n", pid);
            } else {
                wait(NULL);
            }
        }

        free(args);
    }

    for (int i = 0; i < history_count; i++) {
        free(history[i]);
    }

    return 0;
}

