// SPDX-License-Identifier: GPL-3.0
//
// DuckStation as a libretro core.
//
// The frontend owns the window and the Vulkan instance: through the context negotiation interface it lets the core
// make the device (VulkanDevice::CreateExternalDevice), shares it and its queue, and takes each frame as an image.
// The console boots once that device exists (context_reset). Every retro_run() executes one frame:
// System::FrameDone() returns to us after Host::FrameDone(), where the picture is drawn into one of three images at
// internal resolution and handed over; the frontend corrects the aspect ratio, presents and paces. Audio is the SPU's
// output, taken from a null stream after the frame.
//
// Memory cards: each slot holds the game's own card (kept by the frontend as its save RAM, a raw 128 KiB card
// image), a card file of the player's (DuckStation reads and writes it), or nothing (duckstation_card1/2:
// "game", "none" or "file:<path>"). retro_tamariba_ps1_set_cards() plugs and unplugs them while the game runs.
// Discs: games of several discs arrive as an .m3u; the frontend picks the first disc with set_initial_image() and
// changes discs through the disk control interface.
//
// Settings live in memory, made from the frontend's core options when a game loads. The BIOS is OpenBIOS (shipped
// in the system folder) unless duckstation_bios is "own" and a dump of the console's BIOS is in the system folder.

#include "core/achievements.h"
#include "core/analog_controller.h"
#include "core/bios.h"
#include "core/bus.h"
#include "core/controller.h"
#include "core/fullscreen_ui.h"
#include "core/cpu_core.h"
#include "core/game_list.h"
#include "core/gpu.h"
#include "core/host.h"
#include "core/memory_card.h"
#include "core/pad.h"
#include "core/settings.h"
#include "core/spu.h"
#include "core/system.h"

#include "util/audio_stream.h"
#include "util/cd_image.h"
#include "util/gpu_device.h"
#include "util/http_downloader.h"
#include "util/imgui_fullscreen.h"
#include "util/imgui_manager.h"
#include "util/input_manager.h"
#include "util/page_fault_handler.h"
#include "util/platform_misc.h"
#include "util/vulkan_device.h"
#include "util/vulkan_texture.h"

#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/memory_settings_interface.h"
#include "common/path.h"
#include "common/small_string.h"
#include "common/string_util.h"

// After DuckStation's Vulkan headers (no prototypes: its own function pointers).
#include "libretro.h"
#include "libretro_vulkan.h"

#include <array>
#include <cstdarg>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#ifdef __ANDROID__
#include <android/log.h>
#endif
#include <unistd.h>

Log_SetChannel(Libretro);

