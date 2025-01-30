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

#include "abledoom.hpp"

#include "doomgeneric.h"
#include "doomkeys.h"
#include "doomstat.h"
#include "doomtype.h"

#include <algorithm>
#include <cmath>
#include <regex>
#include <string>
#include <tuple>


//////////////////////////////////////////////////////////////////////////////////////////
//
// General utilities
//
//////////////////////////////////////////////////////////////////////////////////////////

namespace
{

// Helper class for RAII cleanup
class ScopeGuard
{
public:
  template <typename Callback>
  explicit ScopeGuard(Callback&& callback)
    : mCallback(std::forward<Callback>(callback))
  {
  }

  ~ScopeGuard() { mCallback(); }

  ScopeGuard(ScopeGuard&& other) noexcept
    : mCallback(std::exchange(other.mCallback, []() {}))
  {
  }

  ScopeGuard& operator=(ScopeGuard&&) = delete;
  ScopeGuard(const ScopeGuard&) = delete;
  ScopeGuard& operator=(const ScopeGuard&) = delete;

private:
  std::function<void()> mCallback;
};


template <typename Callback>
[[nodiscard]] auto defer(Callback&& callback)
{
  return ScopeGuard{std::forward<Callback>(callback)};
}

} // namespace


//////////////////////////////////////////////////////////////////////////////////////////
//
// Push hardware abstraction (pads/buttons, LEDs, display)
//
//////////////////////////////////////////////////////////////////////////////////////////

