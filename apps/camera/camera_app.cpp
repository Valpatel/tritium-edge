#include "camera_app.h"
#include "hal_camera.h"
#include "debug_log.h"

static CameraHAL cam;
static LGFX_Sprite* sprite = nullptr;
static uint32_t _frameCount = 0;
static uint32_t _lastFpsTime = 0;

void CameraApp::setup(LGFX& display) {
    display.fillScreen(TFT_BLACK);

    if (!cam.init(CamResolution::QVGA_320x240, CamPixelFormat::RGB565)) {
        DBG_INFO("camera", "Camera init FAILED");
        display.setTextColor(TFT_RED, TFT_BLACK);
        display.setTextSize(2);
        display.drawString("Camera init failed!", 10, display.height() / 2);
        return;
    }

    // Mirror fix — try no flip first, adjust if needed
    cam.setFlip(false, true);

    // Create full-screen sprite in PSRAM for scaled output
    sprite = new LGFX_Sprite(&display);
    sprite->setPsram(true);
    sprite->setColorDepth(16);
    if (!sprite->createSprite(display.width(), display.height())) {
        DBG_INFO("camera", "Sprite alloc failed, using direct push");
        delete sprite;
        sprite = nullptr;
    }

    DBG_INFO("camera", "Camera ready: %dx%d, sprite: %s",
             cam.getWidth(), cam.getHeight(), sprite ? "yes" : "no");
    _lastFpsTime = millis();
}

void CameraApp::loop(LGFX& display) {
    if (!cam.available()) return;

    CameraFrame* f = cam.capture();
    if (!f || !f->data) return;

    const uint16_t* src = (const uint16_t*)f->data;
    uint16_t fw = f->width;   // 320
    uint16_t fh = f->height;  // 240
    uint16_t dw = display.width();   // 320
    uint16_t dh = display.height();  // 480

    if (sprite) {
        // Scale camera frame to fill display: duplicate each row 2x
        uint16_t* dst = (uint16_t*)sprite->getBuffer();
        for (uint16_t row = 0; row < fh; row++) {
            const uint16_t* srcRow = &src[row * fw];
            uint16_t* dstRow1 = &dst[(row * 2) * dw];
            uint16_t* dstRow2 = &dst[(row * 2 + 1) * dw];
            memcpy(dstRow1, srcRow, fw * 2);
            memcpy(dstRow2, srcRow, fw * 2);
        }
        sprite->pushSprite(0, 0);
    } else {
        // Fallback: just push to top half
        display.pushImage(0, 0, fw, fh, src);
    }

    cam.releaseFrame();

    // FPS counter
    _frameCount++;
    uint32_t now = millis();
    if (now - _lastFpsTime >= 3000) {
        float fps = _frameCount * 1000.0f / (now - _lastFpsTime);
        DBG_INFO("camera", "FPS: %.1f", fps);
        _frameCount = 0;
        _lastFpsTime = now;
    }
}
