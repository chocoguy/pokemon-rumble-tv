/* The Apple TV app: one scene, one window, a view backed by a Metal layer. The game
 * starts on its own threads once the view is on screen; a display link then shows the
 * game's current frame once per screen refresh.
 *
 * Controllers: controllers_tvos.m. The view controller is a GCEventViewController that
 * keeps controller input from UIKit, so the remote's Back (Menu) button is the Classic
 * Controller's + and opens the game's pause menu instead of leaving the app; the TV
 * button still leaves.
 *
 * Sound: platform/apple/port_audio_apple.m, started before the game.
 *
 * Wrappers (shell_tvos.h): the log in Library/Caches/log.txt and env.txt (env_tvos.m,
 * before UIApplicationMain), saves in Library/Caches/save with a copy in the preferences
 * (saves_tvos.m), a status line every minute. While the scene is inactive (the TV
 * button, the Control Center, a screen saver) the game is frozen at a safe point, the
 * display link and sound are paused, so no GPU work is submitted from the background,
 * and a pending save copy is written. The idle timer is off while the app runs. */
#import <GameController/GameController.h>
#import <QuartzCore/QuartzCore.h>
#import <UIKit/UIKit.h>
#include <port_app.h>
#include <port_audio.h>
#include "controllers_tvos.h"
#include "port_gfx_metal.h"
#include "shell_tvos.h"

@interface RumbleView : UIView
@end
@implementation RumbleView
+ (Class)layerClass {
    return [CAMetalLayer class];
}
@end

@interface RumbleViewController : GCEventViewController
- (void)suspendGame;
- (void)resumeGame;
@end

@implementation RumbleViewController {
    CADisplayLink* _link;
    BOOL _started;
    BOOL _suspended;
    /* Presses that arrive as UIKit events (the Simulator's keyboard, a remote UIKit
     * still sees), by press type or key; added to channel 0. */
    NSMutableSet<NSNumber*>* _held;
    /* Presses still shown for this many more refreshes: a tap shorter than a frame
     * (the Simulator's) would otherwise fall between the game's samples. */
    NSMutableDictionary<NSNumber*, NSNumber*>* _taps;
    int _pressesLogged;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.controllerUserInteractionEnabled = NO;
    _held = [NSMutableSet set];
    _taps = [NSMutableDictionary dictionary];
}

/* A UIKit press as a key for _held: the press type, or 1000 + a key's HID usage. */
static NSNumber* PressKey(UIPress* p) {
    if (p.key != nil) {
        return @(1000 + p.key.keyCode);
    }
    return @(p.type);
}

- (void)pressesBegan:(NSSet<UIPress*>*)presses withEvent:(UIPressesEvent*)event {
    (void)event;
    for (UIPress* p in presses) {
        [_held addObject:PressKey(p)];
        _taps[PressKey(p)] = @4;
        if (_pressesLogged < 20) {
            _pressesLogged++;
            RumbleLog(@"UIKit press %@", PressKey(p));
        }
    }
}

- (void)pressesEnded:(NSSet<UIPress*>*)presses withEvent:(UIPressesEvent*)event {
    (void)event;
    for (UIPress* p in presses) {
        [_held removeObject:PressKey(p)];
    }
}

- (void)pressesCancelled:(NSSet<UIPress*>*)presses withEvent:(UIPressesEvent*)event {
    [self pressesEnded:presses withEvent:event];
}

- (BOOL)held:(NSInteger)type key:(UIKeyboardHIDUsage)key {
    return [_held containsObject:@(type)] || [_held containsObject:@(1000 + key)] || _taps[@(type)] != nil ||
           _taps[@(1000 + key)] != nil;
}

/* One refresh has passed: taps count down. */
- (void)ageTaps {
    for (NSNumber* k in _taps.allKeys) {
        int left = _taps[k].intValue - 1;
        if (left <= 0) {
            [_taps removeObjectForKey:k];
        } else {
            _taps[k] = @(left);
        }
    }
}

- (PortClassicState)uikitState {
    PortClassicState s = {0};
    if ([self held:UIPressTypeLeftArrow key:UIKeyboardHIDUsageKeyboardLeftArrow]) s.lx = -1;
    if ([self held:UIPressTypeRightArrow key:UIKeyboardHIDUsageKeyboardRightArrow]) s.lx = 1;
    if ([self held:UIPressTypeUpArrow key:UIKeyboardHIDUsageKeyboardUpArrow]) s.ly = 1;
    if ([self held:UIPressTypeDownArrow key:UIKeyboardHIDUsageKeyboardDownArrow]) s.ly = -1;
    if ([self held:UIPressTypeSelect key:UIKeyboardHIDUsageKeyboardReturnOrEnter]) s.buttons |= PORT_CL_A;
    if ([self held:-1 key:UIKeyboardHIDUsageKeyboardZ]) s.buttons |= PORT_CL_A;
    if ([self held:UIPressTypePlayPause key:UIKeyboardHIDUsageKeyboardX]) s.buttons |= PORT_CL_B;
    if ([self held:UIPressTypeMenu key:UIKeyboardHIDUsageKeyboardEscape]) s.buttons |= PORT_CL_PLUS;
    return s;
}

