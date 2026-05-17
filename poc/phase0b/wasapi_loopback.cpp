// Phase 0B PoC: WASAPI Process Loopback capture
// Captures all system audio (excluding self) via ActivateAudioInterfaceAsync
// using the "VAD\Process_Loopback" device path.
// Prints RMS level continuously. Verify that changing master volume has no effect.
//
// Requirements: Windows 10 20H2 (build 19042)+
// Build: see build.bat

#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define WINVER 0x0A00
#define _WIN32_WINNT 0x0A00

#include <windows.h>
#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <mmdeviceapi.h>
#include <initguid.h>
#include <roapi.h>
#include <ksmedia.h>
#include <stdio.h>
#include <math.h>
#include <synchapi.h>

#pragma comment(lib, "mmdevapi.lib")



// ---- Completion handler ----
class ActivationHandler : public IActivateAudioInterfaceCompletionHandler, public IAgileObject {
public:
    HANDLE          m_event;
    IAudioClient*   m_client;
    HRESULT         m_hr;
    LONG            m_refCount;

    ActivationHandler() : m_event(NULL), m_client(NULL), m_hr(E_PENDING), m_refCount(1) {
        m_event = CreateEvent(NULL, TRUE, FALSE, NULL);
    }
    ~ActivationHandler() {
        if (m_event) CloseHandle(m_event);
        if (m_client) m_client->Release();
    }

    // IUnknown
    ULONG STDMETHODCALLTYPE AddRef()  override { return InterlockedIncrement(&m_refCount); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG ref = InterlockedDecrement(&m_refCount);
        if (ref == 0) delete this;
        return ref;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == __uuidof(IActivateAudioInterfaceCompletionHandler)) {
            *ppv = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
            AddRef();
            return S_OK;
        }
        if (riid == __uuidof(IAgileObject)) {
            *ppv = static_cast<IAgileObject*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = NULL;
        return E_NOINTERFACE;
    }

    // IActivateAudioInterfaceCompletionHandler
    HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation* op) override {
        HRESULT hrActivate = E_FAIL;
        IUnknown* pAudioInterface = NULL;
        op->GetActivateResult(&hrActivate, &pAudioInterface);
        m_hr = hrActivate;
        if (SUCCEEDED(hrActivate) && pAudioInterface) {
            pAudioInterface->QueryInterface(__uuidof(IAudioClient), (void**)&m_client);
            pAudioInterface->Release();
        }
        SetEvent(m_event);
        return S_OK;
    }
};

// ---- RMS helper ----
static double computeRms(const float* samples, UINT32 count) {
    double sum = 0.0;
    for (UINT32 i = 0; i < count; i++) sum += (double)samples[i] * samples[i];
    return (count > 0) ? sqrt(sum / count) : 0.0;
}

static double computeRmsI16(const short* samples, UINT32 count) {
    double sum = 0.0;
    for (UINT32 i = 0; i < count; i++) {
        double s = samples[i] / 32768.0;
        sum += s * s;
    }
    return (count > 0) ? sqrt(sum / count) : 0.0;
}

// ---- Meter bar ----
static void printMeter(double rms) {
    int bars = (int)(rms * 40.0);
    if (bars > 40) bars = 40;
    printf("\rRMS: %6.4f  [", rms);
    for (int i = 0; i < bars; i++) putchar('#');
    for (int i = bars; i < 40; i++) putchar(' ');
    printf("]");
    fflush(stdout);
}

