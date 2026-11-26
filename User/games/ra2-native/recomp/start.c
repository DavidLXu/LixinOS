// A plain shell launch starts the game from the shared retail-data package.
// Explicit upstream options, including the mapping-only dry run, pass through.
#include <stdio.h>
#ifndef LX_RA2_PREFLIGHT
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include "mods.h"
#include "recomp_target.h"
#include "win32hle.h"
extern void __real_mods_start(const char *, const char *, const char *,
                              int, char **, void (*)(void));

void __wrap_mods_start(const char *game, const char *requested, const char *remembered,
                       int argc, char **argv, void (*save)(void)) {
    __real_mods_start(game, requested, remembered, argc, argv, save);
    if (*mods_active() || strcmp(game, "/lib/ra2-content")) return;
#ifdef RA2_TARGET_GAME
    const char *profile = "/scratch/ra2-profile";
#else
    const char *profile = "/scratch/yr-profile";
#endif
    if ((mkdir("/scratch", 0755) && errno != EEXIST) ||
        (mkdir(profile, 0755) && errno != EEXIST)) {
        perror("RA2 profile directory");
        return;
    }
    // The retail game saves its INI a few bytes at a time. The build disk
    // checkpoints scratch files at shutdown, avoiding a snapshot per write.
    hle_set_overlay(game, profile);
}
#endif
extern int __real_main(int,char **);
int __wrap_main(int argc,char **argv) {
#ifdef LX_RA2_PREFLIGHT
    puts("Red Alert 2 recompiled host: retail PE loader (no translated game code).");
    puts("Import the retail data and rebuild with RA2_DIR to play.");
#endif
    if(argc==1) {
        char *args[]={argv[0],"--game","/lib/ra2-content","--run",0};
        return __real_main(4,args);
    }
    return __real_main(argc,argv);
}
