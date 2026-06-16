#ifndef PluginWindow_h
#define PluginWindow_h

#include <juce_audio_processors/juce_audio_processors.h>

using namespace juce;

class IconMenu;
class EditorResizeListener;
ApplicationProperties& getAppProperties();

class PluginWindow  : public DocumentWindow
{
public:
    enum WindowFormatType
    {
        Normal = 0,
        Generic,
        Programs,
        Parameters,
        NumTypes
    };

    PluginWindow (Component* pluginEditor, AudioProcessorGraph::Node::Ptr, WindowFormatType, IconMenu*,
                  int editorPrefW = 400, int editorPrefH = 300);
    ~PluginWindow();

    static PluginWindow* getWindowFor (AudioProcessorGraph::Node::Ptr, WindowFormatType, IconMenu*);

    static void closeCurrentlyOpenWindowsFor (AudioProcessorGraph::NodeID nodeId);
    static void closeAllCurrentlyOpenWindows();
    static bool containsActiveWindows();

    static void updateAllTitlesAndToolbars(IconMenu* iconMenu);
    static bool isWindowOpenFor(AudioProcessorGraph::NodeID nodeId);

    void moved() override;
    void resized() override;
    void closeButtonPressed() override;
    bool keyPressed(const KeyPress& key) override;

    void updateTitleAndToolbar();
    void forceToFront();
    void toggleAlwaysOnTop();
    void updateSizeFromEditor();
    IconMenu* getIconMenu() const      { return iconMenu; }
    int getChainPosition() const       { return chainPosition; }
    Component* getEditorComponent() const;

private:
    AudioProcessorGraph::Node::Ptr owner;
    WindowFormatType type;
    IconMenu* iconMenu = nullptr;
    int chainPosition = -1;
    int editorPrefW;
    int editorPrefH;

    friend class ::EditorResizeListener;
    std::unique_ptr<ComponentListener> editorResizeListener;
    bool isResizingInternally = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginWindow)
};

inline String toString (PluginWindow::WindowFormatType type)
{
    switch (type)
    {
        case PluginWindow::Normal:     return "Normal";
        case PluginWindow::Generic:    return "Generic";
        case PluginWindow::Programs:   return "Programs";
        case PluginWindow::Parameters: return "Parameters";
        default:                       return String();
    }
}

inline String getLastXProp (PluginWindow::WindowFormatType type)    { return "uiLastX_" + toString (type); }
inline String getLastYProp (PluginWindow::WindowFormatType type)    { return "uiLastY_" + toString (type); }
inline String getLastWProp (PluginWindow::WindowFormatType type)    { return "uiLastW_" + toString (type); }
inline String getLastHProp (PluginWindow::WindowFormatType type)    { return "uiLastH_" + toString (type); }
inline String getOpenProp  (PluginWindow::WindowFormatType type)    { return "uiopen_"  + toString (type); }

/** Build an ApplicationProperties key string for a plugin property.
    The key format is "plugin-<type>-<name><version><format>".
    Used by IconMenu, PresetManager, and PluginWindow for persisting
    window geometry, bypass state, and processor state per plugin.
*/
inline String getPluginKey (const String& type, const PluginDescription& plugin)
{
    return "plugin-" + type + "-" + plugin.name + plugin.version + plugin.pluginFormatName;
}

inline String getAlwaysOnTopProp (PluginWindow::WindowFormatType type)
{
    return "uiAlwaysOnTop_" + toString (type);
}

#endif /* PluginWindow_hpp */
