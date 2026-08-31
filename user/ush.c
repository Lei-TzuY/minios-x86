#include "user_syscall.h"

/*
 * ush - a tiny shell running entirely in ring 3. It demonstrates that the
 * miniOS system-call surface (fork, execv, wait, pipe, dup2, chdir, getcwd,
 * read, write) is rich enough to host a real interactive program.
 *
 * Supported: external commands, a two-stage pipeline (cmd1 | cmd2), and the
 * cd / pwd / exit built-ins.
 */

#define MAX_LINE 128
#define MAX_ARGS 16
#define MAX_STAGES 4
#define MAX_ENV 8

static void write_str(const char *s) {
    int len = 0;
    while (s[len]) len++;
    sys_write(s, len);
}

static int streq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static void copy_str(char *dst, const char *src, int max) {
    int i = 0;
    if (max <= 0) return;
    while (i < max - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static int to_int(const char *s) {
    int v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return v;
}

/* Shell variables (set via `export`, referenced as $NAME). */
static char env_name[MAX_ENV][32];
static char env_val[MAX_ENV][64];
static int  env_count = 0;

static const char *get_env(const char *name) {
    for (int i = 0; i < env_count; i++)
        if (streq(env_name[i], name)) return env_val[i];
    return "";
}

static void set_env(const char *name, const char *val) {
    for (int i = 0; i < env_count; i++) {
        if (streq(env_name[i], name)) { copy_str(env_val[i], val, 64); return; }
    }
    if (env_count < MAX_ENV) {
        copy_str(env_name[env_count], name, 32);
        copy_str(env_val[env_count], val, 64);
        env_count++;
    }
}

/* Expand any "$NAME" token to its value (empty string if undefined). */
static void expand_vars(char **argv, int argc) {
    for (int i = 0; i < argc; i++)
        if (argv[i][0] == '$') argv[i] = (char *)get_env(argv[i] + 1);
}

/* Read one line from stdin (the kernel echoes keystrokes). Returns length, or
 * -1 if the line was cancelled with Ctrl+C. */
static int read_line(char *buf, int max) {
    int n = 0;
    while (n < max - 1) {
        char c;
        if (sys_read(&c, 1) != 1) break;
        if (c == '\n' || c == '\r') break;
        if (c == 3) return -1;            /* Ctrl+C cancels the line */
        if (c == '\b') { if (n > 0) n--; continue; }
        buf[n++] = c;
    }
    buf[n] = '\0';
    return n;
}

/* Split a line into space-separated tokens in place. */
static int parse(char *line, char **argv) {
    int argc = 0;
    char *p = line;
    while (*p && argc < MAX_ARGS - 1) {
        while (*p == ' ') p++;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ') p++;
        if (*p) *p++ = '\0';
    }
    argv[argc] = 0;
    return argc;
}

/* Pull "> file" / "< file" out of argv, recording the redirect targets. */
static int extract_redirects(char **argv, int *argc,
                             const char **infile, const char **outfile) {
    int n = *argc, w = 0;
    *infile = 0;
    *outfile = 0;
    for (int i = 0; i < n; i++) {
        if (streq(argv[i], ">")) {
            if (i + 1 >= n) return -1;
            *outfile = argv[++i];
        } else if (streq(argv[i], "<")) {
            if (i + 1 >= n) return -1;
            *infile = argv[++i];
        } else {
            argv[w++] = argv[i];
        }
    }
    argv[w] = 0;
    *argc = w;
    return 0;
}

/* Reap any finished background jobs without blocking. */
static void reap_background(void) {
    int status;
    while (sys_waitpid(-1, &status, WNOHANG) > 0)
        write_str("[bg done]\n");
}

static void run_command(char **argv, int argc, const char *infile,
                        const char *outfile, int background) {
    int pid = sys_fork();
    if (pid == 0) {
        if (infile) {
            int fd = sys_open(infile);
            if (fd < 0) { write_str("ush: cannot open input\n"); sys_exit(1); }
            sys_dup2(fd, 0);
            sys_close(fd);
        }
        if (outfile) {
            int fd;
            sys_unlink(outfile);              /* truncate by recreating */
            fd = sys_create(outfile);
            if (fd < 0) { write_str("ush: cannot open output\n"); sys_exit(1); }
            sys_dup2(fd, 1);
            sys_close(fd);
        }
        sys_execv(argc, (const char **)argv);
        write_str("ush: command not found\n");
        sys_exit(127);
    }
    if (pid <= 0) return;
    if (background) {
        write_str("[bg ");
        write_int(pid);
        write_str("]\n");
    } else {
        sys_wait(pid);
    }
}

/* Run an N-stage pipeline (cmd0 | cmd1 | ... ). Stage i's stdout feeds stage
 * i+1's stdin through a pipe; all stages run concurrently. */
static void run_pipeline(char **argv, int argc) {
    char *stage[MAX_STAGES][MAX_ARGS];
    int   stage_argc[MAX_STAGES];
    int   pipes[MAX_STAGES - 1][2];
    int   pids[MAX_STAGES];
    int   nstages = 0;

    /* Split argv into stages on '|'. */
    stage_argc[0] = 0;
    for (int i = 0; i < argc; i++) {
        if (streq(argv[i], "|")) {
            if (stage_argc[nstages] == 0 || nstages >= MAX_STAGES - 1) {
                write_str("ush: bad pipeline\n");
                return;
            }
            stage[nstages][stage_argc[nstages]] = 0;
            nstages++;
            stage_argc[nstages] = 0;
        } else {
            stage[nstages][stage_argc[nstages]++] = argv[i];
        }
    }
    if (stage_argc[nstages] == 0) {
        write_str("ush: bad pipeline\n");
        return;
    }
    stage[nstages][stage_argc[nstages]] = 0;
    nstages++;

    for (int i = 0; i < nstages - 1; i++) {
        if (sys_pipe(pipes[i]) != 0) {
            for (int k = 0; k < i; k++) { sys_close(pipes[k][0]); sys_close(pipes[k][1]); }
            write_str("ush: pipe failed\n");
            return;
        }
    }

    for (int i = 0; i < nstages; i++) {
        pids[i] = sys_fork();
        if (pids[i] == 0) {
            if (i > 0)            sys_dup2(pipes[i - 1][0], 0);
            if (i < nstages - 1)  sys_dup2(pipes[i][1], 1);
            for (int j = 0; j < nstages - 1; j++) {
                sys_close(pipes[j][0]);
                sys_close(pipes[j][1]);
            }
            sys_execv(stage_argc[i], (const char **)stage[i]);
            sys_exit(127);
        }
    }

    for (int j = 0; j < nstages - 1; j++) {
        sys_close(pipes[j][0]);
        sys_close(pipes[j][1]);
    }
    for (int i = 0; i < nstages; i++)
        if (pids[i] > 0) sys_wait(pids[i]);
}

int main(void) {
    char line[MAX_LINE];
    char *argv[MAX_ARGS];

    write_str("ush ready\n");

    while (1) {
        reap_background();
        write_str("ush$ ");

        int n = read_line(line, sizeof(line));
        if (n <= 0) continue;

        int argc = parse(line, argv);
        if (argc == 0) continue;

        if (streq(argv[0], "exit")) break;

        /* `export NAME=VALUE` sets a shell variable (raw, before expansion).
         * Also publish it to the process environment so spawned children
         * inherit it through fork/execv. */
        if (streq(argv[0], "export")) {
            if (argc > 1) {
                char *eq = argv[1];
                while (*eq && *eq != '=') eq++;
                if (*eq == '=') {
                    *eq = '\0';
                    set_env(argv[1], eq + 1);
                    sys_setenv(argv[1], eq + 1);
                }
            }
            continue;
        }

        /* A trailing '&' runs the command in the background. */
        int background = 0;
        if (argc > 1 && streq(argv[argc - 1], "&")) {
            background = 1;
            argv[--argc] = 0;
        }

        expand_vars(argv, argc);   /* substitute $NAME tokens */

        if (streq(argv[0], "cd")) {
            if (sys_chdir(argc > 1 ? argv[1] : "/") != 0)
                write_str("ush: cd failed\n");
            continue;
        }
        if (streq(argv[0], "pwd")) {
            char cwd[128];
            sys_getcwd(cwd, sizeof(cwd));
            write_str(cwd);
            write_str("\n");
            continue;
        }
        if (streq(argv[0], "ls")) {
            char name[128];
            const char *path = argc > 1 ? argv[1] : ".";
            for (int i = 0; sys_readdir(path, i, name) == 1; i++) {
                write_str(name);
                write_str("\n");
            }
            continue;
        }
        if (streq(argv[0], "mkdir")) {
            if (argc < 2 || sys_mkdir(argv[1]) != 0)
                write_str("ush: mkdir failed\n");
            continue;
        }
        if (streq(argv[0], "rmdir")) {
            if (argc < 2 || sys_rmdir(argv[1]) != 0)
                write_str("ush: rmdir failed\n");
            continue;
        }
        if (streq(argv[0], "rm")) {
            if (argc < 2 || sys_unlink(argv[1]) != 0)
                write_str("ush: rm failed\n");
            continue;
        }
        if (streq(argv[0], "touch")) {
            int fd;
            if (argc < 2) { write_str("ush: touch needs a name\n"); continue; }
            fd = sys_open(argv[1]);
            if (fd < 0) fd = sys_create(argv[1]);
            if (fd >= 0) sys_close(fd);
            else write_str("ush: touch failed\n");
            continue;
        }
        if (streq(argv[0], "ps")) {
            struct uproc procs[16];
            int np = sys_getprocs(procs, 16);
            write_str("ush procs:\n");
            for (int i = 0; i < np; i++) {
                write_str("  ");
                write_int(procs[i].pid);
                write_str(" ");
                write_str(procs[i].name);
                write_str("\n");
            }
            continue;
        }
        if (streq(argv[0], "kill")) {
            if (argc < 2) { write_str("ush: kill needs a pid\n"); continue; }
            int sig = argc > 2 ? to_int(argv[2]) : SIGKILL;
            if (sys_kill(to_int(argv[1]), sig) != 0)
                write_str("ush: kill: no such process\n");
            continue;
        }

        int has_pipe = 0;
        for (int i = 0; i < argc; i++) {
            if (streq(argv[i], "|")) { has_pipe = 1; break; }
        }
        if (has_pipe) {
            run_pipeline(argv, argc);
        } else {
            const char *infile, *outfile;
            if (extract_redirects(argv, &argc, &infile, &outfile) != 0 ||
                argc == 0) {
                write_str("ush: syntax error\n");
                continue;
            }
            run_command(argv, argc, infile, outfile, background);
        }
    }

    write_str("ush bye\n");
    return 0;
}
