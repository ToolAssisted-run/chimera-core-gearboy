// gearboy.cpp - Gearboy as a Chimera core: the functions Chimera calls.
//
// The machine is Gearboy's emulator core (extern/gearboy/src), the part its
// libretro core builds. Gearboy's own programs around it (the desktop
// program with its debugger and MCP server, the libretro frontend) are not
// built; this file takes their place inside the sandbox. It loads the game,
// the boot ROM, the Super Game Boy cartridge and the saved data, gives the
// machine the controls, runs it to the end of a picture, and hands Chimera
// the picture and the sound.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sstream>
#include <streambuf>
#include <string>
#include <strings.h>

#include <emulibc.h>
#include <waterbox_settings.h>
#include <waterbox_slots.h>

#include "gearboy.h"

// Gearboy prints its log to standard output unless it is serving its MCP
// server on it; this core prints nothing, so it says it is.
bool g_mcp_stdio_mode = true;

// ---- what went wrong, for Chimera to show ----------------------------------
/* A text a person reads is a key of languages/en.json, each of its
 * arguments after a TEXT_ARG (chimera docs/translations.md). TEXT_ARG is a
 * string of its own so the byte after it is never read as more of its hex. */
#define TEXT_ARG "\x1f"

static char g_loadError[512];

// The consoles, as the Model setting names them.
enum Model { kAuto, kGb, kGbc, kGba, kSgb };

static GearboyCore *g_core;
static Model g_model;
static char g_gameName[256];

// ---- the clock ---------------------------------------------------------------
// Gearboy's cartridges with a clock (MBC3, HuC3, TAMA5) ask the computer
// for the time: to move the clock on by the time that passed since saved
// data was written, and (HuC3, TAMA5) to run the clock at all. Here the time
// is the machine's own: the second the project's clock starts at (the
// sandbox's answer to time(), the same in every run), plus the time the
// machine has run, counted in its cycles. The MBC3's clock itself counts
// the machine's cycles in Gearboy already.
enum : uint64_t { kClockRate = 4194304 };
static time_t g_clockStart;
static uint64_t g_cycles;
ECL_INVISIBLE static time_t g_exportTime;

static time_t machine_time(void)
{
	return g_clockStart + (time_t)(g_cycles / kClockRate);
}

static time_t export_time(void)
{
	return g_exportTime;
}

// ---- the picture -------------------------------------------------------------
// The Game Boy's screen is 160 by 144; the Super Game Boy draws it inside a
// 256 by 224 border. Gearboy draws 16-bit pixels (RGB565) into g_frame;
// each finished picture is turned into the BGRA Chimera reads.
enum { kGbWidth = 160, kGbHeight = 144, kSgbWidth = 256, kSgbHeight = 224 };
static u16 g_frame[kSgbWidth * kSgbHeight];
static uint32_t g_picture[kSgbWidth * kSgbHeight];
static int g_width = kGbWidth, g_height = kGbHeight;
static bool g_render = true;

static void show(void)
{
	GB_RuntimeInfo info;
	g_core->GetRuntimeInfo(info);
	g_width = info.screen_width;
	g_height = info.screen_height;
	if (!g_render)
		return;
	const int n = g_width * g_height;
	for (int i = 0; i < n; i++)
	{
		const unsigned p = g_frame[i];
		const unsigned r = (p >> 11) & 31, g = (p >> 5) & 63, b = p & 31;
		g_picture[i] = 0xFF000000u | ((r << 3 | r >> 2) << 16) | ((g << 2 | g >> 4) << 8) | (b << 3 | b >> 2);
	}
}

// ---- the sound ---------------------------------------------------------------
// Gearboy's sound chip (Blargg's Gb_Snd_Emu) gives 44,100 stereo samples a
// second, about 739 a frame; a frame with the screen off is up to a quarter
// longer.
static s16 g_audio[AUDIO_BUFFER_SIZE];
static int g_audioValues;

