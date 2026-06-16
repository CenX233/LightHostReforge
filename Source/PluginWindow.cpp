#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginWindow.h"
#include "IconMenu.hpp"
#include "PluginChain.hpp"
#include <atomic>

class PluginWindow;
static Array <PluginWindow*> activePluginWindows;
static constexpr int toolbarHeight = 28;

//==============================================================================
/**
    Listens for editor component resize events and resizes the PluginWindow
    to match. Handles both initial sizing (when the editor first reports a
    valid size that differs from our default) and runtime self-resizing
    (plugins that call resizeView / setSize during operation).
*/
class EditorResizeListener : public ComponentListener
{
public:
    EditorResizeListener (PluginWindow& w) : window (w) {}

    void componentMovedOrResized (Component& comp, bool, bool wasResized) override
    {
        if (! wasResized)   return;
        if (window.isResizingInternally)  return;

        auto w = comp.getWidth();
        auto h = comp.getHeight();
        if (w <= 50 || h <= 50)  return;

        // Resize PluginWindow so content area = editor size + toolbar
        window.isResizingInternally = true;
        window.setContentComponentSize (w, h + toolbarHeight);
        window.isResizingInternally = false;
    }

private:
    PluginWindow& window;
};

//==============================================================================
class ToolbarComponent : public Component, private AudioProcessorListener
{
public:
    ToolbarComponent (PluginWindow& pw, Component* editor, AudioProcessor* processor)
        : pluginWindow (pw), editor (editor), proc (processor)
    {
        proc->addListener (this);

        addAndMakeVisible (bypassButton);
        addAndMakeVisible (moveUpButton);
        addAndMakeVisible (moveDownButton);
        addAndMakeVisible (pinButton);
        addAndMakeVisible (latencyLabel);
        addAndMakeVisible (editor);

        bypassButton.setButtonText (String::fromUTF8 (nonBypassedPluginEmoji) + " Bypass");
        bypassButton.setClickingTogglesState (true);
        bypassButton.onClick = [this]
        {
            if (pluginWindow.getIconMenu() != nullptr)
                pluginWindow.getIconMenu()->togglePluginBypass (pluginWindow.getChainPosition());
        };

        moveUpButton.setButtonText ("Move Up");
        moveUpButton.onClick = [this]
        {
            if (pluginWindow.getIconMenu() != nullptr)
                pluginWindow.getIconMenu()->movePluginUp (pluginWindow.getChainPosition());
        };

        moveDownButton.setButtonText ("Move Down");
        moveDownButton.onClick = [this]
        {
            if (pluginWindow.getIconMenu() != nullptr)
                pluginWindow.getIconMenu()->movePluginDown (pluginWindow.getChainPosition());
        };

        pinButton.setButtonText ("Always On Top");
        pinButton.setClickingTogglesState (true);
        pinButton.onClick = [this] { pluginWindow.toggleAlwaysOnTop(); };

        // Latency display (always visible, including 0)
        int latencySamples = proc->getLatencySamples();
        double sampleRate = proc->getSampleRate();
        int ms = (sampleRate > 0) ? (int) (latencySamples / sampleRate * 1000) : 0;
        latencyLabel.setText ("Latency:" + String (ms) + "ms (" + String (latencySamples) + "sample)",
                              dontSendNotification);
        latencyLabel.setColour (Label::textColourId, findColour (Label::textColourId));

        setSize (400, 300);
    }

    ~ToolbarComponent() override
    {
        proc->removeListener (this);
    }

    void audioProcessorParameterChanged (AudioProcessor*, int, float) override
    {
        // Audio thread — use atomic guard to batch rapid changes
        if (!pendingDirtyNotification.exchange (true))
        {
            auto safeThis = Component::SafePointer<ToolbarComponent> (this);
            MessageManager::callAsync ([safeThis]
            {
                if (auto* self = safeThis.getComponent())
                {
                    self->pendingDirtyNotification = false;
                    if (auto* menu = self->pluginWindow.getIconMenu())
                        menu->markPresetDirty();
                }
            });
        }
    }

