/* The Apple TV app's wrappers around the game (phase 7): log file, env.txt, status log
 * (env_tvos.m) and the save copy kept in the app's preferences (saves_tvos.m). */
#ifndef SHELL_TVOS_H
#define SHELL_TVOS_H

#import <Foundation/Foundation.h>

/* Library/Caches: tvOS gives apps no Documents folder; the system may purge Caches while
 * the app is not running, which is why saves are also copied to the preferences. */
NSString* RumbleCachesDir(void);

/* Before UIApplicationMain: sends stderr (the port's log) to Library/Caches/log.txt,
 * keeping the previous run's as log.prev.txt (not when a debugger is attached, so Xcode's
 * console keeps it, nor with RUMBLE_LOG_FILE=0), then applies env.txt: KEY=VALUE lines
 * from Library/Caches/env.txt, then from env.txt in the app bundle; a variable already
 * set (by the launch environment, or the first file) wins. # starts a comment. */
void RumbleBootEnv(void);

/* A line in the log: "[tvos] ...". */
void RumbleLog(NSString* format, ...) NS_FORMAT_FUNCTION(1, 2);

/* Logs the thermal state when it changes, and every 60 s a status line: thermal state,
 * memory footprint, frames the game showed in that minute. Main thread. */
void RumbleStatusStart(void);
/* Pauses or resumes the status line's frame count (while the game is suspended). */
void RumbleStatusSetActive(BOOL active);

/* Saves (saves_tvos.m). Before the game starts: if the save folder in `nandDir` has none
 * of the game's save files (a new install, or tvOS purged Library/Caches), writes back
 * the copy kept in the preferences. Then watches the game's saves and refreshes that
 * copy a second after each one. */
void RumbleSavesStart(NSString* nandDir);
/* Writes a pending copy now (the app is going inactive). Main thread. */
void RumbleSavesFlush(void);

#endif
