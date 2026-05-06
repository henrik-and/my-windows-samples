// exclusive-audio.cpp : This file contains the 'main' function.
#pragma comment(lib, "avrt.lib")

#include <chrono>
#include <iomanip>
#include <atomic>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <windows.h>
#include <wrl/client.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>
#include <fstream>
#include <algorithm>

using Microsoft::WRL::ComPtr;

// Thread-safe global flag for our audio loops
std::atomic<bool> g_isPlaying{ true };

// --- Constants & Defaults ---
const std::wstring CAPTURE_FILENAME = L"exclusive_capture.wav";
const int REFTIMES_PER_SEC = 10000000;
const int REFTIMES_PER_MILLISEC = 10000;

// --- CLI Configuration State ---
struct AppConfig {
    std::string mode = "output";
    std::string file = "";
    int duration = 0;
    bool continuous = true;
    bool verbose = false;
    bool playAfterRecord = false;
};

// --- Helper Functions ---
#pragma pack(push, 1)
struct WAVHeader {
    char riff[4] = { 'R', 'I', 'F', 'F' };
    uint32_t overall_size = 0;
    char wave[4] = { 'W', 'A', 'V', 'E' };
    char fmt_chunk_marker[4] = { 'f', 'm', 't', ' ' };
    uint32_t length_of_fmt = 16;
    uint16_t format_type = 1;
    uint16_t channels = 0;
    uint32_t sample_rate = 0;
    uint32_t byterate = 0;
    uint16_t block_align = 0;
    uint16_t bits_per_sample = 0;
    char data_chunk_header[4] = { 'd', 'a', 't', 'a' };
    uint32_t data_size = 0;
};
#pragma pack(pop)

void PrintHelp() {
    std::cout << "PowerGadget 0.1.0\n";
    std::cout << "Copyright 2026 Google Inc\n";
    std::cout << "USAGE:\n\n";
    std::cout << "  -m, --mode          (Default: output) Mode: 'input', 'output', or 'loopback'.\n";
    std::cout << "  -f, --file          (Default: '') Path to input wav file for output mode.\n";
    std::cout << "  -d, --duration      (Default: 0 sec) Duration to run in seconds.\n";
    std::cout << "  -o, --once          (Default: false) Play the file only once and exit.\n";
    std::cout << "  -p, --play          (Default: false) Play back the recorded file after Ctrl+C (Input mode only).\n";
    std::cout << "  -v, --verbose       (Default: false) Verbose output.\n";
    std::cout << "  --help              Display this help screen.\n";
}

AppConfig ParseArguments(int argc, char* argv[]) {
    AppConfig config;
    std::vector<std::string> args(argv + 1, argv + argc);

    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--help") {
            PrintHelp();
            exit(0);
        }
        else if ((args[i] == "-m" || args[i] == "--mode") && i + 1 < args.size()) {
            config.mode = args[++i];
        }
        else if ((args[i] == "-f" || args[i] == "--file") && i + 1 < args.size()) {
            config.file = args[++i];
        }
        else if ((args[i] == "-d" || args[i] == "--duration") && i + 1 < args.size()) {
            config.duration = std::stoi(args[++i]);
        }
        else if (args[i] == "-o" || args[i] == "--once") {
            config.continuous = false;
        }
        else if (args[i] == "-p" || args[i] == "--play") {
            config.playAfterRecord = true;
        }
        else if (args[i] == "-v" || args[i] == "--verbose") {
            config.verbose = true;
        }
    }
    return config;
}

// --- Windows Console Control Handler ---
BOOL WINAPI ConsoleCtrlHandler(DWORD dwCtrlType) {
    if (dwCtrlType == CTRL_C_EVENT || dwCtrlType == CTRL_CLOSE_EVENT) {
        std::cout << "\n[Info] Interrupt received (Ctrl+C). Initiating graceful shutdown...\n";

        // Signal the audio loop to break
        g_isPlaying = false;

        // Returning TRUE tells Windows: "I handled this, do not forcefully kill the process."
        return TRUE;
    }
    return FALSE;
}

// --- WASAPI Core Functions ---

// Helper function to create standard PCM formats
WAVEFORMATEX* CreateStandardPCMFormat(WORD channels, DWORD sampleRate, WORD bitsPerSample) {
    WAVEFORMATEX* pFormat = (WAVEFORMATEX*)CoTaskMemAlloc(sizeof(WAVEFORMATEX));
    if (pFormat == nullptr) return nullptr;

    pFormat->wFormatTag = WAVE_FORMAT_PCM;
    pFormat->nChannels = channels;
    pFormat->nSamplesPerSec = sampleRate;
    pFormat->wBitsPerSample = bitsPerSample;
    pFormat->nBlockAlign = (pFormat->nChannels * pFormat->wBitsPerSample) / 8;
    pFormat->nAvgBytesPerSec = pFormat->nSamplesPerSec * pFormat->nBlockAlign;
    pFormat->cbSize = 0;

    return pFormat;
}

