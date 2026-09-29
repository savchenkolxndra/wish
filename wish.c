#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>

#define MAX_TOKENS 256
#define MAX_PATH_DIRS 128
#define MAX_CHILDREN 128

static char *path_dirs[MAX_PATH_DIRS];
static int npath = 0;

static void error(void)
{
    char error_message[30] = "An error has occurred\n";
    write(STDERR_FILENO, error_message, strlen(error_message));
}

static void set_default_path(void)
{
    for (int i = 0; i < npath; i++) {
        free(path_dirs[i]);
    }
    npath = 0;
    path_dirs[npath++] = strdup("/bin");
}

/* вставляє пробіли навколо кожного символу c, щоб операції >, &
 * розпізнавались як окремі токени, навіть якщо написані без пробілів
 * навколо і викликач звільняє пам'ять. */
static char *spacer(const char *s, char c)
{
    size_t len = strlen(s);
    char *out = malloc(len * 3 + 1);
    if (!out) {
        error();
        exit(1);
    }
    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] == c) {
            out[j++] = ' ';
            out[j++] = c;
            out[j++] = ' ';
        } else {
            out[j++] = s[i];
        }
    }
    out[j] = '\0';
    return out;
}

// прибирає пробіли/таби/переноси рядків на початку й у кінці рядка
static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')
        s++;
    if (*s == '\0')
        return s;
    char *end = s + strlen(s) - 1;
    while (end > s && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')) {
        *end = '\0';
        end--;
    }
    return s;
}

// розбивається рядок на сегменти за символом & (паралельні команди)
static int split_ampersand(char *line, char *segments[])
{
    char *spaced = spacer(line, '&');
    int n = 0;
    char *saveptr;
    char *tok = strtok_r(spaced, "&", &saveptr);
    while (tok != NULL && n < MAX_CHILDREN) {
        segments[n++] = tok;
        tok = strtok_r(NULL, "&", &saveptr);
    }
    return n;
}

// розбивається один сегмент команди на токени за пробілами
static int tokenize(char *seg, char *tokens[])
{
    int n = 0;
    char *saveptr;
    char *tok = strtok_r(seg, " \t", &saveptr);
    while (tok != NULL && n < MAX_TOKENS) {
        tokens[n++] = tok;
        tok = strtok_r(NULL, " \t", &saveptr);
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

/* виконує один сегмент (уже розділений по &), повертає pid дочірнього
 * процесу, якщо його треба чекати або -1, якщо чекати нічого не треба */
static pid_t run_segment(char *seg)
{
    char *trimmed = trim(seg);
    if (*trimmed == '\0') {
        return -1; // порожній сегмент пропускаєи
    }

    // робимо > окремим токеном, відділеним пробілами
    char *spaced = spacer(trimmed, '>');

    char *tokens[MAX_TOKENS];
    int ntok = tokenize(spaced, tokens);

    if (ntok == 0) {
        free(spaced);
        return -1;
    }

    // шукаєм символи перенаправлення серед токенів
    int gt_count = 0;
    int gt_index = -1;
    for (int i = 0; i < ntok; i++) {
        if (strcmp(tokens[i], ">") == 0) {
            gt_count++;
            gt_index = i;
        }
    }

    char *redir_file = NULL;
    int cmd_ntok = ntok;

    if (gt_count > 1) {
        error();
        free(spaced);
        return -1;
    } else if (gt_count == 1) {
        // перед > має бути хоча б один токен, після рівно один
        int after = ntok - gt_index - 1;
        if (gt_index == 0 || after != 1) {
            error();
            free(spaced);
            return -1;
        }
        redir_file = tokens[gt_index + 1];
        cmd_ntok = gt_index;
    }

    // формується NULL-термінований argv для частини команди, без > і файлу
    char *argv[MAX_TOKENS + 1];
    for (int i = 0; i < cmd_ntok; i++) {
        argv[i] = tokens[i];
    }
    argv[cmd_ntok] = NULL;

    if (cmd_ntok == 0) {
        error();
        free(spaced);
        return -1;
    }

    // вбудовані команди виконуються синхронно в батьківському процесі,
    // без fork інакше зміна директорії чи шляху не вплинула б на сам шелл
    if (strcmp(argv[0], "exit") == 0) {
        if (cmd_ntok != 1) {
            error();
            free(spaced);
            return -1;
        }
        exit(0);
    } else if (strcmp(argv[0], "cd") == 0) {
        if (cmd_ntok != 2) {
            error();
        } else if (chdir(argv[1]) != 0) {
            error();
        }
        free(spaced);
        return -1;
    } else if (strcmp(argv[0], "path") == 0) {
        for (int i = 0; i < npath; i++) {
            free(path_dirs[i]);
        }
        npath = 0;
        for (int i = 1; i < cmd_ntok && npath < MAX_PATH_DIRS; i++) {
            path_dirs[npath++] = strdup(argv[i]);
        }
        free(spaced);
        return -1;
    }

    // зовнішня команда fork + exec
    char *exe = find_executable(argv[0]);
    if (exe == NULL) {
        error();
        free(spaced);
        return -1;
    }
    // копіюєм шлях, бо find_executable повертає вказівник на
    // статичний буфер, який може бути перезаписаний наступним викликом
    char exe_copy[4096];
    strncpy(exe_copy, exe, sizeof(exe_copy) - 1);
    exe_copy[sizeof(exe_copy) - 1] = '\0';

    pid_t pid = fork();
    if (pid < 0) {
        error();
        free(spaced);
        return -1;
    }
    if (pid == 0) {
        // дочірній процес
        if (redir_file != NULL) {
            int fd = open(redir_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd < 0) {
                error();
                exit(1);
            }
            if (dup2(fd, STDOUT_FILENO) < 0 || dup2(fd, STDERR_FILENO) < 0) {
                error();
                exit(1);
            }
            close(fd);
        }
        execv(exe_copy, argv);
        // execv повертає керування лише у разі помилки 
        error();
        exit(1);
    }

    // батьківський процес спершу запускає всі сегменти, чекаєм пізніше
    free(spaced);
    return pid;
}

static void process_line(char *line)
{
    char *segments[MAX_CHILDREN];
    int nseg = split_ampersand(line, segments);

    pid_t children[MAX_CHILDREN];
    int nchildren = 0;

    // спочатку запуск всіх команд сегмента, паралельно, без очікування
    for (int i = 0; i < nseg; i++) {
        pid_t pid = run_segment(segments[i]);
        if (pid > 0 && nchildren < MAX_CHILDREN) {
            children[nchildren++] = pid;
        }
    }

    // тепер чекаєм завершення всіх запущених процесів
    for (int i = 0; i < nchildren; i++) {
        int status;
        waitpid(children[i], &status, 0);
    }
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

        process_line(line);
    }

    free(line);
    if (!interactive) {
        fclose(input);
    }
    return 0;
}
