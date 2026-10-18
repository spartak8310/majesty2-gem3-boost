// libm2_gamemode.dylib — push the Wine game window into native AppKit fullscreen so gamepolicyd engages macOS Game Mode.
// Universal build: clang -arch x86_64 -arch arm64 -dynamiclib -fobjc-arc -framework AppKit -o build/libm2_gamemode.dylib src/m2_gamemode.m
#import <AppKit/AppKit.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <stdio.h>
#include <unistd.h>

static BOOL m2_done = NO;
static BOOL m2_transitioning = NO;

static void m2_tag_fullscreen_primary(NSWindow *w) {
    if (!w) return;
    NSWindowCollectionBehavior b = w.collectionBehavior;
    b &= ~(NSWindowCollectionBehaviorFullScreenAuxiliary | NSWindowCollectionBehaviorFullScreenNone);
    b |= NSWindowCollectionBehaviorFullScreenPrimary;
    if (w.collectionBehavior != b) {
        w.collectionBehavior = b;
    }
}

static BOOL m2_is_wine_fullscreen(NSWindow *w) {
    SEL sel = NSSelectorFromString(@"isFullscreen");
    if ([w respondsToSelector:sel]) {
        return ((BOOL (*)(id, SEL))objc_msgSend)(w, sel);
    }
    return NO;
}

static BOOL m2_is_game_window(NSWindow *w) {
    if (!w || !w.visible || w.isMiniaturized) return NO;
    NSRect s = w.screen ? w.screen.frame : NSScreen.mainScreen.frame;
    NSRect f = w.frame;
    return ([w.title rangeOfString:@"Majesty"].location != NSNotFound) ||
           (f.size.width >= s.size.width * 0.85 && f.size.height >= s.size.height * 0.85);
}

static void (*orig_adjustFullScreenBehavior)(id, SEL, NSUInteger) = NULL;
static void m2_swizzled_adjustFullScreenBehavior(NSWindow *self, SEL _cmd, NSUInteger behavior) {
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
        NSLog(@"[M2-GameMode] Swizzled -[WineWindow adjustFullScreenBehavior:]");
    }
    Method mFeatures = class_getInstanceMethod(cls, NSSelectorFromString(@"setWindowFeatures:"));
    if (mFeatures) {
        orig_setWindowFeatures = (void (*)(id, SEL, void *))method_getImplementation(mFeatures);
        method_setImplementation(mFeatures, (IMP)m2_swizzled_setWindowFeatures);
        NSLog(@"[M2-GameMode] Swizzled -[WineWindow setWindowFeatures:]");
    }
}

static void m2_try_fullscreen(NSWindow *w) {
    if (m2_done || m2_transitioning || !m2_is_game_window(w)) return;
    m2_swizzle_winewindow_if_needed();
    m2_tag_fullscreen_primary(w);
    if (w.styleMask & NSWindowStyleMaskFullScreen) {
        m2_done = YES;
        NSLog(@"[M2-GameMode] Window '%@' already in native AppKit FullScreen Space; FullScreenPrimary confirmed", w.title);
        return;
    }
    // In AppKit, borderless windows (styleMask=0x0) cannot enter native FullScreen space
    // unless NSWindowStyleMaskResizable is present on styleMask.
    if (!(w.styleMask & NSWindowStyleMaskResizable)) {
        w.styleMask |= NSWindowStyleMaskResizable;
    }
    m2_transitioning = YES;
    NSLog(@"[M2-GameMode] Transitioning window '%@' (styleMask=0x%lx) into native AppKit FullScreen Space for gamepolicyd",
          w.title, (unsigned long)w.styleMask);
    dispatch_async(dispatch_get_main_queue(), ^{
        [w toggleFullScreen:nil];
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
                m2_done = YES;
                NSLog(@"[M2-GameMode] Confirmed NSWindowDidEnterFullScreenNotification for '%@'", w.title);
            }];
        [NSNotificationCenter.defaultCenter addObserverForName:@"NSWindowDidFailToEnterFullScreenNotification"
            object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *n) {
                m2_transitioning = NO;
                NSLog(@"[M2-GameMode] Window '%@' failed to enter FullScreen Space; will not corrupt styleMask",
                      ((NSWindow *)n.object).title);
            }];
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
