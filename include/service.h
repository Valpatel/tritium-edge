#pragma once
#include <cstdint>
#include <cstddef>

enum ServiceCap : uint8_t {
    SVC_TICK       = 0x01,
    SVC_SERIAL_CMD = 0x02,
    SVC_WEB_API    = 0x04,
    SVC_SHUTDOWN   = 0x08,
};

class ServiceInterface {
public:
    virtual ~ServiceInterface() = default;
    virtual const char* name() const = 0;
    virtual uint8_t capabilities() const = 0;
    virtual int initPriority() const { return 100; }
    virtual bool init() = 0;
    virtual void tick() {}
    virtual void shutdown() {}
    virtual bool handleCommand(const char* cmd, const char* args) { (void)cmd; (void)args; return false; }
    virtual int toJson(char* buf, size_t size) { (void)buf; (void)size; return 0; }
};
