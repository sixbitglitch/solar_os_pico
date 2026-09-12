/*
 * dirent.h - POSIX directory API for the RP2350 target.
 *
 * newlib for arm-none-eabi ships no <dirent.h> at all ("#error <dirent.h> not
 * supported"), because bare-metal newlib has no filesystem. ESP-IDF supplies
 * one through its VFS layer; this is the equivalent for this port, implemented
 * over FatFs in solar_os_compat_pico_vfs.c.
 *
 * Five SolarOS sources need it - the file manager, the shell's fs commands,
 * the shell app, the zip service and the command table - so the alternative
 * was losing ls/cd/cat, which is most of what a terminal OS is for.
 *
 * Only the subset those sources actually use is provided: opendir, readdir,
 * closedir, rewinddir. There is no telldir/seekdir/scandir, because FatFs's
 * directory objects have no stable cookie to hand out and a half-working
 * seekdir is worse than none.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* d_type values. Only the two FatFs can distinguish are ever returned. */
#define DT_UNKNOWN 0
#define DT_DIR     4
#define DT_REG     8

/* FatFs is configured with FF_USE_LFN, so long names are available. 255 is
 * FF_MAX_LFN; the +1 is the terminator. */
#define NAME_MAX 255

struct dirent {
    ino_t d_ino;
    unsigned char d_type;
    char d_name[NAME_MAX + 1];
};

typedef struct solar_os_compat_dir DIR;

DIR *opendir(const char *name);
struct dirent *readdir(DIR *dirp);
int closedir(DIR *dirp);
void rewinddir(DIR *dirp);

#ifdef __cplusplus
}
#endif