namespace
{

constexpr auto PUSH_SCREEN_SIZE_BYTES =
  PUSH_SCREEN_HEIGHT * PUSH_SCREEN_STRIDE * sizeof(uint16_t);

// This should be const, but libusb wants a non-const pointer
uint8_t DISPLAY_FRAME_HEADER[] = {
  0xff,
  0xcc,
  0xaa,
  0x88,
  0x00,
  0x00,
  0x00,
  0x00,
  0x00,
  0x00,
  0x00,
  0x00,
  0x00,
  0x00,
  0x00,
  0x00};


// Pad Y coordinate (with 0 = topmost row) to MIDI note number
constexpr int Y_TO_PAD_ROW_START[] = {92, 84, 76, 68, 60, 52, 44, 36};


bool isPad(uint8_t noteNumber)
{
  return noteNumber >= 36 && noteNumber <= 99;
}


std::tuple<uint8_t, uint8_t> noteNumberToPadCoordinate(uint8_t noteNumber)
{
  const auto iRowStart = std::find_if(
    std::begin(Y_TO_PAD_ROW_START),
    std::end(Y_TO_PAD_ROW_START),
    [&noteNumber](int value) { return noteNumber >= value; });

  if (iRowStart != std::end(Y_TO_PAD_ROW_START))
  {
    return std::make_tuple(
      uint8_t(noteNumber - *iRowStart),
      uint8_t(std::distance(std::begin(Y_TO_PAD_ROW_START), iRowStart)));
  }

  return std::make_tuple(0, 0);
}


bool isPushPort(const std::string& portName)
{
  static const auto kRegex = std::regex{".*Ableton Push 3 (Live Port|[0-9][0-9]:0).*"};
  return std::regex_match(portName, kRegex);
}


void initializeMidiIo(RtMidiIn& midiIn, RtMidiOut& midiOut)
{
  // Open Push MIDI input port for receiving button/pad presses
  std::optional<unsigned int> portNumber;

  for (auto i = 0u; i < midiIn.getPortCount(); ++i)
  {
    const auto& portName = midiIn.getPortName(i);
    if (isPushPort(portName))
    {
      portNumber = i;
      break;
    }
  }

  if (portNumber)
  {
    midiIn.openPort(*portNumber, "");
  }
  else
  {
    throw std::runtime_error("Couldn't open MIDI in port");
  }


  // Open Push MIDI output port for setting LED lights
  portNumber.reset();

  for (auto i = 0u; i < midiOut.getPortCount(); ++i)
  {
    const auto& portName = midiOut.getPortName(i);
    if (isPushPort(portName))
    {
      portNumber = i;
      break;
    }
  }

  if (portNumber)
  {
    midiOut.openPort(*portNumber, "");
  }
  else
  {
    throw std::runtime_error("Couldn't open MIDI out port");
  }
}


void throwLibUsbError(int errorCode)
{
  using namespace std::string_literals;

  throw std::runtime_error("libusb error: "s + libusb_error_name(errorCode));
}


libusb_device_handle* openPushDisplayUsbDevice()
{
  libusb_device_handle* pDeviceHandle = nullptr;

  // List available devices
  libusb_device** devices = nullptr;
  const auto count = libusb_get_device_list(nullptr, &devices);
  if (count < 0)
  {
    throwLibUsbError(int(count));
  }
  const auto deviceListGuard = defer([&]() { libusb_free_device_list(devices, 1); });


  // Find Push 3 display device
  libusb_device* pPushDisplay = nullptr;

  for (auto i = 0; devices[i] != nullptr; ++i)
  {
    libusb_device_descriptor descriptor;
    if (const auto result = libusb_get_device_descriptor(devices[i], &descriptor);
        result < 0)
    {
      throwLibUsbError(result);
    }

    if (
      descriptor.idVendor == 0x2982 && descriptor.idProduct == 0x1969
      && descriptor.bDeviceClass == LIBUSB_CLASS_MISCELLANEOUS)
    {
      pPushDisplay = devices[i];
      break;
    }
  }

  // Error out if no device found
  if (!pPushDisplay)
  {
    throw std::runtime_error("Push 3 display device not found!");
  }


  // Open the device
  if (const auto result = libusb_open(pPushDisplay, &pDeviceHandle); result < 0)
  {
    throwLibUsbError(result);
  }

  if (const auto result = libusb_claim_interface(pDeviceHandle, 0); result < 0)
  {
    libusb_close(pDeviceHandle);
    throwLibUsbError(result);
  }


  // Success!
  return pDeviceHandle;
}


// See
// https://github.com/Ableton/push-interface/blob/main/doc/AbletonPush2MIDIDisplayInterface.asc#xoring-pixel-data
void applySignalShapingPattern(uint8_t* pBuffer)
{
  auto pBufferDataAsUint32 = reinterpret_cast<uint32_t*>(pBuffer);
  for (auto i = 0u; i < PUSH_SCREEN_SIZE_BYTES / sizeof(uint32_t); ++i)
  {
    pBufferDataAsUint32[i] ^= 0xffe7f3e7;
  }
}


// Kick-off libusb transfers for a single frame of Push display data
void submitDisplayFrameTransfer(PushHardware::DisplayData* pData)
{
  if (const auto result = libusb_submit_transfer(pData->mpHeaderTransfer); result < 0)
  {
    pData->mDisplayError = result;
    return;
  }

  if (const auto result = libusb_submit_transfer(pData->mpDataTransfer); result < 0)
  {
    pData->mDisplayError = result;
    return;
  }

  // Make sure we don't try to send another frame while this one is still being sent
  pData->mTransferInProgress = true;
}

// Pack color into the 16-bit format expected by the Push display
uint16_t toBGR565(uint32_t color)
{
  const auto r = (color & 0x00FF0000) >> 16;
  const auto g = (color & 0x0000FF00) >> 8;
  const auto b = (color & 0x000000FF);

  return ((b & 0xF8) << 8) | ((g & 0xFC) << 3) | (r >> 3);
}

} // namespace


static void LIBUSB_CALL onTransferFinished(libusb_transfer* transfer)
{
  if (!transfer || !transfer->user_data)
  {
    return;
  }

  auto pData = static_cast<PushHardware::DisplayData*>(transfer->user_data);

  if (
    transfer->status != LIBUSB_TRANSFER_COMPLETED
    || transfer->length != transfer->actual_length)
  {
    // We could do more sophisticated error handling/recovery here, but for now, just
    // bail out if a transfer fails or can only be partially sent
    pData->mTransferFailed = true;
    return;
  }

  if (transfer == pData->mpDataTransfer)
  {
    pData->mTransferInProgress = false;
  }
}


