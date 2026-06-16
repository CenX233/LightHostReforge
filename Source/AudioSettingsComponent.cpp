//
//  AudioSettingsComponent.cpp
//  Light Host
//
//  Custom audio settings dialog with per-device-type state tracking.
//

#include "AudioSettingsComponent.hpp"
#include "IconMenu.hpp"

//==============================================================================
AudioSettingsComponent::AudioSettingsComponent (AudioDeviceManager& dm,
                                                AudioStream& stream,
                                                PluginChain& chain,
                                                const String& initError)
    : deviceManager      (dm),
      audioStream        (stream),
      pluginChain        (chain),
      initialisationError (initError)
{
    // Create the AudioDeviceSelectorComponent on the heap.
    // hideAdvancedOptionsWithButton = false so sample-rate / buffer-size
    // controls are visible by default (our custom controls sit below them).
    selector = std::make_unique<AudioDeviceSelectorComponent> (dm,
                        0, 256, 0, 256,
                        false,    // showMidiInputOptions
                        false,    // showMidiOutputSelector
                        true,     // showChannelsAsStereoPairs
                        false);   // hideAdvancedOptionsWithButton

    // Give the selector its real size NOW.  This triggers
    // AudioDeviceSelectorComponent::resized(), which calls
    // audioDeviceSettingsComp->setBounds(...).  JUCE's setBounds
    // sets the panel's dimensions *before* calling its resized(),
    // so the panel's children are laid out at the correct width.
    selector->setSize (512, 420);
    addAndMakeVisible (selector.get());

    // --- Custom controls --------------------------------------------------

    // Latency / error label — matches JUCE's right-column alignment
    latencyLabel.setColour (Label::textColourId, findColour (Label::textColourId));
    latencyLabel.setFont  (FontOptions (14.0f, Font::bold));
    addAndMakeVisible (latencyLabel);
    updateLatencyLabel();

    // Fade toggle — read initial state from AudioStream
    fadeToggle.setButtonText  ("Enable Fade In/Out");
    fadeToggle.setToggleState (audioStream.fadeEnabled.load(), dontSendNotification);
    fadeToggle.onClick        = [this]
    {
        audioStream.fadeEnabled = fadeToggle.getToggleState();
    };
    addAndMakeVisible (fadeToggle);

    // Listen for device changes to refresh the latency display
    deviceManager.addChangeListener (this);

    // Initialise per-device-type state tracking.
    // Save the current (pre-dialog) state for the active device type so it's
    // available later when the user switches types.
    lastDeviceType = deviceManager.getCurrentAudioDeviceType();
    if (auto state = deviceManager.createStateXml())
        perTypeState[lastDeviceType] = std::make_unique<XmlElement> (*state);

    // Set size LAST — triggers resized() which accesses all children.
    // 650 px high to accommodate the fully-expanded selector plus our
    // extra controls.
    setSize (520, 480);
}

AudioSettingsComponent::~AudioSettingsComponent()
{
    deviceManager.removeChangeListener (this);
}

//==============================================================================
void AudioSettingsComponent::resized()
{
    auto r = getLocalBounds().reduced (4);
    const int itemH = 24;
    const int space = 6;

    // Reserve space at the bottom for our extra controls
    const int ourHeight = itemH * 2 + space * 2;  // latency label + fade toggle + spacings
    auto ourArea = r.removeFromBottom (ourHeight);

    // Give the remaining area to the selector
    selector->setBounds (r);

    // --- Arrange extra controls, aligned with JUCE's right column ---------
    // JUCE's AudioDeviceSettingsPanel uses:
    //   x = proportionOfWidth(0.35f), width = proportionOfWidth(0.6f)
    // We match that so our controls are left-aligned with the combo boxes.
    const int rightColX = proportionOfWidth (0.35f);
    const int rightColW = proportionOfWidth (0.6f);

    ourArea.removeFromTop (space);
    fadeToggle.setBounds      (rightColX, ourArea.getY(), rightColW, itemH);
    ourArea.removeFromTop (itemH);
    ourArea.removeFromTop (space);
    latencyLabel.setBounds    (rightColX, ourArea.getY(), rightColW, itemH);
    ourArea.removeFromTop (itemH);
}

//==============================================================================
void AudioSettingsComponent::changeListenerCallback (ChangeBroadcaster* source)
{
    if (source != &deviceManager)
        return;

    auto currentType = deviceManager.getCurrentAudioDeviceType();

    if (currentType != lastDeviceType && !isRestoring)
    {
        // --- Device type changed ---
        // The OLD type's state was already saved in the perTypeState map by
        // a previous listener call (every non-type-change notification saves).
        // Now restore any previously-saved state for the NEW type.
        isRestoring = true;
        restorePerTypeState (currentType);
        lastDeviceType = currentType;
        isRestoring = false;
    }
    else if (!isRestoring)
    {
        // --- Same device type, some setting changed ---
        // Save the current device state to the in-memory per-type map so it's
        // available if the user switches to another type and back.
        if (auto state = deviceManager.createStateXml())
            perTypeState[currentType] = std::make_unique<XmlElement> (*state);
    }

    updateLatencyLabel();
}

void AudioSettingsComponent::restorePerTypeState (const String& type)
{
    // 1. Check in-memory state (captured earlier in this dialog session)
    auto it = perTypeState.find (type);
    if (it != perTypeState.end() && it->second != nullptr)
    {
        deviceManager.closeAudioDevice();
        deviceManager.initialise (256, 256, it->second.get(), false);
        // Re-save after restore so the map reflects the restored state
        if (auto state = deviceManager.createStateXml())
            perTypeState[type] = std::make_unique<XmlElement> (*state);
        return;
    }

    // 2. Check globally-persisted per-type state (from a previous session)
    auto key = "audioDeviceState_" + type;
    if (auto saved = getAppProperties().getUserSettings()->getXmlValue (key))
    {
        deviceManager.closeAudioDevice();
        deviceManager.initialise (256, 256, saved.get(), false);
        // Cache in memory for faster switching later
        if (auto state = deviceManager.createStateXml())
            perTypeState[type] = std::make_unique<XmlElement> (*state);
        return;
    }

    // 3. No saved state for this type — leave the device as-is (it was
    //    already switched by AudioDeviceManager with default settings).
}

//==============================================================================
void AudioSettingsComponent::updateLatencyLabel()
{
    String text;

    if (auto* device = deviceManager.getCurrentAudioDevice())
    {
        auto sampleRate = device->getCurrentSampleRate();
        if (sampleRate <= 0)
            sampleRate = 44100.0;

        int inputLatency  = device->getInputLatencyInSamples();
        int outputLatency = device->getOutputLatencyInSamples();
        int bufferSize    = device->getCurrentBufferSizeSamples();
        int pluginLatency = pluginChain.getTotalPluginLatencySamples();

        int totalSamples = inputLatency + outputLatency + bufferSize + pluginLatency;
        double totalMs   = totalSamples / sampleRate * 1000.0;

        text = TRANS ("Total latency: ")
               + String (totalMs, 1) + " ms ("
               + String (totalSamples) + " samples)";
    }
    else
    {
        // Device is not open — show the initialisation error if we have one
        text = "ERROR: ";
        if (initialisationError.isNotEmpty())
            text += initialisationError;
        else
            text += TRANS ("No audio device selected");
    }

    latencyLabel.setText (text, dontSendNotification);
}