// Probes the hardware for a supported exclusive mode format
HRESULT NegotiateExclusiveFormat(ComPtr<IAudioClient>& pAudioClient, WAVEFORMATEX** ppFormat, bool prefer16Bit = true) {
    // Array of formats to test: { SampleRate, BitsPerSample, Channels }
    struct FormatConfig { DWORD rate; WORD bits; WORD channels; };

    // We prioritize 16-bit formats if requested, otherwise we try high-res first.
    // We also test both 2-channel and 1-channel, as many mics are strictly mono.
    std::vector<FormatConfig> formats = {
        { 48000, 16, 2 }, { 48000, 16, 1 },
        { 44100, 16, 2 }, { 44100, 16, 1 },
        { 48000, 24, 2 }, { 48000, 24, 1 },
        { 96000, 24, 2 }, { 96000, 24, 1 }
    };

    for (const auto& fmt : formats) {
        WAVEFORMATEX* testFormat = CreateStandardPCMFormat(fmt.channels, fmt.rate, fmt.bits);

        HRESULT hr = pAudioClient->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, testFormat, NULL);

        if (hr == S_OK) {
            std::cout << "[Info] Hardware negotiated format: "
                << fmt.rate << "Hz, " << fmt.bits << "-bit, "
                << fmt.channels << " Channels.\n";
            *ppFormat = testFormat;
            return S_OK;
        }
        CoTaskMemFree(testFormat);
    }

    // If our strict list fails, ask the hardware what it natively wants
    std::cout << "[Warning] Hardware rejected all standard formats. Falling back to device default...\n";
    HRESULT hr = pAudioClient->GetMixFormat(ppFormat);
    if (SUCCEEDED(hr)) {
        std::cout << "[Info] Device forced default format: "
            << (*ppFormat)->nSamplesPerSec << "Hz, "
            << (*ppFormat)->wBitsPerSample << "-bit float, "
            << (*ppFormat)->nChannels << " Channels.\n";
        return S_OK;
    }

    return AUDCLNT_E_UNSUPPORTED_FORMAT;
}

// Your updated Initialization function
HRESULT InitExclusiveAudioClient(EDataFlow dataFlow, ComPtr<IAudioClient>& pAudioClient, WAVEFORMATEX** ppFormat) {
    ComPtr<IMMDeviceEnumerator> pEnumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, IID_PPV_ARGS(&pEnumerator));
    if (FAILED(hr)) return hr;

    ComPtr<IMMDevice> pDevice;
    hr = pEnumerator->GetDefaultAudioEndpoint(dataFlow, eConsole, &pDevice);
    if (FAILED(hr)) return hr;

    hr = pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&pAudioClient);
    if (FAILED(hr)) return hr;

    // 1. Negotiate the best format supported by the physical hardware
    hr = NegotiateExclusiveFormat(pAudioClient, ppFormat);
    if (FAILED(hr)) return hr;

    // 2. Initialize the audio client with the winning format
    REFERENCE_TIME hnsRequestedDuration = REFTIMES_PER_MILLISEC * 10;
    hr = pAudioClient->Initialize(
        AUDCLNT_SHAREMODE_EXCLUSIVE,
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
        hnsRequestedDuration,
        hnsRequestedDuration,
        *ppFormat,
        NULL
    );

    return hr;
}

