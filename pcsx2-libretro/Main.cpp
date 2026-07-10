// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// pcsx2-libretro — libretro core frontend (yaps2_libretro).
//
// Milestone 1 scaffold: the core builds as a shared library, exposes the
// libretro v1 API, and boots the VM headlessly (GS renderer forced to Null,
// no host display surface). retro_run() presents a placeholder XRGB8888
// framebuffer while the emulated machine free-runs on a dedicated CPU
// thread — the same CPU-thread state machine the SDL frontend uses.
//
// Next milestones (mirrors the proven lrps2-libretro architecture):
//  M2: Vulkan HW render via retro_hw_render_context_negotiation_interface
//      (vkCreateInstance/Device wrapped so GSDeviceVK's own init receives the
//      frontend-negotiated instance/device), frame handoff via set_image.
//  M3: retro_run frame pacing (block until MTGS presents exactly one frame),
//      audio batching, libretro input -> Pad, core options -> settings.
//  M4: save states over retro_serialize, disk control, memcard-per-content.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "libretro.h"

#include "fmt/format.h"

#include "common/Assertions.h"
#include "common/Console.h"
#include "common/CrashHandler.h"
#include "common/Error.h"
#include "common/FileSystem.h"
#include "common/MemorySettingsInterface.h"
#include "common/Path.h"
#include "common/ProgressCallback.h"
#include "common/SmallString.h"
#include "common/StringUtil.h"
#include "common/Threading.h"

#include "pcsx2/PrecompiledHeader.h"

#include "pcsx2/Achievements.h"
#include "pcsx2/CDVD/CDVDcommon.h"
#include "pcsx2/GS.h"
#include "pcsx2/GameList.h"
#include "pcsx2/Host.h"
#include "pcsx2/INISettingsInterface.h"
#include "pcsx2/ImGui/FullscreenUI.h"
#include "pcsx2/ImGui/ImGuiFullscreen.h"
#include "pcsx2/ImGui/ImGuiManager.h"
#include "pcsx2/Input/InputManager.h"
#include "pcsx2/MTGS.h"
#include "pcsx2/PerformanceMetrics.h"
#include "pcsx2/Memory.h"
#include "pcsx2/SIO/Pad/Pad.h"
#include "pcsx2/VMManager.h"

#include "svnrev.h"

//////////////////////////////////////////////////////////////////////////
// libretro callbacks + core state
//////////////////////////////////////////////////////////////////////////

static retro_environment_t environ_cb;
static retro_video_refresh_t video_cb;
static retro_audio_sample_t audio_sample_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;
static retro_log_printf_t log_cb;

namespace LibretroCore
{
	static bool InitializeConfig();
	static void CPUThreadMain(VMBootParameters initial_params);
	static void DrainCPUThreadQueue();

	// Placeholder frame while there is no real presentation path (M2).
	static constexpr u32 kFrameWidth = 640;
	static constexpr u32 kFrameHeight = 448;
	static std::vector<u32> s_frame_buffer;

	static std::string s_content_path;
	static std::thread s_cpu_thread;
	static std::atomic<bool> s_vm_thread_running{false};
} // namespace LibretroCore

// Settings persistence (INI under the frontend's system directory).
static std::unique_ptr<INISettingsInterface> s_base_settings;
static std::unique_ptr<INISettingsInterface> s_secrets_settings;

static std::atomic<bool> s_shutdown_requested{false};

// Pending CPU-thread callbacks queued by Host::RunOnCPUThread (same
// mechanism as the SDL frontend; drained by Host::PumpMessagesOnCPUThread
// every vsync).
static std::mutex s_cpu_queue_lock;
static std::deque<std::function<void()>> s_cpu_queue;
static std::condition_variable s_cpu_queue_cv;
static std::atomic<std::thread::id> s_cpu_thread_id{};

static void FallbackLog(enum retro_log_level level, const char* fmt, ...)
{
	va_list va;
	va_start(va, fmt);
	std::vfprintf(stderr, fmt, va);
	va_end(va);
}

//////////////////////////////////////////////////////////////////////////
// Settings + lifecycle
//////////////////////////////////////////////////////////////////////////

