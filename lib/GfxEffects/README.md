# GfxEffects

Resolution-independent graphics effects library for demoscene-style visuals. All effects render to a raw RGB565 buffer, making them compatible with both LovyanGFX sprites and the desktop simulator.

## Effects

| Effect | Class | Description |
|--------|-------|-------------|
| Plasma | `PlasmaEffect` | Classic sinusoidal color blending with rainbow palette |
| Fire | `FireEffect` | Doom-style bottom-up heat propagation |
| Matrix Rain | `MatrixRainEffect` | Digital rain with variable-speed columns and glow |
| Particles | `ParticleEffect` | Firework bursts with gravity and color fading |
| Tunnel | `TunnelEffect` | Infinite zoom tunnel with procedural texture |
| Metaballs | `MetaballsEffect` | Organic blobby shapes with HSV coloring |

## Usage

```cpp
#include "gfx_effects.h"

PlasmaEffect plasma;
plasma.init(320, 480);

// In your render loop:
plasma.update(dt);  // dt in seconds
plasma.render((uint16_t*)sprite->getBuffer(), 320, 480);
sprite->pushSprite(0, 0);
```

## Utilities

- `gfx::rgb565(r, g, b)` — Pack RGB888 to RGB565
- `gfx::hsv565(h, s, v)` — HSV to RGB565 (h: 0-360)
- `gfx::blend565(a, b, t)` — Blend two RGB565 colors
- `gfx::rgb565_unpack(c, r, g, b)` — Unpack RGB565

## Adding a New Effect

1. Create a class inheriting from `GfxEffect`
2. Implement `name()`, `init(w, h)`, `update(dt)`, `render(buf, w, h)`
3. Add it to the effects array in `apps/effects/effects_app.cpp`
