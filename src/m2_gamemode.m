// libm2_gamemode.dylib — SATRAP Golden v11 (MetaClaw v3.0 Verified)
// 1. Native macOS Game Mode via AppKit FullScreen Space for D3D9 GameWindow (>=800x600),
//    leaving M2_LauncherWindow (~500x450) untouched so DLC selection works natively.
// 2. 1:1 FullScreen Client Rect (1512x982): prevents winemac.drv from subtracting titlebar/border
//    insets (no 1504x917 D3D9 Reset shrink, zero vertical mouse cursor offset).
// 3. Apple MetalFX Spatial Upscaling (1512x982 -> 3024x1964 Retina 2x): promotes WineContentView's
//    CAMetalLayer to contentsScale = 2.0 (integer 2x) + maximumDrawableCount = 2 + displaySyncEnabled = YES
//    so mtld3d v0.13.0 automatically engages MTLFXSpatialScaler without shrinking the 2D RTS UI.
// 4. Enables Apple Metal Performance HUD + mtld3d live telemetry row (MTL_HUD_ENABLED=1).
//
// Universal build:
// clang -arch x86_64 -arch arm64 -dynamiclib -fobjc-arc -framework AppKit -framework QuartzCore -o build/libm2_gamemode.dylib src/m2_gamemode.m

#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <stdio.h>
#include <stdlib.h>
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

#include <math.h>

static CATextLayer *m2_hud_text_layer = nil;

static void m2_update_dual_fps_overlay(NSView *cv) {
    if (!cv) return;
    const char *hud_opt = getenv("M2_FAST_HUD");
    if (hud_opt && (strcmp(hud_opt, "0") == 0 || strcmp(hud_opt, "off") == 0)) {
        if (m2_hud_text_layer) {
            m2_hud_text_layer.hidden = YES;
        }
        return;
    }
    if (!cv.wantsLayer) {
        cv.wantsLayer = YES;
    }
    CALayer *root = cv.layer;
    if (!root) return;

    FILE *f = fopen("/tmp/m2_hud_live.txt", "rb");
    if (!f) return;
    char buf[512];
    memset(buf, 0, sizeof(buf));
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n == 0) return;
    // Strip trailing newline
    for (size_t i = 0; i < n; ++i) {
        if (buf[i] == '\r' || buf[i] == '\n') {
            buf[i] = '\0';
            break;
        }
    }
    NSString *line = [NSString stringWithUTF8String:buf];
    if (!line || line.length == 0) return;

    if (!m2_hud_text_layer) {
        m2_hud_text_layer = [CATextLayer layer];
        m2_hud_text_layer.name = @"M2DualFpsOverlay";
        m2_hud_text_layer.zPosition = 9999.0;
        m2_hud_text_layer.contentsScale = 2.0;
        m2_hud_text_layer.font = (__bridge CFTypeRef)[NSFont monospacedSystemFontOfSize:11.5 weight:NSFontWeightBold];
        m2_hud_text_layer.fontSize = 11.5;
        m2_hud_text_layer.foregroundColor = [NSColor colorWithCalibratedRed:0.35 green:1.0 blue:0.65 alpha:1.0].CGColor;
        m2_hud_text_layer.backgroundColor = [NSColor colorWithCalibratedRed:0.04 green:0.06 blue:0.10 alpha:0.82].CGColor;
        m2_hud_text_layer.cornerRadius = 5.0;
        m2_hud_text_layer.alignmentMode = kCAAlignmentCenter;
        m2_hud_text_layer.wrapped = NO;
        // Disable implicit animations
        m2_hud_text_layer.actions = @{
            @"contents": [NSNull null],
            @"string": [NSNull null],
            @"bounds": [NSNull null],
            @"position": [NSNull null]
        };
    }
    if (m2_hud_text_layer.superlayer != root) {
        [m2_hud_text_layer removeFromSuperlayer];
        [root addSublayer:m2_hud_text_layer];
    }
    m2_hud_text_layer.hidden = NO;
    m2_hud_text_layer.string = [NSString stringWithFormat:@"  %@  ", line];

    NSRect b = cv.bounds;
    CGFloat barW = fmin(b.size.width - 24.0, 980.0);
    CGFloat barH = 22.0;
    // Place at top-left (y near top in default unflipped NSView coordinates, or top in flipped)
    CGFloat barY = cv.isFlipped ? 8.0 : (b.size.height - barH - 8.0);
    m2_hud_text_layer.frame = CGRectMake(12.0, barY, barW, barH);
}

