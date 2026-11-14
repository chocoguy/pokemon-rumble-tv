/* NAND: the console's flash file system, as a folder on the host (gPortConfig.nandDir).
 *
 * A NAND path maps to <nandDir><absolute path>; relative paths start at the current
 * directory, which is the title's home directory /title/00010001/57505345/data. The
 * operations run synchronously; the async variants deliver their callback in interrupt
 * context at the next safe point, as the IPC interrupt did on the console. */
#include <port_os.h>
#include <revolution/nand.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define HOME_DIR "/title/00010001/57505345/data"

static char sCurrentDir[NAND_MAX_PATH] = HOME_DIR;
static int sInitialized;

static int MakeDirs(const char* hostPath) {
    char buf[1024];
    char* p;
    snprintf(buf, sizeof(buf), "%s", hostPath);
    for (p = buf + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(buf, 0755);
            *p = '/';
        }
    }
    return mkdir(buf, 0755) == 0 || errno == EEXIST;
}

static void InitOnce(void) {
    char buf[1024];
    if (sInitialized) {
        return;
    }
    sInitialized = 1;
    snprintf(buf, sizeof(buf), "%s%s", gPortConfig.nandDir, HOME_DIR);
    MakeDirs(buf);
    snprintf(buf, sizeof(buf), "%s/tmp", gPortConfig.nandDir);
    MakeDirs(buf);
}

/* Resolves `path` (absolute, or relative to the current directory, with . and ..) to an
 * absolute NAND path. */
static int AbsPath(const char* path, char* out, size_t outSize) {
    char joined[256];
    char* parts[32];
    int count = 0;
    char* token;
    char* save;
    size_t len;
    int i;

    if (path[0] == '/') {
        snprintf(joined, sizeof(joined), "%s", path);
    } else {
        snprintf(joined, sizeof(joined), "%s/%s", sCurrentDir, path);
    }
    for (token = strtok_r(joined, "/", &save); token; token = strtok_r(NULL, "/", &save)) {
        if (strcmp(token, ".") == 0 || token[0] == '\0') {
            continue;
        }
        if (strcmp(token, "..") == 0) {
            if (count > 0) {
                count--;
            }
            continue;
        }
        if (count == 32) {
            return NAND_RESULT_MAXDEPTH;
        }
        parts[count++] = token;
    }
    out[0] = '\0';
    len = 0;
    for (i = 0; i < count; i++) {
        len += (size_t)snprintf(out + len, outSize - len, "/%s", parts[i]);
    }
    if (count == 0) {
        snprintf(out, outSize, "/");
    }
    return NAND_RESULT_OK;
}

static int HostPath(const char* path, char* out, size_t outSize) {
    char abs[256];
    int result;
    InitOnce();
    result = AbsPath(path, abs, sizeof(abs));
    if (result != NAND_RESULT_OK) {
        return result;
    }
    snprintf(out, outSize, "%s%s", gPortConfig.nandDir, abs);
    return NAND_RESULT_OK;
}

static s32 FromErrno(int error) {
    switch (error) {
    case ENOENT:
    case ENOTDIR:
        return NAND_RESULT_NOEXISTS;
    case EEXIST:
        return NAND_RESULT_EXISTS;
    case ENOTEMPTY:
        return NAND_RESULT_NOTEMPTY;
    case EACCES:
    case EPERM:
        return NAND_RESULT_ACCESS;
    case ENOSPC:
        return NAND_RESULT_MAXBLOCKS;
    case EMFILE:
        return NAND_RESULT_MAXFD;
    }
    return NAND_RESULT_UNKNOWN;
}

/* ------------------------------------------------------------------------------------ */
/* Synchronous operations                                                                */

s32 NANDInit(void) {
    InitOnce();
    return NAND_RESULT_OK;
}

s32 NANDGetCurrentDir(char* path) {
    strcpy(path, sCurrentDir);
    return NAND_RESULT_OK;
}

s32 NANDGetHomeDir(char* path) {
    strcpy(path, HOME_DIR);
    return NAND_RESULT_OK;
}

