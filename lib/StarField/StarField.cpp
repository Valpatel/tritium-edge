#include "StarField.h"
#include <cstdlib>
#include <cmath>

static float randf() {
    return (float)rand() / (float)RAND_MAX;
}

static float randf_range(float lo, float hi) {
    return lo + randf() * (hi - lo);
}

StarField::StarField(int screen_width, int screen_height, int num_stars)
    : _w(screen_width), _h(screen_height)
{
    _cx = _w * 0.5f;
    _cy = _h * 0.5f;

    if (num_stars <= 0) {
        int area = _w * _h;
        if (area > 300000)      num_stars = 600;
        else if (area > 150000) num_stars = 400;
        else if (area > 80000)  num_stars = 300;
        else                    num_stars = 200;
    }
    _num_stars = num_stars;

    _stars = new Star[_num_stars];
    for (int i = 0; i < _num_stars; i++) {
        resetStar(_stars[i], true);
    }
}

StarField::~StarField() {
    delete[] _stars;
}

void StarField::resetStar(Star& s, bool randomize_z) {
    // Screen-space: x in [0,1] → [0,_w], y in [0,1] → [0,_h]
    s.x = randf();
    s.y = randf();
    s.z = randomize_z ? randf_range(0.1f, 1.0f) : randf_range(0.7f, 1.0f);
    s.prev_z = s.z;

    // ~75% white, ~10% blue, ~10% yellow, ~5% red
    float r = randf();
    if (r < 0.75f)      s.tint = TINT_WHITE;
    else if (r < 0.85f) s.tint = TINT_BLUE;
    else if (r < 0.95f) s.tint = TINT_YELLOW;
    else                 s.tint = TINT_RED;
}

void StarField::update(float speed) {
    float abs_speed = fabsf(speed);
    float direction = (speed >= 0) ? 1.0f : -1.0f;

    for (int i = 0; i < _num_stars; i++) {
        Star& s = _stars[i];
        s.prev_z = s.z;

        // Parallax lateral drift: closer stars (smaller z) move faster.
        // All stars drift in the same direction — no center clustering.
        // Primary drift: horizontal. Secondary: slight vertical for variety.
        float depth_speed = abs_speed / fmaxf(s.z, 0.1f);
        s.x += direction * depth_speed * 0.15f;
        s.y += direction * depth_speed * 0.04f;  // gentle vertical component

        // Slowly cycle z to create twinkling/depth variation
        s.z -= speed * 0.1f;

        // Wrap horizontally (seamless scrolling)
        if (s.x > 1.05f) { s.x -= 1.1f; }
        if (s.x < -0.05f) { s.x += 1.1f; }

        // Wrap vertically
        if (s.y > 1.05f) { s.y -= 1.1f; }
        if (s.y < -0.05f) { s.y += 1.1f; }

        // Reset z if out of range (cycles brightness over time)
        if (s.z <= 0.05f) {
            s.z = 1.0f;
        }
        if (s.z > 1.2f) {
            s.z = 0.1f;
        }
    }
}

bool StarField::project(const Star& s, int& sx, int& sy, float& brightness) const {
    // Direct screen-space mapping
    sx = (int)(s.x * _w);
    sy = (int)(s.y * _h);

    if (sx < 0 || sx >= _w || sy < 0 || sy >= _h) return false;

    // Brightness: closer stars (small z) are brighter
    // z ranges 0.1 (close, bright) to 1.0 (far, dimmer)
    brightness = 1.0f - (s.z * 0.6f);  // z=0.1 → 0.94, z=1.0 → 0.40
    brightness = fmaxf(0.3f, fminf(1.0f, brightness));
    return true;
}
