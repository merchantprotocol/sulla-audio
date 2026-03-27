#include <gtest/gtest.h>
#include <sulla/DeviceController.h>
#include <sulla/IDeviceEnumerator.h>

using namespace sulla;

// ─── Mock enumerator (no platform deps) ──────────────────────

class MockEnumerator : public IDeviceEnumerator {
public:
    DeviceList devices;
    AudioDevice defaultDevice;

    DeviceList listOutputDevices() override { return devices; }
    AudioDevice getDefaultOutputDevice() override { return defaultDevice; }

    AudioDevice getDeviceById(const std::string& id) override {
        for (const auto& d : devices) {
            if (d.id == id) return d;
        }
        return {};
    }
};

AudioDevice makeDevice(const std::string& id, const std::string& name,
                        bool isDefault = false, bool isLoopback = false) {
    AudioDevice d;
    d.id = id;
    d.name = name;
    d.type = AudioDevice::Type::Output;
    d.isDefault = isDefault;
    d.isLoopbackCapable = isLoopback;
    d.nativeFormat = AudioFormat::wasapi();
    return d;
}

TEST(DeviceController, SelectPreferred_Found) {
    auto mock = std::make_unique<MockEnumerator>();
    auto dev = makeDevice("dev-1", "Speakers", false, true);
    mock->devices = {dev};

    DeviceController ctrl(std::move(mock));
    auto result = ctrl.selectDevice("dev-1");

    EXPECT_TRUE(result.found);
    EXPECT_EQ(result.device.id, "dev-1");
}

TEST(DeviceController, SelectPreferred_NotFound) {
    auto mock = std::make_unique<MockEnumerator>();
    mock->devices = {makeDevice("dev-1", "Speakers", false, true)};

    DeviceController ctrl(std::move(mock));
    auto result = ctrl.selectDevice("nonexistent");

    EXPECT_FALSE(result.found);
    EXPECT_NE(result.message.find("not found"), std::string::npos);
}

TEST(DeviceController, SelectPreferred_NotLoopbackCapable) {
    auto mock = std::make_unique<MockEnumerator>();
    auto dev = makeDevice("dev-1", "Speakers", false, false); // not loopback
    mock->devices = {dev};

    DeviceController ctrl(std::move(mock));
    auto result = ctrl.selectDevice("dev-1");

    EXPECT_FALSE(result.found);
    EXPECT_NE(result.message.find("loopback"), std::string::npos);
}

TEST(DeviceController, ListAllDevices) {
    auto mock = std::make_unique<MockEnumerator>();
    mock->devices = {
        makeDevice("dev-1", "Speakers"),
        makeDevice("dev-2", "Headphones"),
        makeDevice("dev-3", "BlackHole 2ch", false, true),
    };

    DeviceController ctrl(std::move(mock));
    auto list = ctrl.listAllDevices();

    EXPECT_EQ(list.size(), 3u);
}

TEST(DeviceController, NullEnumerator) {
    DeviceController ctrl(nullptr);
    auto result = ctrl.selectDevice();
    EXPECT_FALSE(result.found);
    EXPECT_NE(result.message.find("enumerator"), std::string::npos);
}

// Platform-specific auto-selection is tested via the mock —
// the real platform dispatch happens at compile time in selectDevice().
