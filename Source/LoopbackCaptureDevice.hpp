//
//  LoopbackCaptureDevice.hpp
//  Light Host
//
//  A custom AudioIODevice + AudioIODeviceType that captures Windows desktop
//  audio via WASAPI loopback (AUDCLNT_STREAMFLAGS_LOOPBACK).
//
//  Provides 2 stereo input channels and 0 output channels — the audio
//  flows through the effect chain and is then discarded (no audible output).
//
//  Only available on Windows.
//

#ifndef LoopbackCaptureDevice_hpp
#define LoopbackCaptureDevice_hpp

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_basics/juce_audio_basics.h>

#if JUCE_WINDOWS
#include <Windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#endif

using namespace juce;

//==============================================================================
#if JUCE_WINDOWS

/**
    Simple COM RAII helper for a single interface pointer.
    Releases on destruction.
*/
template <typename Iface>
class ScopedComPtr
{
public:
    ScopedComPtr() = default;
    ~ScopedComPtr() { if (ptr != nullptr) ptr->Release(); }

    ScopedComPtr (ScopedComPtr&& other) noexcept : ptr (other.ptr) { other.ptr = nullptr; }
    ScopedComPtr& operator= (ScopedComPtr&& other) noexcept
    {
        if (this != &other) { release(); ptr = other.ptr; other.ptr = nullptr; }
        return *this;
    }

    ScopedComPtr (const ScopedComPtr&) = delete;
    ScopedComPtr& operator= (const ScopedComPtr&) = delete;

    Iface* get() const noexcept { return ptr; }
    Iface* operator->() const noexcept { return ptr; }
    explicit operator bool() const noexcept { return ptr != nullptr; }

    Iface** resetAndGetPointerAddress()
    {
        release();
        return &ptr;
    }

    Iface* release()
    {
        if (ptr != nullptr)
            ptr->Release();
        ptr = nullptr;
        return nullptr;
    }

    void reset (Iface* p = nullptr)
    {
        if (p != ptr)
        {
            if (ptr != nullptr)
                ptr->Release();
            ptr = p;
        }
    }

private:
    Iface* ptr = nullptr;
};

