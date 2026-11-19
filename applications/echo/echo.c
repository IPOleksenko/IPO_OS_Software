/*
 * echo.c - Command-line echo application for IPO_OS
 *
 * Prints command-line arguments separated by space and a trailing newline,
 * or writes/appends output to a file when '>' or '>>' is specified.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

int main(int argc, char **argv) {
    int redir_idx = -1;
    bool append = false;
    size_t prefix_len = 0;
    const char *inline_file = NULL;

    for (int i = 1; i < argc; i++) {
        const char *gt = strchr(argv[i], '>');
        if (gt) {
            redir_idx = i;
            prefix_len = (size_t)(gt - argv[i]);
            if (gt[1] == '>') {
                append = true;
                if (gt[2] != '\0') {
                    inline_file = gt + 2;
                }
            } else {
                append = false;
                if (gt[1] != '\0') {
                    inline_file = gt + 1;
                }
            }
            break;
        }
    }

    if (redir_idx >= 1) {
        const char *target = inline_file ? inline_file : ((redir_idx + 1 < argc) ? argv[redir_idx + 1] : NULL);
        if (!target || !*target) {
            printf("echo: syntax error near unexpected token `newline'\n");
            return 1;
        }
        FILE *fp = fopen(target, append ? "a" : "w");
        if (!fp) {
            printf("echo: failed to write to '%s'\n", target);
            return 1;
        }
        bool printed_any = false;
        for (int i = 1; i < redir_idx; i++) {
            if (printed_any) fputc(' ', fp);
            fputs(argv[i], fp);
            printed_any = true;
        }
        if (prefix_len > 0) {
            if (printed_any) fputc(' ', fp);
            fwrite(argv[redir_idx], 1, prefix_len, fp);
        }
        fputc('\n', fp);
        fclose(fp);
        return 0;
    }

    for (int i = 1; i < argc; i++) {
        if (i > 1) {
            putchar(' ');
        }
        printf("%s", argv[i]);
    }
    putchar('\n');
    return 0;
}
