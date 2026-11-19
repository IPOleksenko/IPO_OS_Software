/**
 * term_ctl - terminal control utility
 *
 * Usage:
 *   term_ctl <text...>        inject text into the terminal and execute it
 *   term_ctl -w <text...>     inject text into the prompt only (no execution)
 *
 * Without arguments: interactive demo mode.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syscall.h>

static char *join_args_alloc(int argc, char **argv, int start) {
    size_t cap = 64u;
    size_t len = 0u;
    char *out = (char *)malloc(cap);
    if (!out) return NULL;
    out[0] = '\0';

    for (int i = start; i < argc; i++) {
        size_t alen = strlen(argv[i]);
        size_t need = len + alen + (i + 1 < argc ? 1u : 0u) + 1u;
        if (need > cap) {
            while (cap < need) cap *= 2u;
            char *nb = (char *)realloc(out, cap);
            if (!nb) {
                free(out);
                return NULL;
            }
            out = nb;
        }
        memcpy(out + len, argv[i], alen);
        len += alen;
        if (i + 1 < argc) {
            out[len++] = ' ';
        }
        out[len] = '\0';
    }
    return out;
}

static char *read_line_alloc(void) {
    size_t cap = 64u;
    size_t len = 0u;
    char *buf = (char *)malloc(cap);
    if (!buf) return NULL;

    int ch;
    while ((ch = fgetc(stdin)) != EOF && ch != '\n') {
        if (len + 2u > cap) {
            cap *= 2u;
            char *nb = (char *)realloc(buf, cap);
            if (!nb) {
                free(buf);
                return NULL;
            }
            buf = nb;
        }
        buf[len++] = (char)ch;
    }
    buf[len] = '\0';
    return buf;
}

int main(int argc, char **argv) {
    if (argc >= 3 && argv[1][0] == '-' && argv[1][1] == 'w' && argv[1][2] == '\0') {
        /* term_ctl -w <text...> -- write to prompt only, no execution */
        char *buf = join_args_alloc(argc, argv, 2);
        if (!buf) return 1;
        printf("Writing to prompt: %s\n", buf);
        ipo_terminal_input(buf, 0);
        free(buf);
        return 0;
    }

    if (argc >= 2) {
        /* term_ctl <text...> -- inject and auto-execute */
        char *buf = join_args_alloc(argc, argv, 1);
        if (!buf) return 1;
        printf("Executing: %s\n", buf);
        ipo_terminal_input(buf, 1);
        free(buf);
        return 0;
    }

    /* Interactive demo */
    printf("=== term_ctl demo ===\n");
    printf("1. Execute 'hello'\n");
    printf("2. Execute 'ps'\n");
    printf("3. Write custom text to prompt (no execute)\n");
    printf("4. Execute custom command\n");
    printf("Choice [1/2/3/4]: ");

    char *choice = read_line_alloc();
    if (!choice) return 1;

    if (choice[0] == '1') {
        printf("Executing 'hello'...\n");
        ipo_terminal_input("hello", 1);
    } else if (choice[0] == '2') {
        printf("Executing 'ps'...\n");
        ipo_terminal_input("ps", 1);
    } else if (choice[0] == '3' || choice[0] == '4') {
        int exec_mode = (choice[0] == '4') ? 1 : 0;
        printf(exec_mode ? "Enter command: " : "Enter text: ");
        char *text = read_line_alloc();
        if (text) {
            printf(exec_mode ? "Executing: %s\n" : "Writing to prompt: %s\n", text);
            ipo_terminal_input(text, exec_mode);
            free(text);
        }
    } else {
        printf("Unknown choice.\n");
    }

    free(choice);
    return 0;
}
