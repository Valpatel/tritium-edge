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
    DIR_FORWARD = 0,  // classic warp — stars fly toward viewer
    DIR_REVERSE = 1,  // stars recede into distance
    DIR_LEFT    = 2,
    DIR_RIGHT   = 3,
    DIR_UP      = 4,
    DIR_DOWN    = 5,
    DIR_COUNT
};

struct Star {
    float x;        // 3D position — spread across [-1, 1]
    float y;        // 3D position — spread across [-1, 1]
    float z;        // depth [0.01 = closest, MAX_Z = farthest]
    StarTint tint;
};

class StarField {
public:
    StarField(int screen_width, int screen_height, int num_stars = 0);
    ~StarField();

    void update(float speed = 0.015f, StarDirection dir = DIR_FORWARD);

    int getStarCount() const { return _num_stars; }
    const Star* getStars() const { return _stars; }

    // Project star to screen coords via perspective. Returns false if off-screen.
    bool project(const Star& s, int& sx, int& sy, float& brightness) const;

    int width() const { return _w; }
    int height() const { return _h; }

    static constexpr float MAX_Z = 8.0f;
    static constexpr float MIN_Z = 0.01f;

private:
    void spawnStar(Star& s, bool full_depth);
    void spawnAtEdge(Star& s, StarDirection dir);
    Star* _stars;
    int _num_stars;
    int _w, _h;
    float _cx, _cy;
    float _focal;  // perspective focal length
};
