/*
 * solar_os_compat_pico_vfs.c - POSIX filesystem layer over FatFs for RP2350.
 *
 * This is the RP2350 stand-in for ESP-IDF's VFS. It provides:
 *
 *   - the directory API declared in the local dirent.h
 *   - stat/mkdir/rmdir/unlink/rename/access
 *   - the newlib syscall hooks (_open/_close/_read/_write/_lseek/_fstat) that
 *     make fopen/fread/fprintf work on files, so the shell's cat/edit/less
 *     paths behave
 *
 * pico-sdk declares all of those syscalls __weak in
 * pico_clib_interface/newlib_interface.c, so defining them here overrides them
 * cleanly. File descriptors 0, 1 and 2 are deliberately left to pico-sdk's
 * stdio so console I/O keeps working exactly as before; only descriptors this
 * layer hands out are routed to FatFs.
 *
 * PATH MAPPING
 * SolarOS mounts the card at /sdcard (SOLAR_OS_BOARD_STORAGE_DEFAULT_MOUNT_POINT)
 * and every on-disk convention - the .shell, .ssh and .reader directories - is
 * a relative path underneath it. FatFs addresses the same volume as "0:". So
 * "/sdcard/.shell/history" becomes "0:/.shell/history". That translation is
 * the only place the mount point is interpreted; nothing above this file
 * changes, which is what keeps the existing storage layout valid.
 *
 * NOT IMPLEMENTED, and deliberately so:
 *   - symlinks: FAT has none, so lstat is stat and there is no readlink.
 *   - permissions: st_mode reports type plus a fixed rw-r--r--/rwxr-xr-x.
 *     FAT has a read-only attribute and nothing else; inventing modes would
 *     make `ls -l` output look more meaningful than it is.
 *   - timestamps: FAT stores a single modification time. st_atime and
 *     st_ctime are reported as st_mtime rather than as zero, since "unknown"
 *     is closer to mtime than to the epoch.
 *
 * Never run on hardware.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/*
 * FatFs calls its directory object DIR, which collides with the POSIX DIR in
 * dirent.h. Rename FatFs's for the duration of its own header - every f_*
 * prototype picks up the renamed type consistently - then restore the name so
 * dirent.h's POSIX DIR is the one the rest of this file sees.
 */
#define DIR FATFS_DIR
#include "ff.h"
#undef DIR

#include "dirent.h"
#include "pico/stdio.h"
#include "pico/time.h"
#include "solar_os_board_storage.h"

/* --- Path translation ---------------------------------------------------- */

#define VFS_VOLUME_PREFIX "0:"
#define VFS_PATH_MAX 512

/*
 * Translate a SolarOS path into a FatFs path.
 *
 * Accepts both the mount-point form ("/sdcard/x") and a bare absolute path
 * ("/x"), because the shell normalises paths in both shapes depending on
 * whether a cwd was applied. Returns false for anything that cannot be a file
 * on the card, which the callers turn into ENOENT.
 */
static bool vfs_translate(const char *path, char *out, size_t out_len)
{
    if (path == NULL || out == NULL || out_len < 4) {
        return false;
    }

    const char *mount = SOLAR_OS_BOARD_STORAGE_DEFAULT_MOUNT_POINT;
    const size_t mount_len = strlen(mount);
    const char *relative = path;

    if (strncmp(path, mount, mount_len) == 0) {
        relative = path + mount_len;
        /* "/sdcard" exactly means the volume root. */
        if (*relative == '\0') {
            return snprintf(out, out_len, "%s/", VFS_VOLUME_PREFIX) < (int)out_len;
        }
        if (*relative != '/') {
            /* "/sdcardfoo" is not inside the mount point. */
            return false;
        }
    } else if (path[0] != '/') {
        /* Relative paths are resolved by FatFs against its own cwd
         * (FF_FS_RPATH is 2), so they pass through unchanged. */
        return snprintf(out, out_len, "%s", path) < (int)out_len;
    }

    return snprintf(out, out_len, "%s%s", VFS_VOLUME_PREFIX, relative) < (int)out_len;
}