PushHardware::PushHardware(InputCallback inputCallback)
  : mInputCallback(std::move(inputCallback))
  , mpMidiIn(std::make_unique<RtMidiIn>())
  , mpMidiOut(std::make_unique<RtMidiOut>())
{
  mMessageBuffer.resize(3);

  mpMidiIn->setCallback(onMessage, this);

  initializeMidiIo(*mpMidiIn, *mpMidiOut);

  resetLEDs();

  initDisplay();
}


PushHardware::~PushHardware()
{
  // Wait for any current transfers to complete
  struct timeval tv;
  tv.tv_sec = 30;
  tv.tv_usec = 0;
  libusb_handle_events_timeout(nullptr, &tv);

  libusb_free_transfer(mDisplayData.mpDataTransfer);
  libusb_free_transfer(mDisplayData.mpHeaderTransfer);

  libusb_release_interface(mDisplayData.mpUsbDeviceHandle, 0);
  libusb_close(mDisplayData.mpUsbDeviceHandle);
}


void PushHardware::setButtonLight(int number, int value)
{
  // CC
  mMessageBuffer[0] = 0xB0;
  mMessageBuffer[1] = uint8_t(number);
  mMessageBuffer[2] = uint8_t(value);

  mpMidiOut->sendMessage(&mMessageBuffer);
}


void PushHardware::setPadLight(int x, int y, int value)
{
  const auto noteNumber = Y_TO_PAD_ROW_START[y] + x;

  if (value == 0)
  {
    // Note off
    mMessageBuffer[0] = 0x80;
    mMessageBuffer[1] = uint8_t(noteNumber);
    mMessageBuffer[2] = 0;
  }
  else
  {
    // Note on
    mMessageBuffer[0] = 0x90;
    mMessageBuffer[1] = uint8_t(noteNumber);
    mMessageBuffer[2] = uint8_t(value);
  }

  mpMidiOut->sendMessage(&mMessageBuffer);
}


void PushHardware::setLight(const PadId& pad, int value)
{
  setPadLight(pad.x, pad.y, value);
}


void PushHardware::setLight(ButtonId button, int value)
{
  setButtonLight(button, value);
}


void PushHardware::resetLEDs()
{
  constexpr auto kAllButtons = []() {
    std::array<uint8_t, 102> result{3, 9};

    auto index = 2u;
    for (uint8_t value = 20; value <= 119; ++value, ++index)
    {
      switch (value)
      {
      // Skip these numbers (unused, AFAICT)
      case 52:
      case 53:
      case 66:
      case 67:
      case 68:
      case 97:
      case 98:
      case 99:
      case 100:
      case 101:
        break;

      default:
        result[index] = value;
        break;
      }
    }
    return result;
  }();

  for (const auto button : kAllButtons)
  {
    setButtonLight(button, 0);
  }

  for (auto y = 0; y < 8; ++y)
  {
    for (auto x = 0; x < 8; ++x)
    {
      setPadLight(x, y, 0);
    }
  }
}


void PushHardware::copyToScreen(
  const uint32_t* srcBuffer,
  int srcX,
  int srcY,
  int srcWidth,
  int srcHeight,
  int destX,
  int destY)
{
  // Clamp destination rectangle to screen size
  if (destX + srcWidth >= PUSH_SCREEN_WIDTH)
  {
    srcWidth = PUSH_SCREEN_WIDTH - destX;
  }

  if (destY + srcHeight >= PUSH_SCREEN_HEIGHT)
  {
    srcHeight = PUSH_SCREEN_HEIGHT - destY;
  }

  // Copy the specified portion of the framebuffer, converting to Push pixel format as
  // we go.
  for (auto y = 0; y < srcHeight; ++y)
  {
    for (auto x = 0; x < srcWidth; ++x)
    {
      mScreenBuffer[destX + x + (y + destY) * PUSH_SCREEN_STRIDE] =
        toBGR565(srcBuffer[x + srcX + (y + srcY) * DOOMGENERIC_RESX]);
    }
  }
}


