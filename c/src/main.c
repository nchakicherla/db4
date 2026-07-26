#include <stdio.h>
#include <string.h>

#include "linenoise.h"

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
