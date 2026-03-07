#pragma once
#include <cstdint>
#include <cstddef>
#include <functional>

using BleReceiveCallback = std::function<void(const char* data, size_t len)>;

class BleManager {
public:
    BleManager();
    ~BleManager();

    void init(const char* deviceName = "ESP32-HW");
    void shutdown();

    void send(const char* data);
    void send(const char* data, size_t len);

    void onReceive(BleReceiveCallback cb);

    bool isConnected() const;
    bool isAdvertising() const;

    static BleManager* instance();

private:
    static BleManager* _instance;
    BleReceiveCallback _callback = nullptr;
    bool _initialized = false;
    bool _connected = false;
    bool _advertising = false;

    friend class BleSerialCallbacks;
    friend class BleServerCallbacks;
};
