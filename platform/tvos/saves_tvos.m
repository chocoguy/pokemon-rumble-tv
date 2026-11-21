/* Saves on Apple TV (see shell_tvos.h). The game's NAND folder is Library/Caches/save,
 * which tvOS may purge while the app is not running; the preferences (NSUserDefaults)
 * are kept. So after each save the game commits, its three files are copied there,
 * LZFSE-packed, and they are written back at launch when the folder has none of them.
 * A file the game deleted is removed from the copy too, so a deleted save stays deleted.
 * The copy is about 0.5 MB unpacked, well under the ~1 MB tvOS allows the preferences.
 * Deleting the app deletes the preferences as well. */
#import <Foundation/Foundation.h>
#include <compression.h>
#include <port_app.h>
#include <string.h>
#include "shell_tvos.h"

/* Relative to PORT_SAVE_DIR. */
static NSString* const kFiles[] = {@"savedata.bin", @"backup/savedata.bin", @"banner.bin"};
enum { kFileCount = sizeof(kFiles) / sizeof(kFiles[0]) };
static NSString* const kKeyPrefix = @"rumble.save:";
/* "RSZ1", the unpacked size (u32, little-endian), then the LZFSE stream. */
static const char kMagic[4] = {'R', 'S', 'Z', '1'};
enum { kHeader = 8 };

static NSString* sSaveDir;
static dispatch_queue_t sQueue;
static volatile int sPending; /* a copy is scheduled and not yet written */

static NSData* Pack(NSData* raw) {
    size_t room = raw.length + raw.length / 8 + 4096;
    NSMutableData* out = [NSMutableData dataWithLength:kHeader + room];
    uint32_t size = (uint32_t)raw.length;
    size_t n;
    memcpy(out.mutableBytes, kMagic, 4);
    memcpy((uint8_t*)out.mutableBytes + 4, &size, 4);
    n = compression_encode_buffer((uint8_t*)out.mutableBytes + kHeader, room, raw.bytes, raw.length, NULL,
                                  COMPRESSION_LZFSE);
    if (n == 0) {
        return nil;
    }
    out.length = kHeader + n;
    return out;
}

static NSData* Unpack(NSData* packed) {
    uint32_t size;
    NSMutableData* raw;
    if (packed.length < kHeader || memcmp(packed.bytes, kMagic, 4) != 0) {
        return nil;
    }
    memcpy(&size, (const uint8_t*)packed.bytes + 4, 4);
    raw = [NSMutableData dataWithLength:size];
    if (compression_decode_buffer(raw.mutableBytes, size, (const uint8_t*)packed.bytes + kHeader,
                                  packed.length - kHeader, NULL, COMPRESSION_LZFSE) != size) {
        return nil;
    }
    return raw;
}

/* Copies the save folder's current files to the preferences (on sQueue). */
static void Store(void) {
    NSUserDefaults* defaults = NSUserDefaults.standardUserDefaults;
    NSMutableArray<NSString*>* summary = [NSMutableArray array];
    int i;
    sPending = 0;
    for (i = 0; i < kFileCount; i++) {
        NSString* key = [kKeyPrefix stringByAppendingString:kFiles[i]];
        NSData* raw = [NSData dataWithContentsOfFile:[sSaveDir stringByAppendingPathComponent:kFiles[i]]];
        NSData* packed;
        if (raw == nil) {
            [defaults removeObjectForKey:key];
            [summary addObject:[kFiles[i] stringByAppendingString:@" (none)"]];
            continue;
        }
        packed = Pack(raw);
        if (packed == nil) {
            RumbleLog(@"[RED] saves: %@ (%lu bytes) did not pack; the previous copy stays", kFiles[i],
                      (unsigned long)raw.length);
            continue;
        }
        [defaults setObject:packed forKey:key];
        [summary addObject:[NSString stringWithFormat:@"%@ %lu->%lu", kFiles[i], (unsigned long)raw.length,
                                                      (unsigned long)packed.length]];
    }
    if (![defaults synchronize]) {
        RumbleLog(@"[RED] saves: the preferences did not take the copy");
        return;
    }
    RumbleLog(@"saves: copied to the preferences: %@", [summary componentsJoinedByString:@", "]);
}

/* PortSetSaveChangedCallback, on a game thread: the game commits a save in a few moves
 * frames apart, so the copy waits a second after the first. */
static void SaveChanged(void) {
    if (__atomic_exchange_n(&sPending, 1, __ATOMIC_SEQ_CST)) {
        return;
    }
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), sQueue, ^{
      if (sPending) {
          Store();
      }
    });
}

static void Restore(void) {
    NSFileManager* fm = NSFileManager.defaultManager;
    NSUserDefaults* defaults = NSUserDefaults.standardUserDefaults;
    int restored = 0;
    int i;
    for (i = 0; i < kFileCount; i++) {
        if ([fm fileExistsAtPath:[sSaveDir stringByAppendingPathComponent:kFiles[i]]]) {
            RumbleLog(@"saves: %@ is in the save folder", kFiles[i]);
            return;
        }
    }
    for (i = 0; i < kFileCount; i++) {
        NSData* packed = [defaults dataForKey:[kKeyPrefix stringByAppendingString:kFiles[i]]];
        NSString* path = [sSaveDir stringByAppendingPathComponent:kFiles[i]];
        NSData* raw;
        if (packed == nil) {
            continue;
        }
        raw = Unpack(packed);
        if (raw == nil) {
            RumbleLog(@"[RED] saves: the kept copy of %@ is damaged; not restored", kFiles[i]);
            continue;
        }
        [fm createDirectoryAtPath:path.stringByDeletingLastPathComponent
            withIntermediateDirectories:YES
                             attributes:nil
                                  error:nil];
        if (![raw writeToFile:path atomically:YES]) {
            RumbleLog(@"[RED] saves: writing %@ failed", path);
            continue;
        }
        restored++;
    }
    if (restored) {
        RumbleLog(@"saves: the save folder was empty; restored %d files from the preferences", restored);
    } else {
        RumbleLog(@"saves: no save yet");
    }
}

void RumbleSavesStart(NSString* nandDir) {
    sSaveDir = [nandDir stringByAppendingString:@PORT_SAVE_DIR];
    sQueue = dispatch_queue_create("rumble.saves", DISPATCH_QUEUE_SERIAL);
    [NSFileManager.defaultManager createDirectoryAtPath:sSaveDir
                            withIntermediateDirectories:YES
                                             attributes:nil
                                                  error:nil];
    Restore();
    PortSetSaveChangedCallback(SaveChanged);
}

void RumbleSavesFlush(void) {
    if (sQueue == nil) {
        return;
    }
    dispatch_sync(sQueue, ^{
      if (sPending) {
          Store();
      }
    });
}