void PushHardware::copyToScreen(const uint16_t* data)
{
  // Copy raw data directly (must have the correct size)
  std::memcpy(mScreenBuffer.data(), data, mScreenBuffer.size() * sizeof(uint16_t));
}


void PushHardware::submitScreen()
{
  if (!mDisplayData.mTransferFailed && mDisplayData.mDisplayError >= 0)
  {
    // Service any pending libusb events, non-blocking.
    // This may invoke onTransferFinished().
    struct timeval tv;
    memset(&tv, 0, sizeof(tv));

    const auto result = libusb_handle_events_timeout(nullptr, &tv);
    if (result < 0)
    {
      throwLibUsbError(result);
    }
  }

  if (mDisplayData.mDisplayError < 0)
  {
    throwLibUsbError(mDisplayData.mDisplayError);
  }

  if (mDisplayData.mTransferFailed)
  {
    throw std::runtime_error("Display USB transfer failed");
  }

  // If the last transfer is still in progress, we cannot submit the current frame -
  // we could wait but we simply drop it. This shouldn't really happen too often
  // in practice, since Doom only updates at 35 Hz whereas the Push display refresh rate
  // is 60 Hz.
  if (mDisplayData.mTransferInProgress)
  {
    return;
  }

  // Copy frame buffer into USB transfer buffer
  std::memcpy(
    mDisplayData.mUsbTransferBuffer.data(), mScreenBuffer.data(), PUSH_SCREEN_SIZE_BYTES);

  // Apply XOR pattern
  applySignalShapingPattern(mDisplayData.mUsbTransferBuffer.data());

  // Kick-off USB transfers
  submitDisplayFrameTransfer(&mDisplayData);
}


void PushHardware::onMessage(double, std::vector<unsigned char>* pMessage, void* pSelf)
{
  // Ignore any messages that aren't note on/off or CC
  if (pMessage->size() != 3)
    return;

  const auto type = (*pMessage)[0] & 0b11110000;
  const auto number = (*pMessage)[1];
  const auto value = (*pMessage)[2];

  const auto oEvent = [type, number, value]() -> std::optional<PushInputEvent> {
    switch (type)
    {
    case 0x90: // Note On
      if (isPad(number))
      {
        const auto [x, y] = noteNumberToPadCoordinate(number);
        return PushInputEvent{PadId{x, y}, true};
      }
      break;

    case 0x80: // Note Off
      if (isPad(number))
      {
        const auto [x, y] = noteNumberToPadCoordinate(number);
        return PushInputEvent{PadId{x, y}, false};
      }
      break;

    case 0xB0: // Control Change
      return PushInputEvent{ButtonId{number}, value == 127};

    default:
      break;
    }

    return {};
  }();

  if (oEvent)
  {
    static_cast<PushHardware*>(pSelf)->mInputCallback(*oEvent);
  }
}


void PushHardware::initDisplay()
{
  // Allocate buffers
  mScreenBuffer.resize(PUSH_SCREEN_HEIGHT * PUSH_SCREEN_STRIDE);
  mDisplayData.mUsbTransferBuffer.resize(PUSH_SCREEN_SIZE_BYTES);

  // Open the Push display USB device
  mDisplayData.mpUsbDeviceHandle = openPushDisplayUsbDevice();

  // Allocate and set up USB transfers
  mDisplayData.mpHeaderTransfer = libusb_alloc_transfer(0);
  if (!mDisplayData.mpHeaderTransfer)
  {
    throw std::bad_alloc();
  }

  mDisplayData.mpDataTransfer = libusb_alloc_transfer(0);
  if (!mDisplayData.mpDataTransfer)
  {
    libusb_free_transfer(mDisplayData.mpHeaderTransfer);
    throw std::bad_alloc();
  }

  libusb_fill_bulk_transfer(
    mDisplayData.mpHeaderTransfer,
    mDisplayData.mpUsbDeviceHandle,
    0x1,
    DISPLAY_FRAME_HEADER,
    std::size(DISPLAY_FRAME_HEADER),
    onTransferFinished,
    &mDisplayData,
    1000);

  libusb_fill_bulk_transfer(
    mDisplayData.mpDataTransfer,
    mDisplayData.mpUsbDeviceHandle,
    0x1,
    mDisplayData.mUsbTransferBuffer.data(),
    mDisplayData.mUsbTransferBuffer.size(),
    onTransferFinished,
    &mDisplayData,
    1000);
}


