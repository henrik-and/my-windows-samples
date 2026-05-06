// exclusive-audio.cpp : This file contains the 'main' function.
#pragma comment(lib, "avrt.lib")

#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <windows.h>
#include <wrl/client.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>
#include <fstream>
#include <algorithm>

using Microsoft::WRL::ComPtr;

// --- Constants & Defaults ---
const std::wstring CAPTURE_FILENAME = L"exclusive_capture.wav";
const int REFTIMES_PER_SEC = 10000000;
const int REFTIMES_PER_MILLISEC = 10000;

// --- CLI Configuration State ---

// --- CLI Configuration State ---
struct AppConfig {
    std::string mode = "output";  // "input", "output", or "loopback"
    std::string file = "";        // Path to wav file to play
    int duration = 0;
    bool continuous = true;
    bool verbose = false;
};

// --- Helper Functions ---
void PrintHelp() {
    std::cout << "PowerGadget 0.1.0\n";
    std::cout << "Copyright 2026 Google Inc\n";
    std::cout << "USAGE:\n\n";
    std::cout << "  -m, --mode          (Default: output) Mode: 'input', 'output', or 'loopback'.\n";
    std::cout << "  -f, --file          (Default: '') Path to input wav file for output mode.\n";
    std::cout << "  -d, --duration      (Default: 0 sec) Duration to run in seconds.\n";
    std::cout << "  -o, --once          (Default: false) Play the file only once and exit.\n";
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
            config.continuous = false; // <-- Allow opting out of the loop
        }
        else if (args[i] == "-v" || args[i] == "--verbose") {
            config.verbose = true;
        }
    }
    return config;
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
HRESULT NegotiateExclusiveFormat(ComPtr<IAudioClient>& pAudioClient, WAVEFORMATEX** ppFormat) {
    // Array of formats to test: { SampleRate, BitsPerSample }
    // We test highest quality first, down to standard CD quality.
    struct FormatConfig { DWORD rate; WORD bits; };
    // Test our preferred standard formats FIRST
    FormatConfig formats[] = {
        { 48000, 16 },
        { 48000, 24 },
        { 44100, 24 },
        { 44100, 16 },
        // Then fall back to high-res if standard fails
        { 96000, 24 },
        { 96000, 16 }
    };

    for (const auto& fmt : formats) {
        WAVEFORMATEX* testFormat = CreateStandardPCMFormat(2, fmt.rate, fmt.bits);

        // Ask the hardware: "Do you support this in exclusive mode?"
        HRESULT hr = pAudioClient->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, testFormat, NULL);

        if (hr == S_OK) {
            std::cout << "[Info] Hardware accepted format: " << fmt.rate << "Hz, " << fmt.bits << "-bit.\n";
            *ppFormat = testFormat;
            return S_OK;
        }

        // If not supported, free the memory and try the next one in the loop
        CoTaskMemFree(testFormat);
    }

    std::cerr << "[Error] No compatible exclusive mode format found for this device.\n";
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

    bool playing = true;

    // --- Safe Timeout Calculation ---
    // Calculate expected time for one buffer in milliseconds
    DWORD bufferTimeMS = (bufferFrameCount * 1000) / pFormat->nSamplesPerSec;
    // Set timeout to 5x the buffer time to prevent false positives from slight CPU spikes
    DWORD timeoutMS = bufferTimeMS * 5;
    if (timeoutMS < 100) timeoutMS = 100; // Minimum 100ms timeout for safety

    std::cout << "[Debug] Audio loop starting. Buffer time: " << bufferTimeMS
        << "ms, Timeout set to: " << timeoutMS << "ms.\n";

    // --- The Diagnostic Audio Pump ---
    while (playing) {
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
                    playing = false; // Cleanly stop next iteration
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
    if (config.verbose) std::cout << "[Info] Starting Input Mode. Recording to " << std::string(CAPTURE_FILENAME.begin(), CAPTURE_FILENAME.end()) << "\n";

    ComPtr<IAudioClient> pAudioClient;
    WAVEFORMATEX* pFormat = nullptr;

    HRESULT hr = InitExclusiveAudioClient(eCapture, pAudioClient, &pFormat);
    if (FAILED(hr)) {
        std::cerr << "Failed to initialize capture client.\n";
        return;
    }

    ComPtr<IAudioCaptureClient> pCaptureClient;
    pAudioClient->GetService(IID_PPV_ARGS(&pCaptureClient));

    HANDLE hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    pAudioClient->SetEventHandle(hEvent);

    // TODO: Open std::ofstream to write WAV header based on pFormat

    pAudioClient->Start();

    // Capture Loop
    bool capturing = true;
    while (capturing) {
        WaitForSingleObject(hEvent, 2000); // Wait for audio data to be ready

        // 1. Get buffer: pCaptureClient->GetBuffer(...)
        // 2. Write PCM bytes to your constant WAV file (CAPTURE_FILENAME)
        // 3. Release buffer: pCaptureClient->ReleaseBuffer(...)

        if (!config.continuous /* && Check if duration is met */) {
            capturing = false;
        }
    }

    pAudioClient->Stop();
    CloseHandle(hEvent);
    CoTaskMemFree(pFormat);
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
