/*
 * Touch_AXS15231B - LovyanGFX touch driver for AXS15231B integrated touch
 *
 * The AXS15231B requires a 4-byte command handshake (0xB5 0xAB 0xA5 0x5A)
 * before it will return touch data via I2C.
 *
 * No-touch patterns vary by board:
 *   - All 0x00 (3.49 board)
 *   - All 0x77 (3.5B-C board)
 *   - All 0xFF
 * We detect "all bytes same" as no-touch.
 */
#include "Touch_AXS15231B.hpp"
#include <lgfx/v1/platforms/common.hpp>

namespace lgfx
{
  inline namespace v1
  {

    static constexpr uint8_t AXS_TOUCH_CMD[] = { 0xB5, 0xAB, 0xA5, 0x5A };

    bool Touch_AXS15231B::init(void)
    {
      _inited = false;

      if (_cfg.pin_rst >= 0)
      {
        lgfx::pinMode(_cfg.pin_rst, pin_mode_t::output);
        lgfx::gpio_lo(_cfg.pin_rst);
        lgfx::delay(10);
        lgfx::gpio_hi(_cfg.pin_rst);
        lgfx::delay(50);
      }

      if (_cfg.pin_int >= 0)
      {
        lgfx::pinMode(_cfg.pin_int, pin_mode_t::input_pullup);
      }

      if (!lgfx::i2c::init(_cfg.i2c_port, _cfg.pin_sda, _cfg.pin_scl).has_value())
      {
        return false;
      }

      // Verify device presence
      uint8_t buf[8] = {0};
      _inited = lgfx::i2c::transactionWriteRead(
        _cfg.i2c_port, _cfg.i2c_addr,
        AXS_TOUCH_CMD, sizeof(AXS_TOUCH_CMD),
        buf, sizeof(buf), _cfg.freq
      ).has_value();

      return _inited;
    }

    uint_fast8_t Touch_AXS15231B::getTouchRaw(touch_point_t* tp, uint_fast8_t count)
    {
      if (!_inited || count == 0) return 0;

      uint8_t buf[8] = {0};

      if (!lgfx::i2c::transactionWriteRead(
            _cfg.i2c_port, _cfg.i2c_addr,
            AXS_TOUCH_CMD, sizeof(AXS_TOUCH_CMD),
            buf, sizeof(buf), _cfg.freq
          ).has_value())
      {
        return 0;
      }

      // No-touch: all bytes identical (0x00, 0x77, 0xFF, etc.)
      bool all_same = true;
      for (int i = 1; i < 6; i++) {
        if (buf[i] != buf[0]) { all_same = false; break; }
      }
      if (all_same) return 0;

      // Extract coordinates
      int16_t x = ((buf[2] & 0x0F) << 8) | buf[3];
      int16_t y = ((buf[4] & 0x0F) << 8) | buf[5];

      // Sanity: reject out-of-range coordinates
      if (x > 2048 || y > 2048) return 0;

      tp[0].x    = x;
      tp[0].y    = y;
      tp[0].size = 1;
      tp[0].id   = 0;

      return 1;
    }

  }
}