//////////////////////////////////////////////////////////////////////////////////////////
//
// AbleDoom implementation
//
//////////////////////////////////////////////////////////////////////////////////////////

// Bundle the static array for the controls explanation image into our executable
#include "controls_image.ipp"


namespace
{

// Input mapping - Push control (button/pad) to keyboard key
struct InputMapping
{
  ControlId id;
  uint8_t doomKey;
  std::optional<uint8_t> color;
};


constexpr auto INPUT_MAPPING_TABLE = std::array{
  // clang-format off
  InputMapping{PadId{0, 3}, KEY_FIRE},
  InputMapping{PadId{1, 3}, KEY_LALT},
  InputMapping{PadId{2, 3}, KEY_USE},
  InputMapping{PadId{2, 5}, KEY_RSHIFT},

  InputMapping{PadId{6, 2}, KEY_UPARROW},
  InputMapping{PadId{5, 3}, KEY_LEFTARROW},
  InputMapping{PadId{6, 3}, KEY_DOWNARROW},
  InputMapping{PadId{7, 3}, KEY_RIGHTARROW},

  InputMapping{ButtonId{91}, KEY_ENTER},
  InputMapping{ButtonId{33}, KEY_ESCAPE},
  InputMapping{ButtonId{46}, KEY_UPARROW},
  InputMapping{ButtonId{47}, KEY_DOWNARROW},

  InputMapping{ButtonId{82}, KEY_F6},

  // Use a slightly less bright color (121) for the weapon switch pads
  InputMapping{PadId{0, 0}, '1', 121},
  InputMapping{PadId{1, 0}, '2', 121},
  InputMapping{PadId{2, 0}, '3', 121},
  InputMapping{PadId{3, 0}, '4', 121},
  InputMapping{PadId{4, 0}, '5', 121},
  InputMapping{PadId{5, 0}, '6', 121},
  InputMapping{PadId{6, 0}, '7', 121},
  // clang-format on
};


int getCurrentAmmo(void)
{
  ammotype_t ammoType = weaponinfo[players[consoleplayer].readyweapon].ammo;
  if (ammoType == am_noammo)
    return 0;
  return players[consoleplayer].ammo[ammoType];
}


int getCurrentMaxAmmo(void)
{
  ammotype_t ammoType = weaponinfo[players[consoleplayer].readyweapon].ammo;
  if (ammoType == am_noammo)
    return 0;
  return players[consoleplayer].maxammo[ammoType];
}


// Turns value into a number between 0 and 8 for display on Push's LEDs
// (screen buttons and scene launch buttons)
int valueToButtonCount(int value, int max = 100)
{
  return std::clamp(int(std::round((float(value) / float(max)) * 8.0f)), 0, 8);
}


// Light up up to 8 LEDs in a row, starting at a specific button.
// The screen buttons and scene launch buttons happen to be numbered in ascending order
// already, so we make use of that here.
void setButtonBarLights(PushHardware& push, int numLitButtons, int firstButtonId)
{
  auto colorToUse = 126;

  if (numLitButtons < 3)
  {
    colorToUse = 127;
  }
  else if (numLitButtons < 6)
  {
    colorToUse = 7;
  }

  for (auto i = 0; i < 8; ++i)
  {
    push.setButtonLight(firstButtonId + i, i < numLitButtons ? colorToUse : 0);
  }
}

} // namespace


