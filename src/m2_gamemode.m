// libm2_gamemode.dylib — push the Wine game window into native AppKit fullscreen so gamepolicyd engages macOS Game Mode.
// Universal build: clang -arch x86_64 -arch arm64 -dynamiclib -fobjc-arc -framework AppKit -o build/libm2_gamemode.dylib src/m2_gamemode.m
#import <AppKit/AppKit.h>

static BOOL m2_done = NO;

static BOOL m2_is_game_window(NSWindow *w) {
    if (!w || !w.visible || w.isMiniaturized) return NO;
    if (w.styleMask & NSWindowStyleMaskFullScreen) { m2_done = YES; return NO; }
    NSRect s = w.screen ? w.screen.frame : NSScreen.mainScreen.frame;
    NSRect f = w.frame;
    return ([w.title rangeOfString:@"Majesty"].location != NSNotFound) ||
           (f.size.width >= s.size.width * 0.85 && f.size.height >= s.size.height * 0.85);
}

static void m2_try_fullscreen(NSWindow *w) {
    if (m2_done || !m2_is_game_window(w)) return;
    if (w.styleMask & NSWindowStyleMaskFullScreen) {
        m2_done = YES;
        return;
    }
    NSWindowCollectionBehavior b = w.collectionBehavior;
    b &= ~(NSWindowCollectionBehaviorFullScreenAuxiliary | NSWindowCollectionBehaviorFullScreenNone);
    b |= NSWindowCollectionBehaviorFullScreenPrimary;
    w.collectionBehavior = b;
    // winemac.drv borderless windows lack NSWindowStyleMaskResizable which AppKit requires for toggleFullScreen:
    w.styleMask |= NSWindowStyleMaskResizable;
    m2_done = YES;
    NSLog(@"[M2-GameMode] Transitioning window '%@' into native AppKit FullScreen Space for gamepolicyd", w.title);
    [w toggleFullScreen:nil];
}

__attribute__((constructor)) static void m2_gamemode_init(void) {
    dispatch_async(dispatch_get_main_queue(), ^{
        [NSNotificationCenter.defaultCenter addObserverForName:NSWindowDidBecomeMainNotification
            object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *n) {
                m2_try_fullscreen((NSWindow *)n.object);
            }];
        [NSNotificationCenter.defaultCenter addObserverForName:NSWindowDidBecomeKeyNotification
            object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *n) {
                m2_try_fullscreen((NSWindow *)n.object);
            }];
        __block int tries = 0;
        __block void (^poll)(void);
        __weak __block void (^weakPoll)(void);
        weakPoll = poll = ^{
            if (m2_done || ++tries > 60) return;
            for (NSWindow *w in NSApp.windows) m2_try_fullscreen(w);
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), dispatch_get_main_queue(), weakPoll);
        };
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC), dispatch_get_main_queue(), poll);
    });
}
