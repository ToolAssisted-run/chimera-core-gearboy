#!/bin/bash
# The core's tests. Each line says what was compared. The script ends with a
# count of tests that passed, failed and were skipped, and with an error if
# any failed. A skipped test is not a pass.
#
#   waterbox/run-gate.sh
#
# CHIMERA_ROOT=<a Chimera checkout> adds the tests that run the package in
# Chimera's own headless runner (waterbox/build-package.sh -r <it> first),
# and the check of the default key bindings' names.
#
# Needs the native build (meson setup build/meson-native ... && ninja) and
# the sandboxed core (waterbox/setup-guest.sh && ninja -C build/meson-guest):
# docs/BUILDING.md. And Python 3.
#
# WHAT RUNS WITH NO FILES OF YOURS: everything but the boot ROM, Super Game
# Boy cartridge and game tests. tests/make-testprog.py writes a cartridge of
# this repository's own that reads the buttons (and, in its Super Game Boy
# variant, asks for four pads), keeps a battery-backed memory and reads the
# cartridge's clock; tests/make-gb-test.py writes one with a tilt sensor.
#
# WHAT NEEDS YOUR FILES (skipped without them):
#   tests/roms-local/  the boot ROMs and the Super Game Boy cartridge, under
#                      the names the core mounts them by: dmg_boot.bin,
#                      cgb_boot.bin, SGB1.sfc, SGB2.sfc
#   tests/games-local/ any .gb and .gbc games; each runs 2,000 frames
#
# The ways a test passes on a broken thing (Chimera's docs/gates.md) that
# these are written against: a machine that did nothing agrees with itself,
# so a run is first checked to have got somewhere; the end of a run hides a
# run that went another way and came back, so runs are compared as a stream
# of digests; and a comparison that cannot fail proves nothing, so each
# "the same" has a run beside it that must come out different.
set -u
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
native="$root/build/meson-native/run-native"
runwbx="$root/build/meson-native/run-wbx"
core="$root/build/meson-guest/core.wbx"
chimera_root="${CHIMERA_ROOT:-}"
roms="$root/tests/roms-local"
games="${GEARBOY_GAMES:-$root/tests/games-local}"

