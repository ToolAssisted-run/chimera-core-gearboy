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
// The Game Boy Link machine is two Gearboy machines in this one core, joined
// by a link cable: they run side by side, a few cycles at a time, and the
// bytes each sends through its serial port reach the other. Chimera sees one
// machine, with both players' controls and both screens side by side. The
// way the two are stepped and their transfers matched is upstream's own
// (platforms/libretro/libretro_link.cpp, LibretroLink::RunFrame and
// ResolveTransfers), carried over here without its libretro parts.
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

/* this core's windows in Chimera's Emulator menu: miniBox's ready-made Export
 * Save Data and CPU Registers, built on its exports (Chimera has no tool of
 * its own for a core, Chimera issue 610) */
#define WBX_WINDOWS_IMPLEMENTATION
#include <waterbox_windows.h>

// Gearboy prints its log to standard output unless it is serving its MCP
// server on it; this core prints nothing, so it says it is.
bool g_mcp_stdio_mode = true;

// ---- what went wrong, for Chimera to show ----------------------------------
/* A text a person reads is a key of languages/en.json, each of its
 * arguments after a TEXT_ARG (chimera docs/translations.md). TEXT_ARG is a
 * string of its own so the byte after it is never read as more of its hex. */
#define TEXT_ARG "\x1f"

static char g_loadError[512];

// The consoles, as the Model setting names them; LINK is two Game Boys
// joined by a link cable, each one of the first three (linkModel1 and 2).
enum Model { kAuto, kGb, kGbc, kGba, kSgb, kLink };

// ---- the Game Boys -------------------------------------------------------------
enum { kMaxGb = 2, kGbWidth = 160, kGbHeight = 144, kSgbWidth = 256, kSgbHeight = 224 };

struct Player
{
	GearboyCore *core;
	Model model;
	char gameName[256];
	u16 frame[kSgbWidth * kSgbHeight];   // Gearboy draws here (RGB565)
	s16 audio[AUDIO_BUFFER_SIZE];
	int audioValues;
};
static Player g_p[kMaxGb];
static int g_count;            // 1, or 2 for the link machine
static Model g_model;          // the machine's: kLink for the link machine

static bool force_dmg(int i) { return g_p[i].model == kGb || g_p[i].model == kSgb; }
static bool force_gba(int i) { return g_p[i].model == kGba; }

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
// 256 by 224 border; the link machine puts its two screens side by side,
// 320 by 144. Each finished picture is turned into the BGRA Chimera reads.
static uint32_t g_picture[2 * kGbWidth * kSgbHeight];
static int g_width = kGbWidth, g_height = kGbHeight;
static bool g_render = true;

static uint32_t bgra(unsigned p)
{
	const unsigned r = (p >> 11) & 31, g = (p >> 5) & 63, b = p & 31;
	return 0xFF000000u | ((r << 3 | r >> 2) << 16) | ((g << 2 | g >> 4) << 8) | (b << 3 | b >> 2);
}

static void show(void)
{
	if (g_model == kLink)
	{
		g_width = 2 * kGbWidth;
		g_height = kGbHeight;
	}
	else
	{
		GB_RuntimeInfo info;
		g_p[0].core->GetRuntimeInfo(info);
		g_width = info.screen_width;
		g_height = info.screen_height;
	}
	if (!g_render)
		return;
	if (g_model == kLink)
	{
		for (int i = 0; i < g_count; i++)
			for (int y = 0; y < kGbHeight; y++)
				for (int x = 0; x < kGbWidth; x++)
					g_picture[y * g_width + i * kGbWidth + x] = bgra(g_p[i].frame[y * kGbWidth + x]);
		return;
	}
	const int n = g_width * g_height;
	for (int i = 0; i < n; i++)
		g_picture[i] = bgra(g_p[0].frame[i]);
}

// ---- the sound ---------------------------------------------------------------
// Gearboy's sound chip (Blargg's Gb_Snd_Emu) gives 44,100 stereo samples a
// second, about 739 a frame; a frame with the screen off is up to a quarter
// longer. The link machine's two Game Boys are mixed, each at half.
static s16 g_audio[AUDIO_BUFFER_SIZE];
static int g_audioValues;