    void audioProcessorChanged (AudioProcessor*, const AudioProcessorListener::ChangeDetails& details) override
    {
        if (details.latencyChanged)
        {
            // Use SafePointer to prevent use-after-free if this ToolbarComponent
            // is destroyed before the lambda fires (e.g. during preset switch).
            auto safeThis = Component::SafePointer<ToolbarComponent> (this);
            MessageManager::callAsync ([safeThis]
            {
                if (auto* self = safeThis.getComponent())
                {
                    int current = self->proc->getLatencySamples();
                    double sr = self->proc->getSampleRate();
                    int ms = (sr > 0) ? (int) (current / sr * 1000) : 0;
                    self->latencyLabel.setText ("Latency:" + String (ms) + "ms (" + String (current) + "samples)",
                                                dontSendNotification);
                }
            });
        }
    }

    void resized() override
    {
        auto r = getLocalBounds();
        auto toolbar = r.removeFromTop (::toolbarHeight).reduced (2);

        bypassButton.setBounds (toolbar.removeFromLeft (80).reduced (2));
        moveUpButton.setBounds (toolbar.removeFromLeft (80).reduced (2));
        moveDownButton.setBounds (toolbar.removeFromLeft (80).reduced (2));
        pinButton.setBounds (toolbar.removeFromLeft (80).reduced (2));
        latencyLabel.setBounds (toolbar.removeFromRight (200).reduced (2));

        if (editor != nullptr)
            editor->setBounds (r);
    }

    void updateButtonStates()
    {
        if (pluginWindow.getIconMenu() == nullptr)
            return;

        int totalPlugins = pluginWindow.getIconMenu()->getPluginChain().size();
        int pos = pluginWindow.getChainPosition();

        bool isBypassed = pluginWindow.getIconMenu()->isBypassed (pos);
        bypassButton.setToggleState (isBypassed, dontSendNotification);
        bypassButton.setButtonText (isBypassed
            ? String::fromUTF8 (bypassedPluginEmoji) + " Bypass"
            : String::fromUTF8 (nonBypassedPluginEmoji) + " Bypass");
        moveUpButton.setEnabled (pos > 0);
        moveDownButton.setEnabled (pos < totalPlugins - 1);
        pinButton.setToggleState (pluginWindow.isAlwaysOnTop(), dontSendNotification);
    }

    Component* getEditor() const { return editor.getComponent(); }

private:
    PluginWindow& pluginWindow;
    Component::SafePointer<Component> editor;
    AudioProcessor* proc = nullptr;
    TextButton bypassButton, moveUpButton, moveDownButton, pinButton;
    Label latencyLabel;
    std::atomic<bool> pendingDirtyNotification{false};
};