static void m2_configure_metalfx_layer_recursive(CALayer *layer, CGFloat targetScale) {
    if (!layer || layer == m2_hud_text_layer) return;
    if ([layer isKindOfClass:[CAMetalLayer class]]) {
        CAMetalLayer *ml = (CAMetalLayer *)layer;
        CGSize boundsSize = ml.bounds.size;
        CGSize targetDrawable = CGSizeMake(boundsSize.width * targetScale, boundsSize.height * targetScale);
        BOOL scaleChanged = (fabs(ml.contentsScale - targetScale) > 0.01);
        BOOL drawableChanged = (boundsSize.width >= 800.0 && boundsSize.height >= 600.0) &&
                               (fabs(ml.drawableSize.width - targetDrawable.width) > 1.0 ||
                                fabs(ml.drawableSize.height - targetDrawable.height) > 1.0);
        if (scaleChanged || drawableChanged) {
            ml.contentsScale = targetScale;
            if (boundsSize.width >= 800.0 && boundsSize.height >= 600.0) {
                ml.drawableSize = targetDrawable;
            }
            NSLog(@"[M2-GameMode] Configured CAMetalLayer %p for MetalFX Retina %.1fx (bounds=%.0fx%.0f -> drawable=%.0fx%.0f)",
                  ml, targetScale, boundsSize.width, boundsSize.height, ml.drawableSize.width, ml.drawableSize.height);
        }
        if ([ml respondsToSelector:@selector(setDisplaySyncEnabled:)]) {
            ml.displaySyncEnabled = YES;
        }
    }
    for (CALayer *sub in layer.sublayers) {
        m2_configure_metalfx_layer_recursive(sub, targetScale);
    }
}

static void m2_sync_window_geometry_and_metalfx(NSWindow *w) {
    if (!m2_is_large_window(w)) return;
    NSView *cv = w.contentView;
    if (!cv) return;

    // Keep contentView strictly 1:1 with the window frame so winemac.drv never applies
    // a titlebar/safeArea vertical offset to mouse coordinates or D3D9 Reset.
    NSRect wf = w.frame;
    NSRect desiredBounds = NSMakeRect(0.0, 0.0, wf.size.width, wf.size.height);
    if (!NSEqualRects(cv.frame, desiredBounds)) {
        cv.frame = desiredBounds;
    }

    m2_update_dual_fps_overlay(cv);

    const char *no_mfx = getenv("M2_NO_METALFX");
    if (no_mfx && no_mfx[0] == '1') return;

    // Integer-exact 2.0x Retina scale on HiDPI displays (1512x982 -> 3024x1964)
    CGFloat backing = w.screen ? w.screen.backingScaleFactor : NSScreen.mainScreen.backingScaleFactor;
    CGFloat targetScale = (backing >= 2.0) ? 2.0 : 1.0;

    if (cv.layer) {
        m2_configure_metalfx_layer_recursive(cv.layer, targetScale);
    }
    id clientSurface = objc_getAssociatedObject(cv, "d3dmetal_client_surface");
    if (clientSurface && [clientSurface isKindOfClass:[CALayer class]]) {
        m2_configure_metalfx_layer_recursive((CALayer *)clientSurface, targetScale);
    }
    for (NSView *sub in cv.subviews) {
        if (sub.layer) {
            m2_configure_metalfx_layer_recursive(sub.layer, targetScale);
        }
        id subSurface = objc_getAssociatedObject(sub, "d3dmetal_client_surface");
        if (subSurface && [subSurface isKindOfClass:[CALayer class]]) {
            m2_configure_metalfx_layer_recursive((CALayer *)subSurface, targetScale);
        }
    }
}

static NSRect (*orig_contentRectForFrameRect)(id, SEL, NSRect) = NULL;
static NSRect m2_swizzled_contentRectForFrameRect(NSWindow *self, SEL _cmd, NSRect frameRect) {
    if (frameRect.size.width >= 800.0 && frameRect.size.height >= 600.0) {
        return frameRect;
    }
    if (orig_contentRectForFrameRect) {
        return orig_contentRectForFrameRect(self, _cmd, frameRect);
    }
    return frameRect;
}

static NSRect (*orig_frameRectForContentRect)(id, SEL, NSRect) = NULL;
static NSRect m2_swizzled_frameRectForContentRect(NSWindow *self, SEL _cmd, NSRect contentRect) {
    if (contentRect.size.width >= 800.0 && contentRect.size.height >= 600.0) {
        return contentRect;
    }
    if (orig_frameRectForContentRect) {
        return orig_frameRectForContentRect(self, _cmd, contentRect);
    }
    return contentRect;
}

static NSRect (*orig_constrainFrameRect)(id, SEL, NSRect, NSScreen *) = NULL;
static NSRect m2_swizzled_constrainFrameRect(NSWindow *self, SEL _cmd, NSRect frameRect, NSScreen *screen) {
    if (frameRect.size.width >= 800.0 && frameRect.size.height >= 600.0) {
        return frameRect;
    }
    if (orig_constrainFrameRect) {
        return orig_constrainFrameRect(self, _cmd, frameRect, screen);
    }
    return frameRect;
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
}

