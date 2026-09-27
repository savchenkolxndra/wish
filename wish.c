#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

#define MAX_TOKENS 256
#define MAX_PATH_DIRS 128

static char *path_dirs[MAX_PATH_DIRS];
static int npath = 0;

static void error(void)
{
    char error_message[30] = "An error has occurred\n";
    write(STDERR_FILENO, error_message, strlen(error_message));
}

static void set_default_path(void)
{
    npath = 0;
    path_dirs[npath++] = strdup("/bin");
}

// розбивається рядок на токени за пробілами/табами і повертається кількість токенів
static int tokenize(char *line, char *tokens[])
{
    int n = 0;
    char *saveptr;
    char *tok = strtok_r(line, " \t\r\n", &saveptr);
    while (tok != NULL && n < MAX_TOKENS) {
        tokens[n++] = tok;
        tok = strtok_r(NULL, " \t\r\n", &saveptr);
    }
    return n;
}

// шукає виконуваний файл у всіх директоріях поточного шляху
static char *find_executable(const char *cmd)
{
    static char full[4096];
    for (int i = 0; i < npath; i++) {
        snprintf(full, sizeof(full), "%s/%s", path_dirs[i], cmd);
        if (access(full, X_OK) == 0) {
            return full;
        }
    }
    return NULL;
}

// повертає 1, якщо команда була вбудованою, інакше 0
static int run_builtin(char *tokens[], int ntok)
{
    if (strcmp(tokens[0], "exit") == 0) {
        if (ntok != 1) {
            error();
            return 1;
        }
        exit(0);
    }

    if (strcmp(tokens[0], "cd") == 0) {
        if (ntok != 2) {
            error();
        } else if (chdir(tokens[1]) != 0) {
            error();
        }
        return 1;
    }

    if (strcmp(tokens[0], "path") == 0) {
        // команда path завжди перезаписує старий шлях новим
        for (int i = 0; i < npath; i++) {
            free(path_dirs[i]);
        }
        npath = 0;
        for (int i = 1; i < ntok && npath < MAX_PATH_DIRS; i++) {
            path_dirs[npath++] = strdup(tokens[i]);
        }
        return 1;
    }

    return 0;
}

static void run_command(char *tokens[], int ntok)
{
    if (ntok == 0) {
        return;
    }

    if (run_builtin(tokens, ntok)) {
        return;
    }

    char *exe = find_executable(tokens[0]);
    if (exe == NULL) {
        error();
        return;
    }
    // копіюєтьсч шлях, бо find_executable повертає вказівник на статичний буфер, який може бути перезаписаний наступним викликом
    char exe_copy[4096];
    strncpy(exe_copy, exe, sizeof(exe_copy) - 1);
    exe_copy[sizeof(exe_copy) - 1] = '\0';

    char *argv[MAX_TOKENS + 1];
    for (int i = 0; i < ntok; i++) {
        argv[i] = tokens[i];
    }
    argv[ntok] = NULL;

    pid_t pid = fork();
    if (pid < 0) {
        error();
        return;
    }
    if (pid == 0) {
        // дочірній процес виконує програму
        execv(exe_copy, argv);
        // execv повертає керування лише у разі помилки
        error();
        exit(1);
    }

    // Батьківський процес чекає завершення дочірнього
    int status;
    waitpid(pid, &status, 0);
}

int main(int argc, char *argv[])
{
    if (argc > 2) {
        error();
        exit(1);
    }

    set_default_path();

    FILE *input = stdin;
    int interactive = 1;

    if (argc == 2) {
        // бatch-режим читає команди з файлу
        interactive = 0;
        input = fopen(argv[1], "r");
        if (input == NULL) {
            error();
            exit(1);
        }
    }

    char *line = NULL;
    size_t linecap = 0;

    while (1) {
        if (interactive) {
            printf("wish> ");
            fflush(stdout);
        }

        ssize_t nread = getline(&line, &linecap, input);
        if (nread == -1) {
 
            exit(0);
        }

        char *tokens[MAX_TOKENS];
        int ntok = tokenize(line, tokens);
        run_command(tokens, ntok);
    }

    return 0;
}