s32 NANDChangeDir(const char* path) {
    char abs[256];
    char host[1024];
    struct stat st;
    if (AbsPath(path, abs, sizeof(abs)) != NAND_RESULT_OK || HostPath(abs, host, sizeof(host)) != NAND_RESULT_OK) {
        return NAND_RESULT_INVALID;
    }
    if (stat(host, &st) != 0 || !S_ISDIR(st.st_mode)) {
        return NAND_RESULT_NOEXISTS;
    }
    snprintf(sCurrentDir, sizeof(sCurrentDir), "%s", abs);
    return NAND_RESULT_OK;
}

s32 NANDCreate(const char* path, u8 perm, u8 attr) {
    char host[1024];
    int fd;
    (void)perm;
    (void)attr;
    if (HostPath(path, host, sizeof(host)) != NAND_RESULT_OK) {
        return NAND_RESULT_INVALID;
    }
    fd = open(host, O_CREAT | O_EXCL | O_WRONLY, 0644);
    if (fd < 0) {
        return FromErrno(errno);
    }
    close(fd);
    return NAND_RESULT_OK;
}

s32 NANDCreateDir(const char* path, u8 perm, u8 attr) {
    char host[1024];
    (void)perm;
    (void)attr;
    if (HostPath(path, host, sizeof(host)) != NAND_RESULT_OK) {
        return NAND_RESULT_INVALID;
    }
    return mkdir(host, 0755) == 0 ? NAND_RESULT_OK : FromErrno(errno);
}

static int RemoveTree(const char* host) {
    struct stat st;
    if (lstat(host, &st) != 0) {
        return -1;
    }
    if (S_ISDIR(st.st_mode)) {
        DIR* dir = opendir(host);
        struct dirent* entry;
        if (dir == NULL) {
            return -1;
        }
        while ((entry = readdir(dir)) != NULL) {
            char child[1024];
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }
            snprintf(child, sizeof(child), "%s/%s", host, entry->d_name);
            RemoveTree(child);
        }
        closedir(dir);
        return rmdir(host);
    }
    return unlink(host);
}

s32 NANDDelete(const char* path) {
    char host[1024];
    if (HostPath(path, host, sizeof(host)) != NAND_RESULT_OK) {
        return NAND_RESULT_INVALID;
    }
    return RemoveTree(host) == 0 ? NAND_RESULT_OK : FromErrno(errno);
}

/* Moves `path` into the directory `destDir`, keeping its name. */
s32 NANDMove(const char* path, const char* destDir) {
    char from[1024];
    char dirHost[1024];
    char to[1200];
    const char* name = strrchr(path, '/');
    struct stat st;
    name = name ? name + 1 : path;
    if (HostPath(path, from, sizeof(from)) != NAND_RESULT_OK || HostPath(destDir, dirHost, sizeof(dirHost)) != NAND_RESULT_OK) {
        return NAND_RESULT_INVALID;
    }
    if (stat(from, &st) != 0) {
        return NAND_RESULT_NOEXISTS;
    }
    snprintf(to, sizeof(to), "%s/%s", dirHost, name);
    if (strcmp(from, to) == 0) {
        return NAND_RESULT_OK;
    }
    if (stat(to, &st) == 0) {
        /* The console's move fails onto an existing name; the game deletes first. */
        return NAND_RESULT_EXISTS;
    }
    return rename(from, to) == 0 ? NAND_RESULT_OK : FromErrno(errno);
}

s32 NANDOpen(const char* path, NANDFileInfo* info, u8 accType) {
    char host[1024];
    char abs[256];
    int flags;
    int fd;
    struct stat st;

    if (AbsPath(path, abs, sizeof(abs)) != NAND_RESULT_OK || HostPath(abs, host, sizeof(host)) != NAND_RESULT_OK) {
        return NAND_RESULT_INVALID;
    }
    switch (accType) {
    case NAND_ACCESS_READ:
        flags = O_RDONLY;
        break;
    case NAND_ACCESS_WRITE:
        flags = O_WRONLY;
        break;
    case NAND_ACCESS_READ | NAND_ACCESS_WRITE:
        flags = O_RDWR;
        break;
    default:
        return NAND_RESULT_INVALID;
    }
    if (stat(host, &st) != 0) {
        return NAND_RESULT_NOEXISTS;
    }
    if (S_ISDIR(st.st_mode)) {
        return NAND_RESULT_INVALID;
    }
    fd = open(host, flags);
    if (fd < 0) {
        return FromErrno(errno);
    }
    memset(info, 0, sizeof(*info));
    info->fileDescriptor = fd;
    info->origFd = -1;
    snprintf(info->origPath, sizeof(info->origPath), "%s", abs);
    info->accType = accType;
    info->mark = 1;
    return NAND_RESULT_OK;
}

