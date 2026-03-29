#pragma once

#ifdef __APPLE__

#include <CoreGraphics/CoreGraphics.h>
#include <IOKit/hidsystem/ev_keymap.h>
#include <functional>
#include <thread>
#include <atomic>
#include "Logger.h"

namespace sulla {

/**
 * MediaKeyListener — intercepts macOS media key events (volume up/down/mute)
 * so the audio driver can route them to the physical output device.
 *
 * macOS disables keyboard volume control when a multi-output aggregate
 * device is the default output. This listener catches the raw key events
 * before macOS blocks them, and forwards them to a callback that controls
 * the physical sub-device's volume directly.
 *
 * Requires Accessibility permission (System Settings > Privacy & Security).
 */
class MediaKeyListener {
public:
    // macOS volume step per key press (1/16 of full range, matches native behavior)
    static constexpr float kVolumeStep = 0.0625f;

    using VolumeAdjustCallback = std::function<void(float delta)>;
    using MuteToggleCallback   = std::function<void()>;

    MediaKeyListener() = default;
    ~MediaKeyListener() { stop(); }

    void onVolumeAdjust(VolumeAdjustCallback cb) { volumeAdjustCb_ = std::move(cb); }
    void onMuteToggle(MuteToggleCallback cb) { muteToggleCb_ = std::move(cb); }

    bool start() {
        if (running_.load()) return true;

        // NX_SYSDEFINED = 14 — system-defined events including media keys
        CGEventMask mask = (1 << 14);
        eventTap_ = CGEventTapCreate(
            kCGSessionEventTap,
            kCGHeadInsertEventTap,
            kCGEventTapOptionDefault,
            mask,
            eventTapCallback,
            this
        );

        if (!eventTap_) {
            SULLA_LOG_ERROR("MediaKeys", "Failed to create event tap — "
                "grant Accessibility permission to the audio-driver in "
                "System Settings > Privacy & Security > Accessibility");
            return false;
        }

        runLoopSource_ = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, eventTap_, 0);
        if (!runLoopSource_) {
            SULLA_LOG_ERROR("MediaKeys", "Failed to create run loop source");
            CFRelease(eventTap_);
            eventTap_ = nullptr;
            return false;
        }

        running_.store(true);

        tapThread_ = std::thread([this]() {
            runLoop_ = CFRunLoopGetCurrent();
            CFRunLoopAddSource(runLoop_, runLoopSource_, kCFRunLoopCommonModes);
            CGEventTapEnable(eventTap_, true);

            SULLA_LOG_INFO("MediaKeys", "Listening for volume keys");

            while (running_.load()) {
                CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.5, true);
            }

            CGEventTapEnable(eventTap_, false);
            CFRunLoopRemoveSource(runLoop_, runLoopSource_, kCFRunLoopCommonModes);
            SULLA_LOG_DEBUG("MediaKeys", "Run loop exited");
        });

        return true;
    }

    void stop() {
        if (!running_.load()) return;
        running_.store(false);

        if (runLoop_) {
            CFRunLoopStop(runLoop_);
        }

        if (tapThread_.joinable()) {
            tapThread_.join();
        }

        if (runLoopSource_) {
            CFRelease(runLoopSource_);
            runLoopSource_ = nullptr;
        }
        if (eventTap_) {
            CFRelease(eventTap_);
            eventTap_ = nullptr;
        }
        runLoop_ = nullptr;

        SULLA_LOG_INFO("MediaKeys", "Stopped");
    }

private:
    std::atomic<bool> running_{false};
    std::thread tapThread_;
    CFMachPortRef eventTap_ = nullptr;
    CFRunLoopSourceRef runLoopSource_ = nullptr;
    CFRunLoopRef runLoop_ = nullptr;
    VolumeAdjustCallback volumeAdjustCb_;
    MuteToggleCallback muteToggleCb_;

    static CGEventRef eventTapCallback(
        CGEventTapProxy /*proxy*/,
        CGEventType type,
        CGEventRef event,
        void* refcon
    ) {
        auto* self = static_cast<MediaKeyListener*>(refcon);

        // Re-enable tap if macOS disabled it (happens under heavy load)
        if (type == kCGEventTapDisabledByTimeout || type == kCGEventTapDisabledByUserInput) {
            CGEventTapEnable(self->eventTap_, true);
            return event;
        }

        // NX_SYSDEFINED = 14
        if (type != 14) return event;

        // For NX_SYSDEFINED events, the subtype must be 8 (NX_SUBTYPE_AUX_CONTROL_BUTTONS)
        // CGEvent stores NSEvent's data1 in field 87, and subtype in the event flags area.
        // We extract data1 which contains the key code and key state.
        int64_t data1 = CGEventGetIntegerValueField(event, (CGEventField)87);

        // Subtype is in bits [0..7] of another field, but for media keys
        // the reliable check is that data1's structure matches the expected layout.
        // data1 layout: [keyCode:16][keyDown:1][repeat:1][reserved:6][subtype:8]
        int keyCode   = (int)((data1 >> 16) & 0xFFFF);
        int keyFlags  = (int)(data1 & 0xFFFF);
        int keyState  = (keyFlags >> 8) & 0xFF;
        int subtype   = keyFlags & 0xFF;

        // subtype 8 = NX_SUBTYPE_AUX_CONTROL_BUTTONS (media keys)
        if (subtype != 8) return event;

        // keyState: 0x0A = key down, 0x0B = key up
        bool isKeyDown = (keyState == 0x0A);
        if (!isKeyDown) return event;

        switch (keyCode) {
            case NX_KEYTYPE_SOUND_UP:
                if (self->volumeAdjustCb_) {
                    self->volumeAdjustCb_(kVolumeStep);
                    return nullptr; // Consume the event
                }
                break;
            case NX_KEYTYPE_SOUND_DOWN:
                if (self->volumeAdjustCb_) {
                    self->volumeAdjustCb_(-kVolumeStep);
                    return nullptr;
                }
                break;
            case NX_KEYTYPE_MUTE:
                if (self->muteToggleCb_) {
                    self->muteToggleCb_();
                    return nullptr;
                }
                break;
        }

        return event; // Pass through non-volume keys
    }
};

} // namespace sulla

#endif // __APPLE__
