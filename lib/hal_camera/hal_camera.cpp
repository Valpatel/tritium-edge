#include "hal_camera.h"

#ifdef SIMULATOR

// --- Simulator stubs ---

bool CameraHAL::init(CamResolution, CamPixelFormat) { return false; }
void CameraHAL::deinit() {}
CameraFrame* CameraHAL::capture() { return nullptr; }
void CameraHAL::releaseFrame() {}
bool CameraHAL::setResolution(CamResolution) { return false; }
bool CameraHAL::setPixelFormat(CamPixelFormat) { return false; }
bool CameraHAL::setFlip(bool, bool) { return false; }
bool CameraHAL::setBrightness(int) { return false; }
bool CameraHAL::setContrast(int) { return false; }
bool CameraHAL::setSaturation(int) { return false; }

#else // ESP32

#include <Arduino.h>
#include <esp_camera.h>

#ifndef HAS_CAMERA
#define HAS_CAMERA 0
#endif

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static framesize_t mapResolution(CamResolution res) {
    switch (res) {
        case CamResolution::QQVGA_160x120:  return FRAMESIZE_QQVGA;
        case CamResolution::QVGA_320x240:   return FRAMESIZE_QVGA;
        case CamResolution::VGA_640x480:    return FRAMESIZE_VGA;
        case CamResolution::SVGA_800x600:   return FRAMESIZE_SVGA;
        case CamResolution::XGA_1024x768:   return FRAMESIZE_XGA;
        case CamResolution::SXGA_1280x1024: return FRAMESIZE_SXGA;
        case CamResolution::UXGA_1600x1200: return FRAMESIZE_UXGA;
        default:                            return FRAMESIZE_QVGA;
    }
}

static pixformat_t mapPixelFormat(CamPixelFormat fmt) {
    switch (fmt) {
        case CamPixelFormat::RGB565:    return PIXFORMAT_RGB565;
        case CamPixelFormat::JPEG:      return PIXFORMAT_JPEG;
        case CamPixelFormat::GRAYSCALE: return PIXFORMAT_GRAYSCALE;
        default:                        return PIXFORMAT_RGB565;
    }
}

static void resolutionDimensions(CamResolution res, uint16_t &w, uint16_t &h) {
    switch (res) {
        case CamResolution::QQVGA_160x120:  w = 160;  h = 120;  break;
        case CamResolution::QVGA_320x240:   w = 320;  h = 240;  break;
        case CamResolution::VGA_640x480:    w = 640;  h = 480;  break;
        case CamResolution::SVGA_800x600:   w = 800;  h = 600;  break;
        case CamResolution::XGA_1024x768:   w = 1024; h = 768;  break;
        case CamResolution::SXGA_1280x1024: w = 1280; h = 1024; break;
        case CamResolution::UXGA_1600x1200: w = 1600; h = 1200; break;
        default:                            w = 320;  h = 240;  break;
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool CameraHAL::init(CamResolution res, CamPixelFormat fmt) {
#if !HAS_CAMERA
    return false;
#else
    if (_initialized) return true;

    camera_config_t config = {};

    // Pin assignment from board header
    config.pin_pwdn    = CAM_PWDN;
    config.pin_reset   = CAM_RESET;
    config.pin_xclk    = CAM_XCLK;
    config.pin_sccb_sda = CAM_SIOD;
    config.pin_sccb_scl = CAM_SIOC;

    config.pin_d0      = CAM_Y2;
    config.pin_d1      = CAM_Y3;
    config.pin_d2      = CAM_Y4;
    config.pin_d3      = CAM_Y5;
    config.pin_d4      = CAM_Y6;
    config.pin_d5      = CAM_Y7;
    config.pin_d6      = CAM_Y8;
    config.pin_d7      = CAM_Y9;
    config.pin_vsync   = CAM_VSYNC;
    config.pin_href    = CAM_HREF;
    config.pin_pclk    = CAM_PCLK;

    // Clock and format
    config.xclk_freq_hz = 20000000;  // 20 MHz XCLK
    config.ledc_timer   = LEDC_TIMER_0;
    config.ledc_channel = LEDC_CHANNEL_0;
    config.pixel_format = mapPixelFormat(fmt);
    config.frame_size   = mapResolution(res);

    // Frame buffer configuration - use PSRAM
    config.fb_count     = 1;
    config.fb_location  = CAMERA_FB_IN_PSRAM;
    config.grab_mode    = CAMERA_GRAB_LATEST;

    // JPEG quality (lower = better, only relevant for JPEG format)
    config.jpeg_quality = 12;

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        Serial.printf("[CAM] init failed: 0x%x\n", err);
        return false;
    }

    resolutionDimensions(res, _width, _height);
    _initialized = true;
    return true;
#endif
}

void CameraHAL::deinit() {
#if HAS_CAMERA
    if (!_initialized) return;
    releaseFrame();
    esp_camera_deinit();
    _initialized = false;
    _width = 0;
    _height = 0;
#endif
}

CameraFrame* CameraHAL::capture() {
#if !HAS_CAMERA
    return nullptr;
#else
    if (!_initialized) return nullptr;

    // Return any previously held frame buffer first
    releaseFrame();

    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        Serial.println("[CAM] capture failed");
        return nullptr;
    }

    _fb = fb;  // stash for releaseFrame()

    _frame.data   = fb->buf;
    _frame.len    = fb->len;
    _frame.width  = fb->width;
    _frame.height = fb->height;

    // Update cached dimensions from actual frame
    _width  = fb->width;
    _height = fb->height;

    switch (fb->format) {
        case PIXFORMAT_RGB565:    _frame.format = CamPixelFormat::RGB565;    break;
        case PIXFORMAT_JPEG:      _frame.format = CamPixelFormat::JPEG;      break;
        case PIXFORMAT_GRAYSCALE: _frame.format = CamPixelFormat::GRAYSCALE; break;
        default:                  _frame.format = CamPixelFormat::RGB565;    break;
    }

    return &_frame;
#endif
}

