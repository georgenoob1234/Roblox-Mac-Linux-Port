/* Stand-in for RobloxPlayerInstaller/RobloxPlayer: prints a marker if the
   compat shim let the process run. */
#include <stdio.h>
int main(int argc, char **argv) {
    printf("PROBE RAN:");
    for (int i = 1; i < argc; i++) printf(" %s", argv[i]);
    printf("\n");
    return 0;
}