namespace {

constexpr u32 kPorts = 2;
constexpr u32 kFrameImages = 3;  // frames in the frontend's hands while we draw the next
constexpr u32 kAudioChunk = 2048;
constexpr const char* kOpenBiosFile = "openbios.bin";
constexpr const char* kOpenBiosFastFile = "openbios-fast.bin";  // no boot shell: the disc starts at once

retro_environment_t s_environment = nullptr;
retro_video_refresh_t s_video_refresh = nullptr;
retro_audio_sample_batch_t s_audio_batch = nullptr;
retro_input_poll_t s_input_poll = nullptr;
retro_input_state_t s_input_state = nullptr;
retro_log_printf_t s_log = nullptr;

retro_hw_render_callback s_hw_render = {};
retro_hw_render_context_negotiation_interface_vulkan s_negotiation = {};
const retro_hw_render_interface_vulkan* s_vulkan = nullptr;

std::string s_game_path;
std::string s_system_dir;
std::unique_ptr<MemorySettingsInterface> s_settings;
bool s_started = false;  // ProcessStartup() and CPUThreadInitialize()
bool s_booted = false;
bool s_boot_failed = false;
double s_fps = 59.94;
bool s_pal = false;

std::array<std::unique_ptr<GPUTexture>, kFrameImages> s_frames;
u32 s_next_frame = 0;
u32 s_frame_width = 0;
u32 s_frame_height = 0;
float s_frame_aspect = 0.0f;
bool s_frame_presented = false;

std::vector<s16> s_audio;

// The memory card slots: "game", "none" or "file:<path>". The game's card while it is unplugged (or before the
// console runs) waits here, and is the save RAM then.
std::array<std::string, kPorts> s_cards = {"game", "none"};
std::vector<u8> s_game_card;

// Discs: the one to start with, and the frontend's eject state.
unsigned s_initial_disc = 0;
bool s_disc_ejected = false;
unsigned s_pending_disc = 0;

// ---------------------------------------------------------------------------------------------------------------------
// Logging and options
// ---------------------------------------------------------------------------------------------------------------------

void LogCallback(void*, const char* channel, const char*, LOGLEVEL level, std::string_view message)
{
  if (!s_log)
    return;
  retro_log_level rl;
  switch (level)
  {
    case LOGLEVEL_ERROR:
      rl = RETRO_LOG_ERROR;
      break;
    case LOGLEVEL_WARNING:
      rl = RETRO_LOG_WARN;
      break;
    case LOGLEVEL_INFO:
      rl = RETRO_LOG_INFO;
      break;
    default:
      rl = RETRO_LOG_DEBUG;
      break;
  }
  s_log(rl, "[%s] %.*s\n", channel, static_cast<int>(message.size()), message.data());
}

const char* GetOption(const char* key, const char* fallback)
{
  retro_variable var = {key, nullptr};
  if (s_environment && s_environment(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value && var.value[0])
    return var.value;
  return fallback;
}

bool GetOptionEnabled(const char* key, bool fallback)
{
  const char* value = GetOption(key, fallback ? "enabled" : "disabled");
  return std::strcmp(value, "enabled") == 0 || std::strcmp(value, "true") == 0 || std::strcmp(value, "on") == 0;
}

void DeclareOptions()
{
  static const retro_variable variables[] = {
    {"duckstation_bios", "BIOS; openbios|own"},
    {"duckstation_fast_boot", "Start games directly (skip the BIOS intro); enabled|disabled"},
    {"duckstation_resolution_scale", "Internal resolution; 1|2|3|4|5|6|7|8"},
    {"duckstation_texture_filter", "Texture filtering; Nearest|Bilinear|BilinearBinAlpha|JINC2|JINC2BinAlpha|xBR|xBRBinAlpha"},
    {"duckstation_true_color", "True color; enabled|disabled"},
    {"duckstation_pgxp", "Geometry correction (PGXP); disabled|enabled"},
    {"duckstation_widescreen", "Widescreen hack; disabled|enabled"},
    {"duckstation_deinterlacing", "Deinterlacing; Adaptive|Weave|Blend|Disabled"},
    {"duckstation_crop", "Crop; Overscan|None|Borders"},
    {"duckstation_cpu", "CPU; Recompiler|NewRec|CachedInterpreter|Interpreter"},
    {"duckstation_cdrom_speedup", "CD-ROM read speedup; 1|2|3|4|5|6|7|8|9|10"},
    {"duckstation_card1", "Memory card in slot 1; game|none"},
    {"duckstation_card2", "Memory card in slot 2; none|game"},
    {nullptr, nullptr},
  };
  s_environment(RETRO_ENVIRONMENT_SET_VARIABLES, const_cast<retro_variable*>(variables));
}

// The console's own BIOS, when the player chose it and a dump for this region is in the system folder.
std::string FindOwnBios(ConsoleRegion region)
{
  for (const auto& [path, info] : BIOS::FindBIOSImagesInDirectory(s_system_dir.c_str()))
  {
    if (info && BIOS::IsValidBIOSForRegion(region, info->region))
      return path;
  }
  return {};
}

// The player's choices (the frontend's core options) that a running game takes as they change: the picture's, and
// the emulation's speed-ups. The BIOS, starting directly and the cards are read once, as the game starts.
void SetPlayerOptions(SettingsInterface& si)
{
  si.SetIntValue("GPU", "ResolutionScale", StringUtil::FromChars<s32>(GetOption("duckstation_resolution_scale", "1"))
                                             .value_or(1));
  si.SetStringValue("GPU", "TextureFilter", GetOption("duckstation_texture_filter", "Nearest"));
  si.SetBoolValue("GPU", "TrueColor", GetOptionEnabled("duckstation_true_color", true));
  si.SetBoolValue("GPU", "PGXPEnable", GetOptionEnabled("duckstation_pgxp", false));
  si.SetBoolValue("GPU", "WidescreenHack", GetOptionEnabled("duckstation_widescreen", false));
  si.SetStringValue("Display", "DeinterlacingMode", GetOption("duckstation_deinterlacing", "Adaptive"));
  si.SetStringValue("Display", "CropMode", GetOption("duckstation_crop", "Overscan"));
  si.SetStringValue("CPU", "ExecutionMode", GetOption("duckstation_cpu", "Recompiler"));
  si.SetIntValue("CDROM", "ReadSpeedup",
                 StringUtil::FromChars<s32>(GetOption("duckstation_cdrom_speedup", "1")).value_or(1));
}

// The frontend changed options while the game runs: DuckStation compares its settings with the old ones and
// changes what differs (a new resolution rebuilds the GPU's targets, PGXP clears the recompiled code).
void ApplyChangedOptions()
{
  bool updated = false;
  if (!s_environment(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) || !updated || !s_settings)
    return;
  SetPlayerOptions(*s_settings);
  System::ApplySettings(false);
}

void MakeSettings(ConsoleRegion region)
{
  s_settings = std::make_unique<MemorySettingsInterface>();
  SettingsInterface& si = *s_settings;
  Host::Internal::SetBaseSettingsLayer(s_settings.get());
  g_settings = Settings();
  g_settings.Save(si, false);

  // Paths: the frontend installs the game database (resources/) and OpenBIOS (bios/) in <system>/duckstation, and
  // replaces that folder when they change; what DuckStation writes (its shader cache) goes to duckstation-data.
  EmuFolders::AppRoot = Path::Combine(s_system_dir, "duckstation");
  EmuFolders::DataRoot = Path::Combine(s_system_dir, "duckstation-data");
  EmuFolders::Resources = Path::Combine(EmuFolders::AppRoot, "resources");
  EmuFolders::SetDefaults();
  EmuFolders::Save(si);

  // The frontend shows the picture, plays the sound, keeps the saves and paces the game.
  si.SetStringValue("GPU", "Renderer", Settings::GetRendererName(GPURenderer::HardwareVulkan));
  si.SetBoolValue("GPU", "ThreadedPresentation", false);
  si.SetBoolValue("GPU", "UseThread", false);
  si.SetStringValue("Audio", "Backend", AudioStream::GetBackendName(AudioBackend::Null));
  si.SetUIntValue("Audio", "BufferMS", 250);
  si.SetFloatValue("Main", "EmulationSpeed", 1.0f);
  si.SetBoolValue("Main", "SyncToHostRefreshRate", false);
  si.SetBoolValue("Main", "PauseOnFocusLoss", false);
  si.SetBoolValue("Main", "StartPaused", false);
  si.SetBoolValue("Main", "StartFullscreen", false);
  si.SetBoolValue("Main", "SaveStateOnExit", false);
  si.SetBoolValue("Main", "ConfirmPowerOff", false);
  si.SetBoolValue("Main", "RewindEnable", false);
  si.SetUIntValue("Main", "RunaheadFrameCount", 0);
  si.SetBoolValue("Main", "EnableDiscordPresence", false);
  si.SetBoolValue("Main", "LoadDevicesFromSaveStates", false);
  si.SetBoolValue("Main", "ApplyGameSettings", true);  // the game database's fixes
  si.SetBoolValue("Display", "ShowOSDMessages", false);
  si.SetStringValue("Display", "Scaling", Settings::GetDisplayScalingName(DisplayScalingMode::Nearest));
  si.SetStringValue("Display", "AspectRatio", Settings::GetDisplayAspectRatioName(DisplayAspectRatio::Auto));
  si.SetBoolValue("Display", "OptimalFramePacing", false);
  si.SetBoolValue("Display", "VSync", false);
  si.SetBoolValue("Cheevos", "Enabled", false);  // the frontend runs achievements on the core's memory
  // The cards as the game starts: the game's own card is DuckStation's non-persistent kind (the frontend fills and
  // keeps it), a card file DuckStation's shared kind with that path.
  s_cards = {GetOption("duckstation_card1", "game"), GetOption("duckstation_card2", "none")};
  for (u32 slot = 0; slot < kPorts; slot++)
  {
    const std::string& card = s_cards[slot];
    MemoryCardType type = MemoryCardType::None;
    if (card == "game")
      type = MemoryCardType::NonPersistent;
    else if (card.starts_with("file:") && card.size() > 5)
      type = MemoryCardType::Shared;
    si.SetStringValue("MemoryCards", TinyString::from_format("Card{}Type", slot + 1),
                      Settings::GetMemoryCardTypeName(type));
    if (type == MemoryCardType::Shared)
      si.SetStringValue("MemoryCards", TinyString::from_format("Card{}Path", slot + 1), card.substr(5).c_str());
  }
  for (u32 port = 0; port < kPorts; port++)
  {
    si.SetStringValue(TinyString::from_format("Pad{}", port + 1), "Type",
                      Controller::GetControllerInfo(ControllerType::AnalogController)->name);
  }
  for (u32 i = 0; i < static_cast<u32>(InputSourceType::Count); i++)
    si.SetBoolValue("InputSources", InputManager::InputSourceToString(static_cast<InputSourceType>(i)), false);
  si.SetBoolValue("Logging", "LogToConsole", false);
  si.SetBoolValue("Logging", "LogToFile", false);

  // The player's choices (the frontend's core options).
  SetPlayerOptions(si);

  // The BIOS: OpenBIOS from the bios folder, or the player's own dump (any region's file name works). Starting
  // directly: OpenBIOS's build without its shell, or DuckStation's patch for a known BIOS.
  const bool fast_boot = GetOptionEnabled("duckstation_fast_boot", true);
  std::string bios = Path::Combine(Path::Combine(EmuFolders::AppRoot, "bios"), fast_boot ? kOpenBiosFastFile : kOpenBiosFile);
  if (std::strcmp(GetOption("duckstation_bios", "openbios"), "own") == 0)
  {
    if (std::string own = FindOwnBios(region); !own.empty())
      bios = std::move(own);
    else
      WARNING_LOG("No BIOS dump for this region in {}: using OpenBIOS", s_system_dir);
  }
  INFO_LOG("BIOS: {}", bios);
  si.SetStringValue("BIOS", "SearchDirectory", std::string(Path::GetDirectory(bios)).c_str());
  for (const char* key : {"PathNTSCU", "PathNTSCJ", "PathPAL"})
    si.SetStringValue("BIOS", key, std::string(Path::GetFileName(bios)).c_str());
  si.SetBoolValue("BIOS", "PatchFastBoot", fast_boot);

  EmuFolders::LoadConfig(si);
  FileSystem::EnsureDirectoryExists(EmuFolders::DataRoot.c_str(), true);
  EmuFolders::EnsureFoldersExist();
}

// ---------------------------------------------------------------------------------------------------------------------
// Vulkan: the frontend's device, and the frames handed to it
// ---------------------------------------------------------------------------------------------------------------------

bool CreateDevice(retro_vulkan_context* context, VkInstance instance, VkPhysicalDevice gpu, VkSurfaceKHR surface,
                  PFN_vkGetInstanceProcAddr, const char** required_extensions, unsigned num_required_extensions,
                  const char**, unsigned, const VkPhysicalDeviceFeatures*)
{
  if (gpu == VK_NULL_HANDLE)
  {
    ERROR_LOG("The frontend gave no GPU to create the device on");
    return false;
  }

  VulkanDevice::ExternalDevice device;
  Error error;
  if (!VulkanDevice::CreateExternalDevice(instance, gpu, surface, required_extensions, num_required_extensions,
                                          &device, &error))
  {
    ERROR_LOG("Could not create the Vulkan device: {}", error.GetDescription());
    return false;
  }

  context->gpu = device.gpu;
  context->device = device.device;
  context->queue = device.queue;
  context->queue_family_index = device.queue_family;
  context->presentation_queue = device.present_queue;
  context->presentation_queue_family_index = device.present_queue_family;
  return true;
}

void ReleaseFrames()
{
  for (std::unique_ptr<GPUTexture>& frame : s_frames)
    frame.reset();
  s_next_frame = 0;
}

void UpdateGeometry(u32 width, u32 height, float aspect)
{
  if (width == s_frame_width && height == s_frame_height && aspect == s_frame_aspect)
    return;

  s_frame_width = width;
  s_frame_height = height;
  s_frame_aspect = aspect;
  retro_game_geometry geometry = {};
  geometry.base_width = width;
  geometry.base_height = height;
  geometry.max_width = std::max(width, 1024u * 16u);
  geometry.max_height = std::max(height, 512u * 16u);
  geometry.aspect_ratio = aspect;
  s_environment(RETRO_ENVIRONMENT_SET_GEOMETRY, &geometry);
}

// Draws the frame into the next of our images and hands it over (called from Host::FrameDone()).
void PresentFrame()
{
  if (!s_vulkan || !g_gpu || !g_gpu_device)
    return;

  u32 width, height;
  g_gpu->GetLibretroFrameSize(&width, &height);
  if (width == 0 || height == 0)
  {
    // Nothing shown yet (the BIOS is starting): a black picture of the usual size.
    width = 320;
    height = 240;
  }

  std::unique_ptr<GPUTexture>& frame = s_frames[s_next_frame];
  if (!frame || frame->GetWidth() != width || frame->GetHeight() != height)
  {
    frame.reset();
    frame = g_gpu_device->CreateTexture(width, height, 1, 1, 1, GPUTexture::Type::RenderTarget,
                                        GPUTexture::Format::RGBA8);
    if (!frame)
    {
      ERROR_LOG("Could not create a {}x{} frame", width, height);
      return;
    }
  }

  g_gpu->RenderLibretroFrame(frame.get());
  // Submitted before the frontend's copy of it, which is ordered after it on the shared queue.
  static_cast<VulkanDevice*>(g_gpu_device.get())->SubmitCommandBuffer(false);

  const VulkanTexture* texture = static_cast<const VulkanTexture*>(frame.get());
  retro_vulkan_image image = {};
  image.image_view = texture->GetView();
  image.image_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  image.create_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  image.create_info.image = texture->GetImage();
  image.create_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  image.create_info.format = texture->GetVkFormat();
  image.create_info.components = {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B,
                                  VK_COMPONENT_SWIZZLE_A};
  image.create_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  s_vulkan->set_image(s_vulkan->handle, &image, 0, nullptr, VK_QUEUE_FAMILY_IGNORED);

  UpdateGeometry(width, height, g_gpu->ComputeDisplayAspectRatio());
  s_video_refresh(RETRO_HW_FRAME_BUFFER_VALID, width, height, 0);
  s_frame_presented = true;
  s_next_frame = (s_next_frame + 1) % kFrameImages;
}

void LockQueue(void*)
{
  s_vulkan->lock_queue(s_vulkan->handle);
}

void UnlockQueue(void*)
{
  s_vulkan->unlock_queue(s_vulkan->handle);
}

void UpdateTiming()
{
  const double fps = System::GetThrottleFrequency();
  if (fps <= 0.0 || std::abs(fps - s_fps) < 0.001)
    return;

  s_fps = fps;
  retro_system_av_info av = {};
  retro_get_system_av_info(&av);
  s_environment(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &av);
}

// ---------------------------------------------------------------------------------------------------------------------
// Memory cards and discs
// ---------------------------------------------------------------------------------------------------------------------

int GameCardSlot()
{
  for (u32 slot = 0; slot < kPorts; slot++)
  {
    if (s_cards[slot] == "game")
      return static_cast<int>(slot);
  }
  return -1;
}

// Changes one slot's card while the console runs (the game sees the card come out and another go in).
void PlugCard(u32 slot, const std::string& card)
{
  if (s_cards[slot] == card)
    return;

  // The game's card coming out keeps its contents for the save RAM.
  if (s_cards[slot] == "game")
  {
    if (MemoryCard* mc = Pad::GetMemoryCard(slot))
      s_game_card.assign(mc->GetData().begin(), mc->GetData().end());
  }
  Pad::SetMemoryCard(slot, nullptr);  // a card file is written as it goes

  std::unique_ptr<MemoryCard> mc;
  if (card == "game")
  {
    mc = MemoryCard::Create();
    if (s_game_card.size() == MemoryCardImage::DATA_SIZE)
      std::memcpy(mc->GetData().data(), s_game_card.data(), MemoryCardImage::DATA_SIZE);
  }
  else if (card.starts_with("file:") && card.size() > 5)
  {
    mc = MemoryCard::Open(card.substr(5));
  }
  INFO_LOG("Memory card slot {}: {}", slot + 1, card);
  Pad::SetMemoryCard(slot, std::move(mc));
  s_cards[slot] = card;
}

bool SetEjectState(bool ejected)
{
  if (!s_booted)
    return false;
  if (s_disc_ejected && !ejected && s_pending_disc != System::GetMediaSubImageIndex())
  {
    if (!System::SwitchMediaSubImage(s_pending_disc))
      return false;
  }
  s_disc_ejected = ejected;
  return true;
}

bool GetEjectState()
{
  return s_disc_ejected;
}

unsigned GetImageIndex()
{
  if (!s_booted)
    return s_initial_disc;
  return s_disc_ejected ? s_pending_disc : System::GetMediaSubImageIndex();
}

bool SetImageIndex(unsigned index)
{
  if (!s_booted || !s_disc_ejected || index >= System::GetMediaSubImageCount())
    return false;
  s_pending_disc = index;
  return true;
}

unsigned GetNumImages()
{
  return s_booted && System::HasMediaSubImages() ? System::GetMediaSubImageCount() : (s_booted ? 1 : 0);
}

bool ReplaceImageIndex(unsigned, const retro_game_info*)
{
  return false;
}

bool AddImageIndex()
{
  return false;
}

bool SetInitialImage(unsigned index, const char*)
{
  s_initial_disc = index;
  return true;
}

bool GetImageLabel(unsigned index, char* s, size_t len)
{
  if (!s_booted || index >= GetNumImages() || len == 0)
    return false;
  const std::string title = System::GetMediaSubImageTitle(index);
  StringUtil::Strlcpy(s, title.c_str(), len);
  return true;
}

void DeclareDiskControl()
{
  static retro_disk_control_ext_callback callbacks = {};
  callbacks.set_eject_state = &SetEjectState;
  callbacks.get_eject_state = &GetEjectState;
  callbacks.get_image_index = &GetImageIndex;
  callbacks.set_image_index = &SetImageIndex;
  callbacks.get_num_images = &GetNumImages;
  callbacks.replace_image_index = &ReplaceImageIndex;
  callbacks.add_image_index = &AddImageIndex;
  callbacks.set_initial_image = &SetInitialImage;
  callbacks.get_image_label = &GetImageLabel;
  s_environment(RETRO_ENVIRONMENT_SET_DISK_CONTROL_EXT_INTERFACE, &callbacks);
}

// Where the console's memory is, for the frontend's achievements: main RAM, and the scratchpad at its own address.
void SetMemoryMaps()
{
  static retro_memory_descriptor descriptors[2];
  descriptors[0] = {};
  descriptors[0].flags = RETRO_MEMDESC_SYSTEM_RAM;
  descriptors[0].ptr = Bus::g_ram;
  descriptors[0].start = 0;
  descriptors[0].len = Bus::RAM_2MB_SIZE;
  descriptors[1] = {};
  descriptors[1].flags = RETRO_MEMDESC_SYSTEM_RAM;
  descriptors[1].ptr = CPU::g_state.scratchpad.data();
  descriptors[1].start = 0x1F800000;
  descriptors[1].len = CPU::SCRATCHPAD_SIZE;
  retro_memory_map map = {descriptors, 2};
  s_environment(RETRO_ENVIRONMENT_SET_MEMORY_MAPS, &map);
}

void Boot()
{
  INFO_LOG("Booting {}", s_game_path);
  Error error;
  SystemBootParameters params(s_game_path);
  params.media_playlist_index = s_initial_disc;
  if (!System::BootSystem(std::move(params), &error))
  {
    ERROR_LOG("The game did not start: {}", error.GetDescription());
    s_boot_failed = true;
    retro_message message = {"This game could not be started.", 600};
    s_environment(RETRO_ENVIRONMENT_SET_MESSAGE, &message);
    return;
  }

  s_booted = true;
  s_pal = (System::GetRegion() == ConsoleRegion::PAL);
  SetMemoryMaps();
  UpdateTiming();
}

void ContextReset()
{
  if (!s_environment(RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE, &s_vulkan) || !s_vulkan ||
      s_vulkan->interface_type != RETRO_HW_RENDER_INTERFACE_VULKAN ||
      s_vulkan->interface_version < RETRO_HW_RENDER_INTERFACE_VULKAN_VERSION)
  {
    ERROR_LOG("The frontend has no usable Vulkan interface");
    s_vulkan = nullptr;
    s_boot_failed = true;
    return;
  }

  VulkanDevice::SetExternalQueueLock(&LockQueue, &UnlockQueue, nullptr);
  if (!s_booted && !s_boot_failed)
    Boot();
}

void ContextDestroy()
{
  // The game is unloaded first (retro_unload_game()), which leaves nothing of ours on the device.
  s_vulkan = nullptr;
  VulkanDevice::SetExternalQueueLock(nullptr, nullptr, nullptr);
}

// ---------------------------------------------------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------------------------------------------------

struct ButtonMap
{
  unsigned retro;
  AnalogController::Button button;
};

constexpr ButtonMap kButtons[] = {
  {RETRO_DEVICE_ID_JOYPAD_B, AnalogController::Button::Cross},
  {RETRO_DEVICE_ID_JOYPAD_A, AnalogController::Button::Circle},
  {RETRO_DEVICE_ID_JOYPAD_Y, AnalogController::Button::Square},
  {RETRO_DEVICE_ID_JOYPAD_X, AnalogController::Button::Triangle},
  {RETRO_DEVICE_ID_JOYPAD_L, AnalogController::Button::L1},
  {RETRO_DEVICE_ID_JOYPAD_R, AnalogController::Button::R1},
  {RETRO_DEVICE_ID_JOYPAD_L2, AnalogController::Button::L2},
  {RETRO_DEVICE_ID_JOYPAD_R2, AnalogController::Button::R2},
  {RETRO_DEVICE_ID_JOYPAD_L3, AnalogController::Button::L3},
  {RETRO_DEVICE_ID_JOYPAD_R3, AnalogController::Button::R3},
  {RETRO_DEVICE_ID_JOYPAD_SELECT, AnalogController::Button::Select},
  {RETRO_DEVICE_ID_JOYPAD_START, AnalogController::Button::Start},
  {RETRO_DEVICE_ID_JOYPAD_UP, AnalogController::Button::Up},
  {RETRO_DEVICE_ID_JOYPAD_DOWN, AnalogController::Button::Down},
  {RETRO_DEVICE_ID_JOYPAD_LEFT, AnalogController::Button::Left},
  {RETRO_DEVICE_ID_JOYPAD_RIGHT, AnalogController::Button::Right},
};

void SetStick(Controller* controller, unsigned port, unsigned stick, AnalogController::HalfAxis negative_x,
              AnalogController::HalfAxis positive_x, AnalogController::HalfAxis negative_y,
              AnalogController::HalfAxis positive_y)
{
  const float x = static_cast<float>(s_input_state(port, RETRO_DEVICE_ANALOG, stick, RETRO_DEVICE_ID_ANALOG_X)) / 32767.0f;
  const float y = static_cast<float>(s_input_state(port, RETRO_DEVICE_ANALOG, stick, RETRO_DEVICE_ID_ANALOG_Y)) / 32767.0f;
  const u32 axes = static_cast<u32>(AnalogController::Button::Count);
  controller->SetBindState(axes + static_cast<u32>(negative_x), std::clamp(-x, 0.0f, 1.0f));
  controller->SetBindState(axes + static_cast<u32>(positive_x), std::clamp(x, 0.0f, 1.0f));
  controller->SetBindState(axes + static_cast<u32>(negative_y), std::clamp(-y, 0.0f, 1.0f));
  controller->SetBindState(axes + static_cast<u32>(positive_y), std::clamp(y, 0.0f, 1.0f));
}

void UpdateInput()
{
  s_input_poll();
  for (u32 port = 0; port < kPorts; port++)
  {
    Controller* controller = System::GetController(port);
    if (!controller || controller->GetType() != ControllerType::AnalogController)
      continue;

    for (const ButtonMap& map : kButtons)
    {
      const bool pressed = s_input_state(port, RETRO_DEVICE_JOYPAD, 0, map.retro) != 0;
      controller->SetBindState(static_cast<u32>(map.button), pressed ? 1.0f : 0.0f);
    }

    using HA = AnalogController::HalfAxis;
    SetStick(controller, port, RETRO_DEVICE_INDEX_ANALOG_LEFT, HA::LLeft, HA::LRight, HA::LUp, HA::LDown);
    SetStick(controller, port, RETRO_DEVICE_INDEX_ANALOG_RIGHT, HA::RLeft, HA::RRight, HA::RUp, HA::RDown);
  }
}

void SendAudio()
{
  AudioStream* stream = SPU::GetOutputStream();
  if (!stream)
    return;

  s_audio.resize(kAudioChunk * AudioStream::NUM_INPUT_CHANNELS);
  for (;;)
  {
    const u32 frames = stream->TakeFrames(s_audio.data(), kAudioChunk);
    if (frames == 0)
      break;
    s_audio_batch(s_audio.data(), frames);
  }
}

void Shutdown()
{
  if (s_booted)
  {
    // Destroying the system leaves the frontend's device as it found it (VulkanDevice keeps it).
    ReleaseFrames();
    System::ShutdownSystem(false);
    System::Execute();
    s_booted = false;
  }
  if (s_started)
  {
    System::Internal::CPUThreadShutdown();
    s_started = false;
  }
  s_boot_failed = false;
  s_disc_ejected = false;
  s_game_card.clear();
  s_frame_width = s_frame_height = 0;
  s_frame_aspect = 0.0f;
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------------
// Host: what DuckStation asks of its frontend
// ---------------------------------------------------------------------------------------------------------------------

void Host::ReportFatalError(std::string_view title, std::string_view message)
{
  ERROR_LOG("Fatal: {}: {}", title, message);
  std::abort();
}

void Host::ReportErrorAsync(std::string_view title, std::string_view message)
{
  ERROR_LOG("{}: {}", title, message);
}

bool Host::ConfirmMessage(std::string_view title, std::string_view message)
{
  WARNING_LOG("{}: {}", title, message);
  return true;
}

void Host::ReportDebuggerMessage(std::string_view message)
{
  INFO_LOG("{}", message);
}

std::span<const std::pair<const char*, const char*>> Host::GetAvailableLanguageList()
{
  return {};
}

bool Host::ChangeLanguage(const char*)
{
  return false;
}

s32 Host::Internal::GetTranslatedStringImpl(std::string_view, std::string_view msg, char* tbuf, size_t tbuf_space)
{
  if (msg.size() > tbuf_space)
    return -1;
  if (!msg.empty())
    std::memcpy(tbuf, msg.data(), msg.size());
  return static_cast<s32>(msg.size());
}

std::string Host::TranslatePluralToString(const char*, const char* msg, const char*, int count)
{
  std::string ret(msg);
  const std::string count_str = std::to_string(count);
  for (std::string::size_type pos; (pos = ret.find("%n")) != std::string::npos;)
    ret.replace(pos, 2, count_str);
  return ret;
}

SmallString Host::TranslatePluralToSmallString(const char*, const char* msg, const char*, int count)
{
  SmallString ret(msg);
  ret.replace("%n", TinyString::from_format("{}", count));
  return ret;
}

void Host::LoadSettings(SettingsInterface&, std::unique_lock<std::mutex>&)
{
}

void Host::CheckForSettingsChanges(const Settings&)
{
}

void Host::CommitBaseSettingChanges()
{
}

bool Host::ResourceFileExists(std::string_view filename, bool)
{
  const std::string path = Path::Combine(EmuFolders::Resources, filename);
  return FileSystem::FileExists(path.c_str());
}

std::optional<DynamicHeapArray<u8>> Host::ReadResourceFile(std::string_view filename, bool)
{
  const std::string path = Path::Combine(EmuFolders::Resources, filename);
  std::optional<DynamicHeapArray<u8>> ret = FileSystem::ReadBinaryFile(path.c_str());
  if (!ret.has_value())
    ERROR_LOG("Missing resource file {}", path);
  return ret;
}

std::optional<std::string> Host::ReadResourceFileToString(std::string_view filename, bool)
{
  const std::string path = Path::Combine(EmuFolders::Resources, filename);
  std::optional<std::string> ret = FileSystem::ReadFileToString(path.c_str());
  if (!ret.has_value())
    ERROR_LOG("Missing resource file {}", path);
  return ret;
}

std::optional<std::time_t> Host::GetResourceFileTimestamp(std::string_view filename, bool)
{
  const std::string path = Path::Combine(EmuFolders::Resources, filename);
  FILESYSTEM_STAT_DATA sd;
  if (!FileSystem::StatFile(path.c_str(), &sd))
    return std::nullopt;
  return sd.ModificationTime;
}

void Host::OnSystemStarting()
{
}

void Host::OnSystemStarted()
{
}

void Host::OnSystemDestroyed()
{
}

void Host::OnSystemPaused()
{
}

void Host::OnSystemResumed()
{
}

void Host::OnIdleStateChanged()
{
}

void Host::OnPerformanceCountersUpdated()
{
}

void Host::OnGameChanged(const std::string& disc_path, const std::string& game_serial, const std::string& game_name)
{
  INFO_LOG("Game: {} ({})", game_name, game_serial);
}

void Host::OnMediaCaptureStarted()
{
}

void Host::OnMediaCaptureStopped()
{
}

void Host::PumpMessagesOnCPUThread()
{
}

void Host::RunOnCPUThread(std::function<void()> function, bool)
{
  function();  // everything runs on the frontend's emulation thread
}

void Host::RequestResizeHostDisplay(s32, s32)
{
}

void Host::RequestExitApplication(bool)
{
}

void Host::RequestExitBigPicture()
{
}

void Host::RequestSystemShutdown(bool, bool)
{
}

bool Host::IsFullscreen()
{
  return false;
}

void Host::SetFullscreen(bool)
{
}

std::optional<WindowInfo> Host::AcquireRenderWindow(bool)
{
  // No window: the frontend presents our frames.
  WindowInfo wi;
  wi.SetSurfaceless();
  return wi;
}

void Host::ReleaseRenderWindow()
{
}

void Host::FrameDone()
{
  PresentFrame();
}

void Host::OpenURL(std::string_view)
{
}

bool Host::CopyTextToClipboard(std::string_view)
{
  return false;
}

void Host::SetMouseMode(bool, bool)
{
}

void Host::OnAchievementsLoginRequested(Achievements::LoginRequestReason)
{
}

void Host::OnAchievementsLoginSuccess(const char*, u32, u32, u32)
{
}

void Host::OnAchievementsRefreshed()
{
}

void Host::OnAchievementsHardcoreModeChanged(bool)
{
}

void Host::OnCoverDownloaderOpenRequested()
{
}

bool Host::ShouldPreferHostFileSelector()
{
  return false;
}

void Host::OpenHostFileSelectorAsync(std::string_view, bool, FileSelectorCallback callback, FileSelectorFilters,
                                     std::string_view)
{
  callback(std::string());
}

std::optional<u32> InputManager::ConvertHostKeyboardStringToCode(std::string_view)
{
  return std::nullopt;
}

std::optional<std::string> InputManager::ConvertHostKeyboardCodeToString(u32)
{
  return std::nullopt;
}

const char* InputManager::ConvertHostKeyboardCodeToIcon(u32)
{
  return nullptr;
}

void Host::AddFixedInputBindings(SettingsInterface&)
{
}

void Host::OnInputDeviceConnected(std::string_view, std::string_view)
{
}

void Host::OnInputDeviceDisconnected(InputBindingKey, std::string_view)
{
}

std::optional<WindowInfo> Host::GetTopLevelWindowInfo()
{
  return std::nullopt;
}

void Host::RefreshGameListAsync(bool)
{
}

void Host::CancelGameListRefresh()
{
}

BEGIN_HOTKEY_LIST(g_host_hotkeys)
END_HOTKEY_LIST()

// util's InputManager lists core's hotkeys, but util comes after core on the link line: naming them here pulls them in.
[[gnu::used]] static const void* const s_common_hotkeys = &g_common_hotkeys;

// What util/platform_misc_*.cpp gives the desktop builds.
bool PlatformMisc::InitializeSocketSupport(Error*)
{
  return true;
}

void PlatformMisc::SuspendScreensaver()
{
}

void PlatformMisc::ResumeScreensaver()
{
}

size_t PlatformMisc::GetRuntimePageSize()
{
  const long size = sysconf(_SC_PAGESIZE);
  return (size > 0) ? static_cast<size_t>(size) : 0;
}

bool PlatformMisc::PlaySoundAsync(const char*)
{
  return false;
}

// No network from the core (achievements and covers are the frontend's).
std::unique_ptr<HTTPDownloader> HTTPDownloader::Create(std::string)
{
  return {};
}

// ---------------------------------------------------------------------------------------------------------------------
// libretro
// ---------------------------------------------------------------------------------------------------------------------

RETRO_API void retro_set_environment(retro_environment_t cb)
{
  s_environment = cb;
  bool no_game = false;
  cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_game);
  DeclareDiskControl();
}

RETRO_API void retro_set_video_refresh(retro_video_refresh_t cb)
{
  s_video_refresh = cb;
}

RETRO_API void retro_set_audio_sample(retro_audio_sample_t)
{
}

RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb)
{
  s_audio_batch = cb;
}

RETRO_API void retro_set_input_poll(retro_input_poll_t cb)
{
  s_input_poll = cb;
}

RETRO_API void retro_set_input_state(retro_input_state_t cb)
{
  s_input_state = cb;
}

RETRO_API unsigned retro_api_version(void)
{
  return RETRO_API_VERSION;
}

RETRO_API void retro_get_system_info(retro_system_info* info)
{
  std::memset(info, 0, sizeof(*info));
  info->library_name = "DuckStation";
  info->library_version = "0.1-gpl";
  info->valid_extensions = "cue|chd|pbp|m3u|ecm|mds|bin|img|iso|exe|psexe|psf";
  info->need_fullpath = true;
  info->block_extract = true;
}

RETRO_API void retro_get_system_av_info(retro_system_av_info* info)
{
  std::memset(info, 0, sizeof(*info));
  info->timing.fps = s_fps;
  info->timing.sample_rate = static_cast<double>(SPU::SAMPLE_RATE);
  info->geometry.base_width = s_frame_width ? s_frame_width : 320;
  info->geometry.base_height = s_frame_height ? s_frame_height : 240;
  info->geometry.max_width = 1024 * 16;
  info->geometry.max_height = 512 * 16;
  info->geometry.aspect_ratio = s_frame_aspect > 0.0f ? s_frame_aspect : 4.0f / 3.0f;
}

RETRO_API void retro_init(void)
{
  retro_log_callback log = {};
  if (s_environment && s_environment(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log))
    s_log = log.log;
  Log::SetConsoleOutputParams(false);
  Log::SetFileOutputParams(false, nullptr);
  Log::RegisterCallback(&LogCallback, nullptr);
  Log::SetLogLevel(LOGLEVEL_INFO);

  const char* system_dir = nullptr;
  if (s_environment && s_environment(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &system_dir) && system_dir)
    s_system_dir = system_dir;
  else
    s_system_dir = ".";

  DeclareOptions();

  Error error;
  if (!System::Internal::PerformEarlyHardwareChecks(&error) || !System::Internal::ProcessStartup(&error))
    ERROR_LOG("DuckStation could not start: {}", error.GetDescription());
}

RETRO_API void retro_deinit(void)
{
  Shutdown();
  System::Internal::ProcessShutdown();
  // Before this library is unloaded: the host's fault handlers come back.
  PageFaultHandler::Uninstall();
  Log::UnregisterCallback(&LogCallback, nullptr);
  s_log = nullptr;
}

RETRO_API void retro_set_controller_port_device(unsigned, unsigned)
{
}

RETRO_API void retro_reset(void)
{
  if (s_booted)
    System::ResetSystem();
}

RETRO_API bool retro_load_game(const retro_game_info* game)
{
  if (!game || !game->path)
    return false;

  s_game_path = game->path;
  s_booted = false;
  s_boot_failed = false;

  // The disc must open, and tells the region (the BIOS for it, and the frame rate until the console runs).
  ConsoleRegion region = ConsoleRegion::NTSC_U;
  if (System::IsLoadableFilename(s_game_path) && !System::IsExeFileName(s_game_path) &&
      !System::IsPsfFileName(s_game_path))
  {
    Error error;
    std::unique_ptr<CDImage> image = CDImage::Open(s_game_path.c_str(), false, &error);
    if (!image)
    {
      ERROR_LOG("Cannot open {}: {}", s_game_path, error.GetDescription());
      return false;
    }
    region = System::GetConsoleRegionForDiscRegion(System::GetRegionForImage(image.get()));
  }
  s_pal = (region == ConsoleRegion::PAL);
  s_fps = s_pal ? 50.0 : 59.94;

  MakeSettings(region);

  Error error;
  if (!System::Internal::CPUThreadInitialize(&error))
  {
    ERROR_LOG("DuckStation could not start: {}", error.GetDescription());
    return false;
  }
  s_started = true;

  retro_pixel_format format = RETRO_PIXEL_FORMAT_XRGB8888;
  s_environment(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &format);

  s_hw_render = {};
  s_hw_render.context_type = RETRO_HW_CONTEXT_VULKAN;
  s_hw_render.version_major = VK_API_VERSION_1_1;
  s_hw_render.context_reset = &ContextReset;
  s_hw_render.context_destroy = &ContextDestroy;
  if (!s_environment(RETRO_ENVIRONMENT_SET_HW_RENDER, &s_hw_render))
  {
    ERROR_LOG("The frontend cannot give us Vulkan");
    Shutdown();
    return false;
  }

  s_negotiation = {};
  s_negotiation.interface_type = RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN;
  s_negotiation.interface_version = 1;
  s_negotiation.create_device = &CreateDevice;
  s_environment(RETRO_ENVIRONMENT_SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE, &s_negotiation);
  return true;
}

RETRO_API bool retro_load_game_special(unsigned, const retro_game_info*, size_t)
{
  return false;
}

RETRO_API void retro_unload_game(void)
{
  Shutdown();
  s_initial_disc = 0;  // the frontend says again before the next game
}

RETRO_API void retro_run(void)
{
  if (!s_booted)
  {
    // Not running (the device is missing, or the game did not start): keep the frontend's picture moving.
    s_input_poll();
    s_video_refresh(nullptr, s_frame_width ? s_frame_width : 320, s_frame_height ? s_frame_height : 240, 0);
    return;
  }

  ApplyChangedOptions();
  UpdateInput();
  s_frame_presented = false;
  System::Execute();
  if (!s_frame_presented)
    s_video_refresh(nullptr, s_frame_width ? s_frame_width : 320, s_frame_height ? s_frame_height : 240, 0);
  SendAudio();
  UpdateTiming();
}

RETRO_API unsigned retro_get_region(void)
{
  return s_pal ? RETRO_REGION_PAL : RETRO_REGION_NTSC;
}

RETRO_API size_t retro_serialize_size(void)
{
  return s_booted ? System::GetLibretroStateSize() : 0;
}

RETRO_API bool retro_serialize(void* data, size_t size)
{
  if (!s_booted)
    return false;

  Error error;
  if (!System::SaveLibretroState(std::span<u8>(static_cast<u8*>(data), size), &error))
  {
    ERROR_LOG("Saving the state failed: {}", error.GetDescription());
    return false;
  }
  return true;
}

RETRO_API bool retro_unserialize(const void* data, size_t size)
{
  if (!s_booted)
    return false;

  Error error;
  if (!System::LoadLibretroState(std::span<const u8>(static_cast<const u8*>(data), size), &error))
  {
    ERROR_LOG("Loading the state failed: {}", error.GetDescription());
    return false;
  }
  return true;
}

RETRO_API void retro_cheat_reset(void)
{
}

RETRO_API void retro_cheat_set(unsigned, bool, const char*)
{
}

RETRO_API void* retro_get_memory_data(unsigned id)
{
  if (!s_booted)
    return nullptr;

  switch (id)
  {
    case RETRO_MEMORY_SAVE_RAM:
    {
      // The game's own card: in its slot, or out (kept here).
      const int slot = GameCardSlot();
      if (MemoryCard* card = slot >= 0 ? Pad::GetMemoryCard(static_cast<u32>(slot)) : nullptr)
        return card->GetData().data();
      if (s_game_card.size() != MemoryCardImage::DATA_SIZE)
      {
        // Never plugged in this run: a formatted card, until the frontend loads the save into it.
        std::unique_ptr<MemoryCard> blank = MemoryCard::Create();
        s_game_card.assign(blank->GetData().begin(), blank->GetData().end());
      }
      return s_game_card.data();
    }
    case RETRO_MEMORY_SYSTEM_RAM:
      return Bus::g_ram;
    default:
      return nullptr;
  }
}

RETRO_API size_t retro_get_memory_size(unsigned id)
{
  if (!s_booted)
    return 0;

  switch (id)
  {
    case RETRO_MEMORY_SAVE_RAM:
      return MemoryCardImage::DATA_SIZE;
    case RETRO_MEMORY_SYSTEM_RAM:
      return Bus::g_ram_size;
    default:
      return 0;
  }
}

// Tamariba: plugs cards into the two slots while the game runs ("game", "none" or "file:<path>"; null leaves a slot
// as it is). The game's own card stays the save RAM wherever it is.
extern "C" RETRO_API bool retro_tamariba_ps1_set_cards(const char* card1, const char* card2)
{
  if (!s_booted)
    return false;
  const char* cards[kPorts] = {card1, card2};
  for (u32 slot = 0; slot < kPorts; slot++)
  {
    if (cards[slot])
      PlugCard(slot, cards[slot]);
  }
  return true;
}