s32 NANDPrivateOpen(const char* path, NANDFileInfo* info, u8 accType) {
    return NANDOpen(path, info, accType);
}

s32 NANDClose(NANDFileInfo* info) {
    if (info->mark != 1) {
        return NAND_RESULT_INVALID;
    }
    close(info->fileDescriptor);
    info->mark = 0;
    return NAND_RESULT_OK;
}

s32 NANDRead(NANDFileInfo* info, void* buf, u32 length) {
    ssize_t n = read(info->fileDescriptor, buf, length);
    return n < 0 ? FromErrno(errno) : (s32)n;
}

s32 NANDWrite(NANDFileInfo* info, const void* buf, u32 length) {
    ssize_t n = write(info->fileDescriptor, buf, length);
    return n < 0 ? FromErrno(errno) : (s32)n;
}

s32 NANDSeek(NANDFileInfo* info, s32 offset, s32 whence) {
    int hostWhence = whence == NAND_SEEK_SET ? SEEK_SET : whence == NAND_SEEK_CUR ? SEEK_CUR : SEEK_END;
    off_t pos = lseek(info->fileDescriptor, offset, hostWhence);
    return pos < 0 ? NAND_RESULT_INVALID : (s32)pos;
}

s32 NANDGetLength(NANDFileInfo* info, u32* length) {
    struct stat st;
    if (fstat(info->fileDescriptor, &st) != 0) {
        return FromErrno(errno);
    }
    *length = (u32)st.st_size;
    return NAND_RESULT_OK;
}

s32 NANDTell(NANDFileInfo* info, u32* pos) {
    off_t p = lseek(info->fileDescriptor, 0, SEEK_CUR);
    if (p < 0) {
        return NAND_RESULT_INVALID;
    }
    *pos = (u32)p;
    return NAND_RESULT_OK;
}

s32 NANDGetStatus(const char* path, NANDStatus* stat_) {
    char host[1024];
    struct stat st;
    if (HostPath(path, host, sizeof(host)) != NAND_RESULT_OK) {
        return NAND_RESULT_INVALID;
    }
    if (stat(host, &st) != 0) {
        return NAND_RESULT_NOEXISTS;
    }
    stat_->ownerId = 0x1000;
    stat_->groupId = 0x3031;
    stat_->attribute = 0;
    stat_->permission = NAND_PERM_RUSR | NAND_PERM_WUSR | NAND_PERM_RGRP | NAND_PERM_WGRP;
    return NAND_RESULT_OK;
}

/* Blocks of 16 KB and inodes used under `host`. */
static void Usage(const char* host, u32* blocks, u32* inodes) {
    DIR* dir = opendir(host);
    struct dirent* entry;
    if (dir == NULL) {
        return;
    }
    while ((entry = readdir(dir)) != NULL) {
        char child[1024];
        struct stat st;
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        snprintf(child, sizeof(child), "%s/%s", host, entry->d_name);
        if (lstat(child, &st) != 0) {
            continue;
        }
        (*inodes)++;
        if (S_ISDIR(st.st_mode)) {
            Usage(child, blocks, inodes);
        } else {
            *blocks += (u32)((st.st_size + 0x3FFF) / 0x4000);
        }
    }
    closedir(dir);
}

void NANDInitBanner(NANDBanner* banner, u32 flags, const u16* title, const u16* comment) {
    int i;
    memset(banner, 0, sizeof(*banner));
    banner->signature = 0x5749424E; /* "WIBN" */
    banner->flag = flags;
    banner->iconSpeed = 0;
    for (i = 0; i < 31 && title[i] != 0; i++) {
        banner->comment[0][i] = title[i];
    }
    if (comment != NULL) {
        for (i = 0; i < 31 && comment[i] != 0; i++) {
            banner->comment[1][i] = comment[i];
        }
    }
}

/* ------------------------------------------------------------------------------------ */
/* Asynchronous variants                                                                 */

typedef struct NandCompletion {
    NANDCallback callback;
    NANDCommandBlock* block;
    s32 result;
} NandCompletion;