AbleDoom::AbleDoom()
  : mHardware([this](const PushInputEvent& input) { onInput(input); })
  , mLastHealthButtonCount(valueToButtonCount(players[consoleplayer].health))
  , mLastArmorButtonCount(valueToButtonCount(players[consoleplayer].armorpoints))
  , mLastAmmoButtonCount(valueToButtonCount(getCurrentAmmo(), getCurrentMaxAmmo()))
{
  // Turn button lights on for any button that's mapped to a key in the mapping table
  for (const auto& mapping : INPUT_MAPPING_TABLE)
  {
    const auto color = mapping.color.value_or(122);

    std::visit(
      [this, color](const auto& id) { mHardware.setLight(id, color); }, mapping.id);
  }

  // Additionally turn on the Shift button's light
  mHardware.setButtonLight(49, 122);

  // Pre-fill the screen buffer with the static controls help image, we only overwrite
  // other parts of the buffer while this background image remains untouched (see
  // drawFrame()).
  mHardware.copyToScreen(CONTROLS_IMAGE);
}


std::optional<DoomInputEvent> AbleDoom::fetchEvent()
{
  if (mEventQueue.empty())
  {
    return {};
  }

  const auto event = mEventQueue.front();
  mEventQueue.pop_front();
  return event;
}


void AbleDoom::drawFrame(const uint32_t* pFrameBuffer)
{
  // The Push display is only 160 pixels high, so it doesn't fit the entire Doom
  // framebuffer (200 px). To work around that, we display the bottom 40 rows of pixels
  // on the right side of the screen, next to the main framebuffer image.
  const auto mainCenter = (PUSH_SCREEN_WIDTH - DOOMGENERIC_RESX) / 2;

  mHardware.copyToScreen(
    pFrameBuffer, 0, 0, DOOMGENERIC_RESX, PUSH_SCREEN_HEIGHT, mainCenter, 0);
  mHardware.copyToScreen(
    pFrameBuffer,
    0,
    PUSH_SCREEN_HEIGHT,
    DOOMGENERIC_RESX,
    DOOMGENERIC_RESY - PUSH_SCREEN_HEIGHT,
    mainCenter + DOOMGENERIC_RESX,
    0);
  mHardware.submitScreen();

  updateHealthArmorDisplay();
}


void AbleDoom::onInput(const PushInputEvent& input)
{
  if (input.id == ControlId{ButtonId{49}})
  {
    mShiftHeld = input.pressed;
  }

  // Map Push button to Doom key
  const auto iMapping = std::find_if(
    std::begin(INPUT_MAPPING_TABLE),
    std::end(INPUT_MAPPING_TABLE),
    [&input](const InputMapping& mapping) { return mapping.id == input.id; });

  if (iMapping != std::end(INPUT_MAPPING_TABLE))
  {
    auto key = iMapping->doomKey;

    // Do quick load on Shift + Save. The input mapping system here doesn't allow for
    // button combinations, so we handle this as a special case (KEY_F6 is the quick
    // save key, KEY_F9 is quick load).
    if (iMapping->doomKey == KEY_F6 && mShiftHeld)
    {
      key = KEY_F9;
    }

    mEventQueue.push_back(DoomInputEvent{key, input.pressed});
  }
}


void AbleDoom::updateHealthArmorDisplay()
{
  const auto healthButtonCount = valueToButtonCount(players[consoleplayer].health);
  const auto armorButtonCount = valueToButtonCount(players[consoleplayer].armorpoints);
  const auto ammoButtonCount = valueToButtonCount(getCurrentAmmo(), getCurrentMaxAmmo());

  // Show health on screen top buttons
  if (healthButtonCount != mLastHealthButtonCount)
  {
    setButtonBarLights(mHardware, healthButtonCount, 102);
    mLastHealthButtonCount = healthButtonCount;
  }

  // Show armor on screen bottom buttons
  if (armorButtonCount != mLastArmorButtonCount)
  {
    setButtonBarLights(mHardware, armorButtonCount, 20);
    mLastArmorButtonCount = armorButtonCount;
  }

  // Show ammo on scene launch buttons
  if (ammoButtonCount != mLastAmmoButtonCount)
  {
    setButtonBarLights(mHardware, ammoButtonCount, 36);
    mLastAmmoButtonCount = ammoButtonCount;
  }
}
