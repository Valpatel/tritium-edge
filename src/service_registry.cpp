#include <Arduino.h>
#include <cstring>
#include "service_registry.h"

ServiceInterface* ServiceRegistry::_services[MAX_SERVICES] = {};
int  ServiceRegistry::_count  = 0;
bool ServiceRegistry::_sorted = false;

bool ServiceRegistry::add(ServiceInterface* svc) {
    if (_count >= MAX_SERVICES || !svc) return false;
    _services[_count++] = svc;
    _sorted = false;
    return true;
}

int ServiceRegistry::initAll(ServiceInitCallback cb) {
    // Insertion sort by initPriority() — ascending (lower = earlier)
    for (int i = 1; i < _count; i++) {
        ServiceInterface* key = _services[i];
        int j = i - 1;
        while (j >= 0 && _services[j]->initPriority() > key->initPriority()) {
            _services[j + 1] = _services[j];
            j--;
        }
        _services[j + 1] = key;
    }
    _sorted = true;

    int ok = 0;
    for (int i = 0; i < _count; i++) {
        Serial.printf("[svc] init %-20s (pri %3d) ... ", _services[i]->name(), _services[i]->initPriority());
        bool result = _services[i]->init();
        if (result) {
            Serial.printf("OK\n");
            ok++;
        } else {
            Serial.printf("FAIL\n");
        }
        if (cb) {
            cb(_services[i]->name(), result, i, _count);
        }
    }
    Serial.printf("[svc] %d/%d services initialised\n", ok, _count);
    return ok;
}

void ServiceRegistry::tickAll() {
    for (int i = 0; i < _count; i++) {
        if (_services[i]->capabilities() & SVC_TICK) {
            _services[i]->tick();
        }
    }
}

void ServiceRegistry::shutdownAll() {
    // Shutdown in reverse init order
    for (int i = _count - 1; i >= 0; i--) {
        if (_services[i]->capabilities() & SVC_SHUTDOWN) {
            Serial.printf("[svc] shutdown %s\n", _services[i]->name());
            _services[i]->shutdown();
        }
    }
}

bool ServiceRegistry::dispatchCommand(const char* cmd, const char* args) {
    for (int i = 0; i < _count; i++) {
        if ((_services[i]->capabilities() & SVC_SERIAL_CMD) &&
            _services[i]->handleCommand(cmd, args)) {
            return true;
        }
    }
    return false;
}

ServiceInterface* ServiceRegistry::get(const char* name) {
    for (int i = 0; i < _count; i++) {
        if (strcmp(_services[i]->name(), name) == 0) {
            return _services[i];
        }
    }
    return nullptr;
}

int ServiceRegistry::count() {
    return _count;
}

ServiceInterface* ServiceRegistry::at(int index) {
    if (index < 0 || index >= _count) return nullptr;
    return _services[index];
}