// ---- the controls ------------------------------------------------------------
// The Game Boy's eight buttons. The Super Game Boy reads up to four pads,
// when the game asks the SNES for more than one. Power turns the console
// off and on again; the cartridge's battery-backed memory stays. A cartridge
// with a tilt sensor (MBC7: Kirby Tilt 'n' Tumble) reads P1's two tilt axes.
enum { kMaxPads = 4, kPadButtons = 8 };
static const char *const kPadNames[kPadButtons] = {
	"Up", "Down", "Left", "Right", "Start", "Select", "B", "A",
};
static const Gameboy_Keys kPadKeys[kPadButtons] = {
	Up_Key, Down_Key, Left_Key, Right_Key, Start_Key, Select_Key, B_Key, A_Key,
};
enum : int
{
	kPads = kMaxPads * kPadButtons,
	kPower = kPads,
	kButtons,
};
static char g_buttonNames[kButtons][16];
static bool g_held[kButtons];
static bool g_powerWasHeld;

enum { kTiltX, kTiltY, kAxes };
static const char *const kAxisNames[kAxes] = { "P1 Tilt X", "P1 Tilt Y" };
static int32_t g_axes[kAxes];

static int g_inputReads;
static bool g_inputRead;

// Chimera's function to call at each read of the buttons, so a Lua script's
// event.oninputpoll runs there (chimera#344). It is the same callback slot in
// every session and no savestate carries it.
ECL_INVISIBLE static uintptr_t (*g_inputPoll)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);

static void input_read(void *)
{
	g_inputReads++;
	if (g_inputPoll)
		g_inputPoll(0, 0, 0, 0, 0, 0);
}

// ---- files and settings ------------------------------------------------------
static void *read_file(const char *name, long *size)
{
	*size = 0;
	FILE *f = fopen(name, "rb");
	if (!f)
		return nullptr;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	void *data = n > 0 ? malloc((size_t)n) : nullptr;
	if (data && fread(data, 1, (size_t)n, f) != (size_t)n)
	{
		free(data);
		data = nullptr;
	}
	fclose(f);
	*size = n;
	return data;
}

static bool ends_with(const char *s, const char *suffix)
{
	const size_t n = strlen(s), m = strlen(suffix);
	return n >= m && !strcasecmp(s + n - m, suffix);
}

static bool setting_is(const char *name, const char *value, const char *dflt)
{
	char v[32];
	snprintf(v, sizeof v, "%s", dflt);
	wbx_setting_str(name, v, sizeof v);
	return !strcmp(v, value);
}

static Model model_setting(void)
{
	char v[16] = "";
	wbx_setting_str("model", v, sizeof v);
	if (!strcmp(v, "GB"))
		return kGb;
	if (!strcmp(v, "GBC"))
		return kGbc;
	if (!strcmp(v, "GBA"))
		return kGba;
	if (!strcmp(v, "SGB"))
		return kSgb;
	return kAuto;
}

// The DMG palettes: Gearboy's own green, and greys.
static void set_palette(void)
{
	GB_Color c[4];
	if (setting_is("palette", "grey", "green"))
	{
		const u8 grey[4] = { 0xFF, 0xAA, 0x55, 0x00 };
		for (int i = 0; i < 4; i++)
			c[i].red = c[i].green = c[i].blue = grey[i];
	}
	else
	{
		const u8 green[4][3] = { { 0x87, 0x96, 0x03 }, { 0x4D, 0x6B, 0x03 }, { 0x2B, 0x55, 0x03 }, { 0x14, 0x44, 0x03 } };
		for (int i = 0; i < 4; i++)
		{
			c[i].red = green[i][0];
			c[i].green = green[i][1];
			c[i].blue = green[i][2];
		}
	}
	g_core->SetDMGPalette(c[0], c[1], c[2], c[3]);
}

// A boot ROM of its exact size into memory, or the load error.
static u8 *boot_rom(const char *id, long want, long *got)
{
	u8 *data = (u8 *)read_file(id, got);
	if (!data || *got != want)
	{
		free(data);
		snprintf(g_loadError, sizeof g_loadError, "error.boot_rom" TEXT_ARG "%s" TEXT_ARG "%ld", id, want);
		return nullptr;
	}
	return data;
}

