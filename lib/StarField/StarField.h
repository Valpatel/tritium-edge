#pragma once
#include <cstdint>

enum StarTint : uint8_t {
    TINT_WHITE = 0,
    TINT_BLUE,
    TINT_YELLOW,
    TINT_RED,
    TINT_COUNT
};

struct Star {
    float x;        // normalized position [-1, 1]
    float y;        // normalized position [-1, 1]
    float z;        // depth [near 0 = close, 1 = far]
    float prev_z;   // previous z for trail effect
    StarTint tint;   // color variation
};

class StarField {
public:
    StarField(int screen_width, int screen_height, int num_stars = 0);
    ~StarField();

    // Advance simulation one frame. Call before render().
    void update(float speed = 0.015f);

    // Get star data for rendering
    int getStarCount() const { return _num_stars; }
    const Star* getStars() const { return _stars; }

    // Project a star to screen coordinates. Returns false if behind camera.
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