PluginWindow::PluginWindow (Component* const pluginEditor,
                            AudioProcessorGraph::Node::Ptr o,
                            WindowFormatType t,
                            IconMenu* menu,
                            int prefW, int prefH)
    : DocumentWindow (pluginEditor->getName(),
                      LookAndFeel::getDefaultLookAndFeel().findColour(DocumentWindow::backgroundColourId),
                      DocumentWindow::minimiseButton | DocumentWindow::closeButton),
      owner (std::move (o)),
      type (t),
      iconMenu (menu),
      editorPrefW (prefW),
      editorPrefH (prefH)
{
    // Step 1: Set flags that affect getContentComponentBorder() FIRST
    setUsingNativeTitleBar (false);

    {
        bool allowResize = false;
        if (auto* apEditor = dynamic_cast<AudioProcessorEditor*> (pluginEditor))
            allowResize = apEditor->isResizable();
        setResizable (allowResize, false);
    }

    setResizeLimits (300, 200, 4096, 4096);

    // Step 2: Set content component
    auto* wrapper = new ToolbarComponent (*this, pluginEditor, owner->getProcessor());
    setContentOwned (wrapper, true);

    // Step 3: Determine initial size — saved size > editor preferred size > fallback
    if (owner->properties.contains (getLastWProp (type))
        && owner->properties.contains (getLastHProp (type)))
    {
        setSize (owner->properties[getLastWProp (type)],
                 owner->properties[getLastHProp (type)]);
    }
    else if (editorPrefW > 50 && editorPrefH > 50)
    {
        setContentComponentSize (editorPrefW, editorPrefH + toolbarHeight);
    }
    else
    {
        setContentComponentSize (400, 300 + toolbarHeight);
    }

    // Clamp window position to the current desktop so the window
    // is always at least partially visible (avoids Bug 10 where a
    // preset saved on a larger display loads off-screen).
    {
        auto totalBounds = Desktop::getInstance().getDisplays().getTotalBounds (true);
        int storedX = owner->properties.getWithDefault (getLastXProp (type), Random::getSystemRandom().nextInt (500));
        int storedY = owner->properties.getWithDefault (getLastYProp (type), Random::getSystemRandom().nextInt (500));
        int clampedX = jlimit (totalBounds.getX(), jmax (totalBounds.getX(), totalBounds.getRight()  - 200), storedX);
        int clampedY = jlimit (totalBounds.getY(), jmax (totalBounds.getY(), totalBounds.getBottom() - 100), storedY);
        setTopLeftPosition (clampedX, clampedY);
    }

    owner->properties.set (getOpenProp (type), true);

    // Add listener to detect editor self-resize at runtime
    editorResizeListener = std::make_unique<EditorResizeListener> (*this);
    pluginEditor->addComponentListener (editorResizeListener.get());

    updateTitleAndToolbar();

    setVisible (true);
    activePluginWindows.add (this);

    // Bring to front reliably (bypasses Windows focus-stealing prevention)
    forceToFront();

    // Restore user's always-on-top preference for this window type
    if (owner->properties.getWithDefault (getAlwaysOnTopProp (type), false))
        setAlwaysOnTop (true);

    // Async check: after construction & layout, ensure window fits the editor's real size
    Component::SafePointer<PluginWindow> safeThis (this);
    MessageManager::callAsync ([safeThis]
    {
        if (auto* pw = safeThis.getComponent())
            pw->updateSizeFromEditor();
    });
}

void PluginWindow::updateTitleAndToolbar()
{
    if (iconMenu == nullptr)
        return;

    chainPosition = iconMenu->getPluginChain().getChainPositionForNode (owner->nodeID);

    String pluginName = owner->getProcessor()->getName();
    bool dirty = iconMenu->getPresetManager() != nullptr
                 && iconMenu->getPresetManager()->isDirty();
    setName ("[" + String (chainPosition + 1) + "] " + pluginName + (dirty ? " *" : ""));

    // Update toolbar button states
    if (auto* wrapper = dynamic_cast<ToolbarComponent*> (getContentComponent()))
        wrapper->updateButtonStates();
}

void PluginWindow::updateAllTitlesAndToolbars (IconMenu* iconMenu)
{
    ignoreUnused (iconMenu);
    for (auto* w : activePluginWindows)
        w->updateTitleAndToolbar();
}

void PluginWindow::closeCurrentlyOpenWindowsFor (AudioProcessorGraph::NodeID nodeId)
{
    for (int i = activePluginWindows.size(); --i >= 0;)
        if (activePluginWindows.getUnchecked(i)->owner->nodeID == nodeId)
            delete activePluginWindows.getUnchecked (i);
}

void PluginWindow::closeAllCurrentlyOpenWindows()
{
    if (activePluginWindows.size() > 0)
    {
        for (int i = activePluginWindows.size(); --i >= 0;)
            delete activePluginWindows.getUnchecked (i);
    }
}

bool PluginWindow::containsActiveWindows()
{
    return activePluginWindows.size() > 0;
}

