//
//  IconMenu.hpp
//  Light Host
//
//  Created by Rolando Islas on 12/26/15.
//
//

#ifndef IconMenu_hpp
#define IconMenu_hpp

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_extra/juce_gui_extra.h>
#include "PresetManager.hpp"
#include "AudioStream.hpp"
#include "LoopbackCaptureDevice.hpp"

using namespace juce;

class PluginChain;

// Emoji UTF-8 byte sequences for plugin status indicators
constexpr const char* bypassedPluginEmoji    = "\xe2\x9a\xaa"; // ⚪ U+26AA
constexpr const char* nonBypassedPluginEmoji = "\xf0\x9f\x9f\xa2"; // 🟢 U+1F7E2
constexpr const char* failedPluginEmoji      = "\xf0\x9f\x94\xb4"; // 🔴 U+1F534

ApplicationProperties& getAppProperties();

class IconMenu : public SystemTrayIconComponent, private Timer, public ChangeListener
{
public:
    IconMenu();
    ~IconMenu();
    void mouseDown(const MouseEvent&);
    static void menuInvocationCallback(int id, IconMenu*);
    void changeListenerCallback(ChangeBroadcaster* changed);

    PluginChain& getPluginChain() const { return *pluginChain; }
    PresetManager* getPresetManager() const { return presetManager.get(); }

    void saveCurrentPreset();
    void markPresetDirty();

    void togglePluginBypass(int timeSortedIndex);
    void movePluginUp(int timeSortedIndex);
    void movePluginDown(int timeSortedIndex);
    bool isBypassed(int timeSortedIndex);

    const int INDEX_EDIT, INDEX_BYPASS, INDEX_DELETE, INDEX_MOVE_UP, INDEX_MOVE_DOWN;
    const int INDEX_PRESET_SAVE, INDEX_PRESET_SAVE_AS, INDEX_PRESET_LOAD_SELECT, INDEX_PRESET_NEW, INDEX_PRESET_LOAD_FILE;

    /** @internal */
    static constexpr int maxDeviceRecoveryRetries = 5;

private:
    void timerCallback();
    void reloadPlugins();
    void showAudioSettings();
    void setIcon();

    AudioDeviceManager deviceManager;
    AudioPluginFormatManager formatManager;
    KnownPluginList knownPluginList;
    KnownPluginList::SortMethod pluginSortMethod;
    PopupMenu menu;
    bool menuIconLeftClicked;
    AudioProcessorGraph graph;
    AudioStream player;
    #if JUCE_WINDOWS
    int x, y;
    #endif

    std::unique_ptr<PluginChain> pluginChain;
    std::unique_ptr<PresetManager> presetManager;

    // Audio device recovery (sleep/wake, device disconnection)
    bool deviceRecentlyRecovered = false;
    int  deviceRecoveryRetryCount = 0;
    String lastDeviceError;

    void triggerAudioDeviceRecovery();

    class PluginListWindow;
    std::unique_ptr<PluginListWindow> pluginListWindow;
    StringArray presetFilePaths;
};

#endif /* IconMenu_hpp */
