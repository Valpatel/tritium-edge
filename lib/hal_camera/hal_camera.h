#pragma once
// Camera HAL for OV5640 DVP via esp32-camera
//
// Usage:
//   #include "hal_camera.h"
//   CameraHAL cam;
//   cam.init(CamResolution::QVGA_320x240, CamPixelFormat::RGB565);
//   CameraFrame* f = cam.capture();
//   // use f->data, f->len, f->width, f->height
//   cam.releaseFrame();

#include <cstdint>
#include <cstddef>

// Camera resolution presets
enum class CamResolution : uint8_t {
    QQVGA_160x120,
    QVGA_320x240,
    VGA_640x480,
    SVGA_800x600,
    XGA_1024x768,
    SXGA_1280x1024,
    UXGA_1600x1200
};

enum class CamPixelFormat : uint8_t {
    RGB565,
    JPEG,
    GRAYSCALE
};

struct CameraFrame {
    uint8_t *data;
    size_t len;
    uint16_t width;
    uint16_t height;
    CamPixelFormat format;
};

class CameraHAL {
public:
    bool init(CamResolution res = CamResolution::QVGA_320x240,
              CamPixelFormat fmt = CamPixelFormat::RGB565);
    void deinit();
    bool available() const { return _initialized; }

    // Capture a single frame - returned frame is valid until next capture() or deinit()
    CameraFrame* capture();
    void releaseFrame();

    // Settings
    bool setResolution(CamResolution res);
    bool setPixelFormat(CamPixelFormat fmt);
    bool setFlip(bool horizontal, bool vertical);
    bool setBrightness(int level);  // -2 to 2
    bool setContrast(int level);    // -2 to 2
    bool setSaturation(int level);  // -2 to 2

    // Get current resolution
    uint16_t getWidth() const { return _width; }
    uint16_t getHeight() const { return _height; }

private:
    bool _initialized = false;
    uint16_t _width = 0;
    uint16_t _height = 0;
    CameraFrame _frame = {};
    void *_fb = nullptr;  // opaque handle to camera_fb_t
};
