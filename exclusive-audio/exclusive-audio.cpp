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

using Microsoft::WRL::ComPtr;

// --- Constants & Defaults ---
const std::wstring CAPTURE_FILENAME = L"exclusive_capture.wav";
const int REFTIMES_PER_SEC = 10000000;
const int REFTIMES_PER_MILLISEC = 10000;

// --- CLI Configuration State ---
struct AppConfig {
    std::string mode = "output"; // "input", "output", or "loopback"
    std::string file = "";       // Path to wav file to play
    int duration = 0;
    bool continuous = false;
    bool verbose = false;
};

// --- Helper Functions ---
void PrintHelp() {
    std::cout << "ExclusiveAudioDemo 0.1.0\n";
    std::cout << "Copyright 2026 Google Inc\n";
    std::cout << "USAGE:\n\n";
    std::cout << "  -m, --mode          (Default: output) Mode: 'input', 'output', or 'loopback'.\n";
    std::cout << "  -f, --file          (Default: '') Path to input wav file for output mode.\n";
    std::cout << "  -d, --duration      (Default: 0 sec) Duration to run in seconds.\n";
    std::cout << "  -c, --continuous    (Default: false) Run forever. Stop by pressing Ctrl+C.\n";
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
        else if (args[i] == "-c" || args[i] == "--continuous") {
            config.continuous = true;
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
        { 48000, 24 },
        { 48000, 16 },
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
        std::cerr << "Failed to initialize render client. Is the format supported in exclusive mode?\n";
        return;
    }

    ComPtr<IAudioRenderClient> pRenderClient;
    pAudioClient->GetService(IID_PPV_ARGS(&pRenderClient));

    HANDLE hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    pAudioClient->SetEventHandle(hEvent);

    // TODO: Load WAV file (config.file) into memory, ensuring it matches pFormat->nSamplesPerSec.

    pAudioClient->Start();
    if (config.verbose) std::cout << "[Info] Audio playback started.\n";

    // Playback Loop
    bool playing = true;
    while (playing) {
        WaitForSingleObject(hEvent, 2000); // Wait for the audio engine to ask for data

        // 1. Get buffer size: pAudioClient->GetBufferSize(...)
        // 2. Request buffer: pRenderClient->GetBuffer(...)
        // 3. Copy PCM data from your WAV file to the buffer
        // 4. Release buffer: pRenderClient->ReleaseBuffer(...)

        if (!config.continuous /* && Check if duration/file is done */) {
            playing = false;
        }
    }

    pAudioClient->Stop();
    CloseHandle(hEvent);
    CoTaskMemFree(pFormat);
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
