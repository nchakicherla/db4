#include <stdio.h>
#include <string.h>

#include "linenoise.h"

// A placeholder REPL: reads a line, echoes it back, does nothing else.
// arena.c/csv.c are compiled into this binary but not called from
// anywhere yet - scaffolding for later, not wired up.
int main(void) {
    linenoiseHistorySetMaxLen(100);

    char *line;
    while ((line = linenoise("db4> ")) != NULL) {
        if (line[0] == '\0') {
            linenoiseFree(line);
            continue;
        }

        linenoiseHistoryAdd(line);

        if (strcmp(line, ".quit") == 0 || strcmp(line, ".exit") == 0) {
            linenoiseFree(line);
            break;
        }

        printf("%s\n", line);
        linenoiseFree(line);
    }

    return 0;
}
