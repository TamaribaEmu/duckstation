// SPDX-License-Identifier: GPL-3.0
//
// A stand-in for SoundTouch's C API in the libretro core. The core's only audio stream is a null stream that never
// stretches or resamples (the frontend paces the game), so these are never reached with real work; they exist so that
// util/audio_stream.cpp builds without the SoundTouch library.

#pragma once

typedef void* HANDLE;

#define SETTING_USE_AA_FILTER 0
#define SETTING_AA_FILTER_LENGTH 1
#define SETTING_USE_QUICKSEEK 2
#define SETTING_SEQUENCE_MS 3
#define SETTING_SEEKWINDOW_MS 4
#define SETTING_OVERLAP_MS 5

static inline HANDLE soundtouch_createInstance()
{
  return nullptr;
}
static inline void soundtouch_destroyInstance(HANDLE)
{
}
static inline void soundtouch_setRate(HANDLE, float)
{
}
static inline void soundtouch_setTempo(HANDLE, float)
{
}
static inline void soundtouch_setChannels(HANDLE, unsigned int)
{
}
static inline void soundtouch_setSampleRate(HANDLE, unsigned int)
{
}
static inline int soundtouch_setSetting(HANDLE, int, int)
{
  return 0;
}
static inline void soundtouch_putSamples(HANDLE, const float*, unsigned int)
{
}
static inline unsigned int soundtouch_receiveSamples(HANDLE, float*, unsigned int)
{
  return 0;
}
static inline void soundtouch_clear(HANDLE)
{
}
