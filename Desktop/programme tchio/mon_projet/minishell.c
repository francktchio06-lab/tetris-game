/* ============================================================
 * minishell.c  –  Mini-Shell TD  (ET3)
 * Compilez avec : gcc -Wall -Wextra -o minishell minishell.c
 * ============================================================ */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <errno.h>
#include <pwd.h>      /* BONUS : getpwuid pour le prompt */
#include <limits.h>   /* PATH_MAX */

/* ─── Constantes ────────────────────────────────────────────── */
#define MAX_LINE   1024
#define MAX_ARGS   64
#define MAX_PIPES  16    /* BONUS : nombre max de segments de pipe */

/* ─── Variables globales shell ──────────────────────────────── */
int            shell_terminal;
pid_t          shell_pgid;
struct termios shell_tmodes;

/* ─── Structure de redirections (TODO 3a) ───────────────────── */
typedef struct {
    char *input_file;   /* < fichier   */
    char *output_file;  /* > fichier   */
    int   append;       /* >> au lieu de > */
} redirect_t;

/* ─── Structure Job ─────────────────────────────────────────── */
typedef enum { RUNNING, STOPPED, DONE } job_status_t;

typedef struct job {
    int           job_id;
    pid_t         pgid;
    char          command[256];
    job_status_t  status;
    struct job   *next;
} job_t;

job_t *job_list   = NULL;
int    next_job_id = 1;

/* ─── Historique des commandes (BONUS) ──────────────────────── */
#define HISTORY_MAX 100
char *history[HISTORY_MAX];
int   history_count = 0;

/* ─── Prototypes ────────────────────────────────────────────── */
void    init_shell(void);
int     parse_line(char *line, char **argv, int max_args, redirect_t *redir);
void    execute_command(int argc, char **argv, redirect_t *redir);
void    launch_job(int argc, char **argv, int foreground, redirect_t *redir);
void    wait_for_job(pid_t pgid);
job_t  *add_job(pid_t pgid, const char *cmd);
void    remove_job(pid_t pgid);
void    update_job_statuses(void);
void    builtin_jobs(void);
void    builtin_fg(char *arg);
void    builtin_bg(char *arg);
void    apply_redirections(redirect_t *redir);
void    execute_pipe(char **argv, int argc);
job_t  *find_job_by_id(int id);
job_t  *find_job_by_pgid(pid_t pgid);
void    print_prompt(void);
void    history_add(const char *line);
void    builtin_history(void);

/* ================================================================
 * init_shell : prendre le contrôle du terminal
 * ================================================================ */
void init_shell(void) {
    shell_terminal = STDIN_FILENO;
    shell_pgid = getpid();

    signal(SIGINT,  SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);
    signal(SIGCHLD, SIG_DFL);

    tcgetattr(shell_terminal, &shell_tmodes);
}

/* ================================================================
 * print_prompt : afficher un prompt enrichi (BONUS)
 * ================================================================ */
void print_prompt(void) {
    char cwd[PATH_MAX];
    const char *home = getenv("HOME");
    const char *user = getenv("USER");

    if (!user) {
        struct passwd *pw = getpwuid(getuid());
        user = pw ? pw->pw_name : "?";
    }

    if (getcwd(cwd, sizeof(cwd)) == NULL) {
        strcpy(cwd, "?");
    } else if (home && strncmp(cwd, home, strlen(home)) == 0) {
        /* Remplacer le chemin home par ~ */
        char tmp[PATH_MAX];
        snprintf(tmp, sizeof(tmp), "~%s", cwd + strlen(home));
        strcpy(cwd, tmp);
    }

    printf("%s@minishell:%s$ ", user, cwd);
    fflush(stdout);
}

/* ================================================================
 * parse_line : découpe `line` en tokens dans argv[]
 * Détecte et extrait les redirections (TODO 3a)
 * Retourne le nombre de tokens.
 * ================================================================ */