bool LibretroCore::InitializeConfig()
{
	// Everything (resources, bios, memcards, cache, ini) lives under
	// <retro system dir>/pcsx2 so the core is self-contained and shares BIOS
	// files with other PS2 cores' conventions.
	const char* system_base = nullptr;
	if (!environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &system_base) || !system_base)
	{
		log_cb(RETRO_LOG_ERROR, "No system directory from frontend.\n");
		return false;
	}

	EmuFolders::AppRoot = Path::Combine(system_base, "pcsx2");
	EmuFolders::Resources = Path::Combine(EmuFolders::AppRoot, "resources");
	EmuFolders::DataRoot = EmuFolders::AppRoot;

	CrashHandler::SetWriteDirectory(EmuFolders::DataRoot);

	const char* hw_check_error = nullptr;
	if (!VMManager::PerformEarlyHardwareChecks(&hw_check_error))
	{
		log_cb(RETRO_LOG_ERROR, "Early hardware check failed: %s\n",
			hw_check_error ? hw_check_error : "unknown");
		return false;
	}

	// OSD / ImGui font (required by ImGuiManager even when nothing draws).
	{
		const std::string roboto_path =
			EmuFolders::GetOverridableResourcePath("fonts" FS_OSPATH_SEPARATOR_STR "Roboto-Regular.ttf");
		const auto roboto_data = FileSystem::MapBinaryFileForRead(roboto_path.c_str());
		if (!roboto_data.empty())
		{
			std::vector<ImGuiManager::FontInfo> fonts;
			ImGuiManager::FontInfo fi{};
			fi.data = roboto_data;
			fonts.push_back(fi);
			ImGuiManager::SetFonts(std::move(fonts));
		}
		else
		{
			log_cb(RETRO_LOG_WARN, "Missing font resource '%s' (OSD disabled).\n", roboto_path.c_str());
		}
	}

	const std::string ini_path = Path::Combine(EmuFolders::Settings, "yaps2-libretro.ini");
	const bool ini_exists = FileSystem::FileExists(ini_path.c_str());

	s_base_settings = std::make_unique<INISettingsInterface>(ini_path);
	Host::Internal::SetBaseSettingsLayer(s_base_settings.get());

	if (!ini_exists || !s_base_settings->Load() || !VMManager::Internal::CheckSettingsVersion())
		VMManager::SetDefaultSettings(*s_base_settings, true, true, true, true, true);

	const std::string secrets_path = Path::Combine(EmuFolders::Settings, "secrets.ini");
	s_secrets_settings = std::make_unique<INISettingsInterface>(secrets_path);
	Host::Internal::SetSecretsSettingsLayer(s_secrets_settings.get());
	if (FileSystem::FileExists(secrets_path.c_str()))
		s_secrets_settings->Load();

	// Libretro-core overrides. M1: headless Null renderer (no display surface
	// exists yet); SDL input/audio stay available inside the core lib but the
	// libretro input/audio paths will replace them in M3.
	{
		auto lock = Host::GetSettingsLock();
		s_base_settings->SetIntValue("EmuCore/GS", "Renderer", static_cast<int>(GSRendererType::Null));
		s_base_settings->SetBoolValue("InputSources", "SDL", false);
		if (!s_base_settings->ContainsValue("SPU2/Output", "OutputModule"))
			s_base_settings->SetStringValue("SPU2/Output", "OutputModule", "nullout");
	}

	Error save_error;
	if (!s_base_settings->Save(&save_error))
		Console.ErrorFmt("Failed to save config: {}", save_error.GetDescription());

	VMManager::Internal::LoadStartupSettings();
	return true;
}

//////////////////////////////////////////////////////////////////////////
// Host:: callbacks (adapted from pcsx2-sdl; libretro has no windowing,
// no clipboard, no native file picker)
//////////////////////////////////////////////////////////////////////////

void Host::CommitBaseSettingChanges()
{
	if (!s_base_settings)
		return;
	Error err;
	if (!s_base_settings->Save(&err))
		Console.ErrorFmt("Failed to save settings: {}", err.GetDescription());
}

void Host::LoadSettings(SettingsInterface& si, std::unique_lock<std::mutex>& lock)
{
}