//==============================================================================
/**
    Custom AudioIODevice that captures the Windows desktop audio stream via
    WASAPI loopback and feeds it as stereo input to the effect chain.

    Provides 2 input channels, 0 output channels — processed audio is discarded.
*/
class LoopbackCaptureDevice  : public AudioIODevice,
                               private Thread
{
public:
    LoopbackCaptureDevice()
        : AudioIODevice ("System Loopback", "Loopback"),
          Thread ("WASAPI Loopback")
    {
        jassert (mIsOpen == false);

        comInitialised = SUCCEEDED (CoInitializeEx (nullptr, COINIT_MULTITHREADED));

        // Create device enumerator
        ScopedComPtr<IMMDeviceEnumerator> enumPtr;
        if (FAILED (CoCreateInstance (__uuidof (MMDeviceEnumerator), nullptr,
                                       CLSCTX_INPROC_SERVER,
                                       IID_PPV_ARGS (enumPtr.resetAndGetPointerAddress()))))
        {
            lastError = "Failed to create MMDeviceEnumerator";
            return;
        }

        // Get default render endpoint
        ScopedComPtr<IMMDevice> renderDevice;
        if (FAILED (enumPtr->GetDefaultAudioEndpoint (eRender, eConsole,
                                                      renderDevice.resetAndGetPointerAddress())))
        {
            lastError = "Failed to get default audio render endpoint";
            return;
        }

        // Activate IAudioClient for probing
        ScopedComPtr<IAudioClient> tempClient;
        if (FAILED (renderDevice->Activate (__uuidof (IAudioClient), CLSCTX_INPROC_SERVER,
                                            nullptr, (void**) tempClient.resetAndGetPointerAddress())))
        {
            lastError = "Failed to activate IAudioClient";
            return;
        }

        // Get mix format to determine sample rate & channel count
        WAVEFORMATEX* mixFmt = nullptr;
        if (FAILED (tempClient->GetMixFormat (&mixFmt)))
        {
            lastError = "Failed to get mix format";
            return;
        }

        inputChannelsCount = jmin (2, (int) mixFmt->nChannels);
        baseSampleRate = mixFmt->nSamplesPerSec;

        sourceIsFloat = false;
        sourceBytesPerSample = 2;

        if (mixFmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
        {
            auto* ext = reinterpret_cast<WAVEFORMATEXTENSIBLE*> (mixFmt);
            if (ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)
            {
                sourceIsFloat = true;
                sourceBytesPerSample = mixFmt->wBitsPerSample / 8;
            }
            else if (ext->SubFormat == KSDATAFORMAT_SUBTYPE_PCM)
            {
                sourceBytesPerSample = mixFmt->wBitsPerSample / 8;
            }
        }
        else if (mixFmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
        {
            sourceIsFloat = true;
            sourceBytesPerSample = mixFmt->wBitsPerSample / 8;
        }
        else if (mixFmt->wFormatTag == WAVE_FORMAT_PCM)
        {
            sourceBytesPerSample = mixFmt->wBitsPerSample / 8;
        }

        if (sourceBytesPerSample < 2)  sourceBytesPerSample = 2;
        if (sourceBytesPerSample > 4)  sourceBytesPerSample = 4;

        sourceBytesPerFrame = sourceBytesPerSample * inputChannelsCount;

        // Determine buffer sizes from device period
        REFERENCE_TIME defaultPeriod {}, minPeriod {};
        if (SUCCEEDED (tempClient->GetDevicePeriod (&defaultPeriod, &minPeriod)))
        {
            defaultBufferSize = refTimeToSamples (defaultPeriod, baseSampleRate);
            minBufferSize = refTimeToSamples (minPeriod, baseSampleRate);
        }

        bufferSizes.addUsingDefaultSort (defaultBufferSize);
        if (minBufferSize != defaultBufferSize)
            bufferSizes.addUsingDefaultSort (minBufferSize);
        for (auto s : { 64, 128, 256, 512, 1024, 2048 })
            if (! bufferSizes.contains (s))
                bufferSizes.addUsingDefaultSort (s);

        // Persist device ref (tempClient was only for probing)
        mEnum   = std::move (enumPtr);
        mDevice = std::move (renderDevice);

        CoTaskMemFree (mixFmt);
        mClientEvent = CreateEventW (nullptr, FALSE, FALSE, nullptr);
    }

    ~LoopbackCaptureDevice() override
    {
        close();

        if (mClientEvent != nullptr)
            CloseHandle (mClientEvent);

        // Release constructor-created COM pointers BEFORE
        // uninitializing COM, so that Release() calls
        // on mDevice / mEnum run while COM is still active.
        mDevice.reset();
        mEnum.reset();

        if (comInitialised)
            CoUninitialize();
    }

    //==============================================================================
    // AudioIODevice interface
    //==============================================================================

    StringArray getOutputChannelNames() override   { return {}; }

    StringArray getInputChannelNames() override
    {
        StringArray names;
        for (int i = 0; i < inputChannelsCount; ++i)
            names.add ("Loopback Input " + String (i + 1));
        return names;
    }

    Array<double> getAvailableSampleRates() override   { return { (double) baseSampleRate }; }
    Array<int>     getAvailableBufferSizes() override  { return bufferSizes; }
    int            getDefaultBufferSize() override      { return defaultBufferSize; }

    String open (const BigInteger& inputChannels,
                 const BigInteger& /*outputChannels*/,
                 double sampleRate,
                 int bufferSizeSamples) override
    {
        close();
        lastError.clear();

        int numInputs = jmin (inputChannelsCount,
                              (int) inputChannels.countNumberOfSetBits());
        if (numInputs == 0)  numInputs = inputChannelsCount;

        mNumInputChannels    = numInputs;
        mBufferSizeSamples   = bufferSizeSamples > 0 ? bufferSizeSamples : defaultBufferSize;
        mCurrentSampleRate   = sampleRate > 0 ? sampleRate : (double) baseSampleRate;

        // Activate fresh IAudioClient
        if (FAILED (mDevice->Activate (__uuidof (IAudioClient), CLSCTX_INPROC_SERVER,
                                       nullptr, (void**) mClient.resetAndGetPointerAddress())))
        {
            lastError = "Failed to activate IAudioClient";
            return lastError;
        }

        // Get mix format
        WAVEFORMATEX* fmt = nullptr;
        if (FAILED (mClient->GetMixFormat (&fmt)))
        {
            lastError = "Failed to get mix format";
            return lastError;
        }

        REFERENCE_TIME bufDur = samplesToRefTime (mBufferSizeSamples, mCurrentSampleRate);

        // Initialize with LOOPBACK (0x00020000) + EVENTCALLBACK (0x00040000)
        // EVENTCALLBACK is required for SetEventHandle to work.
        HRESULT hr = mClient->Initialize (AUDCLNT_SHAREMODE_SHARED,
                                          0x00020000 | 0x00040000,
                                          bufDur, 0, fmt, nullptr);
        CoTaskMemFree (fmt);

        if (FAILED (hr))
        {
            close();
            lastError = "Failed to init WASAPI loopback (hr="
                        + String::toHexString ((int) hr) + ")";
            return lastError;
        }

        // Get capture client
        if (FAILED (mClient->GetService (__uuidof (IAudioCaptureClient),
                                         (void**) mCapture.resetAndGetPointerAddress())))
        {
            close();
            lastError = "Failed to get IAudioCaptureClient";
            return lastError;
        }

        UINT32 actualSize = 0;
        if (SUCCEEDED (mClient->GetBufferSize (&actualSize)))
            mBufferSizeSamples = (int) actualSize;

        if (FAILED (mClient->SetEventHandle (mClientEvent)))
        {
            close();
            lastError = "Failed to set event handle";
            return lastError;
        }

        createConverter();

        REFERENCE_TIME latency;
        if (SUCCEEDED (mClient->GetStreamLatency (&latency)))
            mLatencySamples = refTimeToSamples (latency, mCurrentSampleRate);

        // Start the capture thread (required for the WASAPI event-driven loop)
        startThread (Priority::high);
        Thread::sleep (5);

        mIsOpen = true;
        return {};
    }

    void close() override
    {
        stop();

        if (mClient)
            mClient->Stop();

        // N.B. sleep 5ms to prevent double-deletion of IAudioSessionEvents
        // on older versions of Windows (matches JUCE's WASAPI pattern).
        Thread::sleep (5);

        signalThreadShouldExit();
        if (mClientEvent)
            SetEvent (mClientEvent);
        stopThread (3000);

        // Clear any stale signal on the event handle before close()
        // returns, matching JUCE's ResetEvent(clientEvent) pattern.
        if (mClientEvent)
            ResetEvent (mClientEvent);

        // Release WASAPI COM references created by open().
        // mDevice / mEnum are created in the constructor and are
        // released in the destructor, NOT here — open() calls close()
        // first and then uses mDevice->Activate().
        mCapture.reset();
        mClient.reset();
        mConverter = nullptr;
        mIsOpen = false;
    }

    bool isOpen() override       { return mIsOpen && isThreadRunning(); }
    bool isPlaying() override    { return mIsStarted && mIsOpen && isThreadRunning(); }

    void start (AudioIODeviceCallback* call) override
    {
        if (! mIsOpen || call == nullptr || mIsStarted)
            return;

        if (! isThreadRunning())
        {
            mIsOpen = false;
            return;
        }

        call->audioDeviceAboutToStart (this);

        {
            const ScopedLock sl (mLock);
            mCallback = call;
            mIsStarted = true;
        }

        if (mClient)
            mClient->Start();
    }

    void stop() override
    {
        if (mIsStarted)
        {
            auto* cb = mCallback;
            {
                const ScopedLock sl (mLock);
                mIsStarted = false;
            }
            if (cb != nullptr)
                cb->audioDeviceStopped();
        }

        if (mClient)
            mClient->Stop();
    }

    String      getLastError() override                { return lastError; }
    int         getCurrentBufferSizeSamples() override  { return mBufferSizeSamples; }
    double      getCurrentSampleRate() override          { return mCurrentSampleRate; }
    int         getCurrentBitDepth() override            { return 32; }
    int         getOutputLatencyInSamples() override     { return 0; }
    int         getInputLatencyInSamples() override      { return mLatencySamples; }

    BigInteger getActiveOutputChannels() const override  { return {}; }

    BigInteger getActiveInputChannels() const override
    {
        BigInteger chans;
        for (int i = 0; i < mNumInputChannels; ++i)
            chans.setBit (i);
        return chans;
    }

private:
    //==============================================================================
    // Member variables (all use ScopedComPtr for RAII)
    //==============================================================================
    ScopedComPtr<IMMDeviceEnumerator>  mEnum;
    ScopedComPtr<IMMDevice>           mDevice;
    ScopedComPtr<IAudioClient>        mClient;
    ScopedComPtr<IAudioCaptureClient> mCapture;
    HANDLE         mClientEvent = nullptr;
    bool           comInitialised = false;

    bool    sourceIsFloat        = true;
    int     sourceBytesPerSample = 4;
    int     sourceBytesPerFrame  = 8;
    int     inputChannelsCount   = 2;
    double  baseSampleRate       = 48000.0;
    int     defaultBufferSize    = 512;
    int     minBufferSize        = 256;
    Array<int> bufferSizes;

    bool    mIsOpen     = false;
    bool    mIsStarted  = false;
    int     mNumInputChannels  = 2;
    int     mBufferSizeSamples = 512;
    double  mCurrentSampleRate = 48000.0;
    int     mLatencySamples    = 0;

    std::unique_ptr<AudioData::Converter> mConverter;
    AudioBuffer<float>    mOutputBuffer;
    AudioIODeviceCallback* mCallback = nullptr;
    CriticalSection        mLock;
    String                 lastError;
    std::atomic<bool>      mShouldShutdown { false };

    //==============================================================================
    // Thread: WASAPI capture loop
    //==============================================================================
    void run() override
    {
        auto threadCom = SUCCEEDED (CoInitializeEx (nullptr, COINIT_MULTITHREADED));

        while (! threadShouldExit())
        {
            if (mShouldShutdown)
                break;

            if (! mCapture)
            {
                Thread::sleep (10);
                continue;
            }

            DWORD waitResult = WaitForSingleObject (mClientEvent, 1000);

            if (waitResult == WAIT_TIMEOUT)
                continue;

            if (waitResult != WAIT_OBJECT_0)
            {
                mShouldShutdown = true;
                break;
            }

            processCapturePackets();
        }

        if (threadCom)
            CoUninitialize();
    }

    void processCapturePackets()
    {
        UINT32 nextSize = 0;

        while (mCapture->GetNextPacketSize (&nextSize) == S_OK && nextSize > 0)
        {
            BYTE*  data   = nullptr;
            UINT32 frames = 0;
            DWORD  flags  = 0;

            HRESULT hr = mCapture->GetBuffer (&data, &frames, &flags, nullptr, nullptr);
            if (FAILED (hr))
                break;

            if (frames > 0 && ! (flags & AUDCLNT_BUFFERFLAGS_SILENT))
                deliverAudio (data, (int) frames);

            mCapture->ReleaseBuffer (frames);
        }
    }

    void deliverAudio (const BYTE* data, int numFrames)
    {
        mOutputBuffer.setSize (jmax (1, mNumInputChannels), numFrames + 16);
        mOutputBuffer.clear();

        auto* const* outChans = mOutputBuffer.getArrayOfWritePointers();

        if (sourceIsFloat && sourceBytesPerSample == 4)
        {
            // Float32 interleaved → manual de-interleave (most common WASAPI path)
            const float* src = reinterpret_cast<const float*> (data);
            const int srcStride = inputChannelsCount;
            for (int ch = 0; ch < mNumInputChannels; ++ch)
            {
                float* dst = outChans[ch];
                for (int i = 0; i < numFrames; ++i)
                    dst[i] = src[i * srcStride + ch];
            }
        }
        else if (mConverter)
        {
            for (int ch = 0; ch < mNumInputChannels; ++ch)
                mConverter->convertSamples (outChans[ch], 0, data, ch, numFrames);
        }
        else
        {
            for (int ch = 0; ch < mNumInputChannels; ++ch)
                zeromem (outChans[ch], (size_t) numFrames * sizeof (float));
        }

        {
            const ScopedTryLock sl (mLock);
            if (sl.isLocked() && mIsStarted && mCallback)
            {
                auto readPtrs = mOutputBuffer.getArrayOfReadPointers();

                mCallback->audioDeviceIOCallbackWithContext (
                    readPtrs,
                    mNumInputChannels,
                    nullptr,
                    0,
                    numFrames,
                    {});
            }
        }
    }

    //==============================================================================
    // Format converter
    //==============================================================================
    void createConverter()
    {
        // Float32 interleaved → non-interleaved is handled by a manual loop
        // in deliverAudio().  Only non-float formats need a ConverterInstance.
        mConverter = nullptr;

        if (sourceIsFloat && sourceBytesPerSample == 4)
            return;

        using FltP   = AudioData::Pointer<AudioData::Float32, AudioData::NativeEndian,
                                          AudioData::NonInterleaved, AudioData::NonConst>;
        using Int16S = AudioData::Pointer<AudioData::Int16,   AudioData::LittleEndian,
                                          AudioData::Interleaved, AudioData::Const>;
        using Int24S = AudioData::Pointer<AudioData::Int24,   AudioData::LittleEndian,
                                          AudioData::Interleaved, AudioData::Const>;
        using Int32S = AudioData::Pointer<AudioData::Int32,   AudioData::LittleEndian,
                                          AudioData::Interleaved, AudioData::Const>;

        if (sourceIsFloat)
        {
            // Non-32-bit float (unusual, but handle it)
            if (sourceBytesPerSample == 2)
                mConverter = std::make_unique<AudioData::ConverterInstance<Int16S, FltP>> (inputChannelsCount, 1);
        }
        else
        {
            if (sourceBytesPerSample == 4)
                mConverter = std::make_unique<AudioData::ConverterInstance<Int32S, FltP>> (inputChannelsCount, 1);
            else if (sourceBytesPerSample == 3)
                mConverter = std::make_unique<AudioData::ConverterInstance<Int24S, FltP>> (inputChannelsCount, 1);
            else
                mConverter = std::make_unique<AudioData::ConverterInstance<Int16S, FltP>> (inputChannelsCount, 1);
        }
    }

    //==============================================================================
    static int refTimeToSamples (REFERENCE_TIME t, double sr) noexcept
    {
        return roundToInt (sr * ((double) t) * 0.0000001);
    }

    static REFERENCE_TIME samplesToRefTime (int n, double sr) noexcept
    {
        return (REFERENCE_TIME) ((n * 10000.0 * 1000.0 / sr) + 0.5);
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LoopbackCaptureDevice)
};

//==============================================================================
/** AudioIODeviceType providing a single "System Loopback" device. */
class LoopbackAudioIODeviceType  : public AudioIODeviceType
{
public:
    LoopbackAudioIODeviceType() : AudioIODeviceType ("Loopback") {}
    ~LoopbackAudioIODeviceType() override = default;

    void scanForDevices() override   { hasScanned = true; }

    StringArray getDeviceNames (bool) const override
    {
        jassert (hasScanned);
        return { "System Loopback" };
    }

    int getDefaultDeviceIndex (bool) const override
    {
        jassert (hasScanned);
        return 0;
    }

    int getIndexOfDevice (AudioIODevice* d, bool) const override
    {
        jassert (hasScanned);
        return dynamic_cast<LoopbackCaptureDevice*> (d) != nullptr ? 0 : -1;
    }

    bool hasSeparateInputsAndOutputs() const override   { return false; }

    AudioIODevice* createDevice (const String&, const String&) override
    {
        jassert (hasScanned);
        auto dev = std::make_unique<LoopbackCaptureDevice>();
        if (dev->getLastError().isNotEmpty())
        {
            Logger::writeToLog ("Loopback: " + dev->getLastError());
            return nullptr;
        }
        return dev.release();
    }

private:
    bool hasScanned = false;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LoopbackAudioIODeviceType)
};

#else // !JUCE_WINDOWS

class LoopbackAudioIODeviceType  : public AudioIODeviceType
{
public:
    LoopbackAudioIODeviceType() : AudioIODeviceType ("Loopback") {}
    void scanForDevices() override {}
    StringArray getDeviceNames (bool) const override { return {}; }
    int getDefaultDeviceIndex (bool) const override { return -1; }
    int getIndexOfDevice (AudioIODevice*, bool) const override { return -1; }
    bool hasSeparateInputsAndOutputs() const override { return false; }
    AudioIODevice* createDevice (const String&, const String&) override { return nullptr; }
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LoopbackAudioIODeviceType)
};

#endif // JUCE_WINDOWS

#endif // LoopbackCaptureDevice_hpp