- (void)loadView {
    self.view = [[RumbleView alloc] initWithFrame:CGRectMake(0, 0, 1920, 1080)];
    self.view.backgroundColor = UIColor.blackColor;
}

- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    [self start];
}

- (void)start {
    NSString* content;
    NSString* nand;
    UIScreen* screen;
    CGFloat scale;
    CGSize pts;
    CAMetalLayer* layer;
    if (_started) {
        return;
    }
    _started = YES;

    /* The drawable at most 1080 lines; the game draws 480. */
    screen = self.view.window.windowScene.screen;
    pts = self.view.bounds.size;
    scale = screen ? screen.nativeScale : 1.0;
    if (pts.height * scale > 1080) {
        scale = 1080.0 / pts.height;
    }
    layer = (CAMetalLayer*)self.view.layer;
    layer.contentsScale = scale;

    content = [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"content"];
    nand = [RumbleCachesDir() stringByAppendingPathComponent:@"save"];
    [NSFileManager.defaultManager createDirectoryAtPath:nand withIntermediateDirectories:YES attributes:nil error:nil];
    RumbleLog(@"screen %.0fx%.0f points, drawable scale %.2f (%.0fx%.0f pixels)", pts.width, pts.height, scale,
              pts.width * scale, pts.height * scale);
    RumbleSavesStart(nand);
    if (!PortAudioDeviceStart()) {
        RumbleLog(@"no sound output; running silent");
    }
    if (!PortStartGame(content.fileSystemRepresentation, nand.fileSystemRepresentation, 2)) {
        RumbleLog(@"[RED] no GPU");
        return;
    }
    UIApplication.sharedApplication.idleTimerDisabled = YES;
    RumbleControllersStart();
    RumbleStatusStart();
    _link = [CADisplayLink displayLinkWithTarget:self selector:@selector(tick:)];
    [_link addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
}

/* The scene is going inactive: stop all game and GPU work until it is active again. */
- (void)suspendGame {
    if (!_started || _suspended) {
        return;
    }
    _suspended = YES;
    _link.paused = YES;
    PortAudioDeviceSetPaused(1);
    PortSuspendGame(1000);
    RumbleStatusSetActive(NO);
    RumbleSavesFlush();
}

- (void)resumeGame {
    if (!_suspended) {
        return;
    }
    _suspended = NO;
    [_held removeAllObjects];
    [_taps removeAllObjects];
    PortResumeGame();
    PortAudioDeviceSetPaused(0);
    RumbleStatusSetActive(YES);
    _link.paused = NO;
}

- (void)tick:(CADisplayLink*)link {
    PortClassicState uikit = [self uikitState];
    (void)link;
    RumbleControllersPoll(&uikit);
    [self ageTaps];
    PortGfxPresentToLayer((CAMetalLayer*)self.view.layer);
}
@end

@interface RumbleSceneDelegate : UIResponder <UIWindowSceneDelegate>
@property(nonatomic, strong) UIWindow* window;
@end

@implementation RumbleSceneDelegate
- (void)scene:(UIScene*)scene willConnectToSession:(UISceneSession*)session options:(UISceneConnectionOptions*)options {
    (void)session;
    (void)options;
    if (![scene isKindOfClass:[UIWindowScene class]]) {
        return;
    }
    self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene*)scene];
    self.window.rootViewController = [RumbleViewController new];
    [self.window makeKeyAndVisible];
}

- (void)sceneWillResignActive:(UIScene*)scene {
    (void)scene;
    RumbleLog(@"scene inactive");
    [(RumbleViewController*)self.window.rootViewController suspendGame];
}

- (void)sceneDidBecomeActive:(UIScene*)scene {
    (void)scene;
    RumbleLog(@"scene active");
    [(RumbleViewController*)self.window.rootViewController resumeGame];
}
@end

@interface RumbleAppDelegate : UIResponder <UIApplicationDelegate>
@end
@implementation RumbleAppDelegate
@end

int main(int argc, char* argv[]) {
    @autoreleasepool {
        RumbleBootEnv();
    }
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass([RumbleAppDelegate class]));
    }
}
