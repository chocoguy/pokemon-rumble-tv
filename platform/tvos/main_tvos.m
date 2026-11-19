/* The Apple TV app (phase 3 smoke test): one scene, one window, a view backed by a Metal
 * layer. The game starts on its own threads once the view is on screen; a display link
 * then shows the game's current frame once per screen refresh.
 *
 * Controllers (phase 4): controllers_tvos.m. The view controller is a
 * GCEventViewController that keeps controller input from UIKit, so the remote's Back
 * button pauses the game instead of leaving the app (the TV button still leaves).
 *
 * Still to come (phase 7): the log file, env.txt, saves copied to NSUserDefaults,
 * stopping GPU work when the app resigns. */
#import <GameController/GameController.h>
#import <QuartzCore/QuartzCore.h>
#import <UIKit/UIKit.h>
#include <port_app.h>
#include "controllers_tvos.h"
#include "port_gfx_metal.h"

@interface RumbleView : UIView
@end
@implementation RumbleView
+ (Class)layerClass {
    return [CAMetalLayer class];
}
@end

@interface RumbleViewController : GCEventViewController
@end

@implementation RumbleViewController {
    CADisplayLink* _link;
    BOOL _started;
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
            NSLog(@"rumble: UIKit press %@", PressKey(p));
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
    nand = [NSSearchPathForDirectoriesInDomains(NSCachesDirectory, NSUserDomainMask, YES).firstObject
        stringByAppendingPathComponent:@"save"];
    [NSFileManager.defaultManager createDirectoryAtPath:nand withIntermediateDirectories:YES attributes:nil error:nil];
    NSLog(@"rumble: screen %.0fx%.0f points, drawable scale %.2f", pts.width, pts.height, scale);
    if (!PortStartGame(content.fileSystemRepresentation, nand.fileSystemRepresentation, 2)) {
        NSLog(@"rumble: no GPU");
        return;
    }
    UIApplication.sharedApplication.idleTimerDisabled = YES;
    RumbleControllersStart();
    _link = [CADisplayLink displayLinkWithTarget:self selector:@selector(tick:)];
    [_link addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
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
@end

@interface RumbleAppDelegate : UIResponder <UIApplicationDelegate>
@end
@implementation RumbleAppDelegate
@end

int main(int argc, char* argv[]) {
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass([RumbleAppDelegate class]));
    }
}