// ---- the controls ------------------------------------------------------------
// The Game Boy's eight buttons. The Super Game Boy reads up to four pads,
// when the game asks the SNES for more than one; the link machine has one
// pad for each Game Boy, P1's and P2's. Power turns the console off and on
// again (on the link machine, both); the cartridge's battery-backed memory
// stays. Toggle Link Connection pulls the link machine's cable out and puts
// it back. A cartridge with a tilt sensor (MBC7: Kirby Tilt 'n' Tumble)
// reads P1's two tilt axes.
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
	kLinkToggle,
	kButtons,
};
static char g_buttonNames[kButtons][32];
static bool g_held[kButtons];
static bool g_powerWasHeld, g_toggleWasHeld;

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

// ---- the link cable ----------------------------------------------------------
// Upstream's LibretroLink, without its own savestates (the sandbox keeps
// this memory with the rest of the machine). Each Game Boy reports its
// serial port's state as it changes and the start of each transfer it
// clocks; a transfer is matched with what the other Game Boy had in its
// port at that cycle, and handed to it when it reaches that cycle.
struct SerialState
{
	u64 cycle;
	u8 sb;
	u8 sc;
};

struct LinkRuntime
{
	u64 frame_cycle;
	u64 origin[2];
	SerialState states[2][16];
	u32 state_count[2];
	GB_LinkCableTransfer transfer[2];
	GB_LinkCableTransfer start[2];
	bool start_pending[2];
	bool pending[2];
};
static LinkRuntime g_link;
static bool g_connected;
static int g_endpoint[2] = { 0, 1 };

static u64 link_cycle(unsigned i)
{
	return g_p[i].core->GetLinkCableCycle() - g_link.origin[i];
}

static void link_state(u64 cycle, u8 sb, u8 sc, GB_SerialEvent, void *data)
{
	const unsigned i = (unsigned)*(int *)data;
	SerialState &s = g_link.states[i][g_link.state_count[i]++ % 16];
	s.cycle = cycle - g_link.origin[i];
	s.sb = sb;
	s.sc = sc;
	if ((sc & 0x81) != 0x80)
		g_link.pending[i] = false;
}

static void link_start(u64 cycle, u64 first_shift, u32 bit_cycles, u8 outgoing, u32 transfer_id, u8 *incoming, void *data)
{
	const unsigned i = (unsigned)*(int *)data;
	*incoming = 0xFF;
	GB_LinkCableTransfer &start = g_link.start[i];
	start.request_cycle = cycle - g_link.origin[i];
	start.first_shift_cycle = first_shift - g_link.origin[i];
	start.bit_cycles = bit_cycles;
	start.transfer_id = transfer_id;
	start.incoming_byte = outgoing;
	g_link.start_pending[i] = true;
}

static bool link_poll(u64 cycle, GB_LinkCableTransfer *transfer, void *data)
{
	const unsigned i = (unsigned)*(int *)data;
	if (!g_link.pending[i] || cycle < g_link.transfer[i].request_cycle)
		return false;
	*transfer = g_link.transfer[i];
	g_link.pending[i] = false;
	return true;
}

static void link_resolve(void)
{
	for (unsigned i = 0; i < 2; i++)
	{
		const unsigned peer = i ^ 1;
		const GB_LinkCableTransfer &start = g_link.start[i];
		if (!g_link.start_pending[i] || link_cycle(peer) < start.request_cycle)
			continue;
		g_link.start_pending[i] = false;
		const u32 count = MIN(g_link.state_count[peer], 16u);
		for (u32 k = 0; k < count; k++)
		{
			const SerialState &s = g_link.states[peer][(g_link.state_count[peer] - 1 - k) % 16];
			if (s.cycle > start.request_cycle)
				continue;
			if ((s.sc & 0x81) == 0x80)
			{
				g_p[i].core->GetProcessor()->SetLinkCableIncomingByte(start.transfer_id, s.sb);
				GB_LinkCableTransfer &t = g_link.transfer[peer];
				t = start;
				t.request_cycle += g_link.origin[peer];
				t.first_shift_cycle += g_link.origin[peer];
				t.local_byte = s.sb;
				g_link.pending[peer] = true;
			}
			break;
		}
	}
}

// the cable plugged in (again): the matching starts afresh from here
static void link_reset(void)
{
	memset(&g_link, 0, sizeof g_link);
	for (int i = 0; i < 2; i++)
		g_link.origin[i] = g_p[i].core->GetLinkCableCycle();
	for (int i = 0; i < 2; i++)
		g_p[i].core->SetLinkCableConnected(g_connected);
}