void CameraHAL::releaseFrame() {
#if HAS_CAMERA
    if (!_initialized) return;
    if (_fb) {
        esp_camera_fb_return(static_cast<camera_fb_t*>(_fb));
        _fb = nullptr;
        _frame.data = nullptr;
        _frame.len  = 0;
    }
#endif
}

bool CameraHAL::setResolution(CamResolution res) {
#if !HAS_CAMERA
    return false;
#else
    if (!_initialized) return false;
    sensor_t *s = esp_camera_sensor_get();
    if (!s) return false;
    if (s->set_framesize(s, mapResolution(res)) != 0) return false;
    resolutionDimensions(res, _width, _height);
    return true;
#endif
}

bool CameraHAL::setPixelFormat(CamPixelFormat fmt) {
#if !HAS_CAMERA
    return false;
#else
    if (!_initialized) return false;
    sensor_t *s = esp_camera_sensor_get();
    if (!s) return false;
    return s->set_pixformat(s, mapPixelFormat(fmt)) == 0;
#endif
}

bool CameraHAL::setFlip(bool horizontal, bool vertical) {
#if !HAS_CAMERA
    return false;
#else
    if (!_initialized) return false;
    sensor_t *s = esp_camera_sensor_get();
    if (!s) return false;
    bool ok = true;
    if (s->set_hmirror(s, horizontal ? 1 : 0) != 0) ok = false;
    if (s->set_vflip(s, vertical ? 1 : 0) != 0) ok = false;
    return ok;
#endif
}

bool CameraHAL::setBrightness(int level) {
#if !HAS_CAMERA
    return false;
#else
    if (!_initialized) return false;
    if (level < -2 || level > 2) return false;
    sensor_t *s = esp_camera_sensor_get();
    if (!s) return false;
    return s->set_brightness(s, level) == 0;
#endif
}

bool CameraHAL::setContrast(int level) {
#if !HAS_CAMERA
    return false;
#else
    if (!_initialized) return false;
    if (level < -2 || level > 2) return false;
    sensor_t *s = esp_camera_sensor_get();
    if (!s) return false;
    return s->set_contrast(s, level) == 0;
#endif
}

bool CameraHAL::setSaturation(int level) {
#if !HAS_CAMERA
    return false;
#else
    if (!_initialized) return false;
    if (level < -2 || level > 2) return false;
    sensor_t *s = esp_camera_sensor_get();
    if (!s) return false;
    return s->set_saturation(s, level) == 0;
#endif
}

#endif // SIMULATOR