/* Map a FatFs result onto errno, and return -1/0 the way POSIX wants. */
static int vfs_errno_from_fresult(FRESULT result)
{
    switch (result) {
    case FR_OK:                 return 0;
    case FR_NO_FILE:
    case FR_NO_PATH:            errno = ENOENT; break;
    case FR_INVALID_NAME:       errno = EINVAL; break;
    case FR_DENIED:             errno = EACCES; break;
    case FR_EXIST:              errno = EEXIST; break;
    case FR_WRITE_PROTECTED:    errno = EROFS; break;
    case FR_NOT_ENABLED:
    case FR_NO_FILESYSTEM:
    case FR_NOT_READY:          errno = ENODEV; break;
    case FR_TIMEOUT:            errno = EBUSY; break;
    case FR_NOT_ENOUGH_CORE:    errno = ENOMEM; break;
    case FR_TOO_MANY_OPEN_FILES: errno = EMFILE; break;
    case FR_INVALID_OBJECT:     errno = EBADF; break;
    case FR_DISK_ERR:
    case FR_INT_ERR:
    default:                    errno = EIO; break;
    }
    return -1;
}

/*
 * Convert a FAT date/time pair into a time_t.
 *
 * FAT date: bits 15:9 year-1980, 8:5 month, 4:0 day.
 * FAT time: bits 15:11 hour, 10:5 minute, 4:0 second/2.
 */
static time_t vfs_fat_to_time(WORD fdate, WORD ftime)
{
    struct tm parts;
    memset(&parts, 0, sizeof(parts));
    parts.tm_year = ((fdate >> 9) & 0x7F) + 80; /* years since 1900 */
    parts.tm_mon = (int)((fdate >> 5) & 0x0F) - 1;
    parts.tm_mday = (int)(fdate & 0x1F);
    parts.tm_hour = (int)((ftime >> 11) & 0x1F);
    parts.tm_min = (int)((ftime >> 5) & 0x3F);
    parts.tm_sec = (int)(ftime & 0x1F) * 2;
    parts.tm_isdst = -1;
    return mktime(&parts);
}

static void vfs_fill_stat(struct stat *out, const FILINFO *info)
{
    memset(out, 0, sizeof(*out));
    const bool is_dir = (info->fattrib & AM_DIR) != 0;
    const bool read_only = (info->fattrib & AM_RDO) != 0;

    /* FAT has one permission bit (read-only) and no ownership. The modes
     * below are a fixed, honest approximation - see the file header. */
    if (is_dir) {
        out->st_mode = S_IFDIR | (read_only ? 0555 : 0755);
    } else {
        out->st_mode = S_IFREG | (read_only ? 0444 : 0644);
    }
    out->st_size = (off_t)info->fsize;
    out->st_nlink = 1;

    const time_t modified = vfs_fat_to_time(info->fdate, info->ftime);
    out->st_mtime = modified;
    /* FAT stores no access or change time; reporting mtime is closer to the
     * truth than reporting the epoch. */
    out->st_atime = modified;
    out->st_ctime = modified;
}

/* --- Directories --------------------------------------------------------- */

struct solar_os_compat_dir {
    FATFS_DIR fat_dir; /* FatFs's directory object, renamed above */
    struct dirent entry;
    char path[VFS_PATH_MAX];
    bool open;
};

/*
 * Bounded pool rather than malloc: directory handles are short-lived and few
 * (the file manager holds one, the shell one), and a fixed pool keeps a leak
 * from fragmenting the heap that task admission is measured against.
 */
#define VFS_MAX_DIRS 4
static struct solar_os_compat_dir vfs_dirs[VFS_MAX_DIRS];