// One frame of the link machine: both Game Boys run 70,224 cycles, the one
// that is behind always going next, with the transfers matched after each
// step. A Game Boy draws into its own picture when it reaches its vertical
// blank. So the link machine's frames are all the same length.
static void link_frame(void)
{
	g_link.frame_cycle += GAMEBOY_CLOCKS_PER_FRAME;
	while (link_cycle(0) < g_link.frame_cycle || link_cycle(1) < g_link.frame_cycle)
	{
		const unsigned i = link_cycle(0) <= link_cycle(1) ? 0 : 1;
		unsigned int clocks;
		if (g_p[i].core->RunCycle(g_p[i].frame, clocks))
			g_p[i].core->RenderFrameBuffer(g_p[i].frame);
		link_resolve();
	}
	for (int i = 0; i < 2; i++)
		g_p[i].core->EndFrame(g_p[i].audio, &g_p[i].audioValues);
	const int n = MAX(g_p[0].audioValues, g_p[1].audioValues);
	for (int k = 0; k < n; k++)
	{
		const int a = k < g_p[0].audioValues ? g_p[0].audio[k] : 0;
		const int b = k < g_p[1].audioValues ? g_p[1].audio[k] : 0;
		g_audio[k] = (s16)((a + b) / 2);
	}
	g_audioValues = n;
	g_cycles += GAMEBOY_CLOCKS_PER_FRAME;
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

static Model model_named(const char *v)
{
	if (!strcmp(v, "GB"))
		return kGb;
	if (!strcmp(v, "GBC"))
		return kGbc;
	if (!strcmp(v, "GBA"))
		return kGba;
	if (!strcmp(v, "SGB"))
		return kSgb;
	if (!strcmp(v, "LINK"))
		return kLink;
	return kAuto;
}

static Model model_setting(const char *name)
{
	char v[16] = "";
	wbx_setting_str(name, v, sizeof v);
	return model_named(v);
}

// The DMG palettes: Gearboy's own green, and greys.
static void set_palette(GearboyCore *core)
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
	core->SetDMGPalette(c[0], c[1], c[2], c[3]);
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
static bool persistent(int p)
{
	GearboyCore *core = g_p[p].core;
	MemoryRule *rule = core->GetMemory()->GetCurrentRule();
	if (!rule)
		return false;
	if (core->GetCartridge()->HasBattery())
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

// the slots of a Game Boy's game and saved data: "game", "savedata" for
// the first, "game2", "savedata2" for the link machine's second
static void slot_id(char *out, size_t size, const char *base, int i)
{
	if (i == 0)
		snprintf(out, size, "%s", base);
	else
		snprintf(out, size, "%s%d", base, i + 1);
}

// Loads Game Boy i: its game, its boot ROM, the Super Game Boy's cartridge,
// its saved data.
static bool load_player(int i)
{
	Player &p = g_p[i];
	char slot[16];
	slot_id(slot, sizeof slot, "game", i);
	p.gameName[0] = '\0';
	if (wbx_slot_count(slot) > 0)
		wbx_slot_name(slot, 0, p.gameName, sizeof p.gameName);
	else if (i == 0)
	{
		// a game opened with no project is mounted under its own name
		FILE *f = fopen("rom.name", "rb");
		if (f)
		{
			size_t got = fread(p.gameName, 1, sizeof p.gameName - 1, f);
			p.gameName[got] = '\0';
			fclose(f);
		}
	}
	else
		// the link machine with no second game: the first one twice
		snprintf(p.gameName, sizeof p.gameName, "%s", g_p[0].gameName);
	if (!p.gameName[0])
	{
		snprintf(g_loadError, sizeof g_loadError, "error.no_file");
		return false;
	}
	long size = 0;
	u8 *rom = (u8 *)read_file(p.gameName, &size);
	if (!rom)
	{
		snprintf(g_loadError, sizeof g_loadError, "error.not_read" TEXT_ARG "%s", p.gameName);
		return false;
	}
	if (size < 0x150)
	{
		free(rom);
		snprintf(g_loadError, sizeof g_loadError, "error.too_short" TEXT_ARG "%s" TEXT_ARG "%ld", p.gameName, size);
		return false;
	}

	// with no Model given (the test runners need not give one), the Game
	// Boy Color for a .gbc file, the Super Game Boy for a .sgb one, and the
	// Game Boy for anything else
	if (p.model == kAuto)
		p.model = ends_with(p.gameName, ".gbc") ? kGbc : ends_with(p.gameName, ".sgb") ? kSgb : kGb;

	p.core = new GearboyCore();
	GearboyCore *core = p.core;
	core->Init(GB_PIXEL_RGB565);
	core->SetSoundSampleRate(44100);
	set_palette(core);
	core->EnableColorCorrection(setting_is("colorCorrection", "on", "off"));

	// The boot ROM: the console's own start-up program, which shows the logo
	// and then starts the game. Without it (Boot ROM off), Gearboy starts the
	// game in the state the boot ROM would have left the console in. Gearboy
	// runs the Game Boy's and the Game Boy Color's; the Game Boy Advance and
	// the Super Game Boy always start without one.
	Memory *mem = core->GetMemory();
	mem->EnableBootromDMG(false);
	mem->EnableBootromGBC(false);
	if ((p.model == kGb || p.model == kGbc) && setting_is("bootRom", "on", "on"))
	{
		const bool cgb = p.model == kGbc;
		const char *id = cgb ? "cgb_boot.bin" : "dmg_boot.bin";
		long got = 0;
		u8 *boot = boot_rom(id, cgb ? 0x900 : 0x100, &got);
		if (!boot)
		{
			free(rom);
			return false;
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
	const bool sgb = p.model == kSgb;
	core->SetSGBEnabled(sgb);
	core->SetSGBModel(sgb);
	core->SetSGBBorder(true);
	core->SetSGBAllGames(false);
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
				return false;
			}
			const bool known = core->GetSGB()->LoadBIOSFromBuffer(bios, (int)got);
			free(bios);
			if (!known)
			{
				free(rom);
				snprintf(g_loadError, sizeof g_loadError, "error.sgb_cartridge" TEXT_ARG "%s", id);
				return false;
			}
			core->SetSGBAllGames(true);
		}
	}

	if (!core->LoadROMFromBuffer(rom, (int)size, force_dmg(i), Cartridge::CartridgeNotSupported, force_gba(i)))
	{
		free(rom);
		snprintf(g_loadError, sizeof g_loadError, "error.not_loaded" TEXT_ARG "%s", p.gameName);
		return false;
	}
	free(rom);

	// the cartridge's memory at the start: empty, or the project's saved data
	slot_id(slot, sizeof slot, "savedata", i);
	if (wbx_slot_count(slot) > 0)
	{
		char name[256];
		if (wbx_slot_name(slot, 0, name, sizeof name))
		{
			long n = 0;
			char *saved = (char *)read_file(name, &n);
			if (saved && n > 0)
			{
				std::istringstream in(std::string(saved, (size_t)n));
				core->GetMemory()->GetCurrentRule()->LoadRam(in, (s32)n);
			}
			free(saved);
		}
	}

	core->GetInput()->SetReadHook(input_read, nullptr);
	return true;
}

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
	g_clockStart = time(nullptr);
	g_cycles = 0;
	Cartridge::SetRTCTimeSource(machine_time);

	g_model = model_setting("model");
	g_count = g_model == kLink ? 2 : 1;
	if (g_model == kLink)
	{
		// each linked Game Boy is a GB, a GBC or a GBA; never a Super Game Boy
		for (int i = 0; i < 2; i++)
		{
			char name[16];
			snprintf(name, sizeof name, "linkModel%d", i + 1);
			const Model m = model_setting(name);
			g_p[i].model = m == kGbc || m == kGba ? m : kGb;
		}
	}
	else
		g_p[0].model = g_model;
	for (int i = 0; i < g_count; i++)
		if (!load_player(i))
			return 0;
	if (g_model == kAuto)
		g_model = g_p[0].model;

	if (g_model == kLink)
	{
		g_connected = true;
		for (int i = 0; i < 2; i++)
			g_p[i].core->SetLinkCableCallbacks(link_state, link_start, link_poll, nullptr, &g_endpoint[i]);
		link_reset();
	}

	for (int p = 0; p < kMaxPads; p++)
		for (int b = 0; b < kPadButtons; b++)
			snprintf(g_buttonNames[p * kPadButtons + b], sizeof g_buttonNames[0], "P%d %s", p + 1, kPadNames[b]);
	snprintf(g_buttonNames[kPower], sizeof g_buttonNames[0], "Power");
	snprintf(g_buttonNames[kLinkToggle], sizeof g_buttonNames[0], "Toggle Link Connection");
	memset(g_held, 0, sizeof g_held);
	memset(g_axes, 0, sizeof g_axes);
	memset(g_picture, 0, sizeof g_picture);
	for (int i = 0; i < g_count; i++)
		memset(g_p[i].frame, 0, sizeof g_p[i].frame);
	g_powerWasHeld = g_toggleWasHeld = false;
	show();
	return 1;
}

