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

#include "doomgeneric.h"

#include "abledoom.hpp"

#include <memory>
#include <stdlib.h>
#include <time.h>
#include <type_traits>
#include <unistd.h>


// Verify correct compile-time configuration
static_assert(DOOMGENERIC_RESX == 320);
static_assert(DOOMGENERIC_RESY == 200);
static_assert(std::is_same_v<pixel_t, uint32_t>);


namespace
{

struct timespec startTime;

std::unique_ptr<AbleDoom> ableDoom;


template <typename Callback>
void runGuarded(Callback&& callback)
{
  try
  {
    callback();
  }
  catch (const std::exception& err)
  {
    fprintf(stderr, "Error: %s\n", err.what());
    exit(-1);
  }
}

} // namespace


void DG_Init()
{
  // Initialize libusb
  {
    const auto result = libusb_init(nullptr);

    if (result < 0)
    {
      fprintf(stderr, "Failed to initialize libusb: %s\n", libusb_error_name(result));
      exit(-1);
    }

    atexit([]() { libusb_exit(nullptr); });

    libusb_set_option(nullptr, LIBUSB_OPTION_LOG_LEVEL, LIBUSB_LOG_LEVEL_ERROR);
  }


  // Store initial time for DG_GetTicksMs()
  clock_gettime(CLOCK_MONOTONIC_RAW, &startTime);


  // Initialize AbleDOOM
  runGuarded([]() {
    ableDoom = std::make_unique<AbleDoom>();
    atexit([]() { ableDoom.reset(); });
  });
}


void DG_DrawFrame()
{
  runGuarded([]() { ableDoom->drawFrame(DG_ScreenBuffer); });
}


int DG_GetKey(int* pressed, unsigned char* doomKey)
{
  auto result = 0;

  runGuarded([&]() {
    if (const auto oEvent = ableDoom->fetchEvent())
    {
      *pressed = oEvent->pressed ? 1 : 0;
      *doomKey = oEvent->key;
      result = 1;
    }
  });

  return result;
}


void DG_SleepMs(uint32_t ms)
{
  usleep(ms * 1000);
}


uint32_t DG_GetTicksMs()
{
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC_RAW, &now);

  const auto secsElapsed = now.tv_sec - startTime.tv_sec;
  const auto nanosElapsed = now.tv_nsec - startTime.tv_nsec;

  return uint32_t(secsElapsed * 1000 + nanosElapsed / 1'000'000);
}


void DG_SetWindowTitle(const char*)
{
  // No-op
}


int main(int argc, char** argv)
{
  doomgeneric_Create(argc, argv);

  for (;;)
  {
    doomgeneric_Tick();
  }

  return 0;
}
