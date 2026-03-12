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

    // Scale star count to screen area if not specified
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
    s.x = randf_range(-1.0f, 1.0f);
    s.y = randf_range(-1.0f, 1.0f);
    s.z = randomize_z ? randf_range(0.01f, 1.0f) : 1.0f;
    s.prev_z = s.z;

    // ~75% white, ~10% blue, ~10% yellow, ~5% red
    float r = randf();
    if (r < 0.75f)      s.tint = TINT_WHITE;
    else if (r < 0.85f) s.tint = TINT_BLUE;
    else if (r < 0.95f) s.tint = TINT_YELLOW;
    else                 s.tint = TINT_RED;
}

void StarField::update(float speed) {
    bool reverse = speed < 0.0f;
    for (int i = 0; i < _num_stars; i++) {
        Star& s = _stars[i];
        s.prev_z = s.z;
        s.z -= speed;

        // Reset if past the camera (forward) or receded past max depth (reverse)
        if (s.z <= 0.001f) {
            resetStar(s, false);
            if (reverse) s.z = 0.01f;  // spawn close when going backward
            continue;
        }
        if (s.z > 1.5f) {
            resetStar(s, false);
            if (!reverse) s.z = 1.0f;  // spawn far when going forward
            continue;
        }

        // Check if projected position is off screen
        float inv_z = 1.0f / s.z;
        float sx = _cx + s.x * _cx * inv_z;
        float sy = _cy + s.y * _cy * inv_z;
        if (sx < -2 || sx >= _w + 2 || sy < -2 || sy >= _h + 2) {
            resetStar(s, false);
            if (reverse) s.z = 0.01f;
        }
    }
}

bool StarField::project(const Star& s, int& sx, int& sy, float& brightness) const {
    if (s.z <= 0.001f) return false;

    float inv_z = 1.0f / s.z;
    float fx = _cx + s.x * _cx * inv_z;
    float fy = _cy + s.y * _cy * inv_z;

    sx = (int)fx;
    sy = (int)fy;

    if (sx < 0 || sx >= _w || sy < 0 || sy >= _h) return false;

    // Brightness: closer stars are brighter (inv_z ranges roughly 1..100)
    brightness = fminf(1.0f, inv_z * 0.15f);
    return true;
}