void RunOutputMode(const AppConfig& config) {
    if (config.verbose) std::cout << "[Info] Starting Output Mode in Exclusive Mode...\n";

    ComPtr<IAudioClient> pAudioClient;
    WAVEFORMATEX* pFormat = nullptr;

    HRESULT hr = InitExclusiveAudioClient(eRender, pAudioClient, &pFormat);
    if (FAILED(hr)) {
        std::cerr << "[Error] Failed to initialize render client.\n";
        return;
    }

    ComPtr<IAudioRenderClient> pRenderClient;
    hr = pAudioClient->GetService(IID_PPV_ARGS(&pRenderClient));

    // Create the event that WASAPI will signal when it needs more data
    HANDLE hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    hr = pAudioClient->SetEventHandle(hEvent);

    // In Exclusive Event-Driven mode, the buffer size is fixed and dictated by your Initialize call
    UINT32 bufferFrameCount;
    hr = pAudioClient->GetBufferSize(&bufferFrameCount);

    DWORD bytesPerFrame = pFormat->nBlockAlign;
    DWORD bufferSizeBytes = bufferFrameCount * bytesPerFrame;

    // --- Open the WAV File ---
    std::ifstream wavFile;
    if (!config.file.empty()) {
        wavFile.open(config.file, std::ios::binary);
        if (!wavFile.is_open()) {
            std::cerr << "[Error] Could not open WAV file: " << config.file << "\n";
            return;
        }
        // Note: For a production app, you must parse the WAV header (the first 44 bytes) 
        // to ensure it matches pFormat, and then seek past it to the raw PCM data.
        // For this demo, we are just skipping a generic 44-byte header.
        wavFile.seekg(44, std::ios::beg);
    }

    // --- Pre-Roll ---
    // You MUST fill the first buffer before starting the clock, otherwise it glitches instantly.
    BYTE* pData;
    hr = pRenderClient->GetBuffer(bufferFrameCount, &pData);
    memset(pData, 0, bufferSizeBytes); // Fill with silence
    hr = pRenderClient->ReleaseBuffer(bufferFrameCount, 0);

    // Start the audio engine
    hr = pAudioClient->Start();
    if (config.verbose) std::cout << "[Info] Audio playback started. Press Ctrl+C to stop.\n";

    g_isPlaying = true; // Ensure it is true before starting

    // --- Safe Timeout Calculation ---
    // Calculate expected time for one buffer in milliseconds
    DWORD bufferTimeMS = (bufferFrameCount * 1000) / pFormat->nSamplesPerSec;
    // Set timeout to 5x the buffer time to prevent false positives from slight CPU spikes
    DWORD timeoutMS = bufferTimeMS * 5;
    if (timeoutMS < 100) timeoutMS = 100; // Minimum 100ms timeout for safety

    std::cout << "[Debug] Audio loop starting. Buffer time: " << bufferTimeMS
        << "ms, Timeout set to: " << timeoutMS << "ms.\n";

    // --- The Diagnostic Audio Pump ---
    while (g_isPlaying) {
        // 1. Wait for the DAC to ask for data
        DWORD waitResult = WaitForSingleObject(hEvent, timeoutMS);

        if (waitResult != WAIT_OBJECT_0) {
            if (waitResult == WAIT_TIMEOUT) {
                std::cerr << "\n[FATAL EXIT] WaitForSingleObject TIMEOUT!\n";
                std::cerr << "The audio engine stopped signaling the event handle. "
                    << "The device was likely invalidated or hijacked by another app.\n";
            }
            else if (waitResult == WAIT_FAILED) {
                std::cerr << "\n[FATAL EXIT] WaitForSingleObject FAILED. Win32 Error: " << GetLastError() << "\n";
            }
            else {
                std::cerr << "\n[FATAL EXIT] WaitForSingleObject returned unknown code: " << waitResult << "\n";
            }
            break; // Exit loop
        }

        // 2. Request the buffer from the hardware
        hr = pRenderClient->GetBuffer(bufferFrameCount, &pData);
        if (hr == AUDCLNT_E_BUFFER_ERROR) {
            std::cout << "[Warning] Buffer error (glitch). Skipping a frame...\n";
            continue;
        }
        else if (FAILED(hr)) {
            std::cerr << "\n[FATAL EXIT] GetBuffer failed. HRESULT: 0x" << std::hex << hr << std::dec << "\n";
            if (hr == AUDCLNT_E_DEVICE_INVALIDATED) std::cerr << "(Error: AUDCLNT_E_DEVICE_INVALIDATED)\n";
            break; // Exit loop
        }

        // 3. Fill the buffer
        if (wavFile.is_open() && !wavFile.eof()) {
            wavFile.read(reinterpret_cast<char*>(pData), bufferSizeBytes);
            std::streamsize bytesRead = wavFile.gcount();

            if (bytesRead < bufferSizeBytes) {
                memset(pData + bytesRead, 0, bufferSizeBytes - bytesRead);

                if (!config.continuous) {
                    std::cout << "\n[CLEAN EXIT] Reached end of WAV file.\n";
                    g_isPlaying = false; // Cleanly stop next iteration
                }
                else {
                    // Rewind for continuous play
                    wavFile.clear();
                    wavFile.seekg(44, std::ios::beg);
                }
            }
        }
        else {
            memset(pData, 0, bufferSizeBytes);
        }

        // 4. Hand the buffer back to the hardware
        hr = pRenderClient->ReleaseBuffer(bufferFrameCount, 0);
        if (FAILED(hr)) {
            std::cerr << "\n[FATAL EXIT] ReleaseBuffer failed. HRESULT: 0x" << std::hex << hr << std::dec << "\n";
            break; // Exit loop
        }
    }

    std::cout << "[Debug] Escaped the audio loop. Cleaning up...\n";

    // Cleanup
    pAudioClient->Stop();
    CloseHandle(hEvent);
    CoTaskMemFree(pFormat);
    if (wavFile.is_open()) wavFile.close();
}

