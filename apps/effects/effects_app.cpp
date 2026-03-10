#include "effects_app.h"
#include "gfx_effects.h"

#ifdef SIMULATOR
#include "sim_hal.h"
#else
#include "tritium_compat.h"
#endif
#include "debug_log.h"

static ScannerEffect      fx_scanner;
static MatrixRainEffect   fx_matrix;
static ParticleEffect     fx_particles;
static FireEffect         fx_fire;
static TunnelEffect       fx_tunnel;
static MetaballsEffect    fx_metaballs;
static PlasmaEffect       fx_plasma;

static GfxEffect* effects[] = {
    &fx_scanner,
    &fx_matrix,
    &fx_particles,
    &fx_fire,
    &fx_tunnel,
    &fx_metaballs,
    &fx_plasma,
};
static constexpr int NUM_EFFECTS = sizeof(effects) / sizeof(effects[0]);

void EffectsApp::setup(LGFX& display) {
    int w = display.width();
    int h = display.height();

    _canvas = new LGFX_Sprite(&display);
    _canvas->setPsram(true);
    _canvas->setColorDepth(16);

    if (!_canvas->createSprite(w, h)) {
        DBG_ERROR("effects", "Sprite alloc failed for %dx%d", w, h);
        delete _canvas;
        _canvas = nullptr;
        return;
    }
    _canvas->fillScreen(TFT_BLACK);

    for (int i = 0; i < NUM_EFFECTS; i++) {
        effects[i]->init(w, h);
    }

    _current = 0;
    _effect_start = millis();
    _fps_timer = millis();
    _last_frame = millis();
    _frame_count = 0;
    _last_time = 0;

    DBG_INFO("effects", "Effects demo: %dx%d, %d effects, target %d FPS", w, h, NUM_EFFECTS, TARGET_FPS);
}

void EffectsApp::drawOverlay(const char* name, int w, int h) {
    if (!_canvas) return;

    // Effect name - bottom left
    _canvas->setTextSize(1);
    _canvas->setTextDatum(bottom_left);
    _canvas->setTextColor(0xFFFF);
    _canvas->drawString(name, 6, h - 6);

    // FPS - top right
    if (_fps > 0) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%.0f fps", _fps);
        _canvas->setTextDatum(top_right);
        _canvas->setTextColor(0x7BEF);
        _canvas->drawString(buf, w - 4, 4);
    }

    // Effect counter - top left
    char idx_buf[16];
    snprintf(idx_buf, sizeof(idx_buf), "%d/%d", _current + 1, NUM_EFFECTS);
    _canvas->setTextDatum(top_left);
    _canvas->setTextColor(0x7BEF);
    _canvas->drawString(idx_buf, 4, 4);
}

void EffectsApp::loop(LGFX& display) {
    if (!_canvas) return;

    uint32_t now = millis();

    // Frame rate limiting — simulate ESP32 speed
    uint32_t elapsed_frame = now - _last_frame;
    if (elapsed_frame < FRAME_MIN_MS) {
        delay(FRAME_MIN_MS - elapsed_frame);
        now = millis();
    }
    _last_frame = now;

    // Compute dt
    float dt = (now - _fps_timer > 0 && _frame_count > 0)
        ? (float)(now - (_fps_timer + (uint32_t)(_frame_count * 1000.0f / fmaxf(_fps, 1.0f)))) / 1000.0f
        : 0.033f;
    // Simpler: use wall clock
    float now_s = now / 1000.0f;
    dt = now_s - _last_time;
    if (dt > 0.1f) dt = 0.033f;  // clamp
    if (dt <= 0) dt = 0.033f;
    _last_time = now_s;

    int w = _canvas->width();
    int h = _canvas->height();

    // Cycle effects
    uint32_t effect_elapsed = now - _effect_start;
    if (effect_elapsed > EFFECT_DURATION_MS) {
        _current = (_current + 1) % NUM_EFFECTS;
        _effect_start = now;
        // Clear buffer for new effect
        memset((uint16_t*)_canvas->getBuffer(), 0, w * h * 2);
    }

    // Update and render
    GfxEffect* fx = effects[_current];
    fx->update(dt);
    fx->render((uint16_t*)_canvas->getBuffer(), w, h);

    drawOverlay(fx->name(), w, h);
    _canvas->pushSprite(0, 0);

    // FPS tracking
    _frame_count++;
    if (now - _fps_timer >= 1000) {
        _fps = _frame_count * 1000.0f / (now - _fps_timer);
        _frame_count = 0;
        _fps_timer = now;
    }
}
