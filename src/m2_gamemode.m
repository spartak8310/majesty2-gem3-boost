// libm2_gamemode.dylib — push the Wine D3D9 game window (>=800x600) into native AppKit fullscreen
// so gamepolicyd engages macOS Game Mode, while leaving the small Win32 M2_LauncherWindow (~500x450)
// completely untouched so DLC selection works natively.
// Universal build: clang -arch x86_64 -arch arm64 -dynamiclib -fobjc-arc -framework AppKit -o build/libm2_gamemode.dylib src/m2_gamemode.m
#import <AppKit/AppKit.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <stdio.h>
#include <unistd.h>

static BOOL m2_done = NO;
static BOOL m2_transitioning = NO;
static NSTimeInterval m2_first_visible_ts = 0.0;

static BOOL m2_is_large_window(NSWindow *w) {
    if (!w) return NO;
    NSRect f = w.frame;
    return (f.size.width >= 800.0 && f.size.height >= 600.0);
}

static BOOL m2_has_visible_launcher_window(void) {
    for (NSWindow *w in NSApp.windows) {
        if (!w || !w.visible || w.isMiniaturized) continue;
        NSRect f = w.frame;
        if (f.size.width >= 150.0 && f.size.width < 800.0 &&
            f.size.height >= 150.0 && f.size.height < 700.0) {
            return YES;
        }
    }
    return NO;
}

static void m2_tag_fullscreen_primary(NSWindow *w) {
    if (!m2_is_large_window(w)) return;
    NSWindowCollectionBehavior b = w.collectionBehavior;
    b &= ~(NSWindowCollectionBehaviorFullScreenAuxiliary | NSWindowCollectionBehaviorFullScreenNone);
    b |= NSWindowCollectionBehaviorFullScreenPrimary;
    if (w.collectionBehavior != b) {
        w.collectionBehavior = b;
    }
}

static BOOL m2_is_game_window(NSWindow *w) {
    if (!w || !w.visible || w.isMiniaturized) return NO;
    if (!m2_is_large_window(w)) return NO;
    if (m2_has_visible_launcher_window()) return NO;
    NSRect s = w.screen ? w.screen.frame : NSScreen.mainScreen.frame;
    NSRect f = w.frame;
    return ([w.title rangeOfString:@"Majesty"].location != NSNotFound) ||
           (f.size.width >= s.size.width * 0.80 && f.size.height >= s.size.height * 0.80);
}

static void (*orig_adjustFullScreenBehavior)(id, SEL, NSUInteger) = NULL;
static void m2_swizzled_adjustFullScreenBehavior(NSWindow *self, SEL _cmd, NSUInteger behavior) {
    if (!m2_is_large_window(self)) {
        if (orig_adjustFullScreenBehavior) {
            orig_adjustFullScreenBehavior(self, _cmd, behavior);
        }
        return;
    }
    behavior &= ~(NSWindowCollectionBehaviorFullScreenAuxiliary | NSWindowCollectionBehaviorFullScreenNone);
    behavior |= NSWindowCollectionBehaviorFullScreenPrimary;
    if (orig_adjustFullScreenBehavior) {
        orig_adjustFullScreenBehavior(self, _cmd, behavior);
    } else {
        self.collectionBehavior = behavior;
    }
    m2_tag_fullscreen_primary(self);
    if (!(self.styleMask & NSWindowStyleMaskResizable)) {
        self.styleMask |= NSWindowStyleMaskResizable;
    }
}

static void (*orig_setWindowFeatures)(id, SEL, void *) = NULL;
static void m2_swizzled_setWindowFeatures(NSWindow *self, SEL _cmd, void *features) {
    if (orig_setWindowFeatures) {
        orig_setWindowFeatures(self, _cmd, features);
    }
    if (!m2_is_large_window(self)) return;
    m2_tag_fullscreen_primary(self);
    if (!(self.styleMask & NSWindowStyleMaskResizable)) {
        self.styleMask |= NSWindowStyleMaskResizable;
    }
}

