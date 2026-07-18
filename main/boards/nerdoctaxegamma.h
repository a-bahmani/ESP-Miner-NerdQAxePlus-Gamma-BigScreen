#pragma once

#include "asic.h"
#include "bm1370.h"
#include "board.h"
#include "nerdqaxeplus2.h"
#include "./drivers/tmp451_mux.h"

// Voltage regulator detection pin (available from hardware rev 3.1+)
// - HIGH (pull-up to 3.3V): TPS53667 regulator
// - LOW (connected to GND): TPS53647 regulator
// - If not connected (older revisions): defaults to TPS53647 (via internal pull-down)
#define VR_DETECT_PIN GPIO_NUM_3

class NerdOctaxeGamma : public NerdQaxePlus2 {
  protected:
    // TMP451 mux chips – only present on rev 3.4
    // [0]: ASICs 0–3 (I2C addr 0x4c)
    // [1]: ASICs 4–7 (I2C addr 0x4e)
    // Both chips share the same MUX select lines: GPIO2=A0, GPIO12=A1.
    Tmp451Mux m_tmp451[2];
    bool      m_hasTMux[2] = {false, false};

  public:
    NerdOctaxeGamma();
    virtual bool initBoard() override;
    virtual void requestChipTemps() override;
    float getVRTemp() override;

    // Display is physically mounted rotated 180° — invert flip-screen toggle relative to other boards
    virtual bool isFlipScreenEnabled() override;

    // NerdOCTAXE-Gamma display: 3.5" 480×320 panel, no GRAM centering gap needed
    virtual int getLCDWidth()             override { return 480;             }
    virtual int getLCDHeight()            override { return 320;             }
    virtual int getLCDYGap()              override { return 0;               }
    virtual uint32_t getLCDPixelClockHz() override { return 480 * 320 * 80;  } // ~12.3 MHz
    // 1.5× zoom (384/256) scales the 320×170 UI to 480×255, centered in 480×320
    virtual uint16_t getLCDScaleZoom()    override { return 384;             }

  private:
    bool m_isTPS53667 = false;
};
