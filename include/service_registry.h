#pragma once
#include "service.h"

/// Callback invoked after each service init during initAll().
/// @param name    service name
/// @param ok      true if init() returned true
/// @param index   0-based index in init order
/// @param total   total number of services
typedef void (*ServiceInitCallback)(const char* name, bool ok, int index, int total);

class ServiceRegistry {
public:
    static constexpr int MAX_SERVICES = 20;
    static bool add(ServiceInterface* svc);
    static int  initAll(ServiceInitCallback cb = nullptr);
    static void tickAll();
    static void shutdownAll();
    static bool dispatchCommand(const char* cmd, const char* args);
    static ServiceInterface* get(const char* name);
    template<typename T> static T* getAs(const char* name) { return static_cast<T*>(get(name)); }
    static int count();
    static ServiceInterface* at(int index);
private:
    static ServiceInterface* _services[MAX_SERVICES];
    static int _count;
    static bool _sorted;
};