void Host::CheckForSettingsChanges(const Pcsx2Config& old_config)
{
}

bool Host::RequestResetSettings(bool folders, bool core, bool controllers, bool hotkeys, bool ui)
{
	return false;
}

void Host::SetDefaultUISettings(SettingsInterface& si)
{
	si.SetBoolValue("UI", "StartBigPictureMode", false);
}

bool Host::LocaleCircleConfirm()
{
	return false;
}

std::unique_ptr<ProgressCallback> Host::CreateHostProgressCallback()
{
	return ProgressCallback::CreateNullProgressCallback();
}

void Host::ReportInfoAsync(const std::string_view title, const std::string_view message)
{
	if (!title.empty() && !message.empty())
		INFO_LOG("{}: {}", title, message);
	else if (!message.empty())
		INFO_LOG("{}", message);
}

void Host::ReportErrorAsync(const std::string_view title, const std::string_view message)
{
	if (!title.empty() && !message.empty())
		ERROR_LOG("{}: {}", title, message);
	else if (!message.empty())
		ERROR_LOG("{}", message);
}

void Host::OpenURL(const std::string_view url)
{
}

bool Host::CopyTextToClipboard(const std::string_view text)
{
	return false;
}

std::string Host::GetTextFromClipboard()
{
	return std::string();
}

void Host::BeginTextInput()
{
}

void Host::EndTextInput()
{
}

static std::optional<WindowInfo> BuildLibretroWindowInfo()
{
	// M1: no surface at all — the Null renderer is the only allowed provider.
	// M2 will return a WindowInfo describing the negotiated Vulkan context.
	WindowInfo wi;
	wi.type = WindowInfo::Type::Surfaceless;
	wi.surface_width = LibretroCore::kFrameWidth;
	wi.surface_height = LibretroCore::kFrameHeight;
	wi.surface_scale = 1.0f;
	return wi;
}

std::optional<WindowInfo> Host::GetTopLevelWindowInfo()
{
	return BuildLibretroWindowInfo();
}

void Host::OnInputDeviceConnected(const std::string_view identifier, const std::string_view device_name)
{
}

void Host::OnInputDeviceDisconnected(const InputBindingKey key, const std::string_view identifier)
{
}

void Host::SetMouseMode(bool relative_mode, bool hide_cursor)
{
}

void Host::SetMouseLock(bool state)
{
}

std::optional<WindowInfo> Host::AcquireRenderWindow(bool recreate_window)
{
	return BuildLibretroWindowInfo();
}

void Host::ReleaseRenderWindow()
{
}

void Host::BeginPresentFrame()
{
}

void Host::RequestResizeHostDisplay(s32 width, s32 height)
{
}

void Host::OnVMStarting()
{
}

void Host::OnVMStarted()
{
}

void Host::OnVMDestroyed()
{
}

void Host::OnVMPaused()
{
}

void Host::OnVMResumed()
{
}

void Host::OnGameChanged(const std::string& title, const std::string& elf_override, const std::string& disc_path,
	const std::string& disc_serial, u32 disc_crc, u32 current_crc)
{
	if (!title.empty())
		INFO_LOG("Game changed: {} (serial {}, CRC {:08X})", title, disc_serial, current_crc);
}

void Host::OnPerformanceMetricsUpdated()
{
}

void Host::OnSaveStateLoading(const std::string_view filename)
{
}

void Host::OnSaveStateLoaded(const std::string_view filename, bool was_successful)
{
}

void Host::OnSaveStateSaved(const std::string_view filename)
{
}

void LibretroCore::DrainCPUThreadQueue()
{
	for (;;)
	{
		std::function<void()> fn;
		{
			std::lock_guard<std::mutex> lock(s_cpu_queue_lock);
			if (s_cpu_queue.empty())
				return;
			fn = std::move(s_cpu_queue.front());
			s_cpu_queue.pop_front();
		}
		fn();
	}
}

void Host::PumpMessagesOnCPUThread()
{
	if (s_shutdown_requested.load(std::memory_order_acquire) && VMManager::HasValidVM())
		VMManager::SetState(VMState::Stopping);

	LibretroCore::DrainCPUThreadQueue();
}