DIR *opendir(const char *name)
{
    char translated[VFS_PATH_MAX];
    if (!vfs_translate(name, translated, sizeof(translated))) {
        errno = ENOENT;
        return NULL;
    }

    struct solar_os_compat_dir *slot = NULL;
    for (size_t i = 0; i < VFS_MAX_DIRS; i++) {
        if (!vfs_dirs[i].open) {
            slot = &vfs_dirs[i];
            break;
        }
    }
    if (slot == NULL) {
        errno = EMFILE;
        return NULL;
    }

    const FRESULT result = f_opendir(&slot->fat_dir, translated);
    if (result != FR_OK) {
        (void)vfs_errno_from_fresult(result);
        return NULL;
    }

    slot->open = true;
    snprintf(slot->path, sizeof(slot->path), "%s", translated);
    return slot;
}

struct dirent *readdir(DIR *dirp)
{
    if (dirp == NULL || !dirp->open) {
        errno = EBADF;
        return NULL;
    }

    FILINFO info;
    const FRESULT result = f_readdir(&dirp->fat_dir, &info);
    if (result != FR_OK) {
        (void)vfs_errno_from_fresult(result);
        return NULL;
    }
    if (info.fname[0] == '\0') {
        /* End of directory. POSIX says leave errno alone here. */
        return NULL;
    }

    memset(&dirp->entry, 0, sizeof(dirp->entry));
    snprintf(dirp->entry.d_name, sizeof(dirp->entry.d_name), "%s", info.fname);
    dirp->entry.d_type = (info.fattrib & AM_DIR) ? DT_DIR : DT_REG;
    /* FAT has no inode numbers. 0 is the conventional "unknown". */
    dirp->entry.d_ino = 0;
    return &dirp->entry;
}

int closedir(DIR *dirp)
{
    if (dirp == NULL || !dirp->open) {
        errno = EBADF;
        return -1;
    }
    const FRESULT result = f_closedir(&dirp->fat_dir);
    dirp->open = false;
    return vfs_errno_from_fresult(result);
}

void rewinddir(DIR *dirp)
{
    if (dirp == NULL || !dirp->open) {
        return;
    }
    /* FatFs rewinds when f_readdir is passed a NULL FILINFO. */
    (void)f_readdir(&dirp->fat_dir, NULL);
}

/* --- Metadata and namespace operations ----------------------------------- */

int stat(const char *path, struct stat *out)
{
    char translated[VFS_PATH_MAX];
    if (out == NULL || !vfs_translate(path, translated, sizeof(translated))) {
        errno = ENOENT;
        return -1;
    }

    /* f_stat cannot describe a volume root, so answer for it directly. */
    const size_t length = strlen(translated);
    if (length <= 3 && strncmp(translated, VFS_VOLUME_PREFIX, 2) == 0) {
        memset(out, 0, sizeof(*out));
        out->st_mode = S_IFDIR | 0755;
        out->st_nlink = 1;
        return 0;
    }

    FILINFO info;
    const FRESULT result = f_stat(translated, &info);
    if (result != FR_OK) {
        return vfs_errno_from_fresult(result);
    }
    vfs_fill_stat(out, &info);
    return 0;
}

int mkdir(const char *path, mode_t mode)
{
    (void)mode; /* FAT has no permission bits to apply. */
    char translated[VFS_PATH_MAX];
    if (!vfs_translate(path, translated, sizeof(translated))) {
        errno = ENOENT;
        return -1;
    }
    return vfs_errno_from_fresult(f_mkdir(translated));
}

int rmdir(const char *path)
{
    char translated[VFS_PATH_MAX];
    if (!vfs_translate(path, translated, sizeof(translated))) {
        errno = ENOENT;
        return -1;
    }
    /* FatFs uses one call for both; it refuses a non-empty directory. */
    return vfs_errno_from_fresult(f_unlink(translated));
}

