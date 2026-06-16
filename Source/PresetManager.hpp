//
//  PresetManager.hpp
//  Light Host
//
//  Handles preset file I/O (.lhp format) for the plugin chain.
//  Extracted from IconMenu to keep file-level concerns separate
//  from audio lifecycle and UI.
//

#ifndef PresetManager_hpp
#define PresetManager_hpp

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>
#include <functional>

class PluginChain;

//==============================================================================
/**
    Manages preset file save/load operations for the plugin effect chain.

    Owns no audio resources itself; receives an ApplicationProperties reference
    for window-geometry persistence and std::function callbacks for audio
    lifecycle (suspend/resume) that the caller (IconMenu) provides.

    The chain serialization format is handled by PluginChain::createPresetXml()
    and PluginChain::loadFromPresetXml().  This class layers on file I/O,
    window-geometry capture/restore, and the "current preset" tracking.
*/
class PresetManager
{
public:
    PresetManager (PluginChain& chain,
                   juce::ApplicationProperties& appProperties);

    //==============================================================================
    /** Save the current plugin chain to a .lhp file.
        Captures running processor state, serializes the chain via
        PluginChain::createPresetXml(), appends window geometry from
        app properties, and writes to disk.
    */
    void savePresetToFile (juce::File file);

    /** Load a plugin chain from a .lhp file.
        Parses the XML, calls suspendAudio, replaces the chain via
        PluginChain::loadFromPresetXml(), rebuilds the graph via
        PluginChain::loadAll(), restores window geometry, then calls
        resumeAudio.

        @param file              the .lhp file to load
        @param suspendAudio      callback to fade out and pause the audio thread
        @param resumeAudio       callback to restart the audio thread and fade in
    */
    void loadPresetFromFile (juce::File file,
                             std::function<void()> suspendAudio,
                             std::function<void()> resumeAudio);

    /** Return the default preset directory
        (<userAppData>/Light Host/Presets/), creating it if necessary.
    */
    static juce::File getDefaultPresetDirectory();

    //==============================================================================
    /// @name Current preset tracking
    //@{

    juce::File getCurrentPresetFile() const noexcept     { return currentPresetFile; }
    void       setCurrentPresetFile (juce::File f)       { currentPresetFile = f; }

    /** True if the preset has been modified since last save/load/new. */
    bool isDirty() const noexcept                        { return dirty; }

    /** Mark the preset as modified (dirty). */
    void markDirty() noexcept                            { dirty = true; }

    /** Mark the preset as clean (saved/loaded/new). */
    void clearDirty() noexcept                           { dirty = false; }

    //@}
    //==============================================================================
    /** Create a new (empty) preset.
        Clears the chain, rebuilds the graph in silence, and resets the
        current preset path in app properties.
    */
    void newPreset (std::function<void()> suspendAudio,
                    std::function<void()> resumeAudio);

private:
    PluginChain&               pluginChain;
    juce::ApplicationProperties& appProperties;
    juce::File                 currentPresetFile;
    bool                       dirty = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PresetManager)
};

#endif // PresetManager_hpp
