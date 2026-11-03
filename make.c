#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

check_deps()
{
    if (system("which gcc >/dev/null 2>&1") != 0) {
        fprintf(stderr, "gcc is not installed.\n");
        fprintf(stderr, "Run ./install.sh (or install gcc manually).\n");
        return 0;
    }
    if (system("echo 'int main(){return 0;}' | "
        "gcc -x c - -lX11 -o /tmp/.tsrchk >/dev/null 2>&1") != 0) {
        fprintf(stderr, "libX11 development files are missing.\n");
        fprintf(stderr, "Run ./install.sh (or install libx11-dev).\n");
        return 0;
        }
        system("rm -f /tmp/.tsrchk");
        return 1;
}

build_term()
{
    if (system("gcc terminator.c -lX11 -w -o a.out") != 0) {
        fprintf(stderr, "Compilation of terminator.c failed.\n");
        return 0;
    }
    return 1;
}

run_term(argc, argv)
int  argc;
char *argv[];
{
    char cmd[256];
    if (argc > 1 &&
        (strcmp(argv[1], "useProcess=True") == 0 ||
        strcmp(argv[1], "useProcess=1") == 0))
        sprintf(cmd, "./a.out %s", argv[1]);
    else
        sprintf(cmd, "./a.out");
    return system(cmd);
}

main(argc, argv)
int  argc;
char *argv[];
{
    if (!check_deps()) return 1;
    if (!build_term()) return 1;
    return run_term(argc, argv) == 0 ? 0 : 1;
}
