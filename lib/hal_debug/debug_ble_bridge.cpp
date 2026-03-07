// Optional BLE bridge for debug logging.
// Links DebugLog's BLE backend to the BleManager.
// Include this file in your build to enable BLE debug output.

#ifndef SIMULATOR
#include "ble_manager.h"

extern "C" void debug_ble_send(const char* data, size_t len) {
    BleManager* ble = BleManager::instance();
    if (ble && ble->isConnected()) {
        ble->send(data, len);
    }
}
#endif