ECL_EXPORT int GetButtonCount(void) { return kButtons; }
ECL_EXPORT const char *GetButtonName(int32_t i) { return i >= 0 && i < kButtons ? g_buttonNames[i] : ""; }
ECL_EXPORT int IsButtonActive(int32_t i)
{
	if (i < 0 || i >= kButtons)
		return 0;
	// P2 is the link machine's second Game Boy's; P2 to P4 are the Super
	// Game Boy's
	if (i < kPads)
	{
		const int pad = i / kPadButtons;
		return pad == 0 || g_model == kSgb || (g_model == kLink && pad == 1);
	}
	if (i == kLinkToggle)
		return g_model == kLink;
	return 1;
}
ECL_EXPORT void SetButton(int32_t i, int32_t state)
{
	if (i >= 0 && i < kButtons)
		g_held[i] = state != 0;
}

static bool tilt_cartridge(void)
{
	GearboyCore *core = g_p[0].core;
	return core && core->GetMemory()->GetCurrentRule()
		&& core->GetMemory()->GetCurrentRule()->GetMapperType() == Cartridge::CartridgeMBC7;
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
	// starts again from power-on, with the cartridge's memory kept; on the
	// link machine both do, and the cable starts afresh
	const bool power = g_held[kPower];
	if (power && !g_powerWasHeld)
	{
		for (int i = 0; i < g_count; i++)
			g_p[i].core->ResetROMPreservingRAM(force_dmg(i), Cartridge::CartridgeNotSupported, force_gba(i));
		if (g_model == kLink)
			link_reset();
	}
	g_powerWasHeld = power;

	// Toggle Link Connection: pressed, the cable comes out or goes back in
	const bool toggle = g_held[kLinkToggle];
	if (g_model == kLink && toggle && !g_toggleWasHeld)
	{
		g_connected = !g_connected;
		link_reset();
	}
	g_toggleWasHeld = toggle;

	// the pads: the Super Game Boy's four, or each linked Game Boy's own
	for (int p = 0; p < kMaxPads; p++)
	{
		GearboyCore *core = g_model == kLink ? (p < 2 ? g_p[p].core : nullptr) : g_p[0].core;
		const int player = g_model == kLink ? 0 : p;
		if (!core)
			continue;
		for (int b = 0; b < kPadButtons; b++)
		{
			if (g_held[p * kPadButtons + b])
				core->KeyPressed(kPadKeys[b], player);
			else
				core->KeyReleased(kPadKeys[b], player);
		}
	}

	// The tilt, -32768 to 32767 each way, is Gearboy's -4 to 4 (as its
	// libretro core clamps it): 8,192 a step.
	g_p[0].core->SetAccelerometer(g_axes[kTiltX] / 8192.0, g_axes[kTiltY] / 8192.0);

	if (g_model == kLink)
		link_frame();
	else
	{
		// one frame: to the start of the next vertical blank, or a frame and
		// a quarter's time when the screen is off
		GearboyCore *core = g_p[0].core;
		const u64 before = core->GetMasterClockCycles();
		int values = 0;
		core->RunToVBlank(g_p[0].frame, g_audio, &values, false, nullptr, true);
		const u64 after = core->GetMasterClockCycles();
		if (after > before)
			g_cycles += after - before;
		g_audioValues = values;
	}
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
// The link machine has each Game Boy's, named with its player: "P2 WRAM".
enum Domain { kRom, kVram, kVram1, kWram, kCartRam, kOam, kHram, kDomains };
static const char *const kDomainNames[kDomains] = { "ROM", "VRAM", "VRAM1", "WRAM", "CartRAM", "OAM", "HRAM" };
static char g_domainNames[kMaxGb * kDomains][24];

static bool domain(int i, u8 **data, int64_t *length)
{
	*data = nullptr;
	*length = 0;
	if (i < 0 || i >= g_count * kDomains || !g_p[i / kDomains].core)
		return false;
	GearboyCore *core = g_p[i / kDomains].core;
	Memory *m = core->GetMemory();
	const bool cgb = core->IsCGB();
	switch (i % kDomains)
	{
	case kRom:
		*data = core->GetCartridge()->GetTheROM();
		*length = core->GetCartridge()->GetTotalSize();
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

ECL_EXPORT int GetMemoryDomainCount(void) { return g_count * kDomains; }
ECL_EXPORT const char *GetMemoryDomainName(int i)
{
	if (i < 0 || i >= g_count * kDomains)
		return "";
	if (g_model != kLink)
		return kDomainNames[i];
	snprintf(g_domainNames[i], sizeof g_domainNames[i], "P%d %s", i / kDomains + 1, kDomainNames[i % kDomains]);
	return g_domainNames[i];
}
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
ECL_EXPORT int GetMemoryDomainWritable(int i) { return i >= 0 && i < g_count * kDomains && i % kDomains != kRom; }

// The system bus: the 64 KB the Game Boy's processor sees, read without
// touching the machine; one for each Game Boy of the link machine. The
// cartridge's area is read as Gearboy's debugger reads it (the boot ROM while
// it is mapped, the banks selected), the video and work memory from the
// banks selected, and the rest (sprites, I/O registers, high memory) as
// Gearboy keeps it in its memory map: an I/O register whose value Gearboy
// works out when it is read (the joypad, the sound) shows what was last kept
// there.
static const int64_t kBusRanges[] = { 0, 0x10000, 0, 0 };
ECL_INVISIBLE static uint8_t g_busBuf[0x10000];
static char g_busNames[kMaxGb][24];

static u8 bus_peek(Memory *m, u16 a)
{
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
		return bus_peek(m, (u16)(a - 0x2000));
	return m->GetMemoryMap()[a];
}

static bool bus_ok(int32_t bus) { return bus >= 0 && bus < g_count && g_p[bus].core; }

ECL_EXPORT int32_t GetBusCount(void) { return g_count; }
ECL_EXPORT const char *GetBusName(int32_t bus)
{
	if (bus < 0 || bus >= g_count)
		return "";
	if (g_model != kLink)
		return "System Bus";
	snprintf(g_busNames[bus], sizeof g_busNames[bus], "P%d System Bus", bus + 1);
	return g_busNames[bus];
}
ECL_EXPORT int64_t GetBusSize(int32_t bus) { return bus >= 0 && bus < g_count ? 0x10000 : 0; }
ECL_EXPORT int32_t GetBusWritable(int32_t bus) { return bus >= 0 && bus < g_count; }
ECL_EXPORT int32_t PeekBus(int32_t bus, int32_t addr)
{
	if (!bus_ok(bus) || addr < 0 || addr >= 0x10000)
		return 0;
	return bus_peek(g_p[bus].core->GetMemory(), (u16)addr);
}
ECL_EXPORT const int64_t *GetBusRanges(int32_t bus) { return bus >= 0 && bus < g_count ? kBusRanges : nullptr; }
ECL_EXPORT const uint8_t *ReadBus(int32_t bus, int64_t addr, int32_t len)
{
	if (!bus_ok(bus) || addr < 0 || len < 0 || addr + len > 0x10000)
		return nullptr;
	Memory *m = g_p[bus].core->GetMemory();
	for (int32_t i = 0; i < len; i++)
		g_busBuf[i] = bus_peek(m, (u16)(addr + i));
	return g_busBuf;
}
// A write from a tool goes into the memory behind the address: the video,
// work, sprite and high memory and the I/O registers' kept values, and the
// cartridge's memory through its chip. The ROM is not written.
ECL_EXPORT void PokeBus(int32_t bus, int32_t addr, int32_t value)
{
	if (!bus_ok(bus) || addr < 0x8000 || addr >= 0x10000)
		return;
	Memory *m = g_p[bus].core->GetMemory();
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
		PokeBus(bus, addr - 0x2000, value);
	else
		m->Load((u16)addr, v);
}

// ---- the processor's registers ---------------------------------------------------
// P1's Game Boy's.
static const char *const kRegisters[] = { "PC", "SP", "AF", "BC", "DE", "HL" };
enum { kRegisterCount = sizeof kRegisters / sizeof kRegisters[0] };

static SixteenBitRegister *reg(int i)
{
	Processor::ProcessorState *s = g_p[0].core->GetProcessor()->GetState();
	SixteenBitRegister *r[kRegisterCount] = { s->PC, s->SP, s->AF, s->BC, s->DE, s->HL };
	return r[i];
}

ECL_EXPORT int32_t GetRegisterCount(void) { return kRegisterCount; }
ECL_EXPORT const char *GetRegisterName(int32_t i) { return i >= 0 && i < kRegisterCount ? kRegisters[i] : ""; }
ECL_EXPORT int64_t GetRegisterValue(int32_t i) { return g_p[0].core && i >= 0 && i < kRegisterCount ? reg(i)->GetValue() : 0; }
ECL_EXPORT int32_t GetRegisterBits(int32_t i) { return i >= 0 && i < kRegisterCount ? 16 : 0; }
ECL_EXPORT const char *GetRegisterRoles(void) { return "pc PC\nsp SP\nflags AF\n"; }

// ---- Export Save Data ------------------------------------------------------------
// The cartridge's battery-backed memory as Gearboy writes its .sav files:
// the memory, and after it, for a cartridge with a clock, the clock's
// registers and the time they were written; for the link machine, each
// Game Boy's, "P2 <game>.sav" for the second. Writing it reads the clock;
// the time the cartridge holds is given back unchanged, so exporting
// changes nothing in the machine. The largest is an MBC6's: 1 MB of flash
// memory, 32 KB of memory and a little more. It is written into memory no
// state carries, without the heap, so exporting leaves the machine's memory
// as it was.
enum { kSaveMax = 0x120000 };
ECL_INVISIBLE static char g_saveBuf[kSaveMax];
ECL_INVISIBLE static size_t g_saveSize;
ECL_INVISIBLE static char g_saveName[256 + 16];

struct FixedBuffer : std::streambuf
{
	FixedBuffer(char *p, size_t n) { setp(p, p + n); }
	size_t size() const { return (size_t)(pptr() - pbase()); }
};

static void export_save(int p)
{
	GearboyCore *core = g_p[p].core;
	g_exportTime = core->GetCartridge()->GetCurrentRTC();
	Cartridge::SetRTCTimeSource(export_time);
	FixedBuffer buf(g_saveBuf, sizeof g_saveBuf);
	std::ostream out(&buf);
	core->GetMemory()->GetCurrentRule()->SaveRam(out);
	Cartridge::SetRTCTimeSource(machine_time);
	g_saveSize = buf.size();
}

// the Game Boy of the i-th file
static int save_owner(int32_t i)
{
	for (int p = 0; p < g_count; p++)
		if (g_p[p].core && persistent(p) && i-- == 0)
			return p;
	return -1;
}

ECL_EXPORT int32_t GetSaveDataFileCount(void)
{
	int n = 0;
	for (int p = 0; p < g_count; p++)
		n += g_p[p].core && persistent(p);
	return n;
}
ECL_EXPORT const char *GetSaveDataFileName(int32_t i)
{
	const int p = save_owner(i);
	if (p < 0)
		return "";
	char base[256];
	snprintf(base, sizeof base, "%s", g_p[p].gameName);
	char *dot = strrchr(base, '.');
	if (dot)
		*dot = '\0';
	if (g_model == kLink)
		snprintf(g_saveName, sizeof g_saveName, "P%d %s.sav", p + 1, base);
	else
		snprintf(g_saveName, sizeof g_saveName, "%s.sav", base);
	return g_saveName;
}
ECL_EXPORT int64_t GetSaveDataFileSize(int32_t i)
{
	const int p = save_owner(i);
	if (p < 0)
		return 0;
	export_save(p);
	return (int64_t)g_saveSize;
}
ECL_EXPORT const uint8_t *GetSaveDataFileBuffer(int32_t i)
{
	const int p = save_owner(i);
	if (p < 0)
		return nullptr;
	export_save(p);
	return (const uint8_t *)g_saveBuf;
}

} // extern "C"
