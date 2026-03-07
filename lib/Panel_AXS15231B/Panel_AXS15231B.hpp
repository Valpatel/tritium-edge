/*
 * Panel_AXS15231B - LovyanGFX panel driver for AXS15231B QSPI LCD
 *
 * Uses the same QSPI command protocol as AMOLED panels (0x02 for commands,
 * 0x32 for pixel data), so we derive from Panel_AMOLED.
 *
 * Used by Waveshare ESP32-S3-Touch-LCD-3.5B-C (320x480) and
 * ESP32-S3-Touch-LCD-3.49 (172x640).
 */
#pragma once

#if defined(ESP_PLATFORM)

#include <lgfx/v1/panel/Panel_AMOLED.hpp>
#include <lgfx/v1/panel/Panel_FrameBufferBase.hpp>
#include <lgfx/v1/platforms/common.hpp>
#include <lgfx/v1/platforms/device.hpp>

#if defined LGFX_USE_QSPI

namespace lgfx
{
  inline namespace v1
  {

    struct Panel_AXS15231B : public Panel_AMOLED
    {
      Panel_AXS15231B(void)
      {
        _cfg.memory_width  = 360;
        _cfg.memory_height = 640;
        _cfg.panel_width   = 320;
        _cfg.panel_height  = 480;
        _write_depth = color_depth_t::rgb565_2Byte;
        _read_depth = color_depth_t::rgb565_2Byte;
      }

      void setInitMode(uint8_t mode) { _init_mode = mode; }
      static constexpr uint8_t INIT_320x480 = 0;
      static constexpr uint8_t INIT_172x640 = 1;

      const uint8_t* getInitCommands(uint8_t listno) const override
      {
        if (listno == 0) {
          return (_init_mode == INIT_172x640) ? _init_172x640 : _init_320x480;
        }
        return nullptr;
      }

    private:

      uint8_t _init_mode = INIT_320x480;

      // Init sequence for 320x480 panel (3.5B-C board)
      static constexpr uint8_t _init_320x480[] = {
        0x11, 0 + CMD_INIT_DELAY, 200,  // Sleep out
        0x29, 0 + CMD_INIT_DELAY, 100,  // Display on
        0xFF, 0xFF, // end
      };

      // Init sequence for 172x640 panel (3.49 board)
      static constexpr uint8_t _init_172x640[] = {
        0x11, 0 + CMD_INIT_DELAY, 200,  // Sleep out
        0x29, 0 + CMD_INIT_DELAY, 100,  // Display on
        0xFF, 0xFF, // end
      };
    };

  }
}

#endif
#endif