void Host::RunOnCPUThread(std::function<void()> function, bool block)
{
	if (block)
	{
		if (s_cpu_thread_id.load(std::memory_order_acquire) == std::this_thread::get_id())
		{
			function();
			return;
		}

		std::mutex done_lock;
		std::condition_variable done_cv;
		bool done = false;
		auto wrapped = [&function, &done_lock, &done_cv, &done]() {
			function();
			std::lock_guard<std::mutex> lk(done_lock);
			done = true;
			done_cv.notify_all();
		};
		{
			std::lock_guard<std::mutex> lock(s_cpu_queue_lock);
			s_cpu_queue.emplace_back(std::move(wrapped));
		}
		s_cpu_queue_cv.notify_all();
		std::unique_lock<std::mutex> lk(done_lock);
		done_cv.wait(lk, [&done]() { return done; });
		return;
	}

	{
		std::lock_guard<std::mutex> lock(s_cpu_queue_lock);
		s_cpu_queue.emplace_back(std::move(function));
	}
	s_cpu_queue_cv.notify_all();
}

void Host::RefreshGameListAsync(bool invalidate_cache)
{
	// The frontend owns the game list.
}

void Host::CancelGameListRefresh()
{
}

bool Host::IsFullscreen()
{
	return true;
}

void Host::SetFullscreen(bool enabled)
{
}

void Host::OnCaptureStarted(const std::string& filename)
{
}

void Host::OnCaptureStopped()
{
}

void Host::RequestExitApplication(bool allow_confirm)
{
	s_shutdown_requested.store(true, std::memory_order_release);
	if (VMManager::HasValidVM())
		VMManager::SetState(VMState::Stopping);
}

void Host::RequestExitBigPicture()
{
}

void Host::RequestVMShutdown(bool allow_confirm, bool allow_save_state, bool default_save_state)
{
	VMManager::SetState(VMState::Stopping);
}

void Host::OnAchievementsLoginSuccess(const char* username, u32 points, u32 sc_points, u32 unread_messages)
{
}

void Host::OnAchievementsLoginRequested(Achievements::LoginRequestReason reason)
{
}

void Host::OnAchievementsHardcoreModeChanged(bool enabled)
{
}

void Host::OnAchievementsRefreshed()
{
}

void Host::OnCoverDownloaderOpenRequested()
{
}

void Host::OnCreateMemoryCardOpenRequested()
{
}

bool Host::InBatchMode()
{
	return true;
}

bool Host::InNoGUIMode()
{
	return true;
}

bool Host::ShouldPreferHostFileSelector()
{
	return false;
}

void Host::OpenHostFileSelectorAsync(std::string_view title, bool select_directory, FileSelectorCallback callback,
	FileSelectorFilters filters, std::string_view initial_directory)
{
	callback(std::string());
}

int Host::LocaleSensitiveCompare(std::string_view lhs, std::string_view rhs)
{
	const int res = std::strncmp(lhs.data(), rhs.data(), std::min(lhs.size(), rhs.size()));
	if (res != 0)
		return res;
	return lhs.size() > rhs.size() ? 1 : (lhs.size() < rhs.size() ? -1 : 0);
}

s32 Host::Internal::GetTranslatedStringImpl(
	const std::string_view context, const std::string_view msg, char* tbuf, size_t tbuf_space)
{
	if (msg.size() > tbuf_space)
		return -1;
	if (msg.empty())
		return 0;

	std::memcpy(tbuf, msg.data(), msg.size());
	return static_cast<s32>(msg.size());
}

std::string Host::TranslatePluralToString(const char* context, const char* msg, const char* disambiguation, int count)
{
	TinyString count_str = TinyString::from_format("{}", count);

	std::string ret(msg);
	for (;;)
	{
		std::string::size_type pos = ret.find("%n");
		if (pos == std::string::npos)
			break;
		ret.replace(pos, 2, count_str.view());
	}
	return ret;
}

std::optional<u32> InputManager::ConvertHostKeyboardStringToCode(const std::string_view str)
{
	return std::nullopt;
}

std::optional<std::string> InputManager::ConvertHostKeyboardCodeToString(u32 code)
{
	return std::nullopt;
}