// Gearboy's battery-backed memory: a cartridge with a battery, and the
// mappers Gearboy keeps memory for without one (its SaveRam's list).
static bool persistent(void)
{
	MemoryRule *rule = g_core->GetMemory()->GetCurrentRule();
	if (!rule)
		return false;
	if (g_core->GetCartridge()->HasBattery())
		return true;
	const u8 t = rule->GetMapperType();
	switch (t)
	{
	case Cartridge::CartridgeMBC6:
	case Cartridge::CartridgeLiCheng:
	case Cartridge::CartridgeNTNew:
	case Cartridge::CartridgeGGB81:
	case Cartridge::CartridgeHitek:
	case Cartridge::CartridgeSintax:
		return true;
	case Cartridge::CartridgeVF001:
	case Cartridge::CartridgeVF001A:
	case Cartridge::CartridgeNTOld1:
	case Cartridge::CartridgeNTOld2:
		return rule->GetRamSize() > 0;
	default:
		return false;
	}
}

static bool force_dmg(void) { return g_model == kGb || g_model == kSgb; }

// ---- Chimera's calls -----------------------------------------------------------
extern "C" {

ECL_EXPORT void SetInputPollCallback(uint64_t fn)
{
	g_inputPoll = (uintptr_t (*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t))(uintptr_t)fn;
}

ECL_EXPORT const char *GetLoadError(void) { return g_loadError; }
ECL_EXPORT const char *DescribeDips(void) { return ""; }

ECL_EXPORT int Init(void)
{
	g_loadError[0] = '\0';

	// the game: the project's, or a game opened with no project, which is
	// mounted under its own name
	g_gameName[0] = '\0';
	if (wbx_slot_count("game") > 0)
		wbx_slot_name("game", 0, g_gameName, sizeof g_gameName);
	else
	{
		FILE *f = fopen("rom.name", "rb");
		if (f)
		{
			size_t got = fread(g_gameName, 1, sizeof g_gameName - 1, f);
			g_gameName[got] = '\0';
			fclose(f);
		}
	}
	if (!g_gameName[0])
	{
		snprintf(g_loadError, sizeof g_loadError, "error.no_file");
		return 0;
	}
	long size = 0;
	u8 *rom = (u8 *)read_file(g_gameName, &size);
	if (!rom)
	{
		snprintf(g_loadError, sizeof g_loadError, "error.not_read" TEXT_ARG "%s", g_gameName);
		return 0;
	}
	if (size < 0x150)
	{
		free(rom);
		snprintf(g_loadError, sizeof g_loadError, "error.too_short" TEXT_ARG "%s" TEXT_ARG "%ld", g_gameName, size);
		return 0;
	}

	// with no Model given (the test runners need not give one), the Game
	// Boy Color for a .gbc file, the Super Game Boy for a .sgb one, and the
	// Game Boy for anything else
	g_model = model_setting();
	if (g_model == kAuto)
		g_model = ends_with(g_gameName, ".gbc") ? kGbc : ends_with(g_gameName, ".sgb") ? kSgb : kGb;

	g_clockStart = time(nullptr);
	g_cycles = 0;
	Cartridge::SetRTCTimeSource(machine_time);

	g_core = new GearboyCore();
	g_core->Init(GB_PIXEL_RGB565);
	g_core->SetSoundSampleRate(44100);
	set_palette();
	g_core->EnableColorCorrection(setting_is("colorCorrection", "on", "off"));

	// The boot ROM: the console's own start-up program, which shows the logo
	// and then starts the game. Without it, Gearboy starts the game in the
	// state the boot ROM would have left the console in. Gearboy runs the
	// Game Boy's and the Game Boy Color's; the Game Boy Advance and the
	// Super Game Boy always start without one.
	Memory *mem = g_core->GetMemory();
	mem->EnableBootromDMG(false);
	mem->EnableBootromGBC(false);
	if ((g_model == kGb || g_model == kGbc) && setting_is("bootRom", "on", "off"))
	{
		const bool cgb = g_model == kGbc;
		const char *id = cgb ? "cgb_boot.bin" : "dmg_boot.bin";
		long got = 0;
		u8 *boot = boot_rom(id, cgb ? 0x900 : 0x100, &got);
		if (!boot)
		{
			free(rom);
			return 0;
		}
		mem->LoadBootromFromBuffer(boot, (int)got, cgb);
		free(boot);
		if (cgb)
			mem->EnableBootromGBC(true);
		else
			mem->EnableBootromDMG(true);
	}

	// The Super Game Boy: Gearboy works out what its SNES side does for a
	// game (borders, palettes, four pads) itself. The Super Game Boy's own
	// cartridge, given as firmware, adds its default border and palette,
	// and the Super Game Boy mode for games that do not ask for it.
	const bool sgb = g_model == kSgb;
	g_core->SetSGBEnabled(sgb);
	g_core->SetSGBModel(sgb);
	g_core->SetSGBBorder(true);
	g_core->SetSGBAllGames(false);
	if (sgb)
	{
		const char *id = setting_is("sgbCartridge", "SGB1", "none") ? "SGB1.sfc"
			: setting_is("sgbCartridge", "SGB2", "none") ? "SGB2.sfc" : nullptr;
		if (id)
		{
			long got = 0;
			u8 *bios = (u8 *)read_file(id, &got);
			if (!bios)
			{
				free(rom);
				snprintf(g_loadError, sizeof g_loadError, "error.sgb_missing" TEXT_ARG "%s", id);
				return 0;
			}
			const bool known = g_core->GetSGB()->LoadBIOSFromBuffer(bios, (int)got);
			free(bios);
			if (!known)
			{
				free(rom);
				snprintf(g_loadError, sizeof g_loadError, "error.sgb_cartridge" TEXT_ARG "%s", id);
				return 0;
			}
			g_core->SetSGBAllGames(true);
		}
	}

	if (!g_core->LoadROMFromBuffer(rom, (int)size, force_dmg(), Cartridge::CartridgeNotSupported, g_model == kGba))
	{
		free(rom);
		snprintf(g_loadError, sizeof g_loadError, "error.not_loaded" TEXT_ARG "%s", g_gameName);
		return 0;
	}
	free(rom);

	// the cartridge's memory at the start: empty, or the project's saved data
	if (wbx_slot_count("savedata") > 0)
	{
		char name[256];
		if (wbx_slot_name("savedata", 0, name, sizeof name))
		{
			long n = 0;
			char *saved = (char *)read_file(name, &n);
			if (saved && n > 0)
			{
				std::istringstream in(std::string(saved, (size_t)n));
				g_core->GetMemory()->GetCurrentRule()->LoadRam(in, (s32)n);
			}
			free(saved);
		}
	}

	g_core->GetInput()->SetReadHook(input_read, nullptr);

	for (int p = 0; p < kMaxPads; p++)
		for (int b = 0; b < kPadButtons; b++)
			snprintf(g_buttonNames[p * kPadButtons + b], sizeof g_buttonNames[0], "P%d %s", p + 1, kPadNames[b]);
	snprintf(g_buttonNames[kPower], sizeof g_buttonNames[0], "Power");
	memset(g_held, 0, sizeof g_held);
	memset(g_axes, 0, sizeof g_axes);
	memset(g_picture, 0, sizeof g_picture);
	memset(g_frame, 0, sizeof g_frame);
	g_powerWasHeld = false;
	show();
	return 1;
}

ECL_EXPORT int GetButtonCount(void) { return kButtons; }
ECL_EXPORT const char *GetButtonName(int32_t i) { return i >= 0 && i < kButtons ? g_buttonNames[i] : ""; }
ECL_EXPORT int IsButtonActive(int32_t i)
{
	if (i < 0 || i >= kButtons)
		return 0;
	// P2 to P4 are the Super Game Boy's
	if (i < kPads)
		return i < kPadButtons || g_model == kSgb;
	return 1;
}
ECL_EXPORT void SetButton(int32_t i, int32_t state)
{
	if (i >= 0 && i < kButtons)
		g_held[i] = state != 0;
}

static bool tilt_cartridge(void)
{
	return g_core && g_core->GetMemory()->GetCurrentRule()
		&& g_core->GetMemory()->GetCurrentRule()->GetMapperType() == Cartridge::CartridgeMBC7;
}

ECL_EXPORT int GetAxisCount(void) { return kAxes; }
ECL_EXPORT const char *GetAxisName(int32_t i) { return i >= 0 && i < kAxes ? kAxisNames[i] : ""; }
ECL_EXPORT int IsAxisActive(int32_t i) { return i >= 0 && i < kAxes && tilt_cartridge(); }
ECL_EXPORT void SetAxis(int32_t i, int32_t value)
{
	if (i >= 0 && i < kAxes)
		g_axes[i] = value < -32768 ? -32768 : value > 32767 ? 32767 : value;
}

ECL_EXPORT void FrameAdvance(uint64_t packed)
{
	(void)packed;
	g_inputReads = 0;

	// Power: pressed (held now and not on the last frame), the console
	// starts again from power-on, with the cartridge's memory kept
	const bool power = g_held[kPower];
	if (power && !g_powerWasHeld)
		g_core->ResetROMPreservingRAM(force_dmg(), Cartridge::CartridgeNotSupported, g_model == kGba);
	g_powerWasHeld = power;

	for (int p = 0; p < kMaxPads; p++)
		for (int b = 0; b < kPadButtons; b++)
		{
			if (g_held[p * kPadButtons + b])
				g_core->KeyPressed(kPadKeys[b], p);
			else
				g_core->KeyReleased(kPadKeys[b], p);
		}

	// The tilt, -32768 to 32767 each way, is Gearboy's -4 to 4 (as its
	// libretro core clamps it): 8,192 a step.
	g_core->SetAccelerometer(g_axes[kTiltX] / 8192.0, g_axes[kTiltY] / 8192.0);

	// one frame: to the start of the next vertical blank, or a frame and a
	// quarter's time when the screen is off
	const u64 before = g_core->GetMasterClockCycles();
	int values = 0;
	g_core->RunToVBlank(g_frame, g_audio, &values, false, nullptr, true);
	const u64 after = g_core->GetMasterClockCycles();
	if (after > before)
		g_cycles += after - before;
	g_audioValues = values;
	show();
	g_inputRead = g_inputReads > 0;
}

ECL_EXPORT int InputWasRead(void) { return g_inputRead; }
ECL_EXPORT void SetRenderingEnabled(int on) { g_render = on != 0; }
ECL_EXPORT uint32_t *GetVideoBgra(void) { return g_picture; }
ECL_EXPORT int GetVideoWidth(void) { return g_width; }
ECL_EXPORT int GetVideoHeight(void) { return g_height; }
ECL_EXPORT int GetDisplayAspectX(void) { return g_width; }
ECL_EXPORT int GetDisplayAspectY(void) { return g_height; }
// 4,194,304 cycles a second over 70,224 a frame
ECL_EXPORT int GetVsyncNumerator(void) { return 4194304; }
ECL_EXPORT int GetVsyncDenominator(void) { return 70224; }
ECL_EXPORT int16_t *GetAudio(void) { return g_audio; }
ECL_EXPORT int GetAudioSampleCount(void) { return g_audioValues / 2; }

// ---- the memory ----------------------------------------------------------------
// The cartridge's ROM (read only), the video memory, the work memory, the
// cartridge's memory, the sprite table (OAM) and the processor's high memory
// (HRAM). The Game Boy Color has a second bank of video memory, shown as
// VRAM1, and eight banks of work memory (32 KB) where a Game Boy has two.
enum Domain { kRom, kVram, kVram1, kWram, kCartRam, kOam, kHram, kDomains };
static const char *const kDomainNames[kDomains] = { "ROM", "VRAM", "VRAM1", "WRAM", "CartRAM", "OAM", "HRAM" };

static bool domain(int i, u8 **data, int64_t *length)
{
	*data = nullptr;
	*length = 0;
	if (!g_core || i < 0 || i >= kDomains)
		return false;
	Memory *m = g_core->GetMemory();
	const bool cgb = g_core->IsCGB();
	switch (i)
	{
	case kRom:
		*data = g_core->GetCartridge()->GetTheROM();
		*length = g_core->GetCartridge()->GetTotalSize();
		break;
	case kVram:
		*data = m->GetVRAMBank0();
		*length = 0x2000;
		break;
	case kVram1:
		*data = cgb ? m->GetVRAMBank1() : nullptr;
		*length = cgb ? 0x2000 : 0;
		break;
	case kWram:
		*data = cgb ? m->GetCGBRAM() : m->GetMemoryMap() + 0xC000;
		*length = cgb ? 0x8000 : 0x2000;
		break;
	case kCartRam:
	{
		MemoryRule *rule = m->GetCurrentRule();
		*data = rule ? rule->GetRamBanks() : nullptr;
		*length = *data ? (int64_t)rule->GetRamSize() : 0;
		break;
	}
	case kOam:
		*data = m->GetMemoryMap() + 0xFE00;
		*length = 0xA0;
		break;
	case kHram:
		*data = m->GetMemoryMap() + 0xFF80;
		*length = 0x7F;
		break;
	}
	return true;
}

ECL_EXPORT int GetMemoryDomainCount(void) { return kDomains; }
ECL_EXPORT const char *GetMemoryDomainName(int i) { return i >= 0 && i < kDomains ? kDomainNames[i] : ""; }
ECL_EXPORT uint8_t *GetMemoryDomainPtr(int i)
{
	u8 *d;
	int64_t n;
	return domain(i, &d, &n) && n > 0 ? d : nullptr;
}
ECL_EXPORT int64_t GetMemoryDomainSize(int i)
{
	u8 *d;
	int64_t n;
	return domain(i, &d, &n) ? n : 0;
}
ECL_EXPORT int GetMemoryDomainWritable(int i) { return i > kRom && i < kDomains; }

// The system bus: the 64 KB the Game Boy's processor sees, read without
// touching the machine. The cartridge's area is read as Gearboy's debugger
// reads it (the boot ROM while it is mapped, the banks selected), the video
// and work memory from the banks selected, and the rest (sprites, I/O
// registers, high memory) as Gearboy keeps it in its memory map: an I/O
// register whose value Gearboy works out when it is read (the joypad, the
// sound) shows what was last kept there.
static const int64_t kBusRanges[] = { 0, 0x10000, 0, 0 };
ECL_INVISIBLE static uint8_t g_busBuf[0x10000];

static u8 bus_peek(u16 a)
{
	Memory *m = g_core->GetMemory();
	if (a < 0x8000)
		return m->DebugRetrieve(a);
	if (a < 0xA000)
		return m->GetVRAM()[a - 0x8000];
	if (a < 0xC000)
		return m->GetCurrentRule()->DebugRead(a);
	if (a < 0xD000)
		return m->GetWRAM0()[a - 0xC000];
	if (a < 0xE000)
		return m->GetWRAM1()[a - 0xD000];
	if (a < 0xFE00)
		return bus_peek((u16)(a - 0x2000));
	return m->GetMemoryMap()[a];
}

ECL_EXPORT int32_t GetBusCount(void) { return 1; }
ECL_EXPORT const char *GetBusName(int32_t bus) { return bus == 0 ? "System Bus" : ""; }
ECL_EXPORT int64_t GetBusSize(int32_t bus) { return bus == 0 ? 0x10000 : 0; }
ECL_EXPORT int32_t GetBusWritable(int32_t bus) { return bus == 0; }
ECL_EXPORT int32_t PeekBus(int32_t bus, int32_t addr)
{
	if (bus != 0 || !g_core || addr < 0 || addr >= 0x10000)
		return 0;
	return bus_peek((u16)addr);
}
ECL_EXPORT const int64_t *GetBusRanges(int32_t bus) { return bus == 0 ? kBusRanges : nullptr; }
ECL_EXPORT const uint8_t *ReadBus(int32_t bus, int64_t addr, int32_t len)
{
	if (bus != 0 || !g_core || addr < 0 || len < 0 || addr + len > 0x10000)
		return nullptr;
	for (int32_t i = 0; i < len; i++)
		g_busBuf[i] = bus_peek((u16)(addr + i));
	return g_busBuf;
}
// A write from a tool goes into the memory behind the address: the video,
// work, sprite and high memory and the I/O registers' kept values, and the
// cartridge's memory through its chip. The ROM is not written.
ECL_EXPORT void PokeBus(int32_t bus, int32_t addr, int32_t value)
{
	if (bus != 0 || !g_core || addr < 0x8000 || addr >= 0x10000)
		return;
	Memory *m = g_core->GetMemory();
	const u8 v = (u8)value;
	if (addr < 0xA000)
		m->GetVRAM()[addr - 0x8000] = v;
	else if (addr < 0xC000)
		m->GetCurrentRule()->PerformWrite((u16)addr, v);
	else if (addr < 0xD000)
		m->GetWRAM0()[addr - 0xC000] = v;
	else if (addr < 0xE000)
		m->GetWRAM1()[addr - 0xD000] = v;
	else if (addr < 0xFE00)
		PokeBus(0, addr - 0x2000, value);
	else
		m->Load((u16)addr, v);
}

// ---- the processor's registers ---------------------------------------------------
static const char *const kRegisters[] = { "PC", "SP", "AF", "BC", "DE", "HL" };
enum { kRegisterCount = sizeof kRegisters / sizeof kRegisters[0] };

static SixteenBitRegister *reg(int i)
{
	Processor::ProcessorState *s = g_core->GetProcessor()->GetState();
	SixteenBitRegister *r[kRegisterCount] = { s->PC, s->SP, s->AF, s->BC, s->DE, s->HL };
	return r[i];
}

ECL_EXPORT int32_t GetRegisterCount(void) { return kRegisterCount; }
ECL_EXPORT const char *GetRegisterName(int32_t i) { return i >= 0 && i < kRegisterCount ? kRegisters[i] : ""; }
ECL_EXPORT int64_t GetRegisterValue(int32_t i) { return g_core && i >= 0 && i < kRegisterCount ? reg(i)->GetValue() : 0; }
ECL_EXPORT int32_t GetRegisterBits(int32_t i) { return i >= 0 && i < kRegisterCount ? 16 : 0; }
ECL_EXPORT const char *GetRegisterRoles(void) { return "pc PC\nsp SP\nflags AF\n"; }

// ---- Export Save Data ------------------------------------------------------------
// The cartridge's battery-backed memory as Gearboy writes its .sav files:
// the memory, and after it, for a cartridge with a clock, the clock's
// registers and the time they were written. Writing it reads the clock;
// the time the cartridge holds is given back unchanged, so exporting
// changes nothing in the machine.
// The largest is an MBC6's: 1 MB of flash memory, 32 KB of memory and a
// little more. It is written into memory no state carries, without the
// heap, so exporting leaves the machine's memory as it was.
enum { kSaveMax = 0x120000 };
ECL_INVISIBLE static char g_saveBuf[kSaveMax];
ECL_INVISIBLE static size_t g_saveSize;
ECL_INVISIBLE static char g_saveName[256 + 8];

struct FixedBuffer : std::streambuf
{
	FixedBuffer(char *p, size_t n) { setp(p, p + n); }
	size_t size() const { return (size_t)(pptr() - pbase()); }
};

static void export_save(void)
{
	g_exportTime = g_core->GetCartridge()->GetCurrentRTC();
	Cartridge::SetRTCTimeSource(export_time);
	FixedBuffer buf(g_saveBuf, sizeof g_saveBuf);
	std::ostream out(&buf);
	g_core->GetMemory()->GetCurrentRule()->SaveRam(out);
	Cartridge::SetRTCTimeSource(machine_time);
	g_saveSize = buf.size();
}

ECL_EXPORT int32_t GetSaveDataFileCount(void) { return g_core && persistent() ? 1 : 0; }
ECL_EXPORT const char *GetSaveDataFileName(int32_t i)
{
	if (i != 0 || !g_core)
		return "";
	char base[256];
	snprintf(base, sizeof base, "%s", g_gameName);
	char *dot = strrchr(base, '.');
	if (dot)
		*dot = '\0';
	snprintf(g_saveName, sizeof g_saveName, "%s.sav", base);
	return g_saveName;
}
ECL_EXPORT int64_t GetSaveDataFileSize(int32_t i)
{
	if (i != 0 || !g_core || !persistent())
		return 0;
	export_save();
	return (int64_t)g_saveSize;
}
ECL_EXPORT const uint8_t *GetSaveDataFileBuffer(int32_t i)
{
	if (i != 0 || !g_core || !persistent())
		return nullptr;
	export_save();
	return (const uint8_t *)g_saveBuf;
}

} // extern "C"
