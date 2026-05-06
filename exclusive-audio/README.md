# WASAPI Exclusive Mode Audio Engine

A high-performance, ultra-low latency command-line audio utility for Windows. Written in C++, this application interfaces directly with audio hardware using the Windows Audio Session API (WASAPI) in **Exclusive Mode**. By bypassing the Windows audio mixer completely, it guarantees bit-perfect audio transfer and minimal latency.

## 🚀 Features

*   **Direct Hardware Access:** Locks the audio hardware exclusively, preventing other Windows applications from interfering with the audio stream.
*   **Native Format Negotiation:** Actively probes hardware capabilities, prioritizing native 16-bit/48kHz PCM streams for zero-conversion efficiency.
*   **Lock-Free Real-Time Loopback:** Routes audio directly from an ADC (Microphone) to a DAC (Speakers/Headphones) using a high-performance memory bridge and staggered clock synchronization.
*   **Hardware Event-Driven:** Uses strict 1:1 hardware interrupt mapping to eliminate CPU polling and prevent micro-stutters.
*   **Smart Software Gain:** Applies an in-place +12dB (4.0x multiplier) software boost to native 16-bit input streams with hard-clipping protection.

---

## 🛠️ Command-Line Usage

**Syntax:**
```cmd
exclusive-audio.exe -m <mode> [options]
```

### Modes (`-m` or `--mode`)
The application requires an operating mode to be specified.

| Mode | Description |
| :--- | :--- |
| `input` | Locks the default capture device (Microphone/Line-In) and records raw PCM audio directly to a `.wav` file. |
| `output` | Locks the default render device (Speakers/Headphones) and plays a `.wav` file directly to the DAC. |
| `loopback` | Connects the capture and render devices in real-time for zero-latency monitoring. |

### Options & Flags

| Flag | Name | Description |
| :--- | :--- | :--- |
| `-f` | `--file` | Specifies the path to the `.wav` file. **Input mode:** where to save the recording. **Output mode:** which file to play. (Defaults to `capture.wav`). |
| `-p` | `--play` | **Continuous Mode.** In Output mode, this seamlessly loops the `.wav` file infinitely until `Ctrl+C` is pressed. |
| `-v` | `--verbose` | Enables detailed console logging. Prints hardware format negotiation results, buffer capacities, and driver initialization states. |
| `-h` | `--help` | Displays the help menu and exits. |

---

## 💻 Examples

**1. Record your microphone to a custom file**
Records your voice with the built-in software gain boost applied. Press `Ctrl+C` to stop and finalize the WAV header safely.
```cmd
exclusive-audio.exe -m input -f my_recording.wav
```

**2. Play an audio file continuously**
Locks the DAC and loops the specified file forever. Great for testing hardware output stability.
```cmd
exclusive-audio.exe -m output -f my_recording.wav -p
```

**3. Test real-time hardware latency (Loopback Mode)**
Routes your microphone directly to your headphones in real-time. Includes detailed hardware logs.
> ⚠️ **Warning:** *Always wear headphones when using Loopback mode. Using open speakers will instantly create a severe audio feedback loop.*
```cmd
exclusive-audio.exe -m loopback -v
```

---

## 🧠 Architecture Notes

*   **1:1 Event Grabs:** To prevent the *Modulo CPU Trap* and drop-outs, this engine avoids `GetNextPacketSize` probing. It relies on strict hardware events to grab audio buffers only when the DMA is ready.
*   **Clock Drift Compensation:** Loopback mode bridges two separate hardware clocks (Input and Output). It utilizes a staggered-start mechanism (50ms pre-roll) and a minimal ring buffer using high-speed `memcpy` to permanently prevent the DAC from starving, ensuring crackle-free audio.