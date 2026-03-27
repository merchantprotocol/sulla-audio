#!/usr/bin/env swift
//
// create-multi-output — creates a Multi-Output Device combining
// the default output device with BlackHole 2ch, then sets it as
// the system default output so all audio mirrors to BlackHole.
//
// Usage:
//   swift create-multi-output.swift          # create + set default
//   swift create-multi-output.swift --check  # exit 0 if already exists
//   swift create-multi-output.swift --remove # destroy the device
//

import CoreAudio
import Foundation

let kDeviceName = "Sulla Audio Mirror"
let kDeviceUID  = "SullaAudioMirror_UID"
let kBlackHoleUID = "BlackHole2ch_UID"

// MARK: - Helpers

func getDefaultOutputDeviceUID() -> String? {
    var deviceID = AudioObjectID(kAudioObjectSystemObject)
    var address = AudioObjectPropertyAddress(
        mSelector: kAudioHardwarePropertyDefaultOutputDevice,
        mScope: kAudioObjectPropertyScopeGlobal,
        mElement: kAudioObjectPropertyElementMain
    )
    var size = UInt32(MemoryLayout<AudioObjectID>.size)
    let status = AudioObjectGetPropertyData(
        AudioObjectID(kAudioObjectSystemObject), &address, 0, nil, &size, &deviceID
    )
    guard status == noErr else { return nil }

    // Now get UID string from device ID
    var uidAddress = AudioObjectPropertyAddress(
        mSelector: kAudioDevicePropertyDeviceUID,
        mScope: kAudioObjectPropertyScopeGlobal,
        mElement: kAudioObjectPropertyElementMain
    )
    var uid: CFString = "" as CFString
    var uidSize = UInt32(MemoryLayout<CFString>.size)
    let uidStatus = AudioObjectGetPropertyData(deviceID, &uidAddress, 0, nil, &uidSize, &uid)
    guard uidStatus == noErr else { return nil }
    return uid as String
}

func findDeviceByUID(_ targetUID: String) -> AudioObjectID? {
    var address = AudioObjectPropertyAddress(
        mSelector: kAudioHardwarePropertyDevices,
        mScope: kAudioObjectPropertyScopeGlobal,
        mElement: kAudioObjectPropertyElementMain
    )
    var size: UInt32 = 0
    AudioObjectGetPropertyDataSize(AudioObjectID(kAudioObjectSystemObject), &address, 0, nil, &size)
    let count = Int(size) / MemoryLayout<AudioObjectID>.size
    var devices = [AudioObjectID](repeating: 0, count: count)
    AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject), &address, 0, nil, &size, &devices)

    for device in devices {
        var uidAddress = AudioObjectPropertyAddress(
            mSelector: kAudioDevicePropertyDeviceUID,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )
        var uid: CFString = "" as CFString
        var uidSize = UInt32(MemoryLayout<CFString>.size)
        let status = AudioObjectGetPropertyData(device, &uidAddress, 0, nil, &uidSize, &uid)
        if status == noErr && (uid as String) == targetUID {
            return device
        }
    }
    return nil
}

func setDefaultOutputDevice(_ deviceID: AudioObjectID) -> Bool {
    var address = AudioObjectPropertyAddress(
        mSelector: kAudioHardwarePropertyDefaultOutputDevice,
        mScope: kAudioObjectPropertyScopeGlobal,
        mElement: kAudioObjectPropertyElementMain
    )
    var mutableID = deviceID
    let status = AudioObjectSetPropertyData(
        AudioObjectID(kAudioObjectSystemObject), &address, 0, nil,
        UInt32(MemoryLayout<AudioObjectID>.size), &mutableID
    )
    return status == noErr
}

// MARK: - Commands

func checkExists() -> Bool {
    return findDeviceByUID(kDeviceUID) != nil
}