work="$(mktemp -d)"
# GATE_KEEP=1 keeps the work folder (the runs' output and dumps) and says where
if [ -n "${GATE_KEEP:-}" ]; then trap 'echo "work folder kept: $work"' EXIT; else trap 'rm -rf "$work"' EXIT; fi
ok=0; failed=0; skipped=0
report() { # <test> <PASS|FAIL|SKIP> <what>
	printf '%-24s %-6s %s\n' "$1" "$2" "$3"
	case "$2" in PASS) ok=$((ok + 1)) ;; FAIL) failed=$((failed + 1)) ;; *) skipped=$((skipped + 1)) ;; esac
}
# check <test> <what> <condition...>
check() {
	ck_test="$1"; ck_what="$2"; shift 2
	if "$@"; then report "$ck_test" PASS "$ck_what"; else report "$ck_test" FAIL "$ck_what"; fi
}
stream() { grep '^frame' "$1"; }
same() { [ -s "$1" ] && [ -s "$2" ] && cmp -s "$1" "$2"; }
differ() { [ -s "$1" ] && [ -s "$2" ] && ! cmp -s "$1" "$2"; }
# field <file> <name>: the value after a name in the file's last report line
field() { stream "$1" | tail -1 | awk -v f="$2" '{for (i = 1; i <= NF; i++) if ($i == f) { print $(i + 1); exit }}'; }
pictures() { stream "$1" | awk '{print $7}' | sort -u | wc -l; }
sounds() { stream "$1" | awk '{print $10}' | sort -u | wc -l; }
size() { stream "$1" | tail -1 | awk '{print $6}'; }
# r <WRAM dump> <name>: what the test program left in its memory
r() { python3 "$root/tests/results.py" "$1" "$2" 2> /dev/null; }
# byte <file> <offset>: one byte of a dump, in decimal
byte() { od -An -tu1 -j "$2" -N 1 "$1" 2> /dev/null | tr -d ' '; }
# folder <name> <settings json> <game> [<save data>] [<firmware>...]: a work
# folder as the sandbox mounts a project - every file under its name
folder() {
	fd_name="$1"; fd_settings="$2"; fd_game="$3"; fd_save="${4:-}"
	shift 3; [ $# -gt 0 ] && shift
	rm -rf "${work:?}/$fd_name"; mkdir -p "$work/$fd_name"
	cp "$fd_game" "$work/$fd_name/$(basename "$fd_game")"
	if [ -n "$fd_save" ]; then
		cp "$fd_save" "$work/$fd_name/$(basename "$fd_save")"
		printf '{"game":["%s"],"savedata":["%s"]}' "$(basename "$fd_game")" "$(basename "$fd_save")" > "$work/$fd_name/slots"
	else
		printf '{"game":["%s"]}' "$(basename "$fd_game")" > "$work/$fd_name/slots"
	fi
	for fw in "$@"; do cp "$fw" "$work/$fd_name/"; done
	printf '%s' "$fd_settings" > "$work/$fd_name/settings"
}
nat() { nt_out="$1"; nt_dir="$2"; shift 2; "$native" "$work/$nt_dir" "$@" > "$work/$nt_out.out" 2> "$work/$nt_out.err"; }
box() { bx_out="$1"; bx_dir="$2"; shift 2; "$runwbx" "$core" "$work/$bx_dir" "$@" > "$work/$bx_out.out" 2> "$work/$bx_out.err"; }
# index <folder> <control>: the control's place in the panel
index() { "$native" "$work/$1" --frames 1 --list-panel 2> /dev/null | grep "'$2'" | head -1 | awk '{print $2}'; }
# repeats <file> <first> <second> <count> <step>: do the report lines from
# frame <second> on repeat those from frame <first> on (run-wbx --rewind)
repeats() {
	awk -v a="$2" -v b="$3" -v n="$4" -v step="$5" '
		/^frame/ { f = $2; $1 = ""; $2 = ""; sub(/ lag [0-9]+$/, ""); line[f] = $0 }
		END {
			for (i = step; i < n; i += step) {
				if (!((a + i) in line) || !((b + i) in line) || line[a + i] != line[b + i]) exit 1
				seen++
			}
			exit seen > 0 ? 0 : 1
		}' "$1"
}

if [ ! -x "$native" ] || [ ! -x "$runwbx" ] || [ ! -f "$core" ]; then
	echo "not built: this needs build/meson-native/run-native and run-wbx, and build/meson-guest/core.wbx (docs/BUILDING.md)" >&2
	exit 1
fi

# ------------------------------------------------- the source and the package
patches="$(ls "$root"/patches/*.patch 2> /dev/null | wc -l)"
check "source:pinned" "extern/gearboy is the pinned commit, with the $patches patches of patches/" \
	sh -c "sh '$here/apply-patches.sh' > '$work/patches.log' 2>&1 && [ \"\$(git -C '$root' ls-files -s extern/gearboy | awk '{print \$2}')\" = \"\$(git -C '$root/extern/gearboy' rev-parse HEAD)\" ]"

stale=""
for src in "$here"/gearboy.cpp "$root"/meson.build "$root"/patches/*.patch; do
	[ -e "$src" ] || continue
	[ "$core" -nt "$src" ] || stale="$stale $(basename "$src")"
	[ "$native" -nt "$src" ] || stale="$stale $(basename "$src")(native)"
done
check "package:fresh" "core.wbx and run-native are not older than their sources${stale:+ (older than:$stale)}" [ -z "$stale" ]

# ------------------------------------------------- the test programs
progs="$work/progs"
mkdir -p "$progs"
python3 "$root/tests/make-testprog.py" "$progs/testprog.gb"
python3 "$root/tests/make-testprog.py" "$progs/testprog.gbc"
python3 "$root/tests/make-testprog.py" "$progs/nopad.gb" nopad
python3 "$root/tests/make-testprog.py" "$progs/sgbprog.gb" sgb
python3 "$root/tests/make-gb-test.py" "$progs" > /dev/null
folder t '{"model":"GB"}' "$progs/testprog.gb"
folder c '{"model":"GBC"}' "$progs/testprog.gbc"
folder q '{"model":"GB"}' "$progs/nopad.gb"
folder s '{"model":"SGB"}' "$progs/sgbprog.gb"
folder tilt '{"model":"GB"}' "$progs/gb-tilt.gb"

"$native" "$work/t" --frames 1 --list-panel > "$work/panel.out" 2> /dev/null
"$native" "$work/s" --frames 1 --list-panel > "$work/panel-sgb.out" 2> /dev/null
"$native" "$work/tilt" --frames 1 --list-panel > "$work/panel-tilt.out" 2> /dev/null
check "package:controls" "each of the package's machines declares the core's own $(grep -c '^panel' "$work/panel.out") buttons and $(grep -c '^axis' "$work/panel.out") axes, in the core's order; on a Game Boy the first pad and Power are live, on the Super Game Boy all four pads; the tilt axes are live only with a tilt cartridge" \
	python3 - "$here/waterbox.config" "$work/panel.out" "$work/panel-sgb.out" "$work/panel-tilt.out" <<'PY'
import json, sys
cfg = json.load(open(sys.argv[1]))
def panel(path):
    lines = open(path).read().splitlines()
    names = [l[l.index("'") + 1:l.rindex("'")] for l in lines if l.startswith("panel ")]
    live = [l[l.index("'") + 1:l.rindex("'")] for l in lines if l.startswith("panel ") and l.endswith("active")]
    axes = [l.endswith("active") for l in lines if l.startswith("axis ")]
    return names, live, axes
one, one_live, one_axes = panel(sys.argv[2])
sgb, sgb_live, _ = panel(sys.argv[3])
_, _, tilt_axes = panel(sys.argv[4])
want_live = ["P1 " + b for b in ("Up", "Down", "Left", "Right", "Start", "Select", "B", "A")] + ["Power"]
ok = one_live == want_live and len(one) == 33 and len(sgb_live) == 33 and one_axes == [False, False] and tilt_axes == [True, True]
for m in cfg["machines"]:
    ok = ok and m["input"]["buttons"] == one and [a["name"] for a in m["input"]["axes"]] == ["P1 Tilt X", "P1 Tilt Y"]
sys.exit(0 if ok else 1)
PY

if [ -n "$chimera_root" ] && [ -f "$chimera_root/source/ui/qt/key_codes.inc" ]; then
	check "package:key-names" "every host key the package's default bindings name is a key Chimera knows: $(python3 "$root/tests/check-key-names.py" "$chimera_root" "$here/default_keybinds.json" 2>&1)" \
		sh -c "python3 '$root/tests/check-key-names.py' '$chimera_root' '$here/default_keybinds.json' > /dev/null 2>&1"
else
	report "package:key-names" SKIP "needs CHIMERA_ROOT, whose source names the keys Chimera knows"
fi

nat idle t --frames 300 --report 25 --dump-domain WRAM "$work/idle.wram"
check "prog:runs" "the test program runs with no boot ROM: it counted $(r "$work/idle.wram" frames) frames in 300, with $(pictures "$work/idle.out") different pictures in 12 reports, at $(size "$work/idle.out")" \
	sh -c "[ '$(r "$work/idle.wram" frames)' -ge 295 ] && [ $(pictures "$work/idle.out") -ge 12 ] && [ '$(size "$work/idle.out")' = 160x144 ]"

# each button held alone from frame 60; what the program then read
: > "$work/buttons.txt"
for ((b = 0; b < 8; b++)); do
	"$native" "$work/t" --frames 100 --press "$b":60:60 --dump-domain WRAM "$work/eb.wram" > /dev/null 2>&1
	echo "$b $(r "$work/eb.wram" padSeen)" >> "$work/buttons.txt"
done
# the program's bits: Down, Up, Left, Right on top, Start, Select, B, A below
check "prog:buttons" "each of P1's 8 buttons, held alone, is the Game Boy's own bit for it and no other ($(awk '{printf "%s ", $2}' "$work/buttons.txt"))" \
	[ "$(awk '{printf "%s ", $2}' "$work/buttons.txt")" = "64 128 32 16 8 4 2 1 " ]

nat lag q --frames 300 --report 100
check "prog:lag" "a frame is a lag frame when the program did not read the buttons: the program that reads them every frame has $(field "$work/idle.out" lag) in 300 (the first frame ends before the program reads them), its twin that never does $(field "$work/lag.out" lag)" \
	[ "$(field "$work/idle.out" lag) $(field "$work/lag.out" lag)" = "1 300" ]

nat dark t --frames 200 --report 10 --press 5:50:60 --dump-domain WRAM "$work/dark.wram"
dark_pics="$(stream "$work/dark.out" | awk '$2 >= 60 && $2 <= 110 {print $7}' | sort -u | wc -l)"
check "prog:screen-off" "with the screen off (Select held, frames 50 to 110) every frame shows the same picture ($dark_pics picture in the 6 reports), and the program goes on counting ($(r "$work/dark.wram" frames) frames in 200)" \
	sh -c "[ $dark_pics = 1 ] && [ $(pictures "$work/dark.out") -ge 8 ] && [ '$(r "$work/dark.wram" frames)' -ge 190 ]"

nat clock t --frames 1200 --report 600 --dump-domain WRAM "$work/clock.wram"
nat clock600 t --frames 600 --report 600 --dump-domain WRAM "$work/clock600.wram"
check "prog:clock" "the cartridge's clock counts the machine's own time: $(r "$work/clock600.wram" clockSeconds) seconds after 600 frames and $(r "$work/clock.wram" clockSeconds) after 1,200 (70,224 cycles of 4,194,304 a frame)" \
	[ "$(r "$work/clock600.wram" clockSeconds) $(r "$work/clock.wram" clockSeconds)" = "10 20" ]

mkdir -p "$work/snd-av" "$work/idle-av"
nat snd t --frames 600 --report 5 --press 7:100:200 --av-stream "$work/snd-av"
nat idleav t --frames 600 --report 5 --av-stream "$work/idle-av"
total="$(( $(wc -c < "$work/snd-av/audio.raw") / 4 ))"
want="$(( 600 * 70224 * 44100 / 4194304 ))"
check "prog:sound" "$total sound samples in 600 frames, 44,100 a second ($want for 600 frames of 70,224 cycles); the pitch follows the buttons: with A held the sound differs from the run without" \
	sh -c "[ $((want - total)) -ge -2 ] && [ $((want - total)) -le 740 ] && ! cmp -s '$work/snd-av/audio.raw' '$work/idle-av/audio.raw' && [ $(sounds "$work/snd.out") -ge 3 ]"

pw="$(index t Power)"
nat power t --frames 300 --press "$pw":100:1 --dump-domain WRAM "$work/power.wram"
check "prog:power" "Power starts the console again: the program counted $(r "$work/power.wram" frames) frames after it (of 200), and its count of starts in the cartridge's memory is $(r "$work/power.wram" starts), one more than with no Power ($(r "$work/idle.wram" starts): Gearboy's cartridge memory starts as 255s)" \
	sh -c "[ '$(r "$work/power.wram" frames)' -ge 195 ] && [ '$(r "$work/power.wram" frames)' -le 200 ] && [ '$(r "$work/power.wram" starts)' = $(( ($(r "$work/idle.wram" starts) + 1) % 256 )) ]"

# the saved data: exported after a run, and a project begins from it
mkdir -p "$work/sv"
nat sv t --frames 300 --press 7:100:5 --savedata-out "$work/sv"
sav="$work/sv/testprog.sav"
folder sv2 '{"model":"GB"}' "$progs/testprog.gb" "$sav"
nat sv2 sv2 --frames 300 --dump-domain WRAM "$work/sv2.wram"
check "prog:save-data" "Export Save Data writes testprog.sav: the 8 KB of memory and the clock's registers ($(wc -c < "$sav" 2> /dev/null) bytes); a project with it begins from it: the count of starts goes on ($(r "$work/sv2.wram" starts)) and the clock from its 5 seconds ($(r "$work/sv2.wram" clockSeconds) after 300 frames more)" \
	sh -c "[ '$(wc -c < "$sav" 2> /dev/null || echo 0)' -gt 8192 ] && [ '$(r "$work/sv2.wram" starts)' = $(( ($(r "$work/idle.wram" starts) + 1) % 256 )) ] && [ '$(r "$work/sv2.wram" clockSeconds)' = 10 ]"

# The time beside the clock's registers in the saved data is the machine's:
# the second the clock starts at (1495889068, the sandbox's own unless a
# project names another) plus the time the machine ran. Saved data written
# an hour and five seconds before that start moves the clock on by as much
# when it is loaded.
start=1495889068
stamp="$(python3 -c "import struct, sys; d = open(sys.argv[1], 'rb').read(); print(struct.unpack_from('<i', d, 8192 + 40)[0])" "$sav" 2> /dev/null)"
python3 -c "import struct, sys; d = bytearray(open(sys.argv[1], 'rb').read()); struct.pack_into('<i', d, 8192 + 40, $start - 3605); open(sys.argv[2], 'wb').write(d)" "$sav" "$work/old.sav"
folder sv3 '{"model":"GB"}' "$progs/testprog.gb" "$work/old.sav"
nat sv3 sv3 --frames 300 --dump-domain WRAM "$work/sv3.wram"
check "prog:clock-time" "the saved clock carries the machine's time, $((${stamp:-0} - start)) seconds after the clock's start at 300 frames; saved data stamped an hour and 5 seconds before the start moves the clock on by that when loaded ($(r "$work/sv3.wram" clockSeconds) seconds after 300 frames, 10 without)" \
	sh -c "[ $((${stamp:-0} - start)) -ge 4 ] && [ $((${stamp:-0} - start)) -le 5 ] && [ '$(r "$work/sv3.wram" clockSeconds)' = 15 ]"

# exporting the saved data reads the cartridge's clock: it must not change
# the machine (a run exporting after every frame against one that never does)
nat svx t --frames 300 --report 10 --export-every
box svx-box t --frames 300 --report 10 --export-every
nat idle10 t --frames 300 --report 10
for n in svx svx-box idle10; do stream "$work/$n.out" > "$work/$n.stream"; done
check "prog:save-data-looks" "exporting the saved data after every frame changes nothing in the run, native or in the sandbox" \
	sh -c "cmp -s '$work/svx.stream' '$work/idle10.stream' && cmp -s '$work/svx-box.stream' '$work/idle10.stream'"

nat bus t --frames 300 --report 25 --dump-domain bus:49152:8 "$work/bus.bin"
head -c 8 "$work/idle.wram" > "$work/idle8.bin"
stream "$work/bus.out" > "$work/bus.stream"; stream "$work/idle.out" > "$work/idle.stream"
check "prog:bus" "the system bus at C000 holds what the work memory does ($(od -An -tx1 -N6 "$work/bus.bin" | tr -d ' ')), and reading it changes nothing in the run" \
	sh -c "cmp -s '$work/bus.bin' '$work/idle8.bin' && cmp -s '$work/bus.stream' '$work/idle.stream'"

# ------------------------------------------------- the consoles
nat gbc c --frames 300 --report 25 --dump-domain WRAM "$work/gbc.wram"
folder auto '{}' "$progs/testprog.gbc"
nat auto auto --frames 300 --report 25
folder gba '{"model":"GBA"}' "$progs/testprog.gbc"
folder gbcdmg '{"model":"GB"}' "$progs/testprog.gbc"
nat gba gba --frames 300 --report 25 --dump-domain WRAM "$work/gba.wram"
nat sgb s --frames 300 --report 25 --dump-domain WRAM "$work/sgb.wram" --vid-out "$work/sgb.vid"
nat gbcdmg gbcdmg --frames 300 --report 25 --dump-domain WRAM "$work/gbcdmg.wram"
stream "$work/gbc.out" > "$work/gbc.stream"; stream "$work/auto.out" > "$work/auto.stream"
check "consoles:no-model" "with no Model given (Chimera always gives one; the test runners need not) a .gbc file runs on the Game Boy Color: the same run as with Model GBC ($(r "$work/gbc.wram" frames) frames counted)" \
	sh -c "[ '$(r "$work/gbc.wram" frames)' -ge 295 ] && cmp -s '$work/gbc.stream' '$work/auto.stream'"
boot_regs() { echo "$(r "$1" bootA)/$(r "$1" bootB)/$(r "$1" bootC)"; }
check "consoles:models" "the Game Boy Color, the Game Boy Advance, the Super Game Boy and the Game Boy with a Game Boy Color game each run the program (frames $(r "$work/gbc.wram" frames), $(r "$work/gba.wram" frames), $(r "$work/sgb.wram" frames), $(r "$work/gbcdmg.wram" frames)); each tells the game which console it is by the registers it starts it with, A/B/C: Game Boy $(boot_regs "$work/idle.wram"), Game Boy Color $(boot_regs "$work/gbc.wram"), Game Boy Advance $(boot_regs "$work/gba.wram"), Super Game Boy $(boot_regs "$work/sgb.wram"), the Game Boy with a Game Boy Color game $(boot_regs "$work/gbcdmg.wram"); the Super Game Boy's picture is $(size "$work/sgb.out") with its border" \
	sh -c "[ '$(r "$work/gbc.wram" frames)' -ge 295 ] && [ '$(r "$work/gba.wram" frames)' -ge 295 ] && [ '$(r "$work/sgb.wram" frames)' -ge 280 ] && [ '$(r "$work/gbcdmg.wram" frames)' -ge 295 ] && [ '$(size "$work/sgb.out")' = 256x224 ] && [ '$(boot_regs "$work/idle.wram") $(boot_regs "$work/gbc.wram") $(boot_regs "$work/gba.wram") $(boot_regs "$work/sgb.wram") $(boot_regs "$work/gbcdmg.wram")' = '1/0/19 17/0/0 17/1/0 1/0/20 1/0/19' ]"
check "consoles:sgb-screen" "the Super Game Boy's picture has the Game Boy's screen at its place, 48 pixels in and 40 down: $(python3 "$root/tests/sgb-screen.py" "$work/sgb.vid" 2>&1)" \
	python3 "$root/tests/sgb-screen.py" "$work/sgb.vid"
# each of the four pads, one button each, at its own time; the Game Boy's
# own pad must not see the other three
p2up="$(index s 'P2 Up')"; p3a="$(index s 'P3 A')"; p4st="$(index s 'P4 Start')"
nat sgbpads s --frames 120 --press 0:100:30 --press "$p2up":100:30 --press "$p3a":100:30 --press "$p4st":100:30 --dump-domain WRAM "$work/sgbpads.wram"
nat gbpads t --frames 120 --press "$p2up":100:30 --press "$p3a":100:30 --press "$p4st":100:30 --dump-domain WRAM "$work/gbpads.wram"
pads="$(for k in 16 17 18 19; do byte "$work/sgbpads.wram" $k; done | tr '\n' ' ')"
check "consoles:sgb-pads" "on the Super Game Boy, after the program asks for four controllers, each pad reaches the program as its own: P1 Up, P2 Up, P3 A and P4 Start read as $pads(64 64 1 8); on a Game Boy, P2 to P4 reach nothing (it saw $(r "$work/gbpads.wram" padSeen))" \
	sh -c "[ '$pads' = '64 64 1 8 ' ] && [ '$(r "$work/gbpads.wram" padSeen)' = 0 ]"

# ------------------------------------------------- the tilt sensor
tx="$(index tilt 'P1 Tilt X')"
nat tilt0 tilt --frames 60 --dump-domain WRAM "$work/tilt0.wram"
nat tilt1 tilt --frames 60 --axis 0:16384:30:31 --axis 1:-8192:30:31 --dump-domain WRAM "$work/tilt1.wram"
tilt_at() { python3 -c "import sys; w = open(sys.argv[1], 'rb').read(); print('%04x %04x' % (w[0x10] | w[0x11] << 8, w[0x12] | w[0x13] << 8))" "$1" 2> /dev/null; }
check "tilt:axes" "an MBC7 cartridge's tilt sensor reads P1 Tilt X and Y: level it reads $(tilt_at "$work/tilt0.wram") (Gearboy's 81d0 81d0), tilted by 16384 and -8192 it reads $(tilt_at "$work/tilt1.wram") (0x70 a step of 8,192: 82b0 8160)" \
	[ "$(tilt_at "$work/tilt0.wram") $(tilt_at "$work/tilt1.wram")" = "81d0 81d0 82b0 8160" ]

# ------------------------------------------------- native and sandbox, states
# Select held from frame 280 to 320 turns the program's screen off around
# the frame where --session moves the machine to a new sandbox (300)
allrun="--frames 600 --report 10 --press 7:50:40 --press 0:80:30 --press 4:200:5 --press 5:280:40 --press 2:330:60 --press $pw:400:1"
nat all t $allrun
nat idle600 t --frames 600 --report 10
box all-box t $allrun
box all-rerecord t $allrun --rerecord
box all-session t $allrun --session
for n in all all-box all-rerecord all-session idle600; do stream "$work/$n.out" > "$work/$n.stream"; done
check "prog:sandbox" "native and sandbox print the same $(wc -l < "$work/all.stream") digests of memory, picture and sound, with buttons pressed and Power" \
	same "$work/all.stream" "$work/all-box.stream"
check "prog:presses-change-the-run" "the run with presses differs from the idle one" differ "$work/all.stream" "$work/idle600.stream"
check "prog:state-every-frame" "a state saved and loaded every frame changes nothing ($(grep -o 'stateBytes=[0-9]*' "$work/all-rerecord.err" | head -1 | sed 's/stateBytes=//') bytes a state)" same "$work/all-box.stream" "$work/all-rerecord.stream"
check "prog:state-other-box" "a state moved to a new sandbox half way changes nothing" same "$work/all-box.stream" "$work/all-session.stream"
box rewind t --frames 1600 --report 5 --rewind --press 5:790:40 --press 5:1190:40
check "prog:state-rewind" "a state kept at frame 800, with the screen off, and loaded at 1200: the frames after the load repeat those after the save" \
	repeats "$work/rewind.out" 800 1200 380 5

folder gbcc '{"model":"GBC","colorCorrection":"on"}' "$progs/testprog.gbc"
folder grey '{"model":"GB","palette":"grey"}' "$progs/testprog.gb"
sgbrun="--frames 600 --report 10 --press 7:50:40 --press $p2up:80:30 --press $p3a:200:5 --press 5:280:40 --press $p4st:330:60 --press $pw:400:1"
tiltrun="--frames 300 --report 10 --axis 0:20000:50:100 --axis 1:-30000:120:100 --press $pw:200:1"
for m in c:gbc gba:gba s:sgb tilt:tilt gbcc:gbcc grey:grey; do
	d="${m%%:*}"; n="${m#*:}"
	run="$allrun"; [ "$n" = sgb ] && run="$sgbrun"; [ "$n" = tilt ] && run="$tiltrun"
	nat "x-$n" "$d" $run
	box "x-$n-box" "$d" $run
	box "x-$n-rr" "$d" $run --rerecord
	for x in "x-$n" "x-$n-box" "x-$n-rr"; do stream "$work/$x.out" > "$work/$x.stream"; done
done
consoles_same=1
for n in gbc gba sgb tilt gbcc grey; do
	cmp -s "$work/x-$n.stream" "$work/x-$n-box.stream" && cmp -s "$work/x-$n.stream" "$work/x-$n-rr.stream" && [ -s "$work/x-$n.stream" ] || consoles_same=0
done
check "consoles:sandbox" "on the Game Boy Color, the Game Boy Advance, the Super Game Boy (its four pads pressed), the tilt cartridge (both axes moved), with colour correction and with grey shades, native, sandbox and a state saved and loaded every frame print the same digests" \
	[ "$consoles_same" = 1 ]
check "consoles:picture-settings" "colour correction and the grey shades change the picture and nothing else: the memory digests are those of the run without them, the pictures are not" \
	python3 - "$work/x-gbc.stream" "$work/x-gbcc.stream" "$work/all.stream" "$work/x-grey.stream" <<'PY'
import sys
def cols(path, i):
    return [l.split()[i] for l in open(path)]
a, b, c, d = sys.argv[1:5]
ok = cols(a, 3) == cols(b, 3) and cols(a, 6) != cols(b, 6) and cols(c, 3) == cols(d, 3) and cols(c, 6) != cols(d, 6)
sys.exit(0 if ok else 1)
PY

# ------------------------------------------------- what is refused
mkdir -p "$work/none"
printf '{"game":[]}' > "$work/none/slots"; printf '{}' > "$work/none/settings"
nat none none --frames 1
check "refused:no-game" "a machine with no game is refused, and says so" grep -q "Init failed.*error.no_file" "$work/none.out" "$work/none.err"
printf 'not a game' > "$work/junk.gb"
folder junk '{}' "$work/junk.gb"
nat junk junk --frames 1
check "refused:not-a-game" "a file too short to be a game is refused, and says so" grep -q "Init failed.*error.too_short" "$work/junk.out" "$work/junk.err"
folder nobios '{"model":"GB","bootRom":"on"}' "$progs/testprog.gb"
nat nobios nobios --frames 1
check "refused:no-boot-rom" "with Boot ROM 'on' and no boot ROM given, the machine is refused, and says which file it needs" grep -q "Init failed.*error.boot_rom.dmg_boot.bin" "$work/nobios.out" "$work/nobios.err"
folder nosgb '{"model":"SGB","sgbCartridge":"SGB1"}' "$progs/sgbprog.gb"
nat nosgb nosgb --frames 1
check "refused:no-sgb-cartridge" "with a Super Game Boy cartridge asked for and none given, the machine is refused, and says which file it needs" grep -q "Init failed.*error.sgb_missing.SGB1.sfc" "$work/nosgb.out" "$work/nosgb.err"
head -c 262144 /dev/zero > "$work/SGB1.sfc"
folder badsgb '{"model":"SGB","sgbCartridge":"SGB1"}' "$progs/sgbprog.gb" "" "$work/SGB1.sfc"
nat badsgb badsgb --frames 1
check "refused:unknown-sgb-cartridge" "a file of the right size that is not a Super Game Boy cartridge Gearboy knows is refused, and says so" grep -q "Init failed.*error.sgb_cartridge.SGB1.sfc" "$work/badsgb.out" "$work/badsgb.err"

# ------------------------------------------------- with the boot ROMs
# the frame the program starts at with no boot ROM, from the runs above
nobios_GB=$((300 - $(r "$work/idle.wram" frames))); nobios_GBC=$((300 - $(r "$work/gbc.wram" frames)))
for pair in "dmg_boot.bin:GB:testprog.gb" "cgb_boot.bin:GBC:testprog.gbc"; do
	fw="${pair%%:*}"; rest="${pair#*:}"; model="${rest%%:*}"; prog="${rest#*:}"
	eval "nobios=\$nobios_$model"
	if [ ! -f "$roms/$fw" ]; then
		report "boot:$model" SKIP "no $fw in tests/roms-local"
		continue
	fi
	folder "b-$model" "{\"model\":\"$model\",\"bootRom\":\"on\"}" "$progs/$prog" "" "$roms/$fw"
	nat "b-$model" "b-$model" --frames 900 --report 50 --dump-domain WRAM "$work/b-$model.wram"
	box "b-$model-box" "b-$model" --frames 900 --report 50
	stream "$work/b-$model.out" > "$work/b-$model.stream"; stream "$work/b-$model-box.out" > "$work/b-$model-box.stream"
	started=$((900 - $(r "$work/b-$model.wram" frames)))
	check "boot:$model" "the $model boot ROM runs first, then the program, which started at frame $started (at frame $nobios with no boot ROM); the sandbox gives the native build's digests" \
		sh -c "[ $started -ge $((nobios + 20)) ] && [ $started -lt 800 ] && cmp -s '$work/b-$model.stream' '$work/b-$model-box.stream'"
done
for v in SGB1 SGB2; do
	if [ ! -f "$roms/$v.sfc" ]; then
		report "sgb-cartridge:$v" SKIP "no $v.sfc in tests/roms-local"
		continue
	fi
	folder "sc-$v" "{\"model\":\"SGB\",\"sgbCartridge\":\"$v\"}" "$progs/testprog.gb" "" "$roms/$v.sfc"
	nat "sc-$v" "sc-$v" --frames 300 --report 50 --vid-out "$work/sc-$v.vid"
	box "sc-$v-box" "sc-$v" --frames 300 --report 50
	stream "$work/sc-$v.out" > "$work/sc-$v.stream"; stream "$work/sc-$v-box.out" > "$work/sc-$v-box.stream"
	check "sgb-cartridge:$v" "with $v.sfc a game made only for the Game Boy runs in the Super Game Boy's border ($(size "$work/sc-$v.out")); the sandbox gives the native build's digests" \
		sh -c "[ '$(size "$work/sc-$v.out")' = 256x224 ] && cmp -s '$work/sc-$v.stream' '$work/sc-$v-box.stream'"
done

# ------------------------------------------------- the games of yours
found=0
for g in "$games"/*.gb "$games"/*.gbc; do
	[ -f "$g" ] || continue
	found=1
	n="$(basename "$g")"
	ext="${n##*.}"
	model=GB; [ "$ext" = gbc ] && model=GBC
	folder "g-$n" "{\"model\":\"$model\"}" "$g"
	gp="--frames 2000 --report 100 --press 4:600:5 --press 4:900:5 --press 7:1100:5 --press 4:1300:5 --press 7:1500:5 --press 3:1600:200"
	nat "g-$n" "g-$n" $gp
	box "g-$n-box" "g-$n" $gp
	stream "$work/g-$n.out" > "$work/g-$n.stream"; stream "$work/g-$n-box.out" > "$work/g-$n-box.stream"
	check "game:$n" "runs 2,000 frames on the $model with no boot ROM, Start and A pressed: $(pictures "$work/g-$n.out") different pictures and $(sounds "$work/g-$n.out") sounds in 20 reports; the sandbox gives the native build's digests" \
		sh -c "[ \$(wc -l < '$work/g-$n.stream') = 20 ] && [ $(pictures "$work/g-$n.out") -ge 5 ] && cmp -s '$work/g-$n.stream' '$work/g-$n-box.stream'"
	# a game that says it works with the Super Game Boy runs on it too
	if [ "$(byte "$g" 326) $(byte "$g" 331)" = "3 51" ]; then
		folder "gs-$n" '{"model":"SGB"}' "$g"
		nat "gs-$n" "gs-$n" $gp
		box "gs-$n-box" "gs-$n" $gp
		stream "$work/gs-$n.out" > "$work/gs-$n.stream"; stream "$work/gs-$n-box.out" > "$work/gs-$n-box.stream"
		check "game-sgb:$n" "runs 2,000 frames on the Super Game Boy, Start and A pressed: $(pictures "$work/gs-$n.out") different pictures in 20 reports, at $(size "$work/gs-$n.out"); the sandbox gives the native build's digests" \
			sh -c "[ \$(wc -l < '$work/gs-$n.stream') = 20 ] && [ $(pictures "$work/gs-$n.out") -ge 5 ] && [ '$(size "$work/gs-$n.out")' = 256x224 ] && cmp -s '$work/gs-$n.stream' '$work/gs-$n-box.stream'"
	fi
done
[ "$found" = 1 ] || report "game:*" SKIP "no .gb or .gbc game in tests/games-local"

# ------------------------------------------------- Chimera's engine
runner="$chimera_root/build/meson-linux/chimera-run"
package="$chimera_root/build/Cores/gearboy.chimeraCore"
if [ -n "$chimera_root" ] && [ -x "$runner" ] && [ -f "$package" ]; then
	mkdir -p "$work/proj"
	cp "$progs/testprog.gb" "$work/proj/"
	python3 "$root/tests/make-project.py" "$package" "$work/proj/test.chimeraProject" 300 model=GB \
		"press=P1 A:199:101" "press=P1 Down:149:151" panel="$work/panel.out" -- game="$work/proj/testprog.gb" > /dev/null
	"$runner" --project "$work/proj/test.chimeraProject" "$package" --files "$work/proj" --frames 300 \
		--dump "WRAM=$work/engine.wram" > "$work/engine.log" 2>&1
	box eng t --frames 300 --press "$(index t "P1 A")":200:101 --press "$(index t "P1 Down")":150:151 --dump-domain WRAM "$work/eng.wram"
	check "engine:project" "a project of the test program with presses plays in Chimera's headless runner to the sandbox runner's memory ($(wc -c < "$work/eng.wram" 2> /dev/null) bytes; the program read the buttons as $(r "$work/engine.wram" padSeen), Down and A)" \
		sh -c "cmp -s '$work/engine.wram' '$work/eng.wram' && [ '$(r "$work/engine.wram" padSeen)' = 129 ]"
	# each read of the buttons is reported to Chimera as it happens, so a Lua
	# script's event.oninputpoll runs there (chimera#344): every frame the
	# program read them has at least one, and counting them moves nothing
	"$runner" --project "$work/proj/test.chimeraProject" "$package" --files "$work/proj" --frames 300 --count-input-polls \
		--dump "WRAM=$work/polls.wram" --meta "$work/polls.meta" > "$work/polls.log" 2>&1
	polls=$(sed -n 's/^inputPolls=//p' "$work/polls.meta" 2> /dev/null)
	lags=$(sed -n 's/^lagFrames=//p' "$work/polls.meta" 2> /dev/null)
	check "engine:input-polls" "the program's reads of the buttons reach Chimera as they happen: ${polls:-none} in 300 frames, ${lags:-?} of them lag frames; the memory is the run's without counting" \
		sh -c "[ '${polls:-none}' != none ] && [ '${polls:-0}' -ge $(( 300 - ${lags:-300} )) ] && [ '${lags:-300}' -lt 300 ] && cmp -s '$work/polls.wram' '$work/engine.wram'"
	# a project of the Super Game Boy test program: P3's A, through the engine
	mkdir -p "$work/sproj"
	cp "$progs/sgbprog.gb" "$work/sproj/"
	python3 "$root/tests/make-project.py" "$package" "$work/sproj/sgb.chimeraProject" 300 model=SGB \
		"press=P3 A:199:101" panel="$work/panel-sgb.out" -- game="$work/sproj/sgbprog.gb" > /dev/null
	"$runner" --project "$work/sproj/sgb.chimeraProject" "$package" --files "$work/sproj" --frames 300 \
		--dump "WRAM=$work/sengine.wram" > "$work/sengine.log" 2>&1
	box seng s --frames 300 --press "$p3a":200:101 --dump-domain WRAM "$work/seng.wram"
	check "engine:sgb-project" "a Super Game Boy project with P3's A pressed plays in Chimera's headless runner to the sandbox runner's memory (P3's pad read as $(byte "$work/sengine.wram" 18))" \
		sh -c "cmp -s '$work/sengine.wram' '$work/seng.wram' && [ '$(byte "$work/sengine.wram" 18)' = 1 ]"
else
	for t in engine:project engine:input-polls engine:sgb-project; do
		report "$t" SKIP "needs CHIMERA_ROOT with build/meson-linux/chimera-run and build/Cores/gearboy.chimeraCore"
	done
fi

echo
echo "passed $ok, failed $failed, skipped $skipped"
[ "$failed" -eq 0 ]
