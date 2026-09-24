#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

#define MAX_TOKENS 256

static const char *SEARCH_DIR = "/bin";

static void error(void)
{
    char error_message[30] = "An error has occurred\n";
    write(STDERR_FILENO, error_message, strlen(error_message));
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

static void run_command(char *tokens[], int ntok)
{
    if (ntok == 0) {
        return; 
    }

    if (strcmp(tokens[0], "exit") == 0) {
        exit(0);
    }

    char full_path[4096];
    snprintf(full_path, sizeof(full_path), "%s/%s", SEARCH_DIR, tokens[0]);
    if (access(full_path, X_OK) != 0) {
        error();
        return;
    }

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
        // Дочірній процес виконує програму
        execv(full_path, argv);
        // execv повертає керування лише у разі помилки
        error();
        exit(1);
    }

    // батьківський процес чекає завершення дочірнього
    int status;
    waitpid(pid, &status, 0);
}

int main(int argc, char *argv[])
{
    if (argc > 2) {
        error();
        exit(1);
    }

    FILE *input = stdin;
    int interactive = 1;

    if (argc == 2) {
        // бatch-режим читає команди з файлу, а не з клавіатури
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