int unlink(const char *path)
{
    char translated[VFS_PATH_MAX];
    if (!vfs_translate(path, translated, sizeof(translated))) {
        errno = ENOENT;
        return -1;
    }
    return vfs_errno_from_fresult(f_unlink(translated));
}

int rename(const char *from, const char *to)
{
    char translated_from[VFS_PATH_MAX];
    char translated_to[VFS_PATH_MAX];
    if (!vfs_translate(from, translated_from, sizeof(translated_from)) ||
        !vfs_translate(to, translated_to, sizeof(translated_to))) {
        errno = ENOENT;
        return -1;
    }
    /*
     * POSIX rename replaces an existing destination; f_rename refuses with
     * FR_EXIST. Remove the destination first so callers see POSIX behaviour.
     * Not atomic - a crash between the two leaves the destination gone. FAT
     * offers no atomic replace, so this is the best available.
     */
    FILINFO info;
    if (f_stat(translated_to, &info) == FR_OK) {
        (void)f_unlink(translated_to);
    }
    return vfs_errno_from_fresult(f_rename(translated_from, translated_to));
}

int access(const char *path, int mode)
{
    struct stat info;
    if (stat(path, &info) != 0) {
        return -1;
    }
    if ((mode & W_OK) != 0 && (info.st_mode & S_IWUSR) == 0) {
        errno = EACCES;
        return -1;
    }
    return 0;
}

/* --- newlib syscall hooks ------------------------------------------------ */

/*
 * File descriptors. 0/1/2 belong to pico-sdk's stdio and are passed straight
 * through to its weak implementations; anything from VFS_FD_BASE up is ours.
 */
#define VFS_FD_BASE 3
#define VFS_MAX_FILES 8

typedef struct {
    FIL file;
    bool open;
} vfs_file_t;

static vfs_file_t vfs_files[VFS_MAX_FILES];

static vfs_file_t *vfs_file_for(int fd)
{
    const int index = fd - VFS_FD_BASE;
    if (index < 0 || index >= VFS_MAX_FILES || !vfs_files[index].open) {
        return NULL;
    }
    return &vfs_files[index];
}

/*
 * Console passthrough for descriptors 0/1/2.
 *
 * pico-sdk's _read/_write are __weak, so the strong definitions below replace
 * them outright and there is no __real_ symbol to chain to. These reproduce
 * exactly what pico-sdk's versions do for the console handles, so stdout,
 * stderr and stdin behave identically to a plain pico-sdk program.
 */
static int vfs_console_read(int handle, char *buffer, int length)
{
    if (handle != 0) {
        errno = EBADF;
        return -1;
    }
    return stdio_get_until(buffer, length, at_the_end_of_time);
}

static int vfs_console_write(int handle, char *buffer, int length)
{
    if (handle != 1 && handle != 2) {
        errno = EBADF;
        return -1;
    }
    stdio_put_string(buffer, length, false, true);
    return length;
}

int _open(const char *name, int flags, ...)
{
    char translated[VFS_PATH_MAX];
    if (!vfs_translate(name, translated, sizeof(translated))) {
        errno = ENOENT;
        return -1;
    }

    int index = -1;
    for (int i = 0; i < VFS_MAX_FILES; i++) {
        if (!vfs_files[i].open) {
            index = i;
            break;
        }
    }
    if (index < 0) {
        errno = EMFILE;
        return -1;
    }

    /* Translate POSIX open flags into FatFs mode bits. */
    BYTE mode = 0;
    const int accmode = flags & O_ACCMODE;
    if (accmode == O_RDONLY) {
        mode = FA_READ;
    } else if (accmode == O_WRONLY) {
        mode = FA_WRITE;
    } else {
        mode = FA_READ | FA_WRITE;
    }
    if ((flags & O_CREAT) != 0) {
        mode |= (flags & O_EXCL) ? FA_CREATE_NEW : FA_OPEN_ALWAYS;
    }
    if ((flags & O_TRUNC) != 0) {
        mode |= FA_CREATE_ALWAYS;
    }
    if ((flags & O_APPEND) != 0) {
        mode |= FA_OPEN_APPEND;
    }

    const FRESULT result = f_open(&vfs_files[index].file, translated, mode);
    if (result != FR_OK) {
        return vfs_errno_from_fresult(result);
    }
    vfs_files[index].open = true;
    return VFS_FD_BASE + index;
}

