# Rhino Audio Router

A REAPER plugin, designed for screen reader users, that does two jobs:

1. **Capture a program.** It brings the sound of one program, such as Chrome, Discord, Zoom or a game, into a REAPER track, so you can record it. Put the plugin on several tracks to record several programs, each on its own track.
2. **Send a track to a device.** It plays a REAPER track to any Windows output device. Paired with a virtual cable, that lets another program such as Zoom hear REAPER as a microphone.

REAPER keeps using your audio interface (for example a MOTU M2) through ASIO the whole time. No virtual audio drivers are needed for capturing.

The window uses only standard Windows controls, so NVDA and JAWS read it without scripts. Status changes are spoken automatically. It works with OSARA.

Status: early version (0.2.0). Recording a program (Chrome) into REAPER has been tested and works. Sending a track to a device has not been tested yet. Please report what works and what does not.

## Requirements

- Windows 11, or Windows 10 version 2004 or later. Capturing a single program needs these versions.
- REAPER 6.71 or later, which supports CLAP plugins. OSARA is recommended.
- Only for sending a track into another program: a free virtual cable such as VB-Cable from vb-audio.com. Capturing programs does not need it.

## Installing

1. Download the plugin from [RhinoAudioRouter-windows-x64.zip](downloads/RhinoAudioRouter-windows-x64.zip) (the file is in this repository's `downloads` folder), and unzip it. It contains `RhinoAudioRouter.clap` and this guide. To update an older version, close REAPER first, then replace the file in the next step.
2. Copy `RhinoAudioRouter.clap` into `C:\Program Files\Common Files\CLAP`. Create the `CLAP` folder if it does not exist. Windows will ask for administrator permission.
3. In REAPER, open Preferences (Control+P), go to **Plug-ins**, then **CLAP**, and activate **Re-scan**. Or simply restart REAPER.
4. The plugin appears in the FX browser as **CLAP: Rhino Audio Router (BlindRyan)**. Type "Rhino" in the FX browser's filter to find it.

### Upgrading from App Audio Bridge

Rhino Audio Router was called App Audio Bridge before version 0.2.0. To upgrade, close REAPER, delete `AppAudioBridge.clap` from `C:\Program Files\Common Files\CLAP`, and copy `RhinoAudioRouter.clap` there instead. Projects saved with App Audio Bridge open with Rhino Audio Router and keep their settings.

## The plugin window

When you add the plugin, or open it from the track's FX chain, press Tab to move into its controls. In order, they are:

1. **Mode** (combo box): "Capture audio from a program" or "Send this track to an output device". Choose with the arrow keys.
2. One of these, depending on the mode:
   - **Program to capture** (list): programs that have played sound, then, if the next checkbox is checked, other programs with windows. Each item is the program's name and file name, such as "Google Chrome (chrome.exe)".
   - **Output device to send to** (list): every active Windows output device.
3. **Show all programs, not only ones that have played sound** (checkbox, capture mode only). Useful when a program has not made any sound yet since it started.
4. **Use selected** (button): starts capturing the selected program, or sending to the selected device. Pressing Enter in a list does the same.
5. **Stop** (button): stops capturing or sending.
6. **Refresh lists** (button): re-reads the programs and devices and says how many it found.
7. **Announce level** (button): says the loudest level over the last 3 seconds, such as "Captured audio peak over the last 3 seconds: -14.2 dB", or says it is silent. Use it to check that sound is really arriving.
8. **Status** (read-only text): the latest message. Messages are also spoken as they happen, such as "Capturing Google Chrome (chrome.exe)." or "Discord closed. Waiting for it to start again."

The plugin also has one parameter, **Gain**, from -60 to +12 dB. It adjusts the captured sound in capture mode, and the sent sound in send mode. You can reach it with OSARA's View FX parameters command, or automate it like any other parameter.

Your choices are saved with the REAPER project. The plugin remembers programs by file name. If you choose Chrome and later close and reopen it, capture resumes by itself.

### If keys do not reach the plugin

REAPER may use some keys itself (Space for play, for example) instead of passing them to the plugin window. If that happens, turn on REAPER's **Send all keyboard input to plug-in** option for this FX, in the FX window's menu. Tab, Shift+Tab, the arrow keys and Enter are handled by the plugin itself, so they should work either way.

## Recording a program into REAPER

1. Insert a new track (Control+T) and name it after the program.
2. Add Rhino Audio Router to the track's FX.
3. In the plugin window, leave Mode on "Capture audio from a program". Select the program in the list and press Enter or Use selected. You should hear "Capturing ...".
4. Play something in the program, then use **Announce level** to confirm sound is arriving.
5. Set the track to record its output, not its input. In the track's record-arm context menu, choose **Record: output**, then **Record: output (stereo)**.
6. Arm the track and record as usual. The plugin replaces the track's input with the program's sound, so the track's input selection does not matter.
7. Play back the recording as usual. While REAPER is playing without recording, the plugin lets the track's recorded items through instead of the live program, so you hear what you recorded. (Versions before 0.1.2 played the live program instead, so recordings seemed silent on playback.)

Repeat on more tracks for more programs. Your microphone can be on its own track, recording from the M2's inputs as normal.

### Hearing the program twice

The program still plays through its usual device, and REAPER also plays the captured copy. If you hear an echo, turn off the track's master send in its routing window. The recording is not affected, because the track's output is recorded before it reaches the master.

### Smooth playback on tracks that are not armed

REAPER may process FX ahead of time on tracks that are not armed, which does not suit live sound. If captured sound stutters on a track that is not armed, turn on **Prevent anticipative FX** in the track's performance options.

## Sending a track into another program (for example Zoom)

1. Install VB-Cable and restart Windows.
2. Add Rhino Audio Router to the track you want to send, for example your microphone track, or a bus with several tracks routed to it.
3. Set Mode to "Send this track to an output device", select **CABLE Input (VB-Audio Virtual Cable)**, and press Enter.
4. In Zoom, Discord or any other program, choose **CABLE Output** as the microphone.

The track's sound still passes through REAPER normally. The plugin only sends a copy.

## What cannot be captured

- Programs that play through ASIO, or through WASAPI exclusive mode, bypass Windows' mixer. Windows cannot capture them per program. Most everyday programs use normal shared mode, so this mostly affects other music software.
- Protected video, such as some streaming services in a browser, may capture as silence.
- REAPER itself is never listed, to avoid feedback.

## Troubleshooting messages

- **"Waiting for ... to start."** The program is not running. Capture starts when it does.
- **"Could not capture ...: this version of Windows does not support per-program capture."** Update Windows to one of the versions listed under Requirements.
- **"Not running yet."** or **"Stopped. REAPER has paused audio for this plugin."** REAPER's audio engine is off, or the FX is bypassed or offline.
- Occasional small clicks: the program's audio and your audio interface run on separate clocks. The plugin corrects this by skipping or waiting when they drift apart. Smoother correction is planned.

## For developers

The plugin is C++17 and uses the CLAP plugin format, a single header-only SDK that is fetched automatically.

- `src/engine.cpp`: background thread doing WASAPI process-loopback capture, or event-driven rendering to a device.
- `src/ring_buffer.h`: lock-free single-producer, single-consumer ring between that thread and REAPER's audio thread. It holds latency steady and absorbs clock drift.
- `src/win_audio.cpp`: listing programs through audio sessions and the process tree, listing devices, and activating process loopback.
- `src/gui.cpp` and `src/announce.cpp`: the accessible window and UI Automation announcements.
- `src/plugin.cpp` and `src/entry.cpp`: CLAP glue for parameters, state and the window.

Build on Windows with Visual Studio 2022 and CMake:

```
cmake -S . -B build -A x64
cmake --build build --config Release
```

Cross-compile from Linux with MinGW-w64:

```
cmake -S . -B build-win -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake
cmake --build build-win
```

Run the unit tests on any platform:

```
cmake -S . -B build-tests && cmake --build build-tests && ctest --test-dir build-tests
```

Planned next: smooth drift correction by resampling instead of skipping, a VST3 build for other hosts, and a release download page.
