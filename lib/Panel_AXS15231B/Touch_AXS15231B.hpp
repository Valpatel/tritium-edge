/*
 * Touch_AXS15231B - LovyanGFX touch driver for AXS15231B integrated touch
 *
 * The AXS15231B has an integrated capacitive touch controller accessible
 * via I2C at address 0x3B. Touch data is 8 bytes starting at register 0x00.
 */
#pragma once

#include <lgfx/v1/Touch.hpp>

namespace lgfx
{
  inline namespace v1
  {

    struct Touch_AXS15231B : public ITouch
    {
      Touch_AXS15231B(void)
      {
        _cfg.i2c_addr = 0x3B;
        _cfg.x_min = 0;
        _cfg.x_max = 319;
        _cfg.y_min = 0;
        _cfg.y_max = 479;
        _cfg.freq  = 400000;
      }

      bool init(void) override;
      void wakeup(void) override {}
      void sleep(void) override {}
      uint_fast8_t getTouchRaw(touch_point_t* tp, uint_fast8_t count) override;
    };

  }
}
