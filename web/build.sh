#!/bin/bash
# Super Off Road in WebAssembly: the same program (src/) and the same shell
# (sdl/main.c). ASYNCIFY turns the machine's waits (SDL_Delay for the next
# tick, the loops that wait for a key) into returns to the browser, so the
# program keeps the loops it has; setjmp/longjmp (the end of a run) go
# through Emscripten's JS version, which ASYNCIFY supports.
# Out: $OUT/offroad.{js,wasm,data} (default build/web), the original files of
# $DATA (default OFFROAD, else ../OFFROAD) packed at /OFFROAD.
# The site builds with OUT=../../public/assets/offroad.
set -e
cd "$(dirname "$0")/.."
if ! command -v emcc >/dev/null; then
	. "${EMSDK:-/opt/emsdk}/emsdk_env.sh" >/dev/null 2>&1
fi
export EM_CACHE="${EM_CACHE:-$HOME/.emscripten_cache}"

OUT="${OUT:-build/web}"
if [ -z "$DATA" ]; then
	if [ -f OFFROAD/game.exe ]; then DATA=OFFROAD; else DATA=../OFFROAD; fi
fi
if [ ! -f "$DATA/game.exe" ]; then
	echo "build.sh: the original files were not found in $DATA" >&2
	exit 1
fi
mkdir -p "$OUT"

emcc \
	-o "$OUT/offroad.js" \
	src/*.c sdl/main.c third_party/nuked-opl3/opl3.c \
	-Iinclude -Ithird_party/nuked-opl3 \
	-std=gnu11 -O2 \
	-Wall -Wextra -Wno-unused-parameter \
	-sUSE_SDL=2 \
	-sASYNCIFY -sASYNCIFY_STACK_SIZE=65536 \
	-sSTACK_SIZE=1048576 \
	-sALLOW_MEMORY_GROWTH=1 \
	-sMODULARIZE=1 -sEXPORT_NAME=SuperOffRoad \
	-sEXPORTED_RUNTIME_METHODS=ccall,cwrap,FS,ENV \
	-sEXPORTED_FUNCTIONS=_main,_offroad_riduci,_offroad_chiudi,_offroad_apri,_offroad_muto,_offroad_tick,_offroad_fuoco_perso \
	-lidbfs.js \
	--preload-file "$DATA@/OFFROAD"

echo "fatto: $OUT/offroad.{js,wasm,data}"
ls -la "$OUT"
