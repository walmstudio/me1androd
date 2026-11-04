/* me1check: the launcher self-check as a command-line tool (adb shell / host): me1check GAME_DIR DOCS_DIR */
#include <stdio.h>
#include <stdlib.h>
char* me1port_check(const char* game, const char* docs);
int main(int c, char** v) { char* s = me1port_check(v[1], v[2]); fputs(s, stdout); free(s); return 0; }