int parse_line(char *line, char **argv, int max_args, redirect_t *redir) {
    int   argc  = 0;
    char *token = strtok(line, " \t");

    /* Initialiser les redirections */
    redir->input_file  = NULL;
    redir->output_file = NULL;
    redir->append      = 0;

    while (token && argc < max_args - 1) {
        if (strcmp(token, "<") == 0) {
            token = strtok(NULL, " \t");
            if (token) redir->input_file = token;
        } else if (strcmp(token, ">>") == 0) {
            token = strtok(NULL, " \t");
            if (token) { redir->output_file = token; redir->append = 1; }
        } else if (strcmp(token, ">") == 0) {
            token = strtok(NULL, " \t");
            if (token) { redir->output_file = token; redir->append = 0; }
        } else {
            argv[argc++] = token;
        }
        token = strtok(NULL, " \t");
    }
    argv[argc] = NULL;
    return argc;
}

/* ================================================================
 * apply_redirections : ouvrir les fichiers et rediriger les fd
 * ================================================================ */
void apply_redirections(redirect_t *redir) {
    if (redir->input_file) {
        int fd = open(redir->input_file, O_RDONLY);
        if (fd < 0) { perror(redir->input_file); exit(EXIT_FAILURE); }
        dup2(fd, STDIN_FILENO);
        close(fd);
    }
    if (redir->output_file) {
        int flags = O_WRONLY | O_CREAT | (redir->append ? O_APPEND : O_TRUNC);
        int fd = open(redir->output_file, flags, 0644);
        if (fd < 0) { perror(redir->output_file); exit(EXIT_FAILURE); }
        dup2(fd, STDOUT_FILENO);
        close(fd);
    }
}

/* ================================================================
 * execute_pipe : chaîne de pipes cmd1 | cmd2 | ... (TODO 3b)
 * argv contient tous les tokens, '|' sépare les segments.
 * ================================================================ */
void execute_pipe(char **argv, int argc) {
    /* Découper argv en segments séparés par '|' */
    char  *segments[MAX_PIPES][MAX_ARGS];
    int    seg_argc[MAX_PIPES];
    int    nseg = 0;
    int    start = 0;

    for (int i = 0; i <= argc; i++) {
        if (argv[i] == NULL || strcmp(argv[i], "|") == 0) {
            int len = i - start;
            if (len == 0) { fprintf(stderr, "minishell: syntax error near '|'\n"); return; }
            for (int k = 0; k < len; k++)
                segments[nseg][k] = argv[start + k];
            segments[nseg][len] = NULL;
            seg_argc[nseg]      = len;
            nseg++;
            start = i + 1;
            if (nseg >= MAX_PIPES) break;
        }
    }
    if (nseg < 2) { fprintf(stderr, "minishell: pipe sans commandes\n"); return; }

    int pipes[MAX_PIPES - 1][2];
    pid_t pgid = 0;

    /* Créer tous les pipes */
    for (int i = 0; i < nseg - 1; i++) {
        if (pipe(pipes[i]) < 0) { perror("pipe"); return; }
    }

    for (int i = 0; i < nseg; i++) {
        pid_t pid = fork();
        if (pid == 0) {
            /* Fils : réinitialiser les signaux */
            signal(SIGINT,  SIG_DFL);
            signal(SIGQUIT, SIG_DFL);
            signal(SIGTSTP, SIG_DFL);
            signal(SIGTTIN, SIG_DFL);
            signal(SIGTTOU, SIG_DFL);

            /* Mettre dans le groupe */
            if (pgid == 0) pgid = getpid();
            setpgid(0, pgid);

            /* Brancher stdin depuis le pipe précédent */
            if (i > 0) {
                dup2(pipes[i-1][0], STDIN_FILENO);
            }
            /* Brancher stdout vers le pipe suivant */
            if (i < nseg - 1) {
                dup2(pipes[i][1], STDOUT_FILENO);
            }
            /* Fermer tous les pipes dans le fils */
            for (int j = 0; j < nseg - 1; j++) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }
            execvp(segments[i][0], segments[i]);
            perror(segments[i][0]);
            exit(EXIT_FAILURE);
        } else if (pid > 0) {
            if (pgid == 0) pgid = pid;
            setpgid(pid, pgid);
        } else {
            perror("fork");
        }
    }

    /* Fermer tous les pipes dans le père */
    for (int i = 0; i < nseg - 1; i++) {
        close(pipes[i][0]);
        close(pipes[i][1]);
    }

    /* Ajouter le job et attendre */
    char cmd_str[256] = "";
    for (int i = 0; i < argc && strlen(cmd_str) < 200; i++) {
        if (argv[i]) { strcat(cmd_str, argv[i]); strcat(cmd_str, " "); }
    }
    add_job(pgid, cmd_str);
    tcsetpgrp(shell_terminal, pgid);
    wait_for_job(pgid);
    tcsetpgrp(shell_terminal, shell_pgid);
    tcsetattr(shell_terminal, TCSADRAIN, &shell_tmodes);
}