bool PluginWindow::isWindowOpenFor(AudioProcessorGraph::NodeID nodeId)
{
    for (auto* w : activePluginWindows)
        if (w->owner->nodeID == nodeId)
            return true;
    return false;
}

//==============================================================================
class ProcessorProgramPropertyComp : public PropertyComponent,
                                     private AudioProcessorListener
{
public:
    ProcessorProgramPropertyComp (const String& name, AudioProcessor& p, int index_)
        : PropertyComponent (name),
          owner (p),
          index (index_)
    {
        owner.addListener (this);
    }

    ~ProcessorProgramPropertyComp()
    {
        owner.removeListener (this);
    }

    void refresh() { }
    void audioProcessorChanged (AudioProcessor*, const ChangeDetails&) override { }
    void audioProcessorParameterChanged(AudioProcessor* processor, int, float) override { }

private:
    AudioProcessor& owner;
    const int index;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ProcessorProgramPropertyComp)
};

class ProgramAudioProcessorEditor : public AudioProcessorEditor
{
public:
    ProgramAudioProcessorEditor (AudioProcessor* const p)
        : AudioProcessorEditor (p)
    {
        jassert (p != nullptr);
        setOpaque (true);

        addAndMakeVisible (panel);

        Array<PropertyComponent*> programs;

        const int numPrograms = p->getNumPrograms();
        int totalHeight = 0;

        for (int i = 0; i < numPrograms; ++i)
        {
            String name (p->getProgramName (i).trim());

            if (name.isEmpty())
                name = "Unnamed";

            ProcessorProgramPropertyComp* const pc = new ProcessorProgramPropertyComp (name, *p, i);
            programs.add (pc);
            totalHeight += pc->getPreferredHeight();
        }

        panel.addProperties (programs);

        setSize (400, jlimit (25, 400, totalHeight));
    }

    void paint (Graphics& g)
    {
        g.fillAll (findColour(DocumentWindow::backgroundColourId));
    }

    void resized()
    {
        panel.setBounds (getLocalBounds());
    }

private:
    PropertyPanel panel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ProgramAudioProcessorEditor)
};

//==============================================================================
PluginWindow* PluginWindow::getWindowFor (AudioProcessorGraph::Node::Ptr node,
                                          WindowFormatType type,
                                          IconMenu* menu)
{
    jassert (node != nullptr);

    for (int i = activePluginWindows.size(); --i >= 0;)
        if (activePluginWindows.getUnchecked(i)->owner == node
             && activePluginWindows.getUnchecked(i)->type == type)
            return activePluginWindows.getUnchecked(i);

    AudioProcessor* processor = node->getProcessor();
    AudioProcessorEditor* ui = nullptr;

    if (type == Normal)
    {
        ui = processor->createEditorIfNeeded();

        if (ui == nullptr)
            type = Generic;
    }

    if (ui == nullptr)
    {
        if (type == Generic || type == Parameters)
            ui = new GenericAudioProcessorEditor (processor);
        else if (type == Programs)
            ui = new ProgramAudioProcessorEditor (processor);
    }

    if (ui != nullptr)
    {
        if (AudioPluginInstance* const plugin = dynamic_cast<AudioPluginInstance*> (processor))
            ui->setName (plugin->getName());

        // Capture editor's preferred size BEFORE any window manipulation
        int prefW = ui->getWidth();
        int prefH = ui->getHeight();

        return new PluginWindow (ui, std::move (node), type, menu, prefW, prefH);
    }

    return nullptr;
}

PluginWindow::~PluginWindow()
{
    activePluginWindows.removeFirstMatchingValue (this);

    // Remove listener from editor before both are destroyed — prevents
    // dangling pointer crash when the editor fires events later.
    if (auto* editor = getEditorComponent())
        editor->removeComponentListener (editorResizeListener.get());

    clearContentComponent();
}