int _close(int fd)
{
    vfs_file_t *entry = vfs_file_for(fd);
    if (entry == NULL) {
        /* Not ours: leave the console descriptors alone. */
        if (fd >= 0 && fd < VFS_FD_BASE) {
            return 0;
        }
        errno = EBADF;
        return -1;
    }
    const FRESULT result = f_close(&entry->file);
    entry->open = false;
    return vfs_errno_from_fresult(result);
}

int _read(int fd, char *buffer, int length)
{
    vfs_file_t *entry = vfs_file_for(fd);
    if (entry == NULL) {
        return vfs_console_read(fd, buffer, length);
    }
    if (buffer == NULL || length < 0) {
        errno = EINVAL;
        return -1;
    }

    UINT read = 0;
    const FRESULT result = f_read(&entry->file, buffer, (UINT)length, &read);
    if (result != FR_OK) {
        return vfs_errno_from_fresult(result);
    }
    return (int)read;
}

int _write(int fd, char *buffer, int length)
{
    vfs_file_t *entry = vfs_file_for(fd);
    if (entry == NULL) {
        return vfs_console_write(fd, buffer, length);
    }
    if (buffer == NULL || length < 0) {
        errno = EINVAL;
        return -1;
    }

    UINT written = 0;
    const FRESULT result = f_write(&entry->file, buffer, (UINT)length, &written);
    if (result != FR_OK) {
        return vfs_errno_from_fresult(result);
    }
    return (int)written;
}

off_t _lseek(int fd, off_t offset, int whence)
{
    vfs_file_t *entry = vfs_file_for(fd);
    if (entry == NULL) {
        /* Console descriptors are not seekable. */
        errno = ESPIPE;
        return (off_t)-1;
    }

    FSIZE_t target;
    switch (whence) {
    case SEEK_SET:
        target = (FSIZE_t)offset;
        break;
    case SEEK_CUR:
        target = f_tell(&entry->file) + (FSIZE_t)offset;
        break;
    case SEEK_END:
        target = f_size(&entry->file) + (FSIZE_t)offset;
        break;
    default:
        errno = EINVAL;
        return (off_t)-1;
    }

    const FRESULT result = f_lseek(&entry->file, target);
    if (result != FR_OK) {
        (void)vfs_errno_from_fresult(result);
        return (off_t)-1;
    }
    return (off_t)f_tell(&entry->file);
}

int _fstat(int fd, struct stat *out)
{
    if (out == NULL) {
        errno = EINVAL;
        return -1;
    }

    vfs_file_t *entry = vfs_file_for(fd);
    if (entry == NULL) {
        /* Console descriptor: report a character device so stdio does not
         * try to buffer it as a regular file. */
        memset(out, 0, sizeof(*out));
        out->st_mode = S_IFCHR;
        return 0;
    }

    memset(out, 0, sizeof(*out));
    out->st_mode = S_IFREG | 0644;
    out->st_size = (off_t)f_size(&entry->file);
    out->st_nlink = 1;
    return 0;
}

int _isatty(int fd)
{
    return vfs_file_for(fd) == NULL ? 1 : 0;
}

int _unlink(const char *path)
{
    return unlink(path);
}

int _stat(const char *path, struct stat *out)
{
    return stat(path, out);
}

int fsync(int fd)
{
    vfs_file_t *entry = vfs_file_for(fd);
    if (entry == NULL) {
        return 0;
    }
    return vfs_errno_from_fresult(f_sync(&entry->file));
}