int wmain(int argc, wchar_t** argv) {
    printf("Phase 0B: WASAPI Process Loopback PoC\n");
    printf("Windows build: %lu\n", GetVersion() & 0xFFFF);
    printf("Captures ALL system audio (excluding self). Master volume should have no effect.\n");
    printf("Press Ctrl+C to stop.\n\n");

    HRESULT hr = RoInitialize(RO_INIT_MULTITHREADED);
    printf("RoInitialize: 0x%08lX (%s)\n", hr, SUCCEEDED(hr) ? "OK" : "FAILED");
    if (FAILED(hr)) return 1;

    // Build activation params for Process Loopback
    AUDIOCLIENT_ACTIVATION_PARAMS params = {};
    params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    params.ProcessLoopbackParams.TargetProcessId = GetCurrentProcessId();
    params.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE;

    PROPVARIANT pv = {};
    PropVariantInit(&pv);
    pv.vt           = VT_BLOB;
    pv.blob.cbSize  = sizeof(params);
    pv.blob.pBlobData = (BYTE*)&params;

    ActivationHandler* handler = new ActivationHandler();

    IActivateAudioInterfaceAsyncOperation* asyncOp = NULL;
    hr = ActivateAudioInterfaceAsync(
        VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
        __uuidof(IAudioClient),
        &pv,
        handler,
        &asyncOp
    );
    if (FAILED(hr)) {
        printf("ActivateAudioInterfaceAsync failed: 0x%08lX\n", hr);
        printf("Note: Requires Windows 10 20H2 (build 19042) or later.\n");
        handler->Release();
        return 1;
    }

    printf("Waiting for audio client activation...\n");
    WaitForSingleObject(handler->m_event, 5000);

    if (FAILED(handler->m_hr) || !handler->m_client) {
        printf("Activation failed: 0x%08lX\n", handler->m_hr);
        asyncOp->Release();
        handler->Release();
        return 1;
    }

    IAudioClient* client = handler->m_client;
    handler->m_client = NULL; // take ownership
    asyncOp->Release();
    handler->Release();

    printf("Audio client activated.\n");

    // Get mix format (shared mode uses device's native format)
    // Process loopback does not support GetMixFormat().
    // Get the format from the default output device instead.
    WAVEFORMATEX* pFmt = NULL;
    {
        IMMDeviceEnumerator* pEnum = NULL;
        IMMDevice* pDevice = NULL;
        IAudioClient* pTmpClient = NULL;
        CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
                         __uuidof(IMMDeviceEnumerator), (void**)&pEnum);
        pEnum->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice);
        pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&pTmpClient);
        pTmpClient->GetMixFormat(&pFmt);
        pTmpClient->Release();
        pDevice->Release();
        pEnum->Release();
    }
    if (!pFmt) { printf("Failed to get mix format from default device\n"); return 1; }

    printf("Format: %u ch, %lu Hz, %u bits (%s)\n",
        pFmt->nChannels,
        pFmt->nSamplesPerSec,
        pFmt->wBitsPerSample,
        (pFmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE) ? "EXTENSIBLE" :
        (pFmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) ? "FLOAT32" : "PCM");

    // Initialize: shared loopback, 100ms buffer
    REFERENCE_TIME bufDur = 1000000; // 100ms in 100ns units
    hr = client->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK,
        bufDur, 0,
        pFmt, NULL
    );
    if (FAILED(hr)) {
        printf("Initialize failed: 0x%08lX\n", hr);
        CoTaskMemFree(pFmt);
        return 1;
    }

    IAudioCaptureClient* capture = NULL;
    hr = client->GetService(__uuidof(IAudioCaptureClient), (void**)&capture);
    if (FAILED(hr)) { printf("GetService(IAudioCaptureClient) failed: 0x%08lX\n", hr); return 1; }

    hr = client->Start();
    if (FAILED(hr)) { printf("Start failed: 0x%08lX\n", hr); return 1; }

    printf("Capturing... change master volume and verify the meter does NOT change.\n\n");

    bool isFloat = (pFmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
        (pFmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
         ((WAVEFORMATEXTENSIBLE*)pFmt)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT));

    CoTaskMemFree(pFmt);

    // Capture loop
    BYTE* pData;
    UINT32 numFrames;
    DWORD flags;
    UINT64 devPos, qpcPos;

    while (true) {
        Sleep(20); // poll every 20ms

        hr = capture->GetBuffer(&pData, &numFrames, &flags, &devPos, &qpcPos);
        if (hr == AUDCLNT_S_BUFFER_EMPTY) continue;
        if (FAILED(hr)) { printf("\nGetBuffer failed: 0x%08lX\n", hr); break; }

        if (numFrames > 0 && !(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
            double rms;
            if (isFloat) {
                rms = computeRms((const float*)pData, numFrames * 2); // stereo
            } else {
                rms = computeRmsI16((const short*)pData, numFrames * 2);
            }
            printMeter(rms);
        }

        capture->ReleaseBuffer(numFrames);
    }

    client->Stop();
    capture->Release();
    client->Release();
    RoUninitialize();
    return 0;
}
