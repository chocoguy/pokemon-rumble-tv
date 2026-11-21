/* Log file, env.txt and the status log for the Apple TV app (see shell_tvos.h). */
#import <Foundation/Foundation.h>
#include <mach/mach.h>
#include <port_app.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include "shell_tvos.h"

NSString* RumbleCachesDir(void) {
    return NSSearchPathForDirectoriesInDomains(NSCachesDirectory, NSUserDomainMask, YES).firstObject;
}

void RumbleLog(NSString* format, ...) {
    va_list args;
    NSString* line;
    va_start(args, format);
    line = [[NSString alloc] initWithFormat:format arguments:args];
    va_end(args);
    fprintf(stderr, "[tvos] %s\n", line.UTF8String);
}

static BOOL DebuggerAttached(void) {
    struct kinfo_proc info;
    size_t size = sizeof(info);
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()};
    memset(&info, 0, sizeof(info));
    if (sysctl(mib, 4, &info, &size, NULL, 0) != 0) {
        return NO;
    }
    return (info.kp_proc.p_flag & P_TRACED) != 0;
}

static void ApplyEnvFile(NSString* path) {
    NSString* text = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil];
    for (NSString* raw in [text componentsSeparatedByCharactersInSet:NSCharacterSet.newlineCharacterSet]) {
        NSString* line = [raw stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
        NSRange eq = [line rangeOfString:@"="];
        NSString* key;
        NSString* value;
        if (line.length == 0 || [line hasPrefix:@"#"] || eq.location == NSNotFound || eq.location == 0) {
            continue;
        }
        key = [[line substringToIndex:eq.location] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
        value = [[line substringFromIndex:eq.location + 1] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
        if (getenv(key.UTF8String) != NULL) {
            continue;
        }
        setenv(key.UTF8String, value.UTF8String, 0);
        RumbleLog(@"env %@=%@ (%@)", key, value, path.lastPathComponent);
    }
}

void RumbleBootEnv(void) {
    NSFileManager* fm = NSFileManager.defaultManager;
    NSString* caches = RumbleCachesDir();
    NSString* log = [caches stringByAppendingPathComponent:@"log.txt"];
    NSString* prev = [caches stringByAppendingPathComponent:@"log.prev.txt"];
    const char* logSetting = getenv("RUMBLE_LOG_FILE");
    NSDictionary* info = NSBundle.mainBundle.infoDictionary;

    [fm createDirectoryAtPath:caches withIntermediateDirectories:YES attributes:nil error:nil];
    if (DebuggerAttached() || (logSetting != NULL && strcmp(logSetting, "0") == 0)) {
        RumbleLog(@"log: stderr (debugger attached or RUMBLE_LOG_FILE=0)");
    } else {
        [fm removeItemAtPath:prev error:nil];
        [fm moveItemAtPath:log toPath:prev error:nil];
        if (freopen(log.fileSystemRepresentation, "w", stderr) != NULL) {
            setvbuf(stderr, NULL, _IOLBF, 0);
        }
    }
    RumbleLog(@"Rumble %@ (%@), tvOS %@", info[@"CFBundleShortVersionString"], info[@"CFBundleVersion"],
              NSProcessInfo.processInfo.operatingSystemVersionString);
    ApplyEnvFile([caches stringByAppendingPathComponent:@"env.txt"]);
    ApplyEnvFile([NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"env.txt"]);
}

/* ------------------------------------------------------------------------------------ */
/* Status                                                                                */

static NSTimer* sStatusTimer;
static unsigned int sFramesAtLastStatus;
static BOOL sActive = YES;

static const char* ThermalName(NSProcessInfoThermalState state) {
    switch (state) {
    case NSProcessInfoThermalStateNominal:
        return "nominal";
    case NSProcessInfoThermalStateFair:
        return "fair";
    case NSProcessInfoThermalStateSerious:
        return "serious";
    case NSProcessInfoThermalStateCritical:
        return "critical";
    }
    return "?";
}

static double FootprintMB(void) {
    task_vm_info_data_t vm;
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vm, &count) != KERN_SUCCESS) {
        return -1.0;
    }
    return (double)vm.phys_footprint / (1024.0 * 1024.0);
}

static void LogStatus(void) {
    unsigned int frames = PortFramesPresented();
    RumbleLog(@"status: thermal %s, memory %.0f MB, %u frames in the last minute%s",
              ThermalName(NSProcessInfo.processInfo.thermalState), FootprintMB(), frames - sFramesAtLastStatus,
              sActive ? "" : " (suspended)");
    sFramesAtLastStatus = frames;
}

void RumbleStatusStart(void) {
    RumbleLog(@"thermal state %s at start", ThermalName(NSProcessInfo.processInfo.thermalState));
    [NSNotificationCenter.defaultCenter addObserverForName:NSProcessInfoThermalStateDidChangeNotification
                                                    object:nil
                                                     queue:NSOperationQueue.mainQueue
                                                usingBlock:^(NSNotification* note) {
                                                  (void)note;
                                                  RumbleLog(@"thermal state now %s",
                                                            ThermalName(NSProcessInfo.processInfo.thermalState));
                                                }];
    sFramesAtLastStatus = PortFramesPresented();
    sStatusTimer = [NSTimer scheduledTimerWithTimeInterval:60.0
                                                   repeats:YES
                                                     block:^(NSTimer* timer) {
                                                       (void)timer;
                                                       LogStatus();
                                                     }];
}

void RumbleStatusSetActive(BOOL active) {
    sActive = active;
}