/* ================================================================
 * execute_command : point d'entrée principal
 * ================================================================ */
void execute_command(int argc, char **argv, redirect_t *redir) {

    if (strcmp(argv[0], "cd") == 0) {
        const char *dir = (argc > 1) ? argv[1] : getenv("HOME");
        if (!dir) dir = "/";
        if (chdir(dir) < 0) perror("cd");
        return;
    }

    if (strcmp(argv[0], "jobs") == 0) { builtin_jobs(); return; }
    if (strcmp(argv[0], "fg") == 0) { builtin_fg(argc > 1 ? argv[1] : NULL); return; }
    if (strcmp(argv[0], "bg") == 0) { builtin_bg(argc > 1 ? argv[1] : NULL); return; }
    if (strcmp(argv[0], "history") == 0) { builtin_history(); return; }

    if (strcmp(argv[0], "pwd") == 0) {
        char cwd[PATH_MAX];
        if (getcwd(cwd, sizeof(cwd))) puts(cwd);
        else perror("pwd");
        return;
    }

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "|") == 0) {
            execute_pipe(argv, argc);
            return;
        }
    }

    int foreground = 1;
    if (argc > 0 && strcmp(argv[argc - 1], "&") == 0) {
        foreground = 0;
        argv[--argc] = NULL;
    }

    launch_job(argc, argv, foreground, redir);  /* ← passer redir directement */
}

/* ================================================================
 * launch_job : fork + exec avec gestion des groupes de processus
 * ================================================================ */