static void Complete(void* param) {
    NandCompletion* c = (NandCompletion*)param;
    NANDCallback callback = c->callback;
    NANDCommandBlock* block = c->block;
    s32 result = c->result;
    free(c);
    callback(result, block);
}

static s32 Finish(s32 result, NANDCallback cb, NANDCommandBlock* block) {
    NandCompletion* c;
    if (cb == NULL) {
        return result;
    }
    c = (NandCompletion*)malloc(sizeof(*c));
    c->callback = cb;
    c->block = block;
    c->result = result;
    block->callback = cb;
    PortPostCallback(Complete, c);
    return NAND_RESULT_OK;
}

s32 NANDCreateAsync(const char* path, u8 perm, u8 attr, NANDCallback cb, NANDCommandBlock* block) {
    return Finish(NANDCreate(path, perm, attr), cb, block);
}
s32 NANDPrivateCreateAsync(const char* path, u8 perm, u8 attr, NANDCallback cb, NANDCommandBlock* block) {
    return Finish(NANDCreate(path, perm, attr), cb, block);
}
s32 NANDCreateDirAsync(const char* path, u8 perm, u8 attr, NANDCallback cb, NANDCommandBlock* block) {
    return Finish(NANDCreateDir(path, perm, attr), cb, block);
}
s32 NANDPrivateCreateDirAsync(const char* path, u8 perm, u8 attr, NANDCallback cb, NANDCommandBlock* block) {
    return Finish(NANDCreateDir(path, perm, attr), cb, block);
}
s32 NANDDeleteAsync(const char* path, NANDCallback cb, NANDCommandBlock* block) {
    return Finish(NANDDelete(path), cb, block);
}
s32 NANDPrivateDeleteAsync(const char* path, NANDCallback cb, NANDCommandBlock* block) {
    return Finish(NANDDelete(path), cb, block);
}
s32 NANDMoveAsync(const char* path, const char* destDir, NANDCallback cb, NANDCommandBlock* block) {
    return Finish(NANDMove(path, destDir), cb, block);
}
s32 NANDOpenAsync(const char* path, NANDFileInfo* info, u8 accType, NANDCallback cb, NANDCommandBlock* block) {
    block->fileInfo = info;
    return Finish(NANDOpen(path, info, accType), cb, block);
}
s32 NANDPrivateOpenAsync(const char* path, NANDFileInfo* info, u8 accType, NANDCallback cb, NANDCommandBlock* block) {
    return NANDOpenAsync(path, info, accType, cb, block);
}
s32 NANDCloseAsync(NANDFileInfo* info, NANDCallback cb, NANDCommandBlock* block) {
    block->fileInfo = info;
    return Finish(NANDClose(info), cb, block);
}
s32 NANDReadAsync(NANDFileInfo* info, void* buf, u32 length, NANDCallback cb, NANDCommandBlock* block) {
    block->fileInfo = info;
    return Finish(NANDRead(info, buf, length), cb, block);
}
s32 NANDWriteAsync(NANDFileInfo* info, const void* buf, u32 length, NANDCallback cb, NANDCommandBlock* block) {
    block->fileInfo = info;
    return Finish(NANDWrite(info, buf, length), cb, block);
}
s32 NANDSeekAsync(NANDFileInfo* info, s32 offset, s32 whence, NANDCallback cb, NANDCommandBlock* block) {
    block->fileInfo = info;
    return Finish(NANDSeek(info, offset, whence), cb, block);
}
s32 NANDPrivateGetStatusAsync(const char* path, NANDStatus* stat_, NANDCallback cb, NANDCommandBlock* block) {
    return Finish(NANDGetStatus(path, stat_), cb, block);
}

s32 NANDCheckAsync(u32 requiredSpace, u32 requiredInodes, u32* answer, NANDCallback cb, NANDCommandBlock* block) {
    (void)requiredSpace;
    (void)requiredInodes;
    *answer = 0; /* enough room */
    return Finish(NAND_RESULT_OK, cb, block);
}

s32 NANDGetHomeDirUsageAsync(u32* blocks, u32* inodes, NANDCallback cb, NANDCommandBlock* block) {
    char host[1024];
    *blocks = 0;
    *inodes = 0;
    if (HostPath(HOME_DIR, host, sizeof(host)) == NAND_RESULT_OK) {
        Usage(host, blocks, inodes);
    }
    return Finish(NAND_RESULT_OK, cb, block);
}