void RunInputMode(const AppConfig& config) {
    if (config.verbose) std::cout << "[Info] Starting Input Mode in Exclusive Mode...\n";

    ComPtr<IAudioClient> pAudioClient;
    WAVEFORMATEX* pFormat = nullptr;

    // Use eCapture to get the input device (microphone / line in)
    HRESULT hr = InitExclusiveAudioClient(eCapture, pAudioClient, &pFormat);
    if (FAILED(hr)) {
        std::cerr << "[Error] Failed to initialize capture client.\n";
        return;
    }

    ComPtr<IAudioCaptureClient> pCaptureClient;
    hr = pAudioClient->GetService(IID_PPV_ARGS(&pCaptureClient));

    HANDLE hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    hr = pAudioClient->SetEventHandle(hEvent);

    // Get buffer parameters
    UINT32 bufferFrameCount;
    hr = pAudioClient->GetBufferSize(&bufferFrameCount);
    DWORD bytesPerFrame = pFormat->nBlockAlign;

    // --- Prepare the Output File ---
    std::string outFilename = config.file.empty() ? std::string(CAPTURE_FILENAME.begin(), CAPTURE_FILENAME.end()) : config.file;
    std::ofstream wavFile(outFilename, std::ios::binary);
    if (!wavFile.is_open()) {
        std::cerr << "[Error] Could not open output file: " << outFilename << "\n";
        return;
    }

    // Populate and write a dummy WAV header
    WAVHeader header;
    header.channels = pFormat->nChannels;
    header.sample_rate = pFormat->nSamplesPerSec;
    header.byterate = pFormat->nAvgBytesPerSec;
    header.block_align = pFormat->nBlockAlign;
    header.bits_per_sample = pFormat->wBitsPerSample;
    wavFile.write(reinterpret_cast<const char*>(&header), sizeof(WAVHeader));

    uint32_t totalDataBytesWritten = 0;

    // Start the audio engine
    hr = pAudioClient->Start();
    if (config.verbose) std::cout << "[Info] Recording started. Writing to " << outFilename << ". Press Ctrl+C to stop.\n";

    g_isPlaying = true;
    DWORD timeoutMS = ((bufferFrameCount * 1000) / pFormat->nSamplesPerSec) * 5;
    if (timeoutMS < 100) timeoutMS = 100;

    // --- Time Tracking Variables ---
    auto startTime = std::chrono::steady_clock::now();
    auto lastPrintTime = startTime;

    // Print the initial 00:00 state
    std::cout << "[Info] Recording... [00:00]" << std::flush;

    // --- The Audio Pump ---
    while (g_isPlaying) {
        DWORD waitResult = WaitForSingleObject(hEvent, timeoutMS);

        if (waitResult != WAIT_OBJECT_0) {
            std::cerr << "\n[FATAL EXIT] Device invalidated or timeout.\n";
            break;
        }

        // --- Non-blocking UI Heartbeat ---
        auto currentTime = std::chrono::steady_clock::now();
        auto elapsedSincePrint = std::chrono::duration_cast<std::chrono::seconds>(currentTime - lastPrintTime).count();

        // Update the UI every second
        if (elapsedSincePrint >= 1) {
            auto totalElapsed = std::chrono::duration_cast<std::chrono::seconds>(currentTime - startTime).count();
            int mins = totalElapsed / 60;
            int secs = totalElapsed % 60;

            // Use \r to return to the beginning of the line and overwrite it
            std::cout << "\r[Info] Recording... ["
                << std::setfill('0') << std::setw(2) << mins << ":"
                << std::setfill('0') << std::setw(2) << secs << "]" << std::flush;

            lastPrintTime = currentTime;
        }

        UINT32 packetLength = 0;
        hr = pCaptureClient->GetNextPacketSize(&packetLength);

        // Pull all available packets from the hardware
        while (packetLength != 0) {
            BYTE* pData;
            UINT32 numFramesAvailable;
            DWORD flags;

            // 1. Get the captured buffer
            hr = pCaptureClient->GetBuffer(&pData, &numFramesAvailable, &flags, NULL, NULL);
            if (FAILED(hr)) break;

            DWORD bytesToWrite = numFramesAvailable * bytesPerFrame;

            // 2. Check if the hardware flagged this packet as silent
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                // The hardware dropped audio. Write pure silence to keep timing accurate.
                for (DWORD i = 0; i < bytesToWrite; ++i) {
                    wavFile.put(0);
                }
            }
            else {
                // Write the raw PCM data to disk
                wavFile.write(reinterpret_cast<const char*>(pData), bytesToWrite);
            }

            totalDataBytesWritten += bytesToWrite;

            // 3. Release the buffer back to the hardware
            hr = pCaptureClient->ReleaseBuffer(numFramesAvailable);

            // Get the size of the next packet (if any)
            hr = pCaptureClient->GetNextPacketSize(&packetLength);
        }
    }

    // Notice the \n to drop down to the next line safely after the \r timer finishes
    std::cout << "\n[Debug] Recording stopped. Finalizing WAV file...\n";

    // --- Finalize the WAV Header ---
    // Now that recording is stopped, we know exactly how much data we recorded.
    // Rewind to the beginning of the file and write the correct file sizes.
    header.data_size = totalDataBytesWritten;
    header.overall_size = totalDataBytesWritten + sizeof(WAVHeader) - 8;

    wavFile.seekp(0, std::ios::beg);
    wavFile.write(reinterpret_cast<const char*>(&header), sizeof(WAVHeader));
    wavFile.close();

    // Cleanup
    pAudioClient->Stop();
    CloseHandle(hEvent);
    CoTaskMemFree(pFormat);
    std::cout << "[Info] File saved successfully.\n";
}