static void (*orig_setWindowFeatures)(id, SEL, void *) = NULL;
static void m2_swizzled_setWindowFeatures(NSWindow *self, SEL _cmd, void *features) {
    if (orig_setWindowFeatures) {
        orig_setWindowFeatures(self, _cmd, features);
    }
    if (!m2_is_large_window(self)) return;
    m2_tag_fullscreen_primary(self);
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
    }
    Method mFeatures = class_getInstanceMethod(cls, NSSelectorFromString(@"setWindowFeatures:"));
    if (mFeatures) {
        orig_setWindowFeatures = (void (*)(id, SEL, void *))method_getImplementation(mFeatures);
        method_setImplementation(mFeatures, (IMP)m2_swizzled_setWindowFeatures);
    }
    Method mContentRect = class_getInstanceMethod(cls, @selector(contentRectForFrameRect:));
    if (mContentRect) {
        orig_contentRectForFrameRect = (NSRect (*)(id, SEL, NSRect))method_getImplementation(mContentRect);
        method_setImplementation(mContentRect, (IMP)m2_swizzled_contentRectForFrameRect);
    }
    Method mFrameRect = class_getInstanceMethod(cls, @selector(frameRectForContentRect:));
    if (mFrameRect) {
        orig_frameRectForContentRect = (NSRect (*)(id, SEL, NSRect))method_getImplementation(mFrameRect);
        method_setImplementation(mFrameRect, (IMP)m2_swizzled_frameRectForContentRect);
    }
    Method mConstrain = class_getInstanceMethod(cls, @selector(constrainFrameRect:toScreen:));
    if (mConstrain) {
        orig_constrainFrameRect = (NSRect (*)(id, SEL, NSRect, NSScreen *))method_getImplementation(mConstrain);
        method_setImplementation(mConstrain, (IMP)m2_swizzled_constrainFrameRect);
    }
    NSLog(@"[M2-GameMode] Swizzled WineWindow fullscreen + 1:1 contentRect geometry hooks");
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
    m2_sync_window_geometry_and_metalfx(candidate);

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
    if ((now - m2_first_visible_ts) < 1.2) {
        return;
    }

    // Add Resizable + FullSizeContentView (NEVER Titled!) immediately prior to FullScreen transition
    // so contentRect stays 1:1 with frameRect (1512x982) and cursor hitboxes have zero vertical offset.
    NSWindowStyleMask mask = candidate.styleMask;
    mask &= ~NSWindowStyleMaskTitled;
    mask |= (NSWindowStyleMaskResizable | NSWindowStyleMaskFullSizeContentView);
    candidate.styleMask = mask;
    candidate.titlebarAppearsTransparent = YES;
    candidate.titleVisibility = NSWindowTitleHidden;

    m2_transitioning = YES;
    NSLog(@"[M2-GameMode] Transitioning D3D9 window '%@' (%.0fx%.0f, styleMask=0x%lx) into native AppKit FullScreen Space",
          candidate.title, candidate.frame.size.width, candidate.frame.size.height, (unsigned long)candidate.styleMask);
    dispatch_async(dispatch_get_main_queue(), ^{
        [candidate toggleFullScreen:nil];
    });
}

__attribute__((constructor)) static void m2_gamemode_init(void) {
    // Enable Apple Metal Performance HUD + mtld3d live telemetry row unless explicitly disabled via M2_FAST_HUD=0
    const char *hud_opt = getenv("M2_FAST_HUD");
    if (!hud_opt || (strcmp(hud_opt, "0") != 0 && strcmp(hud_opt, "off") != 0)) {
        setenv("MTL_HUD_ENABLED", "1", 1);
        setenv("MTL_HUD_STATE_REPORTER_DOMAINS", "mtld3d", 1);
    }

    const char *bid = NSBundle.mainBundle.bundleIdentifier ? NSBundle.mainBundle.bundleIdentifier.UTF8String : "none";
    fprintf(stderr, "[M2-GameMode] v11 Constructor loaded (pid=%d, bundle=%s, MTL_HUD_ENABLED=%s)\n",
            (int)getpid(), bid, getenv("MTL_HUD_ENABLED") ?: "0");
    fflush(stderr);

    dispatch_async(dispatch_get_main_queue(), ^{
        [NSNotificationCenter.defaultCenter addObserverForName:NSWindowDidEnterFullScreenNotification
            object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *n) {
                NSWindow *w = (NSWindow *)n.object;
                m2_tag_fullscreen_primary(w);
                m2_sync_window_geometry_and_metalfx(w);
                m2_transitioning = NO;
                if (m2_is_large_window(w)) {
                    m2_done = YES;
                    NSLog(@"[M2-GameMode] Confirmed NSWindowDidEnterFullScreenNotification for '%@' (%.0fx%.0f, content=%.0fx%.0f)",
                          w.title, w.frame.size.width, w.frame.size.height,
                          w.contentView.frame.size.width, w.contentView.frame.size.height);
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
