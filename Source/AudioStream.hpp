//
//  AudioStream.hpp
//  Light Host
//
//  Audio processor player with gain-ramp (fade) support.
//  Extracted from IconMenu as a reusable audio infrastructure class.
//

#ifndef AudioStream_hpp
#define AudioStream_hpp

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <atomic>

using namespace juce;

//==============================================================================
/**
    An AudioProcessorPlayer that applies a gain ramp (fade-in / fade-out)
    over the output buffers.

    Call fadeTo(target, rampMs) to start a linear ramp from the current gain
    to the target over the specified duration.  The ramp is sample-accurate
    and runs on the audio thread with no extra allocations.

    Use setGainImmediately(gain) to snap the gain without a ramp.
*/
class AudioStream  : public AudioProcessorPlayer
{
public:
    AudioStream()  : AudioProcessorPlayer (false) {}

    static constexpr int fadeRampMs   = 5;
    static constexpr int fadeExtraMs  = 5;

    /** When true, gain transitions use a linear ramp (fadeIn/fadeOut).
        When false, gain changes are instant (snap).  The ramp duration
        is always the fadeRmpMs / fadeExtraMs compile-time constants —
        unchecking simply skips the ramp, re-checking restores it. */
    std::atomic<bool> fadeEnabled{ true };

    /** Called when the audio device reports a runtime error
        (e.g. after sleep/wake or device disconnection). */
    std::function<void(const String&)> onDeviceError;

    /** Called when the audio device has been stopped. */
    std::function<void()> onDeviceStopped;

    //==============================================================================
    void audioDeviceIOCallbackWithContext (const float* const* inputChannelData,
                                           int numInputChannels,
                                           float* const* outputChannelData,
                                           int numOutputChannels,
                                           int numSamples,
                                           const AudioIODeviceCallbackContext& context) override
    {
        AudioProcessorPlayer::audioDeviceIOCallbackWithContext (inputChannelData,
                                                                 numInputChannels,
                                                                 outputChannelData,
                                                                 numOutputChannels,
                                                                 numSamples,
                                                                 context);
        int rem = remainingSamples.load();
        if (rem > 0)
        {
            const int total = totalRampSamples.load();
            const float target = targetGain.load();
            const float start = startGain.load();
            for (int s = 0; s < numSamples; ++s)
            {
                float gain;
                if (rem > 0)
                {
                    const float progress = 1.0f - (float) rem / (float) total;
                    gain = start + (target - start) * progress;
                    curGain = gain;
                    --rem;
                }
                else { gain = target; }
                for (int c = 0; c < numOutputChannels; ++c)
                    outputChannelData[c][s] *= gain;
            }
            remainingSamples.store (rem);
        }
        else
        {
            const float gain = targetGain.load();
            if (gain != 1.0f)
                for (int c = 0; c < numOutputChannels; ++c)
                    FloatVectorOperations::multiply (outputChannelData[c], gain, numSamples);
        }
    }

    //==============================================================================
    void audioDeviceAboutToStart (AudioIODevice* device) override
    {
        AudioProcessorPlayer::audioDeviceAboutToStart (device);
        sampleRate = device->getCurrentSampleRate();
        // Fade in to eliminate pop when switching devices (e.g., Loopback ↔ WASAPI)
        setGainImmediately (0.0f);
        if (fadeEnabled)
            fadeTo (1.0f, fadeRampMs);
        else
            setGainImmediately (1.0f);
    }

    //==============================================================================
    void audioDeviceError (const String& message) override
    {
        AudioProcessorPlayer::audioDeviceError (message);
        if (onDeviceError)
            onDeviceError (message);
    }

    //==============================================================================
    void audioDeviceStopped() override
    {
        AudioProcessorPlayer::audioDeviceStopped();
        if (onDeviceStopped)
            onDeviceStopped();
    }

    //==============================================================================
    /** Start a linear gain ramp from the current gain to @a target
        over @a rampMs milliseconds.
    */
    void fadeTo (float target, int rampMs)
    {
        startGain.store (curGain);
        const int ramp = jmax (1, (int) (sampleRate * rampMs / 1000));
        totalRampSamples.store (ramp);

        if (target <= curGain)
        {
            targetGain.store (target);
            remainingSamples.store (ramp);
        }
        else
        {
            remainingSamples.store (ramp);
            targetGain.store (target);
        }
    }

    //==============================================================================
    void suspend (AudioDeviceManager& dm)
    {
        dm.removeAudioCallback (this);
        setProcessor (nullptr);
    }

    void resume (AudioDeviceManager& dm, AudioProcessorGraph& graph)
    {
        setProcessor (&graph);
        dm.addAudioCallback (this);
        setGainImmediately (0.0f);
        if (fadeEnabled)
            fadeTo (1.0f, fadeRampMs);
        else
            setGainImmediately (1.0f);
    }

    //==============================================================================
    /** Set the gain immediately (no ramp). */
    void setGainImmediately (float gain)
    {
        curGain = gain;
        startGain.store (gain);
        targetGain.store (gain);
        remainingSamples.store (0);
        totalRampSamples.store (0);
    }

private:
    //==============================================================================
    std::atomic<double> sampleRate{ 44100.0 };
    std::atomic<float> curGain{ 1.0f };
    std::atomic<float> startGain{ 1.0f };
    std::atomic<float> targetGain{ 1.0f };
    std::atomic<int> remainingSamples{ 0 };
    std::atomic<int> totalRampSamples{ 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioStream)
};

#endif // AudioStream_hpp
