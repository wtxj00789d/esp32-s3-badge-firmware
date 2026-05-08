#pragma once

enum DeviceState {
    kDeviceStateStarting,
    kDeviceStateWifiConfiguring,
    kDeviceStateIdle,
    kDeviceStateConnecting,
    kDeviceStateListening,
    kDeviceStateAudioTesting,
    kDeviceStateSpeaking,
    kDeviceStateUpgrading,
    kDeviceStateActivating,
};

class Application {
public:
    static Application& GetInstance()
    {
        static Application instance;
        return instance;
    }

    DeviceState GetDeviceState() const { return kDeviceStateIdle; }
    bool IsVoiceDetected() const { return false; }
};
