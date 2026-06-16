//
//  AudioSettingsComponent.hpp
//  Light Host
//
//  Custom audio settings dialog wrapping AudioDeviceSelectorComponent
//  with extra controls for fade toggle and a live latency / error label.
//

#ifndef AudioSettingsComponent_hpp
#define AudioSettingsComponent_hpp

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include "AudioStream.hpp"
#include "PluginChain.hpp"

#include <map>

using namespace juce;

//==============================================================================
/**
    A self-contained audio settings component.

    Wraps the standard AudioDeviceSelectorComponent and adds:
      - A live total-latency label (or error message if the device failed)
      - "Enable Fade In/Out" toggle (controls AudioStream::fadeEnabled)

    The AudioDeviceSelectorComponent is heap-allocated so we can set its size
    *after* construction, ensuring the internal AudioDeviceSettingsPanel does
    its initial layout with a non-zero width.
*/
class AudioSettingsComponent  : public Component,
                                private ChangeListener
{
public:
    AudioSettingsComponent (AudioDeviceManager& dm,
                            AudioStream& stream,
                            PluginChain& chain,
                            const String& initError);
    ~AudioSettingsComponent() override;

    bool isFadeEnabled() const noexcept         { return fadeToggle.getToggleState(); }

    /** Returns the per-device-type state map accumulated during this dialog session.
        IconMenu uses this at dialog close to persist all configured types to global settings. */
    const std::map<String, std::unique_ptr<XmlElement>>& getPerTypeState() const { return perTypeState; }

    void resized() override;

private:
    void changeListenerCallback (ChangeBroadcaster* source) override;
    void updateLatencyLabel();
    void restorePerTypeState (const String& type);

    //--------------------------------------------------------------------------
    AudioDeviceManager&         deviceManager;
    AudioStream&                audioStream;
    PluginChain&                pluginChain;

    std::unique_ptr<AudioDeviceSelectorComponent> selector;
    Label                       latencyLabel;
    ToggleButton                fadeToggle;

    String                      initialisationError;

    // Per-device-type state tracking (for type switching within the dialog)
    std::map<String, std::unique_ptr<XmlElement>> perTypeState;
    String lastDeviceType;
    bool   isRestoring = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioSettingsComponent)
};

#endif // AudioSettingsComponent_hpp