void RunLoopbackMode(const AppConfig& config) {
    if (config.verbose) std::cout << "[Info] Starting Loopback Mode (Capture -> Render)...\n";
    // Loopback requires setting up both the Capture and Render clients in Exclusive mode.
    // Because clocks drift between physical input and output hardware, you will need to implement
    // a circular ring buffer (lock-free is best) between the capture thread and the render thread.

    // Thread 1: Runs logic similar to RunInputMode, pushing data into a thread-safe ring buffer.
    // Thread 2: Runs logic similar to RunOutputMode, pulling data from the ring buffer.
    std::cout << "Loopback architecture requires threading and a lock-free ring buffer.\n";
}

int main(int argc, char* argv[]) {
    // Register the Ctrl+C handler
    if (!SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE)) {
        std::cerr << "[Warning] Could not set control handler. Ctrl+C will forcefully exit.\n";
    }

    AppConfig config = ParseArguments(argc, argv);

    // Initialize COM on the main thread (Apartment Threaded)
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        std::cerr << "Failed to initialize COM library.\n";
        return -1;
    }

    // Boost thread priority for Audio Processing
    DWORD taskIndex = 0;
    HANDLE hTask = AvSetMmThreadCharacteristics(L"Pro Audio", &taskIndex);

    if (config.mode == "output") {
        RunOutputMode(config);
    }
    else if (config.mode == "input") {
        RunInputMode(config);
        
        if (config.playAfterRecord) {
            // 1. Reset the global flag so Ctrl+C works again for playback
            g_isPlaying = true;

            // 2. Ensure Output Mode knows exactly what file we just made
            if (config.file.empty()) {
                config.file = std::string(CAPTURE_FILENAME.begin(), CAPTURE_FILENAME.end());
            }

            // 3. Read the exact duration from the new WAV file's header
            double durationSecs = 0.0;
            std::ifstream checkFile(config.file, std::ios::binary);
            if (checkFile.is_open()) {
                WAVHeader parsedHeader;
                checkFile.read(reinterpret_cast<char*>(&parsedHeader), sizeof(WAVHeader));
                if (parsedHeader.byterate > 0) {
                    // Duration = Total Audio Bytes / Bytes per Second
                    durationSecs = static_cast<double>(parsedHeader.data_size) / parsedHeader.byterate;
                }
                checkFile.close();
            }

            // 4. Print the formatted message
            std::cout << "\n[Info] Initiating playback of recorded file: "
                << config.file << " ("
                << std::fixed << std::setprecision(2) << durationSecs << " seconds)...\n";

            // 5. Force it to only play once instead of looping forever
            config.continuous = false;

            // 6. Start the output engine
            RunOutputMode(config);
        }
    }
    else if (config.mode == "loopback") {
        RunLoopbackMode(config);
    }
    else {
        std::cerr << "Unknown mode: " << config.mode << "\n";
        PrintHelp();
    }

    if (hTask != NULL) {
        AvRevertMmThreadCharacteristics(hTask);
    }

    CoUninitialize();
    return 0;
}
