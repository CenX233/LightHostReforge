//
//  NoneAudioDevice.hpp
//  Light Host
//
//  A dummy AudioIODevice + AudioIODeviceType that provides no audio I/O.
//  Used when a preset's saved audio device type is not available on the
//  current system — the audio engine remains silent until the user
//  selects a real device type.
//

#ifndef NoneAudioDevice_hpp
#define NoneAudioDevice_hpp

#include <juce_audio_devices/juce_audio_devices.h>

using namespace juce;

//==============================================================================
/**
    A silent AudioIODevice that reports 0 input / 0 output channels.

    start() calls audioDeviceAboutToStart() on the callback, but with
    zero channels no audio data is exchanged — the effect chain runs
    but receives/produces silence.
*/
class DummyAudioIODevice  : public AudioIODevice
{
public:
    DummyAudioIODevice() : AudioIODevice ("None", "None") {}

    //==============================================================================
    // Channel configuration — 0 input, 0 output
    //==============================================================================
    StringArray getOutputChannelNames() override   { return {}; }
    StringArray getInputChannelNames() override    { return {}; }
    BigInteger  getActiveOutputChannels() const override { return {}; }
    BigInteger  getActiveInputChannels() const override  { return {}; }

    //==============================================================================
    // Sample rate / buffer size
    //==============================================================================
    Array<double> getAvailableSampleRates() override            { return { 44100.0, 48000.0, 96000.0 }; }
    Array<int>    getAvailableBufferSizes() override            { return { 64, 128, 256, 512, 1024 }; }
    int           getDefaultBufferSize() override               { return 512; }
    int           getCurrentBufferSizeSamples() override        { return bufferSize; }
    double        getCurrentSampleRate() override               { return sampleRate; }
    int           getCurrentBitDepth() override                 { return 32; }

    //==============================================================================
    // Latency — 0 for dummy device
    //==============================================================================
    int  getOutputLatencyInSamples() override  { return 0; }
    int  getInputLatencyInSamples() override   { return 0; }
    int  getXRunCount() const noexcept override { return 0; }

    //==============================================================================
    // Lifecycle
    //==============================================================================
    String open (const BigInteger& /*inputChannels*/,
                 const BigInteger& /*outputChannels*/,
                 double sr, int bs) override
    {
        sampleRate = sr > 0.0 ? sr : 44100.0;
        bufferSize = bs > 0   ? bs : 512;
        isOpen_ = true;
        return {};
    }

    void close() override
    {
        isOpen_    = false;
        isStarted_ = false;
    }

    void start (AudioIODeviceCallback* call) override
    {
        if (call != nullptr && isOpen_)
        {
            call->audioDeviceAboutToStart (this);
            isStarted_ = true;
        }
    }

    void stop() override
    {
        isStarted_ = false;
    }

    bool isOpen() override    { return isOpen_; }
    bool isPlaying() override { return isStarted_; }

    String getLastError() override { return {}; }

private:
    double sampleRate = 44100.0;
    int    bufferSize = 512;
    bool   isOpen_    = false;
    bool   isStarted_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DummyAudioIODevice)
};

//==============================================================================
/**
    AudioIODeviceType providing a single "None" device (no audio I/O).

    Registered in the device manager alongside standard device types so the
    system can fall back to silence when a preset's audio device type is
    unavailable.
*/
class NoneAudioIODeviceType  : public AudioIODeviceType
{
public:
    NoneAudioIODeviceType() : AudioIODeviceType ("None") {}
    ~NoneAudioIODeviceType() override = default;

    void scanForDevices() override   { hasScanned = true; }

    StringArray getDeviceNames (bool) const override
    {
        jassert (hasScanned);
        return { "None" };
    }

    int getDefaultDeviceIndex (bool) const override
    {
        jassert (hasScanned);
        return 0;
    }

    int getIndexOfDevice (AudioIODevice* d, bool) const override
    {
        jassert (hasScanned);
        return dynamic_cast<DummyAudioIODevice*> (d) != nullptr ? 0 : -1;
    }

    bool hasSeparateInputsAndOutputs() const override   { return false; }

    AudioIODevice* createDevice (const String& /*deviceName*/,
                                 const String& /*deviceTypeName*/) override
    {
        jassert (hasScanned);
        return new DummyAudioIODevice();
    }

private:
    bool hasScanned = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NoneAudioIODeviceType)
};

#endif // NoneAudioDevice_hpp