func createMultiOutput() -> Bool {
    // Don't create if already exists
    if checkExists() {
        print("Multi-Output Device '\(kDeviceName)' already exists.")
        return true
    }

    // Get the default output device UID (speakers/headphones)
    guard let defaultUID = getDefaultOutputDeviceUID() else {
        fputs("Error: could not determine default output device.\n", stderr)
        return false
    }

    // Don't create if default is already BlackHole (would create a loop)
    if defaultUID == kBlackHoleUID {
        fputs("Error: default output is already BlackHole. Set a physical output device first.\n", stderr)
        return false
    }

    // Don't create if default is already our mirror device
    if defaultUID == kDeviceUID {
        print("Multi-Output Device is already the default output.")
        return true
    }

    // Verify BlackHole exists
    guard findDeviceByUID(kBlackHoleUID) != nil else {
        fputs("Error: BlackHole 2ch not found. Install it first.\n", stderr)
        return false
    }

    // Create the aggregate device description
    // Main device = physical output (speakers), sub-device = BlackHole
    // kAudioAggregateDeviceIsStackedKey = 1 makes it Multi-Output (stacked)
    let description: [String: Any] = [
        kAudioAggregateDeviceNameKey as String: kDeviceName,
        kAudioAggregateDeviceUIDKey as String: kDeviceUID,
        kAudioAggregateDeviceIsStackedKey as String: 1 as UInt32,
        kAudioAggregateDeviceSubDeviceListKey as String: [
            [kAudioSubDeviceUIDKey as String: defaultUID],
            [kAudioSubDeviceUIDKey as String: kBlackHoleUID],
        ]
    ]

    var aggregateID: AudioObjectID = 0
    let status = AudioHardwareCreateAggregateDevice(description as CFDictionary, &aggregateID)

    if status != noErr {
        fputs("Error: AudioHardwareCreateAggregateDevice failed with status \(status)\n", stderr)
        return false
    }

    print("Created Multi-Output Device '\(kDeviceName)' (ID: \(aggregateID))")

    // Set as default output
    if setDefaultOutputDevice(aggregateID) {
        print("Set '\(kDeviceName)' as default output device.")
    } else {
        fputs("Warning: created device but failed to set as default output.\n", stderr)
        fputs("Set it manually in System Settings > Sound > Output.\n", stderr)
    }

    return true
}

func removeMultiOutput() -> Bool {
    guard let deviceID = findDeviceByUID(kDeviceUID) else {
        print("Multi-Output Device not found. Nothing to remove.")
        return true
    }

    // Before destroying, switch default output back to something else
    // Find the first non-aggregate output device
    if let defaultUID = getDefaultOutputDeviceUID(), defaultUID == kDeviceUID {
        // Find a physical device to fall back to
        var address = AudioObjectPropertyAddress(
            mSelector: kAudioHardwarePropertyDevices,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )
        var size: UInt32 = 0
        AudioObjectGetPropertyDataSize(AudioObjectID(kAudioObjectSystemObject), &address, 0, nil, &size)
        let count = Int(size) / MemoryLayout<AudioObjectID>.size
        var devices = [AudioObjectID](repeating: 0, count: count)
        AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject), &address, 0, nil, &size, &devices)

        for dev in devices {
            var uidAddr = AudioObjectPropertyAddress(
                mSelector: kAudioDevicePropertyDeviceUID,
                mScope: kAudioObjectPropertyScopeGlobal,
                mElement: kAudioObjectPropertyElementMain
            )
            var uid: CFString = "" as CFString
            var uidSize = UInt32(MemoryLayout<CFString>.size)
            AudioObjectGetPropertyData(dev, &uidAddr, 0, nil, &uidSize, &uid)
            let uidStr = uid as String
            if uidStr != kDeviceUID && uidStr != kBlackHoleUID && !uidStr.isEmpty {
                // Check it has output channels
                var outputAddr = AudioObjectPropertyAddress(
                    mSelector: kAudioDevicePropertyStreamConfiguration,
                    mScope: kAudioObjectPropertyScopeOutput,
                    mElement: kAudioObjectPropertyElementMain
                )
                var bufSize: UInt32 = 0
                AudioObjectGetPropertyDataSize(dev, &outputAddr, 0, nil, &bufSize)
                if bufSize > 0 {
                    let _ = setDefaultOutputDevice(dev)
                    print("Switched default output back to device: \(uidStr)")
                    break
                }
            }
        }
    }

    let status = AudioHardwareDestroyAggregateDevice(deviceID)
    if status != noErr {
        fputs("Error: AudioHardwareDestroyAggregateDevice failed with status \(status)\n", stderr)
        return false
    }

    print("Removed Multi-Output Device '\(kDeviceName)'.")
    return true
}

// MARK: - Main

let args = CommandLine.arguments
if args.contains("--check") {
    exit(checkExists() ? 0 : 1)
} else if args.contains("--remove") {
    exit(removeMultiOutput() ? 0 : 1)
} else {
    exit(createMultiOutput() ? 0 : 1)
}
