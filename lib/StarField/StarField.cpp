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
    s.x = randf();
    s.y = randf();
    s.z = randomize_z ? randf_range(0.1f, 1.0f) : randf_range(0.7f, 1.0f);
    s.prev_z = s.z;

    float r = randf();
    if (r < 0.75f)      s.tint = TINT_WHITE;
    else if (r < 0.85f) s.tint = TINT_BLUE;
    else if (r < 0.95f) s.tint = TINT_YELLOW;
    else                 s.tint = TINT_RED;
}

void StarField::update(float speed, StarDirection dir) {
    float abs_speed = fabsf(speed);

    for (int i = 0; i < _num_stars; i++) {
        Star& s = _stars[i];
        s.prev_z = s.z;

        // Depth-based parallax: closer stars (small z) move faster
        float depth_speed = abs_speed / fmaxf(s.z, 0.1f);

        switch (dir) {
            case DIR_RIGHT:
                s.x += depth_speed * 0.15f;
                s.y += depth_speed * 0.02f;  // subtle drift
                break;
            case DIR_LEFT:
                s.x -= depth_speed * 0.15f;
                s.y -= depth_speed * 0.02f;
                break;
            case DIR_DOWN:
                s.y += depth_speed * 0.15f;
                s.x += depth_speed * 0.02f;
                break;
            case DIR_UP:
                s.y -= depth_speed * 0.15f;
                s.x -= depth_speed * 0.02f;
                break;
            case DIR_OUT: {
                // Radial outward from center
                float dx = (s.x - 0.5f);
                float dy = (s.y - 0.5f);
                float dist = sqrtf(dx * dx + dy * dy);
                if (dist > 0.001f) {
                    float radial = depth_speed * 0.12f / dist;
                    // Cap radial speed to prevent extreme center spawn
                    if (radial > depth_speed * 2.0f) radial = depth_speed * 2.0f;
                    s.x += dx * radial;
                    s.y += dy * radial;
                }
                break;
            }
            case DIR_IN: {
                // Radial inward toward center
                float dx = (s.x - 0.5f);
                float dy = (s.y - 0.5f);
                float dist = sqrtf(dx * dx + dy * dy);
                if (dist > 0.001f) {
                    float radial = depth_speed * 0.12f / dist;
                    if (radial > depth_speed * 2.0f) radial = depth_speed * 2.0f;
                    s.x -= dx * radial;
                    s.y -= dy * radial;
                }
                break;
            }
            default:
                s.x += depth_speed * 0.15f;
                break;
        }

        // Slowly cycle z for brightness variation
        s.z -= speed * 0.1f;

        // Wrap at screen edges (seamless scrolling)
        if (s.x > 1.05f) s.x -= 1.1f;
        if (s.x < -0.05f) s.x += 1.1f;
        if (s.y > 1.05f) s.y -= 1.1f;
        if (s.y < -0.05f) s.y += 1.1f;

        // Cycle z for depth variation
        if (s.z <= 0.05f) s.z = 1.0f;
        if (s.z > 1.2f)   s.z = 0.1f;
    }
}

bool StarField::project(const Star& s, int& sx, int& sy, float& brightness) const {
    sx = (int)(s.x * _w);
    sy = (int)(s.y * _h);

    if (sx < 0 || sx >= _w || sy < 0 || sy >= _h) return false;

    brightness = 1.0f - (s.z * 0.6f);
    brightness = fmaxf(0.3f, fminf(1.0f, brightness));
    return true;
}