void PluginWindow::moved()
{
    // Window position changes are saved to app properties but
    // intentionally do NOT mark the preset dirty.  Dragging and
    // resizing windows is a high-frequency cosmetic operation
    // that should not trigger a dirty "*" indicator.
    //
    // Window geometry IS persisted in preset files via
    // PresetManager::savePresetToFile() (reads winX/Y from app
    // properties and writes them to the preset XML), so on
    // explicit user save the current layout is captured.

    owner->properties.set (getLastXProp (type), getX());
    owner->properties.set (getLastYProp (type), getY());

    if (iconMenu != nullptr)
    {
        auto nodeId = owner->nodeID;
        int activeIdx = iconMenu->getPluginChain().getSlotIndexForNode (nodeId);
        if (activeIdx >= 0 && activeIdx < iconMenu->getPluginChain().size())
        {
            auto& slot = iconMenu->getPluginChain()[activeIdx];
            auto& props = *getAppProperties().getUserSettings();
            props.setValue (getPluginKey ("winX", slot.desc), getX());
            props.setValue (getPluginKey ("winY", slot.desc), getY());
        }
    }
}

void PluginWindow::resized()
{
    // Same as moved(): window resize is a cosmetic, high-frequency
    // operation that intentionally does NOT mark the preset dirty.
    // Geometry IS captured in preset files on explicit save.
    // Guard: prevent EditorResizeListener from fighting host-initiated
    // resize (user dragging the window edge).  The prevResizing pattern
    // correctly handles re-entrancy: an outer listener's setSize triggers
    // this resized(), and we restore the outer listener's flag value.
    bool prevResizing = isResizingInternally;
    isResizingInternally = true;

    DocumentWindow::resized();

    isResizingInternally = prevResizing;

    owner->properties.set (getLastWProp (type), getWidth());
    owner->properties.set (getLastHProp (type), getHeight());

    if (iconMenu != nullptr)
    {
        auto nodeId = owner->nodeID;
        int activeIdx = iconMenu->getPluginChain().getSlotIndexForNode (nodeId);
        if (activeIdx >= 0 && activeIdx < iconMenu->getPluginChain().size())
        {
            auto& slot = iconMenu->getPluginChain()[activeIdx];
            auto& props = *getAppProperties().getUserSettings();
            props.setValue (getPluginKey ("winW", slot.desc), getWidth());
            props.setValue (getPluginKey ("winH", slot.desc), getHeight());
        }
    }
}

void PluginWindow::closeButtonPressed()
{
    owner->properties.set (getOpenProp (type), false);
    delete this;
}

bool PluginWindow::keyPressed(const KeyPress& key)
{
    // Ctrl+S (Windows/Linux) or Cmd+S (macOS) → save the current preset
    if (key == KeyPress('s', ModifierKeys::commandModifier, 0))
    {
        if (iconMenu != nullptr)
        {
            iconMenu->saveCurrentPreset();
            return true;
        }
    }
    return false;
}

void PluginWindow::forceToFront()
{
    setAlwaysOnTop (true);
    toFront (true);
    setAlwaysOnTop (false);
}

void PluginWindow::toggleAlwaysOnTop()
{
    bool newState = ! isAlwaysOnTop();
    setAlwaysOnTop (newState);
    owner->properties.set (getAlwaysOnTopProp (type), newState);
}

void PluginWindow::updateSizeFromEditor()
{
    if (isResizingInternally)
        return;

    auto* editor = getEditorComponent();
    if (editor == nullptr)
        return;

    int w = editor->getWidth();
    int h = editor->getHeight();

    // Use preferred size if the editor hasn't established its real size yet
    if ((w <= 50 || h <= 50) && editorPrefW > 50 && editorPrefH > 50)
    {
        w = editorPrefW;
        h = editorPrefH;
    }

    if (w <= 50 || h <= 50)
        return;

    isResizingInternally = true;
    setContentComponentSize (w, h + toolbarHeight);
    isResizingInternally = false;
}

Component* PluginWindow::getEditorComponent() const
{
    if (auto* toolbar = dynamic_cast<ToolbarComponent*> (getContentComponent()))
        return toolbar->getEditor();
    return nullptr;
}