const char* InputManager::ConvertHostKeyboardCodeToIcon(u32 code)
{
	return nullptr;
}

BEGIN_HOTKEY_LIST(g_host_hotkeys)
END_HOTKEY_LIST()

//////////////////////////////////////////////////////////////////////////
// CPU thread
//////////////////////////////////////////////////////////////////////////

void LibretroCore::CPUThreadMain(VMBootParameters initial_params)
{
	Threading::SetNameOfCurrentThread("CPU Thread");
	s_cpu_thread_id.store(std::this_thread::get_id(), std::memory_order_release);
	s_vm_thread_running.store(true, std::memory_order_release);

	if (!VMManager::Internal::CPUThreadInitialize())
	{
		Console.Error("CPU thread init failed.");
		VMManager::Internal::CPUThreadShutdown();
		s_vm_thread_running.store(false, std::memory_order_release);
		return;
	}

	VMManager::ApplySettings();

	std::optional<VMBootParameters> pending_boot = std::move(initial_params);

	while (!s_shutdown_requested.load(std::memory_order_acquire))
	{
		DrainCPUThreadQueue();

		const VMState state = VMManager::GetState();
		switch (state)
		{
			case VMState::Initializing:
				continue;

			case VMState::Running:
				VMManager::Execute();
				continue;

			case VMState::Resetting:
				VMManager::Reset();
				continue;

			case VMState::Stopping:
				VMManager::Shutdown(false);
				continue;

			case VMState::Paused:
			case VMState::Shutdown:
			{
				if (pending_boot.has_value())
				{
					VMBootParameters bp = std::move(pending_boot.value());
					pending_boot.reset();
					const VMBootResult br = VMManager::Initialize(bp);
					if (br != VMBootResult::StartupSuccess)
					{
						Console.ErrorFmt("VMManager::Initialize failed (result {}).", static_cast<int>(br));
						s_shutdown_requested.store(true, std::memory_order_release);
						break;
					}
					VMManager::SetState(VMState::Running);
					continue;
				}

				if (state == VMState::Shutdown)
				{
					// Game exited on its own; nothing more to run.
					s_shutdown_requested.store(true, std::memory_order_release);
					break;
				}

				std::unique_lock<std::mutex> lock(s_cpu_queue_lock);
				s_cpu_queue_cv.wait_for(lock, std::chrono::milliseconds(16),
					[]() { return !s_cpu_queue.empty(); });
				continue;
			}

			default:
				continue;
		}
	}

	if (VMManager::HasValidVM())
		VMManager::Shutdown(false);
	if (MTGS::IsOpen())
	{
		MTGS::SetRunIdle(false);
		MTGS::WaitForClose();
	}

	VMManager::Internal::CPUThreadShutdown();
	s_cpu_thread_id.store(std::thread::id{}, std::memory_order_release);
	s_vm_thread_running.store(false, std::memory_order_release);
}

//////////////////////////////////////////////////////////////////////////
// libretro API
//////////////////////////////////////////////////////////////////////////

RETRO_API unsigned retro_api_version(void)
{
	return RETRO_API_VERSION;
}

RETRO_API void retro_set_environment(retro_environment_t cb)
{
	environ_cb = cb;

	struct retro_log_callback log_iface;
	if (cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log_iface))
		log_cb = log_iface.log;
	else
		log_cb = FallbackLog;

	bool support_no_game = false;
	cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &support_no_game);
}

RETRO_API void retro_set_video_refresh(retro_video_refresh_t cb)
{
	video_cb = cb;
}

RETRO_API void retro_set_audio_sample(retro_audio_sample_t cb)
{
	audio_sample_cb = cb;
}

RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb)
{
	audio_batch_cb = cb;
}

RETRO_API void retro_set_input_poll(retro_input_poll_t cb)
{
	input_poll_cb = cb;
}

RETRO_API void retro_set_input_state(retro_input_state_t cb)
{
	input_state_cb = cb;
}

RETRO_API void retro_init(void)
{
	Log::SetConsoleOutputLevel(LOGLEVEL_INFO);
	LibretroCore::s_frame_buffer.assign(
		LibretroCore::kFrameWidth * LibretroCore::kFrameHeight, 0);
}

