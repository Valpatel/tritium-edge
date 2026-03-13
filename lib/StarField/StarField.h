#pragma once
#include <cstdint>

enum StarTint : uint8_t {
    TINT_WHITE = 0,
    TINT_BLUE,
    TINT_YELLOW,
    TINT_RED,
    TINT_COUNT
};

enum StarDirection : uint8_t {
    DIR_OUT   = 0,  // radial outward from center
    DIR_IN    = 1,  // radial inward toward center
    DIR_LEFT  = 2,
    DIR_RIGHT = 3,
    DIR_UP    = 4,
    DIR_DOWN  = 5,
    DIR_COUNT
};

struct Star {
    float x;        // screen-space position [0, 1]
    float y;        // screen-space position [0, 1]
    float z;        // depth [0.1 = close, 1.0 = far]
    float prev_z;   // previous z for trail effect
    StarTint tint;   // color variation
};

class StarField {
public:
    StarField(int screen_width, int screen_height, int num_stars = 0);
    ~StarField();

    // Advance simulation one frame with given speed and direction.
    void update(float speed = 0.015f, StarDirection dir = DIR_RIGHT);

    // Get star data for rendering
    int getStarCount() const { return _num_stars; }
    const Star* getStars() const { return _stars; }

    // Project a star to screen coordinates. Returns false if off-screen.
    bool project(const Star& s, int& sx, int& sy, float& brightness) const;

    int width() const { return _w; }
    int height() const { return _h; }

private:
    void resetStar(Star& s, bool randomize_z = false);
    Star* _stars;
    int _num_stars;
    int _w, _h;
    float _cx, _cy; // screen center
};
