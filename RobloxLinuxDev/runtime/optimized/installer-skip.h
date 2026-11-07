#ifndef RBX_INSTALLER_SKIP_H
#define RBX_INSTALLER_SKIP_H

// Decides whether this process is Roblox's *background* updater
// (`RobloxPlayerInstaller -background true -stage prepare`), which segfaults in
// mldr's teardown. Kept free of platform headers so it can be unit-tested.
#include <string.h>

// Last path component of `path`, or NULL.
static inline const char *rbxInstallerBaseName(const char *path) {
    if (!path) return NULL;
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

// True only for an executable named RobloxPlayerInstaller that was asked to
// run in the background. A foreground install, or any other program whose path
// merely contains the installer's name, must run normally.
static inline int rbxIsBackgroundInstaller(const char *progname, int argc, char **argv) {
    const char *name = rbxInstallerBaseName(progname);
    if (!name || strcmp(name, "RobloxPlayerInstaller") != 0) {
        const char *arg0 = argc > 0 && argv ? rbxInstallerBaseName(argv[0]) : NULL;
        if (!arg0 || strcmp(arg0, "RobloxPlayerInstaller") != 0) return 0;
    }
    for (int i = 1; argv && i + 1 < argc; i++)
        if (argv[i] && argv[i + 1] && !strcmp(argv[i], "-background") && !strcmp(argv[i + 1], "true"))
            return 1;
    return 0;
}

#endif
