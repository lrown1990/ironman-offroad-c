# Ironman Offroad in C

A rewrite in C of the 1990 DOS version of *Ivan "Ironman" Stewart's Super Off
Road*, the Graftgold conversion of the Leland arcade game. It plays the same
races as the original: the same tracks, the same trucks, the same computer
drivers, the same music and sound effects.

**The original game is not included.** This repository holds only the new
code. To play you need the files of your own copy of the DOS game.

## How it was made

The game was taken apart routine by routine, and every part of it was first
rebuilt as a model and checked against the original program and against
recordings of the real game running in DOSBox. The C code was then written
piece by piece and compared with those models call by call, down to every
byte of memory, every sound register and every pixel, until whole seasons of
play came out identical.

The C works on the same memory layout as the original, so the comments in the
code quote the addresses of the routines they come from. The models, the
disassembly and the recordings are not published here, because they contain
the original game.

## What you need

- the files of the DOS game, in lower case (`game.exe`, `ega.exe`,
  `readkey.exe`, `adlib.sfx`, and the `.xx`, `.pcg` and `.bin` files), in a
  folder called `OFFROAD` next to this README;
- a C11 compiler and [SDL2](https://www.libsdl.org/).

On macOS with Homebrew:

```sh
brew install sdl2
make game
./build/offroad
```

`./build/offroad path/to/folder` uses another folder for the original files.
The program only reads them, it never writes there. The high scores are kept
in the user's application data folder (on macOS
`~/Library/Application Support/carlosinatra/Super Off Road`).

It has been built and played on macOS. On Linux the same commands should work
with the SDL2 development package (`libsdl2-dev` on Debian and Ubuntu), but
that has not been tried yet.

## Controls

| | Accelerate | Left | Right | Nitro |
|---|---|---|---|---|
| Red truck (keyboard) | W | A | D | C |
| Blue truck (keyboard) | I | J | L | Right Shift |
| Yellow truck (controller) | R2 or stick forward | D-pad or stick left | D-pad or stick right | Cross / A |

Esc asks whether to quit. A controller can be plugged in or out during a game;
when it arrives the game calibrates it, so leave the stick centred for a
moment.

The start-up questions of the original (joystick, sound card) are answered
automatically: joystick yes, AdLib music.

## Fixing the acceleration bug

In the original, buying acceleration in the shop does nothing: a slip in the
code takes the player's acceleration from the truck's starting position
instead of the level bought, so it is at the top from the very first race.
The computer's trucks are not affected. Start the game with

```sh
./build/offroad --fix-acceleration
```

to correct it. Your truck then starts slower and the shop upgrade does what
it says, which makes the first races harder. The browser build has the fix
turned on.

## In the browser

```sh
./web/build.sh
```

compiles the same code with [Emscripten](https://emscripten.org/) into
`build/web/offroad.{js,wasm,data}`, with the original files packed inside
`offroad.data` (so that file is not something to share). It is a module meant
to be driven by a host page, which this repository does not include. `OUT` and `DATA` choose the output folder and the
folder of the original files, `EMSDK` the Emscripten installation (default
`/opt/emsdk`, unless `emcc` is already on the path).

## Credits

- *Ivan "Ironman" Stewart's Super Off Road*: arcade game by Leland
  Corporation, DOS conversion by Graftgold. All rights to the game, its name,
  its graphics, music and data belong to their owners. This project is not
  affiliated with or endorsed by them.
- Sound: [Nuked OPL3](https://github.com/nukeykt/Nuked-OPL3) by Nuke.YKT,
  under the GNU LGPL 2.1, in `third_party/nuked-opl3`.

## License

The code in this repository, except `third_party`, is released under the MIT
License: see `LICENSE`.
