/* AbleDOOM - Doom on Ableton Push 3 Standalone!
 *
 * Copyright (C) 2024 Nikolai Wuttke-Hohendorf
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#pragma once

#include "RtMidi.h"

#include <libusb-1.0/libusb.h>

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <variant>


constexpr auto PUSH_SCREEN_WIDTH = 960;
constexpr auto PUSH_SCREEN_HEIGHT = 160;
constexpr auto PUSH_SCREEN_STRIDE = 1024;


// For queuing up Doom input events (fake keypresses)
struct DoomInputEvent
{
  uint8_t key;
  bool pressed;
};


// Identifies a pad on the Push hardware
struct PadId
{
  uint8_t x, y;

  friend bool operator==(const PadId& lhs, const PadId& rhs)
  {
    return std::make_tuple(lhs.x, lhs.y) == std::make_tuple(rhs.x, rhs.y);
  }
};


// Identifies a button on the Push hardware
using ButtonId = uint8_t;

// Identifies a control (button or pad) on the Push hardware
using ControlId = std::variant<PadId, ButtonId>;


// Parsed Push MIDI input event
struct PushInputEvent
{
  ControlId id;
  bool pressed;
};


// Facade for Push hardware interactions (display and buttons/pads/LEDs)
class PushHardware
{
public:
  struct DisplayData
  {
    libusb_device_handle* mpUsbDeviceHandle = nullptr;
    bool mTransferFailed = false;
    int mDisplayError = 0;

    libusb_transfer* mpHeaderTransfer = nullptr;
    libusb_transfer* mpDataTransfer = nullptr;

    std::vector<uint8_t> mUsbTransferBuffer;
    bool mTransferInProgress = false;
  };

  // Callback that will be invoked for every incoming Push event
  // (button or pad press/release)
  using InputCallback = std::function<void(PushInputEvent)>;

  explicit PushHardware(InputCallback inputCallback);
  ~PushHardware();

  PushHardware(const PushHardware&) = delete;
  PushHardware& operator=(const PushHardware&) = delete;

  // Set LED lights for buttons/pads on Push. The meaning of `value` depends on the
  // specific control.
  // See
  // https://github.com/Ableton/push-interface/blob/main/doc/AbletonPush2MIDIDisplayInterface.asc#LEDs
  void setButtonLight(int number, int value);
  void setPadLight(int x, int y, int value);
  void setLight(const PadId& pad, int value);
  void setLight(ButtonId button, int value);

  // Turn off all LEDs
  void resetLEDs();

  // Copy a rectangular portion of the specified source buffer to the specified position
  // on the Push display. This function only copies into a buffer, call submitScreen()
  // to actually send the image to the Push display.
  void copyToScreen(
    const uint32_t* srcBuffer,
    int srcX,
    int srcY,
    int srcWidth,
    int srcHeight,
    int destX,
    int destY);

  // Copy raw data to Push screen. `data` must be a pointer to
  // PUSH_SCREEN_STRIDE * PUSH_SCREEN_HEIGHT uint16_t values holding pixel data in
  // the format expected by Push (BGR 5-6-5)
  void copyToScreen(const uint16_t* data);

  // Submit current frame to Push display (returns immediately, the transmission
  // happens asynchronously)
  void submitScreen();

private:
  static void onMessage(double, std::vector<unsigned char>* pMessage, void* pSelf);

  void initDisplay();

  // MIDI I/O
  InputCallback mInputCallback;
  std::unique_ptr<RtMidiIn> mpMidiIn;
  std::unique_ptr<RtMidiOut> mpMidiOut;

  // Buffer for sending MIDI messages to Push. To avoid frequent allocations, keep
  // using the same buffer
  std::vector<unsigned char> mMessageBuffer;

  // Current frame buffer (both copyToScreen() overloads write into this)
  std::vector<uint16_t> mScreenBuffer;

  // Display I/O
  DisplayData mDisplayData;
};


// A B L E D O O M !!!
// \m/ \m/
class AbleDoom
{
public:
  AbleDoom();

  // Fetch pending input event, if any
  std::optional<DoomInputEvent> fetchEvent();

  // Copy Doom frame buffer to Push screen (and update health/armor/ammo display on Push's
  // LEDs)
  void drawFrame(const uint32_t* pFrameBuffer);

private:
  void onInput(const PushInputEvent& event);
  void updateHealthArmorDisplay();

  std::deque<DoomInputEvent> mEventQueue;
  PushHardware mHardware;
  int mLastHealthButtonCount;
  int mLastArmorButtonCount;
  int mLastAmmoButtonCount;
  bool mShiftHeld = false;
};