void launch_job(int argc, char **argv, int foreground, redirect_t *redir) {
    (void)argc;
    pid_t pid = fork();

    if (pid == 0) {
        /* TODO 4e : mettre le fils dans son propre groupe */
        setpgid(0, 0);

        /* TODO 4f : si foreground, donner le terminal au fils */
        if (foreground)
            tcsetpgrp(shell_terminal, getpid());

        /* TODO 4g : rétablir les signaux par défaut */
        signal(SIGINT,  SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        signal(SIGTSTP, SIG_DFL);
        signal(SIGTTIN, SIG_DFL);
        signal(SIGTTOU, SIG_DFL);

        /* TODO 3a : appliquer les redirections */
        apply_redirections(redir);

        execvp(argv[0], argv);
        perror(argv[0]);
        exit(EXIT_FAILURE);

    } else if (pid > 0) {
        /* TODO 4h : setpgid depuis le père (race condition) */
        setpgid(pid, pid);

        /* TODO 5c : ajouter le job à job_list */
        char cmd_str[256] = "";
        for (int i = 0; argv[i]; i++) {
            strncat(cmd_str, argv[i], sizeof(cmd_str) - strlen(cmd_str) - 2);
            if (argv[i+1]) strcat(cmd_str, " ");
        }
        add_job(pid, cmd_str);

        if (foreground) {
            /* TODO 4i : donner le terminal au fils depuis le père */
            tcsetpgrp(shell_terminal, pid);
            wait_for_job(pid);
            /* TODO 4j : reprendre le terminal */
            tcsetpgrp(shell_terminal, shell_pgid);
            tcsetattr(shell_terminal, TCSADRAIN, &shell_tmodes);
        } else {
            printf("[%d] %d\n", next_job_id - 1, pid);
        }
    } else {
        perror("fork");
    }
}

/* ================================================================
 * wait_for_job : attendre qu'un job passe en STOPPED ou DONE
 * ================================================================ */
void wait_for_job(pid_t pgid) {
    int   status;
    pid_t p;

    /* TODO 4k : boucler sur waitpid */
    do {
        p = waitpid(-pgid, &status, WUNTRACED);
        if (p < 0 && errno != EINTR && errno != ECHILD) {
            perror("waitpid");
            break;
        }
        if (p > 0) {
            job_t *j = find_job_by_pgid(pgid);
            if (WIFSTOPPED(status)) {
                if (j) j->status = STOPPED;
                printf("\n[%d]+ Stopped\t%s\n", j ? j->job_id : 0, j ? j->command : "");
                return;
            }
            if (WIFEXITED(status) || WIFSIGNALED(status)) {
                if (j) j->status = DONE;
                /* BONUS : afficher le signal si tué */
                if (WIFSIGNALED(status))
                    printf("\n[%d]+ killed by signal %d\n", j ? j->job_id : 0, WTERMSIG(status));
                return;
            }
        }
    } while (p > 0);
}

/* ================================================================
 * Gestion de la liste de jobs
 * ================================================================ */
job_t *add_job(pid_t pgid, const char *cmd) {
    job_t *j = malloc(sizeof(job_t));
    if (!j) { perror("malloc"); return NULL; }

    /* TODO 5d : initialiser les champs */
    j->job_id = next_job_id++;
    j->pgid   = pgid;
    j->status = RUNNING;
    strncpy(j->command, cmd, sizeof(j->command) - 1);
    j->command[sizeof(j->command) - 1] = '\0';

    /* Insérer en tête */
    j->next   = job_list;
    job_list  = j;
    return j;
}

void remove_job(pid_t pgid) {
    /* TODO 5e : retirer le job de job_list */
    job_t **p = &job_list;
    while (*p) {
        if ((*p)->pgid == pgid) {
            job_t *tmp = *p;
            *p = (*p)->next;
            free(tmp);
            return;
        }
        p = &(*p)->next;
    }
}

job_t *find_job_by_pgid(pid_t pgid) {
    for (job_t *j = job_list; j; j = j->next)
        if (j->pgid == pgid) return j;
    return NULL;
}

job_t *find_job_by_id(int id) {
    for (job_t *j = job_list; j; j = j->next)
        if (j->job_id == id) return j;
    return NULL;
}

void update_job_statuses(void) {
    /* TODO 5f : waitpid non bloquant en boucle */
    int   status;
    pid_t p;

    while ((p = waitpid(-1, &status, WNOHANG | WUNTRACED)) > 0) {
        /* Trouver le job correspondant */
        job_t *j = NULL;
        for (job_t *it = job_list; it; it = it->next) {
            if (it->pgid == p || it->pgid == (pid_t)-p) { j = it; break; }
        }
        /* Chercher par pgid du process */
        if (!j) {
            for (job_t *it = job_list; it; it = it->next) {
                /* On suppose une correspondance pgid == pid pour job simple */
                if (it->pgid == p) { j = it; break; }
            }
        }

        if (WIFSTOPPED(status) && j)  j->status = STOPPED;
        else if ((WIFEXITED(status) || WIFSIGNALED(status)) && j) {
            printf("[%d]+ Done\t%s\n", j->job_id, j->command);
            j->status = DONE;
        }
    }

    /* Nettoyer les jobs DONE */
    job_t **p2 = &job_list;
    while (*p2) {
        if ((*p2)->status == DONE) {
            job_t *tmp = *p2;
            *p2 = (*p2)->next;
            free(tmp);
        } else {
            p2 = &(*p2)->next;
        }
    }
}

void builtin_jobs(void) {
    /* TODO 5g : afficher job_list */
    const char *status_str[] = { "Running", "Stopped", "Done" };
    for (job_t *j = job_list; j; j = j->next) {
        printf("[%d] %-10s %s\n", j->job_id, status_str[j->status], j->command);
    }
}

void builtin_fg(char *arg) {
    /* TODO 5h : remettre un job en foreground */
    job_t *j = NULL;

    if (arg && arg[0] == '%') {
        int id = atoi(arg + 1);
        j = find_job_by_id(id);
    } else {
        /* Dernier job stoppé */
        for (job_t *it = job_list; it; it = it->next)
            if (it->status == STOPPED) { j = it; break; }
        if (!j) j = job_list; /* premier job sinon */
    }

    if (!j) { fprintf(stderr, "fg: no such job\n"); return; }

    j->status = RUNNING;
    printf("%s\n", j->command);

    /* Donner le terminal au job */
    tcsetpgrp(shell_terminal, j->pgid);

    /* SIGCONT au groupe */
    kill(-(j->pgid), SIGCONT);

    wait_for_job(j->pgid);

    /* Reprendre le terminal */
    tcsetpgrp(shell_terminal, shell_pgid);
    tcsetattr(shell_terminal, TCSADRAIN, &shell_tmodes);
}

void builtin_bg(char *arg) {
    /* TODO 5i : relancer un job en background */
    job_t *j = NULL;

    if (arg && arg[0] == '%') {
        int id = atoi(arg + 1);
        j = find_job_by_id(id);
    } else {
        for (job_t *it = job_list; it; it = it->next)
            if (it->status == STOPPED) { j = it; break; }
    }

    if (!j) { fprintf(stderr, "bg: no such job\n"); return; }

    j->status = RUNNING;
    printf("[%d]+ %s &\n", j->job_id, j->command);
    kill(-(j->pgid), SIGCONT);
}

/* ================================================================
 * BONUS : historique des commandes
 * ================================================================ */
void history_add(const char *line) {
    if (history_count < HISTORY_MAX) {
        history[history_count++] = strdup(line);
    } else {
        free(history[0]);
        memmove(history, history + 1, (HISTORY_MAX - 1) * sizeof(char *));
        history[HISTORY_MAX - 1] = strdup(line);
    }
}

void builtin_history(void) {
    for (int i = 0; i < history_count; i++)
        printf("%4d  %s\n", i + 1, history[i]);
}

/* ================================================================
 * main
 * ================================================================ */
int main(void) {
    char  line[MAX_LINE];
    char *argv[MAX_ARGS];

    init_shell();

    while (1) {
        /* TODO 5j : nettoyer les zombies */
        update_job_statuses();

        /* TODO 1a : afficher le prompt (BONUS : prompt enrichi) */
        print_prompt();

        /* TODO 1b : lire une ligne */
        if (!fgets(line, sizeof(line), stdin)) {
            /* TODO 1c : gérer EOF (Ctrl-D) */
            clearerr(stdin);
            printf("\nexit\n");
            continue;
            break;
        }

        /* TODO 1d : supprimer le '\n' final */
        line[strcspn(line, "\n")] = '\0';

        /* Ignorer les lignes vides */
        if (line[0] == '\0') continue;

        /* BONUS : sauvegarder dans l'historique */
        history_add(line);

        /* Copie pour parse (strtok modifie la chaîne) */
        char line_copy[MAX_LINE];
        strncpy(line_copy, line, sizeof(line_copy) - 1);
        line_copy[sizeof(line_copy) - 1] = '\0';

        redirect_t redir;
        int argc = parse_line(line_copy, argv, MAX_ARGS, &redir);
        if (argc == 0) continue;

        /* TODO 1e : gérer le builtin exit */
        if (strcmp(argv[0], "exit") == 0) {
            printf("exit\n");
            /* BONUS : libérer l'historique */
            for (int i = 0; i < history_count; i++) free(history[i]);
            break;
        }

        execute_command(argc, argv,&redir);
    }

    return 0;
}
