#pragma once

#ifdef _WIN32

#include <sulla/IDeviceEnumerator.h>
#include <sulla/AudioDevice.h>
#include <sulla/AudioFormat.h>

#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <combaseapi.h>
#include <string>
#include <vector>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")

namespace sulla {

/**
 * WasapiDeviceEnumerator — lists audio output devices via Windows WASAPI.
 *
 * Thin wrapper around IMMDeviceEnumerator COM interface.
 * No business decisions — just translates COM results to AudioDevice models.
 */
class WasapiDeviceEnumerator : public IDeviceEnumerator {
public:
    WasapiDeviceEnumerator() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        CoCreateInstance(
            __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            __uuidof(IMMDeviceEnumerator),
            reinterpret_cast<void**>(&enumerator_)
        );
    }

    ~WasapiDeviceEnumerator() override {
        if (enumerator_) enumerator_->Release();
    }

    DeviceList listOutputDevices() override {
        DeviceList devices;
        if (!enumerator_) return devices;

        IMMDeviceCollection* collection = nullptr;
        HRESULT hr = enumerator_->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);
        if (FAILED(hr)) return devices;

        UINT count = 0;
        collection->GetCount(&count);

        // Get default device ID for comparison
        std::wstring defaultId;
        IMMDevice* defaultDevice = nullptr;
        if (SUCCEEDED(enumerator_->GetDefaultAudioEndpoint(eRender, eConsole, &defaultDevice))) {
            LPWSTR id = nullptr;
            defaultDevice->GetId(&id);
            if (id) { defaultId = id; CoTaskMemFree(id); }
            defaultDevice->Release();
        }

        for (UINT i = 0; i < count; ++i) {
            IMMDevice* device = nullptr;
            if (FAILED(collection->Item(i, &device))) continue;
            devices.push_back(deviceFromMMDevice(device, defaultId));
            device->Release();
        }

        collection->Release();
        return devices;
    }

    AudioDevice getDefaultOutputDevice() override {
        if (!enumerator_) return {};

        IMMDevice* device = nullptr;
        HRESULT hr = enumerator_->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        if (FAILED(hr) || !device) return {};

        auto result = deviceFromMMDevice(device, L"");
        result.isDefault = true;
        device->Release();
        return result;
    }

    AudioDevice getDeviceById(const std::string& deviceId) override {
        if (!enumerator_) return {};

        std::wstring wideId(deviceId.begin(), deviceId.end());
        IMMDevice* device = nullptr;
        HRESULT hr = enumerator_->GetDevice(wideId.c_str(), &device);
        if (FAILED(hr) || !device) return {};

        auto result = deviceFromMMDevice(device, L"");
        device->Release();
        return result;
    }

private:
    IMMDeviceEnumerator* enumerator_ = nullptr;

    AudioDevice deviceFromMMDevice(IMMDevice* device, const std::wstring& defaultId) {
        AudioDevice ad;

        // Get device ID
        LPWSTR id = nullptr;
        device->GetId(&id);
        if (id) {
            // Convert wide string to narrow
            int len = WideCharToMultiByte(CP_UTF8, 0, id, -1, nullptr, 0, nullptr, nullptr);
            ad.id.resize(len - 1);
            WideCharToMultiByte(CP_UTF8, 0, id, -1, ad.id.data(), len, nullptr, nullptr);
            ad.isDefault = (defaultId == id);
            CoTaskMemFree(id);
        }

        // Get friendly name
        IPropertyStore* props = nullptr;
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props))) {
            PROPVARIANT pv;
            PropVariantInit(&pv);
            if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR) {
                int len = WideCharToMultiByte(CP_UTF8, 0, pv.pwszVal, -1, nullptr, 0, nullptr, nullptr);
                ad.name.resize(len - 1);
                WideCharToMultiByte(CP_UTF8, 0, pv.pwszVal, -1, ad.name.data(), len, nullptr, nullptr);
            }
            PropVariantClear(&pv);
            props->Release();
        }

        ad.type = AudioDevice::Type::Output;
        ad.isLoopbackCapable = true;  // All WASAPI render endpoints support loopback
        ad.nativeFormat = AudioFormat::wasapi();  // WASAPI default is 48kHz/32-bit/float

        return ad;
    }
};

} // namespace sulla

#endif // _WIN32