RETRO_API void retro_deinit(void)
{
	LibretroCore::s_frame_buffer.clear();
	LibretroCore::s_frame_buffer.shrink_to_fit();
}

RETRO_API void retro_get_system_info(struct retro_system_info* info)
{
	std::memset(info, 0, sizeof(*info));
	info->library_name = "yaps2";
	info->library_version = GIT_REV;
	info->valid_extensions = "elf|iso|ciso|chd|cso|zso|bin|mdf|nrg|dump|gz|img|irx";
	info->need_fullpath = true;
	info->block_extract = true;
}

RETRO_API void retro_get_system_av_info(struct retro_system_av_info* info)
{
	std::memset(info, 0, sizeof(*info));
	info->geometry.base_width = LibretroCore::kFrameWidth;
	info->geometry.base_height = LibretroCore::kFrameHeight;
	info->geometry.max_width = 1920;
	info->geometry.max_height = 1080;
	info->geometry.aspect_ratio = 4.0f / 3.0f;
	info->timing.fps = 59.94;
	info->timing.sample_rate = 48000.0;
}

RETRO_API void retro_set_controller_port_device(unsigned port, unsigned device)
{
}

RETRO_API void retro_reset(void)
{
	if (VMManager::HasValidVM())
		VMManager::SetState(VMState::Resetting);
}

RETRO_API bool retro_load_game(const struct retro_game_info* game)
{
	int format = RETRO_PIXEL_FORMAT_XRGB8888;
	if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &format))
	{
		log_cb(RETRO_LOG_ERROR, "XRGB8888 not supported by frontend.\n");
		return false;
	}

	CrashHandler::Install();

	if (!LibretroCore::InitializeConfig())
	{
		log_cb(RETRO_LOG_ERROR, "Failed to initialize config.\n");
		return false;
	}

	VMBootParameters params;
	if (game && game->path)
	{
		LibretroCore::s_content_path = game->path;
		params.filename = LibretroCore::s_content_path;
	}
	else
	{
		params.source_type = CDVD_SourceType::NoDisc;
	}
	params.fast_boot = true;

	s_shutdown_requested.store(false, std::memory_order_release);
	SysMemory::ReserveMemory();

	LibretroCore::s_cpu_thread = std::thread([params = std::move(params)]() mutable {
		LibretroCore::CPUThreadMain(std::move(params));
	});

	return true;
}

RETRO_API bool retro_load_game_special(unsigned game_type, const struct retro_game_info* info, size_t num_info)
{
	return false;
}

RETRO_API void retro_unload_game(void)
{
	s_shutdown_requested.store(true, std::memory_order_release);
	if (VMManager::HasValidVM())
		VMManager::SetState(VMState::Stopping);
	s_cpu_queue_cv.notify_all();
	if (LibretroCore::s_cpu_thread.joinable())
		LibretroCore::s_cpu_thread.join();

	s_base_settings.reset();
	s_secrets_settings.reset();
	LibretroCore::s_content_path.clear();
}

RETRO_API void retro_run(void)
{
	input_poll_cb();

	// M1: the VM free-runs on the CPU thread with the Null GS renderer; there
	// is no frame handoff yet, so present the placeholder buffer. M2/M3 turn
	// this into "block until MTGS presents one frame, then hand it over".
	video_cb(LibretroCore::s_frame_buffer.data(), LibretroCore::kFrameWidth,
		LibretroCore::kFrameHeight, LibretroCore::kFrameWidth * sizeof(u32));
}

RETRO_API size_t retro_serialize_size(void)
{
	return 0;
}

RETRO_API bool retro_serialize(void* data, size_t size)
{
	return false;
}

RETRO_API bool retro_unserialize(const void* data, size_t size)
{
	return false;
}

RETRO_API void retro_cheat_reset(void)
{
}

RETRO_API void retro_cheat_set(unsigned index, bool enabled, const char* code)
{
}

RETRO_API unsigned retro_get_region(void)
{
	return RETRO_REGION_NTSC;
}

RETRO_API void* retro_get_memory_data(unsigned id)
{
	return nullptr;
}

RETRO_API size_t retro_get_memory_size(unsigned id)
{
	return 0;
}
