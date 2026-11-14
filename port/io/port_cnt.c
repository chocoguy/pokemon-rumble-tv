/* CNT: files inside the title's content archives. Content N is the U8 archive
 * <romDir>/0000000N.app (content IDs equal indices in this title, see title.tmd). The
 * archive's header and table are read into memory from the allocator the game passes,
 * as the SDK did; file data is read from the host file on demand.
 *
 * Contents 2-4 are the Home Button Menu's shared contents, which a WAD does not carry;
 * the menu is stubbed, so their absence is only logged. */
#include <port_os.h>
#include <port_endian.h>
#include <revolution/cnt.h>
#include <revolution/mem/allocator.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void CNTInit(void) {}

s32 CNTShutdown(void) {
    return CNT_RESULT_OK;
}

s32 CNTInitHandle(s32 contentNum, CNTHandle* handle, MEMAllocator* allocator) {
    char path[1024];
    uint8_t header[32];
    uint32_t fstStart;
    uint32_t fstSize;
    void* table;
    int fd;

    snprintf(path, sizeof(path), "%s/%08x.app", gPortConfig.romDir, (unsigned)contentNum);
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        PortLog("CNTInitHandle: content %d is not available (%s)", (int)contentNum, path);
        return CNT_RESULT_NOT_FOUND;
    }
    if (pread(fd, header, sizeof(header), 0) != (ssize_t)sizeof(header) || PortBE32(header) != 0x55AA382Du) {
        PortRed("CNTInitHandle: %s is not a U8 archive", path);
        close(fd);
        return CNT_RESULT_FATAL;
    }
    fstStart = PortBE32(header + 4);
    fstSize = PortBE32(header + 8);
    table = MEMAllocFromAllocator(allocator, fstStart + fstSize);
    if (table == NULL) {
        close(fd);
        return CNT_RESULT_ALLOC_FAILED;
    }
    if (pread(fd, table, fstStart + fstSize, 0) != (ssize_t)(fstStart + fstSize)) {
        PortRed("CNTInitHandle: short read of %s", path);
        MEMFreeToAllocator(allocator, table);
        close(fd);
        return CNT_RESULT_FATAL;
    }
    memset(handle, 0, sizeof(*handle));
    if (!ARCInitHandle(table, &handle->nand.ArcHandle)) {
        MEMFreeToAllocator(allocator, table);
        close(fd);
        return CNT_RESULT_FATAL;
    }
    handle->nand.FileDescriptor = fd;
    handle->nand.allocator = allocator;
    handle->type = CNT_TYPE_NAND;
    return CNT_RESULT_OK;
}

s32 CNTReleaseHandle(CNTHandle* handle) {
    if (handle->type != CNT_TYPE_NAND) {
        return CNT_RESULT_UNKNOWN;
    }
    close(handle->nand.FileDescriptor);
    MEMFreeToAllocator(handle->nand.allocator, handle->nand.ArcHandle.archiveStartAddr);
    handle->nand.FileDescriptor = -1;
    return CNT_RESULT_OK;
}

s32 CNTOpen(CNTHandle* handle, const char* path, CNTFileInfo* fileInfo) {
    ARCFileInfo arc;
    if (handle->type != CNT_TYPE_NAND) {
        return CNT_RESULT_UNKNOWN;
    }
    if (!ARCOpen(&handle->nand.ArcHandle, path, &arc)) {
        return CNT_RESULT_NOT_FOUND;
    }
    fileInfo->nand.CntHandle = &handle->nand;
    fileInfo->nand.startOffset = arc.startOffset;
    fileInfo->nand.length = arc.length;
    fileInfo->nand.readOffset = 0;
    fileInfo->type = CNT_TYPE_NAND;
    return CNT_RESULT_OK;
}

s32 CNTReadAt(CNTFileInfo* fileInfo, void* buffer, s32 length, s32 offset) {
    s32 position;
    ssize_t n;
    if (fileInfo->type != CNT_TYPE_NAND) {
        return CNT_RESULT_UNKNOWN;
    }
    position = fileInfo->nand.readOffset + offset;
    if (position < 0 || (u32)position > fileInfo->nand.length) {
        return CNT_RESULT_NOT_FOUND;
    }
    if ((u32)length > fileInfo->nand.length - (u32)position) {
        length = (s32)(fileInfo->nand.length - (u32)position);
    }
    n = pread(fileInfo->nand.CntHandle->FileDescriptor, buffer, (size_t)length,
              (off_t)fileInfo->nand.startOffset + position);
    return n < 0 ? CNT_RESULT_FATAL : (s32)n;
}

u32 CNTGetLength(CNTFileInfo* fileInfo) {
    return fileInfo->type == CNT_TYPE_NAND ? fileInfo->nand.length : 0;
}

s32 CNTClose(CNTFileInfo* fileInfo) {
    (void)fileInfo;
    return CNT_RESULT_OK;
}
