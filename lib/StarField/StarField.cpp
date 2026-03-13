#include "StarField.h"
#include <cstdlib>
#include <cmath>
#include <new>

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
    // Focal length controls field-of-view. Larger = narrower FOV, less fisheye.
    _focal = fminf((float)_w, (float)_h) * 0.5f;

    if (num_stars <= 0) {
        int area = _w * _h;
        if (area > 300000)      num_stars = 600;
        else if (area > 150000) num_stars = 400;
        else if (area > 80000)  num_stars = 300;
        else                    num_stars = 200;
    }
    _num_stars = num_stars;

    _stars = new (std::nothrow) Star[_num_stars];
    if (!_stars) { _num_stars = 0; return; }
    for (int i = 0; i < _num_stars; i++) {
        spawnStar(_stars[i], true);
    }
}

StarField::~StarField() {
    delete[] _stars;
}

void StarField::spawnStar(Star& s, bool full_depth) {
    // Spread stars across a wide 3D volume so perspective projection fills screen
    // x,y range: [-spread, +spread] where spread scales with aspect ratio
    float spread = 1.5f;
    s.x = randf_range(-spread, spread);
    s.y = randf_range(-spread, spread);
    s.z = full_depth ? randf_range(MIN_Z + 0.5f, MAX_Z) : randf_range(MAX_Z * 0.6f, MAX_Z);

    float r = randf();
    if (r < 0.75f)      s.tint = TINT_WHITE;
    else if (r < 0.85f) s.tint = TINT_BLUE;
    else if (r < 0.95f) s.tint = TINT_YELLOW;
    else                 s.tint = TINT_RED;
}

void StarField::spawnAtEdge(Star& s, StarDirection dir) {
    float spread = 1.5f;
    s.z = randf_range(0.5f, MAX_Z);

    float r = randf();
    if (r < 0.75f)      s.tint = TINT_WHITE;
    else if (r < 0.85f) s.tint = TINT_BLUE;
    else if (r < 0.95f) s.tint = TINT_YELLOW;
    else                 s.tint = TINT_RED;

    switch (dir) {
        case DIR_LEFT:
            s.x = spread;
            s.y = randf_range(-spread, spread);
            break;
        case DIR_RIGHT:
            s.x = -spread;
            s.y = randf_range(-spread, spread);
            break;
        case DIR_UP:
            s.y = spread;
            s.x = randf_range(-spread, spread);
            break;
        case DIR_DOWN:
            s.y = -spread;
            s.x = randf_range(-spread, spread);
            break;
        default:
            s.x = randf_range(-spread, spread);
            s.y = randf_range(-spread, spread);
            break;
    }
}

void StarField::update(float speed, StarDirection dir) {
    for (int i = 0; i < _num_stars; i++) {
        Star& s = _stars[i];

        switch (dir) {
            case DIR_FORWARD:
                // Stars approach viewer — z decreases
                s.z -= speed * 4.0f;
                if (s.z <= MIN_Z) {
                    spawnStar(s, false);
                }
                break;

            case DIR_REVERSE:
                // Stars recede — z increases
                s.z += speed * 4.0f;
                if (s.z >= MAX_Z) {
                    // Respawn close to viewer
                    spawnStar(s, false);
                    s.z = randf_range(MIN_Z + 0.1f, MIN_Z + 1.0f);
                }
                break;

            case DIR_LEFT:
            case DIR_RIGHT:
            case DIR_UP:
            case DIR_DOWN: {
                // Lateral movement with parallax (closer = faster)
                float parallax = speed * 3.0f / fmaxf(s.z, 0.3f);
                if (dir == DIR_RIGHT) s.x -= parallax;
                else if (dir == DIR_LEFT) s.x += parallax;
                else if (dir == DIR_DOWN) s.y -= parallax;
                else if (dir == DIR_UP) s.y += parallax;

                // Check if projected position is off-screen — if so, respawn at opposite edge
                int sx, sy;
                float br;
                if (!project(s, sx, sy, br)) {
                    spawnAtEdge(s, dir);
                }
                break;
            }

            default:
                s.z -= speed * 4.0f;
                if (s.z <= MIN_Z) spawnStar(s, false);
                break;
        }
    }
}

bool StarField::project(const Star& s, int& sx, int& sy, float& brightness) const {
    if (s.z <= MIN_Z) return false;

    // Perspective projection: screen = center + (pos * focal / z)
    float inv_z = _focal / s.z;
    sx = (int)(_cx + s.x * inv_z);
    sy = (int)(_cy + s.y * inv_z);

    if (sx < 0 || sx >= _w || sy < 0 || sy >= _h) return false;

    // Brightness: closer stars are brighter
    brightness = 1.0f - (s.z / MAX_Z);
    brightness = fmaxf(0.15f, fminf(1.0f, brightness));
    return true;
}