static void m2_swizzle_winewindow_if_needed(void) {
    static BOOL swizzled = NO;
    if (swizzled) return;
    Class cls = NSClassFromString(@"WineWindow");
    if (!cls) return;
    swizzled = YES;
    Method mAdjust = class_getInstanceMethod(cls, NSSelectorFromString(@"adjustFullScreenBehavior:"));
    if (mAdjust) {
        orig_adjustFullScreenBehavior = (void (*)(id, SEL, NSUInteger))method_getImplementation(mAdjust);
        method_setImplementation(mAdjust, (IMP)m2_swizzled_adjustFullScreenBehavior);
        NSLog(@"[M2-GameMode] Swizzled -[WineWindow adjustFullScreenBehavior:] (large-window gated)");
    }
    Method mFeatures = class_getInstanceMethod(cls, NSSelectorFromString(@"setWindowFeatures:"));
    if (mFeatures) {
        orig_setWindowFeatures = (void (*)(id, SEL, void *))method_getImplementation(mFeatures);
        method_setImplementation(mFeatures, (IMP)m2_swizzled_setWindowFeatures);
        NSLog(@"[M2-GameMode] Swizzled -[WineWindow setWindowFeatures:] (large-window gated)");
    }
}

static void m2_poll_windows(void) {
    m2_swizzle_winewindow_if_needed();
    NSWindow *candidate = nil;
    for (NSWindow *w in NSApp.windows) {
        if (m2_is_game_window(w)) {
            candidate = w;
            break;
        }
    }
    if (!candidate) {
        m2_first_visible_ts = 0.0;
        if (m2_done) {
            m2_done = NO;
        }
        return;
    }
    m2_tag_fullscreen_primary(candidate);
    if (candidate.styleMask & NSWindowStyleMaskFullScreen) {
        if (!m2_done) {
            m2_done = YES;
            NSLog(@"[M2-GameMode] Window '%@' (%.0fx%.0f) confirmed in native AppKit FullScreen Space",
                  candidate.title, candidate.frame.size.width, candidate.frame.size.height);
        }
        return;
    }
    if (m2_done || m2_transitioning) return;

    NSTimeInterval now = [NSDate timeIntervalSinceReferenceDate];
    if (m2_first_visible_ts == 0.0) {
        m2_first_visible_ts = now;
        return;
    }
    // Require the >=800x600 game window to remain continuously visible for >=1.2s
    // so the 0.25s pre-launcher splash window never triggers FullScreen before M2_LauncherWindow opens.
    if ((now - m2_first_visible_ts) < 1.2) {
        return;
    }

    if (!(candidate.styleMask & NSWindowStyleMaskResizable)) {
        candidate.styleMask |= NSWindowStyleMaskResizable;
    }
    m2_transitioning = YES;
    NSLog(@"[M2-GameMode] Transitioning D3D9 window '%@' (%.0fx%.0f, styleMask=0x%lx) into native AppKit FullScreen Space",
          candidate.title, candidate.frame.size.width, candidate.frame.size.height, (unsigned long)candidate.styleMask);
    dispatch_async(dispatch_get_main_queue(), ^{
        [candidate toggleFullScreen:nil];
    });
}

__attribute__((constructor)) static void m2_gamemode_init(void) {
    const char *bid = NSBundle.mainBundle.bundleIdentifier ? NSBundle.mainBundle.bundleIdentifier.UTF8String : "none";
    fprintf(stderr, "[M2-GameMode] Constructor loaded (pid=%d, bundle=%s)\n", (int)getpid(), bid);
    fflush(stderr);
    dispatch_async(dispatch_get_main_queue(), ^{
        [NSNotificationCenter.defaultCenter addObserverForName:NSWindowDidEnterFullScreenNotification
            object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *n) {
                NSWindow *w = (NSWindow *)n.object;
                m2_tag_fullscreen_primary(w);
                m2_transitioning = NO;
                if (m2_is_large_window(w)) {
                    m2_done = YES;
                    NSLog(@"[M2-GameMode] Confirmed NSWindowDidEnterFullScreenNotification for '%@' (%.0fx%.0f)",
                          w.title, w.frame.size.width, w.frame.size.height);
                }
            }];
        [NSNotificationCenter.defaultCenter addObserverForName:@"NSWindowDidFailToEnterFullScreenNotification"
            object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *n) {
                m2_transitioning = NO;
                m2_first_visible_ts = 0.0;
                NSLog(@"[M2-GameMode] Window '%@' failed to enter FullScreen Space; will retry",
                      ((NSWindow *)n.object).title);
            }];
        [NSNotificationCenter.defaultCenter addObserverForName:NSWindowDidExitFullScreenNotification
            object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *n) {
                m2_transitioning = NO;
                m2_done = NO;
                m2_first_visible_ts = 0.0;
            }];
        __block void (^poll)(void);
        __weak __block void (^weakPoll)(void);
        weakPoll = poll = ^{
            m2_poll_windows();
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.5 * NSEC_PER_SEC)), dispatch_get_main_queue(), weakPoll);
        };
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(1.0 * NSEC_PER_SEC)), dispatch_get_main_queue(), poll);
    });
}
