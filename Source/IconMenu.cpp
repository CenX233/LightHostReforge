//
//  IconMenu.cpp
//  Light Host
//
//  Created by Rolando Islas on 12/26/15.
//
//

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <BinaryData.h>
#include "IconMenu.hpp"
#include "PluginChain.hpp"
#include "PluginWindow.h"
#include "AudioSettingsComponent.hpp"
#include "NoneAudioDevice.hpp"
#include <algorithm>
#include <ctime>
#include <climits>
#if JUCE_WINDOWS
#include "Windows.h"
#endif

//==============================================================================
/** Remove plugins with fewer than 2 input or output channels from the list.
    Called when the plugin-selection window closes to keep the list clean.
*/
static void removePluginsLackingInputOutput (KnownPluginList& knownPluginList)
{
    // Reverse iteration: removing the current element only shifts already-
    // processed tail entries, so the index adjustment is trivially correct.
    // A copy of the PluginDescription is passed to removeType rather than a
    // reference into the list's internal array, because the removal operation
    // shifts array elements which would invalidate any interior reference.
    for (int i = knownPluginList.getNumTypes(); --i >= 0;)
    {
        if (auto* plugin = knownPluginList.getType (i))
        {
            if (plugin->numInputChannels < 2 || plugin->numOutputChannels < 2)
            {
                auto desc = *plugin;
                knownPluginList.removeType (desc);
            }
        }
    }
}

class IconMenu::PluginListWindow : public DocumentWindow
{
public:
    PluginListWindow(IconMenu& owner_, AudioPluginFormatManager& pluginFormatManager_)
        : DocumentWindow("Select Plugins",
            LookAndFeel::getDefaultLookAndFeel().findColour(DocumentWindow::backgroundColourId),
            DocumentWindow::minimiseButton | DocumentWindow::closeButton),
        owner(owner_), pluginFormatManager(pluginFormatManager_)
    {
        setContentOwned(new ContentComponent(*this), true);

        optionsButton.setButtonText("Options");
        optionsButton.onClick = [this] { showOptionsMenu(); };

        detailLabel.setText("Select a plugin", dontSendNotification);
        treeView.setDefaultOpenness(false);
        treeView.setRootItemVisible(false);
        treeView.setColour(TreeView::selectedItemBackgroundColourId,
            findColour(DirectoryContentsDisplayComponent::highlightColourId));
        rebuildTree();
        setResizable(true, false);
        setResizeLimits(400, 300, 800, 1500);
        setTopLeftPosition(60, 60);

        restoreWindowStateFromString(getAppProperties().getUserSettings()->getValue("listWindowPos"));
        setVisible(true);
    }

    ~PluginListWindow()
    {
        getAppProperties().getUserSettings()->setValue("listWindowPos", getWindowStateAsString());
        clearContentComponent();
    }

    void closeButtonPressed() override
    {
        removePluginsLackingInputOutput (owner.knownPluginList);
        // Defer destruction to avoid use-after-free: the DocumentWindow base
        // class continues to access `this` after closeButtonPressed() returns,
        // so synchronously setting the unique_ptr to nullptr would destroy the
        // object while it is still in use.
        auto* ownerPtr = &owner;
        MessageManager::callAsync ([ownerPtr]() {
            ownerPtr->pluginListWindow = nullptr;
        });
    }

private:
    // --- Root item (invisible, holds manufacturer groups) ---
    class RootItem : public TreeViewItem
    {
    public:
        RootItem() {}
        bool mightContainSubItems() override { return true; }
        int getItemHeight() const override { return 0; }
        void paintItem(Graphics&, int, int) override {}
        String getUniqueName() const override { return "root"; }
    };

    // --- Content component for layout ---
    class ContentComponent : public Component
    {
    public:
        ContentComponent(PluginListWindow& w) : window(w)
        {
            setOpaque(true);
            addAndMakeVisible(window.optionsButton);
            addAndMakeVisible(window.detailLabel);
            addAndMakeVisible(window.treeView);
        }

        void paint(Graphics& g) override
        {
            g.fillAll(findColour(DocumentWindow::backgroundColourId));
        }

        void resized() override
        {
            auto r = getLocalBounds();
            auto toolbar = r.removeFromTop(26);

            auto btnWidth = jmin(100, toolbar.getWidth() / 3);
            window.optionsButton.setBounds(toolbar.removeFromLeft(btnWidth).reduced(2, 2));
            window.detailLabel.setBounds(toolbar.reduced(4, 2));

            window.treeView.setBounds(r.reduced(2));
        }

    private:
        PluginListWindow& window;
    };

    // --- Tree item classes ---
    class ManufacturerItem : public TreeViewItem
    {
    public:
        ManufacturerItem(const String& name) : mfrName(name) {}

        bool mightContainSubItems() override { return true; }
        String getUniqueName() const override { return mfrName; }

        void itemClicked(const MouseEvent& e) override
        {
            TreeViewItem::itemClicked(e);
            setOpen(!isOpen());
        }

        void itemDoubleClicked(const MouseEvent&) override {}

        void paintItem(Graphics& g, int width, int height) override
        {
            if (isSelected())
            {
                g.fillAll(getOwnerView()->findColour(TreeView::selectedItemBackgroundColourId));
                g.setColour(getOwnerView()->findColour(ListBox::textColourId));
            }
            else
            {
                g.setColour(getOwnerView()->findColour(ListBox::textColourId));
            }
            g.setFont(Font(height * 0.7f, Font::bold));
            g.drawText(mfrName, 4, 0, width - 8, height, Justification::centredLeft, true);
        }

    private:
        String mfrName;
    };

    class PluginItem : public TreeViewItem
    {
    public:
        PluginItem(const PluginDescription& desc, PluginListWindow& w)
            : pluginDesc(desc), window(w) {}

        bool mightContainSubItems() override { return false; }
        String getUniqueName() const override
        {
            return pluginDesc.name + "_" + pluginDesc.pluginFormatName + "_" + pluginDesc.version;
        }

        void paintItem(Graphics& g, int width, int height) override
        {
            if (isSelected())
            {
                g.fillAll(getOwnerView()->findColour(TreeView::selectedItemBackgroundColourId));
                g.setColour(getOwnerView()->findColour(ListBox::textColourId));
            }
            else
            {
                g.setColour(getOwnerView()->findColour(ListBox::textColourId));
            }
            g.setFont(Font(height * 0.6f));
            g.drawText(pluginDesc.name, 4, 0, width - 8, height, Justification::centredLeft, true);
        }

        void itemClicked(const MouseEvent& e) override
        {
            TreeViewItem::itemClicked(e);
            window.showPluginDetails(pluginDesc);
        }

        void itemDoubleClicked(const MouseEvent& e) override
        {
            window.addPluginToChain(pluginDesc);
        }

    private:
        PluginDescription pluginDesc;
        PluginListWindow& window;
    };

    // --- Members ---
    IconMenu& owner;
    AudioPluginFormatManager& pluginFormatManager;
    TextButton optionsButton;
    Label detailLabel;
    TreeView treeView;

    // --- Methods ---
    void showOptionsMenu()
    {
        PopupMenu menu;
        menu.addItem(1, "Scan for new or updated plug-ins...");
        menu.addItem(2, "Remove dead plug-ins from list");
        menu.addItem(3, "Clear plug-in list");

        menu.showMenuAsync(PopupMenu::Options(), [this](int result) {
            if (result == 1) scanForPlugins();
            else if (result == 2) removeDeadPlugins();
            else if (result == 3) clearPluginList();
        });
    }

    // --- Scan dialog ---
    class ScanDialogContent : public Component, private ListBoxModel
    {
    public:
        ScanDialogContent(PluginListWindow& pw, AudioPluginFormatManager& fmtMgr,
            KnownPluginList& pluginList, IconMenu& owner)
            : pluginWindow(pw), formatManager(fmtMgr),
              knownList(pluginList), iconMenu(owner)
        {
            String saved = getAppProperties().getUserSettings()->getValue("pluginScanPaths");
            if (saved.isNotEmpty())
                paths.addTokens(saved, ";", "");
            if (paths.size() == 0)
            {
            #if JUCE_WINDOWS
                paths.add(File::getSpecialLocation(File::globalApplicationsDirectory)
                    .getChildFile("Common Files").getChildFile("VST3").getFullPathName());
                paths.add(File::getSpecialLocation(File::globalApplicationsDirectory)
                    .getChildFile("Steinberg").getChildFile("VstPlugins").getFullPathName());
                paths.add(File::getSpecialLocation(File::globalApplicationsDirectory)
                    .getChildFile("VstPlugins").getFullPathName());
            #elif JUCE_MAC
                paths.add("/Library/Audio/Plug-Ins/VST3");
                paths.add("~/Library/Audio/Plug-Ins/VST3");
                paths.add("/Library/Audio/Plug-Ins/VST");
                paths.add("~/Library/Audio/Plug-Ins/VST");
            #elif JUCE_LINUX
                paths.add("/usr/lib/vst3");
                paths.add("~/.vst3");
                paths.add("/usr/lib/vst");
                paths.add("/usr/local/lib/vst");
                paths.add("~/.vst");
            #endif
            }

            statusLabel.setText("Ready to scan.", dontSendNotification);
            listBox.setModel(this);
            addAndMakeVisible(statusLabel);
            addAndMakeVisible(listBox);
            addAndMakeVisible(addButton);
            addAndMakeVisible(removeButton);
            addAndMakeVisible(clearButton);
            addAndMakeVisible(scanButton);
            addAndMakeVisible(cancelButton);

            addButton.setButtonText("Add");
            removeButton.setButtonText("Remove");
            clearButton.setButtonText("Clear...");
            scanButton.setButtonText("Scan");
            cancelButton.setButtonText("Cancel");

            addButton.onClick = [this] { addPath(); };
            removeButton.onClick = [this] { removeSelectedPath(); };
            clearButton.onClick = [this] { clearList(); };
            scanButton.onClick = [this] { runScan(); };
            cancelButton.onClick = [this] { exitDialog(0); };

            setSize(500, 400);
        }

        void resized() override
        {
            auto r = getLocalBounds().reduced(8);
            auto bottomBar = r.removeFromBottom(28);
            auto midBar = r.removeFromBottom(28);

            auto scanW = bottomBar.getWidth() / 2;
            scanButton.setBounds(bottomBar.removeFromLeft(scanW).reduced(4));
            cancelButton.setBounds(bottomBar.reduced(4));

            auto btnW = midBar.getWidth() / 3;
            addButton.setBounds(midBar.removeFromLeft(btnW).reduced(4));
            removeButton.setBounds(midBar.removeFromLeft(btnW).reduced(4));
            clearButton.setBounds(midBar.reduced(4));

            auto statusBar = r.removeFromBottom(22);
            statusLabel.setBounds(statusBar.reduced(4));

            listBox.setBounds(r);
        }

    private:
        PluginListWindow& pluginWindow;
        AudioPluginFormatManager& formatManager;
        KnownPluginList& knownList;
        IconMenu& iconMenu;
        StringArray paths;
        ListBox listBox;
        TextButton addButton, removeButton, clearButton, scanButton, cancelButton;
        Label statusLabel;

        int getNumRows() override { return paths.size(); }

        void paintListBoxItem(int rowNumber, Graphics& g, int width, int height, bool rowIsSelected) override
        {
            if (rowIsSelected)
                g.fillAll(findColour(DirectoryContentsDisplayComponent::highlightColourId));
            g.setColour(findColour(ListBox::textColourId));
            g.setFont(Font(13.0f));
            g.drawText(paths[rowNumber], 4, 0, width - 8, height, Justification::centredLeft, true);
        }

        void addPath()
        {
            FileChooser chooser("Select a plug-in folder to scan");
            if (chooser.browseForDirectory())
            {
                paths.add(chooser.getResult().getFullPathName());
                listBox.updateContent();
                savePaths();
            }
        }

        void removeSelectedPath()
        {
            int sel = listBox.getSelectedRow();
            if (sel >= 0 && sel < paths.size())
            {
                paths.remove(sel);
                listBox.updateContent();
                listBox.deselectAllRows();
                savePaths();
            }
        }

        void clearList()
        {
            knownList.clear();
            pluginWindow.rebuildTree();
        }

        void runScan()
        {
            savePaths();
            scanButton.setEnabled(false);
            scanButton.setButtonText("Scanning...");
            statusLabel.setText("Scanning...", dontSendNotification);

            FileSearchPath searchPaths;
            for (auto& p : paths)
                searchPaths.add(File(p));

            File deadMansPedal(getAppProperties().getUserSettings()
                ->getFile().getSiblingFile("RecentlyCrashedPluginsList"));

            for (int i = 0; i < formatManager.getNumFormats(); ++i)
            {
                auto* format = formatManager.getFormat(i);
                if (format == nullptr) continue;

                PluginDirectoryScanner scanner(knownList, *format,
                    searchPaths, true, deadMansPedal);

                String pluginName;
                while (scanner.scanNextFile(true, pluginName))
                {
                    statusLabel.setText("Scanning: " + scanner.getNextPluginFileThatWillBeScanned(), dontSendNotification);
                    MessageManager::getInstance()->runDispatchLoopUntil(2);
                }
            }

            statusLabel.setText("Ready to scan.", dontSendNotification);
            scanButton.setButtonText("Scan");
            scanButton.setEnabled(true);
            pluginWindow.rebuildTree();
        }

        void savePaths()
        {
            getAppProperties().getUserSettings()->setValue("pluginScanPaths",
                paths.joinIntoString(";"));
            getAppProperties().saveIfNeeded();
        }

        void exitDialog(int result)
        {
            if (auto* dw = findParentComponentOfClass<DialogWindow>())
                dw->exitModalState(result);
        }
    };

    void scanForPlugins()
    {
        DialogWindow::LaunchOptions opts;
        opts.content.setOwned(new ScanDialogContent(*this, pluginFormatManager,
            owner.knownPluginList, owner));
        opts.dialogTitle = "Scan for Plugins";
        opts.componentToCentreAround = this;
        opts.escapeKeyTriggersCloseButton = true;
        opts.useNativeTitleBar = false;
        opts.dialogBackgroundColour = LookAndFeel::getDefaultLookAndFeel().findColour(DocumentWindow::backgroundColourId);
        opts.resizable = false;
        opts.runModal();
        rebuildTree();
    }

    void removeDeadPlugins()
    {
        for (int i = owner.knownPluginList.getNumTypes() - 1; i >= 0; i--)
        {
            if (auto* desc = owner.knownPluginList.getType(i))
            {
                if (!File(desc->fileOrIdentifier).exists())
                    owner.knownPluginList.removeType(*desc);
            }
        }
        rebuildTree();
    }

    void clearPluginList()
    {
        owner.knownPluginList.clear();
        rebuildTree();
    }

    void rebuildTree()
    {
        auto* root = static_cast<TreeViewItem*>(treeView.getRootItem());
        if (root != nullptr)
            root->clearSubItems();
        else
        {
            root = new RootItem();
            treeView.setRootItem(root);
        }

        std::map<String, std::vector<PluginDescription>> byMfr;
        for (int i = 0; i < owner.knownPluginList.getNumTypes(); i++)
        {
            if (auto* desc = owner.knownPluginList.getType(i))
                byMfr[desc->manufacturerName].push_back(*desc);
        }

        for (auto& pair : byMfr)
        {
            std::sort(pair.second.begin(), pair.second.end(),
                [](const PluginDescription& a, const PluginDescription& b) {
                    return a.name.compareIgnoreCase(b.name) < 0;
                });
            auto* mfrItem = new ManufacturerItem(pair.first);
            for (auto& plugin : pair.second)
                mfrItem->addSubItem(new PluginItem(plugin, *this));
            root->addSubItem(mfrItem);
        }
    }

    void showPluginDetails(const PluginDescription& desc)
    {
        String info = desc.category + " / " + desc.pluginFormatName + " / v" + desc.version;
        detailLabel.setText(info, dontSendNotification);
    }

    void addPluginToChain(const PluginDescription& desc)
    {
        owner.pluginChain->add(desc);

        auto* ownerPtr = &owner;
        MessageManager::callAsync([ownerPtr]() {
            ownerPtr->pluginListWindow = nullptr;
        });
    }
};

IconMenu::IconMenu() : INDEX_EDIT(1000000), INDEX_BYPASS(2000000), INDEX_DELETE(3000000),
INDEX_MOVE_UP(4000000), INDEX_MOVE_DOWN(5000000),
INDEX_PRESET_SAVE(6000000), INDEX_PRESET_SAVE_AS(6000001),
INDEX_PRESET_LOAD_SELECT(6000002), INDEX_PRESET_NEW(6000003),
INDEX_PRESET_LOAD_FILE(7000000)
{
    // Initialization
    formatManager.addDefaultFormats();
#if JUCE_WINDOWS
    x = y = 0;
#endif
    // Register the Loopback audio device type (WASAPI loopback capture).
    // Must create default types first, then add our custom type, then call
    // initialise — this ensures both the standard types (Windows Audio, ASIO)
    // and "Loopback" are available, and the saved device state can correctly
    // restore "Loopback" if it was the previously selected type.
    {
        OwnedArray<AudioIODeviceType> defaultTypes;
        deviceManager.createAudioDeviceTypes(defaultTypes);
        for (auto* t : defaultTypes)
        {
            t->scanForDevices();
            deviceManager.addAudioDeviceType(std::unique_ptr<AudioIODeviceType>(t));
        }
        defaultTypes.clear(false);

        auto* loopbackType = new LoopbackAudioIODeviceType();
        loopbackType->scanForDevices();
        deviceManager.addAudioDeviceType(std::unique_ptr<AudioIODeviceType>(loopbackType));

        // Register the silent "None" device type — used as a fallback when
        // a preset's saved audio device type is not available on this system.
        auto* noneType = new NoneAudioIODeviceType();
        noneType->scanForDevices();
        deviceManager.addAudioDeviceType(std::unique_ptr<AudioIODeviceType>(noneType));
    }

    // Audio device
    auto* settings = getAppProperties().getUserSettings();
    auto savedAudioState = settings->getXmlValue("audioDeviceState");

    // Try to load per-device-type state: extract the device type from the
    // generic key and look for a more recent type-specific key.
    if (savedAudioState)
    {
        if (auto* typeEl = savedAudioState->getChildByName("DEVICETYPE"))
        {
            auto deviceType = typeEl->getAllSubText();
            if (deviceType.isNotEmpty())
            {
                auto perTypeKey = "audioDeviceState_" + deviceType;
                if (auto perTypeState = settings->getXmlValue(perTypeKey))
                    savedAudioState = std::move(perTypeState);
            }
        }
    }

    String audioInitError = deviceManager.initialise(256, 256, savedAudioState.get(), false);
    if (audioInitError.isNotEmpty())
        lastDeviceError = audioInitError;
    // Note: when savedAudioState is null (first launch), initialise() falls back
    // to initialiseDefault() automatically — that path is fine.  Only when a
    // previously-saved device is unavailable do we stay silent (no fallback).
    player.setProcessor(&graph);
    deviceManager.addAudioCallback(&player);

    // Register audio device error/stopped callbacks for automatic recovery
    // after sleep/wake or device disconnection.
    player.onDeviceError = [this](const String& msg) {
        lastDeviceError = msg;
        triggerAudioDeviceRecovery();
    };
    player.onDeviceStopped = [this]() {
        if (deviceManager.getCurrentAudioDevice() == nullptr)
            triggerAudioDeviceRecovery();
    };
    // Plugins - all
    auto savedPluginList = getAppProperties().getUserSettings()->getXmlValue("pluginList");
    if (savedPluginList != nullptr)
        knownPluginList.recreateFromXml(*savedPluginList);
    pluginSortMethod = KnownPluginList::sortByManufacturer;
    knownPluginList.addChangeListener(this);

    // PluginChain: unified plugin chain management
    pluginChain = std::make_unique<PluginChain>(graph, formatManager, player);
    presetManager = std::make_unique<PresetManager>(*pluginChain, getAppProperties());

    // Load chain from app properties (new format) or from old format
    pluginChain->loadFromProperties(getAppProperties());

    if (pluginChain->size() == 0)
    {
        // Old format migration — use a local KnownPluginList instead of
        // a member variable, since this migration runs only once per fresh start.
        KnownPluginList oldActiveList;
        auto savedPluginListActive = getAppProperties().getUserSettings()->getXmlValue("pluginListActive");
        if (savedPluginListActive != nullptr)
            oldActiveList.recreateFromXml(*savedPluginListActive);

        for (int i = 0; i < oldActiveList.getNumTypes(); i++)
        {
            if (auto* desc = oldActiveList.getType(i))
            {
                PluginSlot slot;
                slot.desc = *desc;
                slot.bypassed = getAppProperties().getUserSettings()
                    ->getBoolValue(getPluginKey("bypass", *desc), false);
                String stateKey = getPluginKey("state", *desc);
                String stateStr = getAppProperties().getUserSettings()->getValue(stateKey);
                if (stateStr.isNotEmpty())
                    slot.state.fromBase64Encoding(stateStr);
                pluginChain->addSlot(std::move(slot));
            }
        }
        // Clean up old individual keys
        for (int i = 0; i < pluginChain->size(); i++)
        {
            auto& slot = (*pluginChain)[i];
            getAppProperties().getUserSettings()->removeValue(getPluginKey("state", slot.desc));
            getAppProperties().getUserSettings()->removeValue(getPluginKey("bypass", slot.desc));
        }
        getAppProperties().saveIfNeeded();
        pluginChain->saveToProperties(getAppProperties());
    }

    // Build graph from chain (pause audio, rebuild, resume)
    deviceManager.removeAudioCallback(&player);
    player.setProcessor(nullptr);
    pluginChain->loadAll();
    player.setProcessor(&graph);
    deviceManager.addAudioCallback(&player);

    setIcon();
    setIconTooltip(JUCEApplication::getInstance()->getApplicationName());
    String savedPresetPath = getAppProperties().getUserSettings()->getValue("currentPresetPath");
    if (savedPresetPath.isNotEmpty())
        presetManager->setCurrentPresetFile(File(savedPresetPath));

    // Create default preset if no preset files exist
    File presetDir = PresetManager::getDefaultPresetDirectory();
    Array<File> existingPresets = presetDir.findChildFiles(File::findFiles, false, "*.lhp");
    if (existingPresets.size() == 0)
    {
        File defaultFile = presetDir.getChildFile("default.lhp");
        presetManager->savePresetToFile(defaultFile);
        presetManager->setCurrentPresetFile(defaultFile);
        getAppProperties().getUserSettings()->setValue("currentPresetPath",
            defaultFile.getFullPathName());
        getAppProperties().saveIfNeeded();
    }
};

IconMenu::~IconMenu()
{
    pluginChain->fadeOut();
    player.suspend(deviceManager);

    // Save chain state before closing
    if (pluginChain != nullptr)
        pluginChain->saveToProperties(getAppProperties());

    PluginWindow::closeAllCurrentlyOpenWindows();
}

void IconMenu::setIcon()
{
    // Load icons via ImageCache (avoids decoding PNG on every menu open)
#if JUCE_MAC
    if (Desktop::isDarkModeActive())
    {
        auto img = ImageCache::getFromMemory(BinaryData::menu_icon_white_png, BinaryData::menu_icon_white_pngSize);
        setIconImage(img, img);
    }
    else
    {
        auto img = ImageCache::getFromMemory(BinaryData::menu_icon_png, BinaryData::menu_icon_pngSize);
        setIconImage(img, img);
    }
#else
    String defaultColor;
#if JUCE_WINDOWS
    defaultColor = "white";
#elif JUCE_LINUX
    defaultColor = "black";
#endif
    if (!getAppProperties().getUserSettings()->containsKey("icon"))
        getAppProperties().getUserSettings()->setValue("icon", defaultColor);
    String color = getAppProperties().getUserSettings()->getValue("icon");
    Image icon;
    if (color.equalsIgnoreCase("white"))
        icon = ImageCache::getFromMemory(BinaryData::menu_icon_white_png, BinaryData::menu_icon_white_pngSize);
    else if (color.equalsIgnoreCase("black"))
        icon = ImageCache::getFromMemory(BinaryData::menu_icon_png, BinaryData::menu_icon_pngSize);
    setIconImage(icon, icon);
#endif
}

void IconMenu::changeListenerCallback(ChangeBroadcaster* changed)
{
    if (changed == &knownPluginList)
    {
        // When user clears the plugin list (count drops to 0), also clear WaveShell from blacklist
        if (knownPluginList.getNumTypes() == 0)
        {
            for (auto& b : knownPluginList.getBlacklistedFiles())
                if (b.containsIgnoreCase("WaveShell"))
                    knownPluginList.removeFromBlacklist(b);
        }

        auto savedPluginList = knownPluginList.createXml();
        if (savedPluginList != nullptr)
        {
            getAppProperties().getUserSettings()->setValue("pluginList", savedPluginList.get());
            getAppProperties().saveIfNeeded();
        }
    }
}

void IconMenu::timerCallback()
{
    stopTimer();
    menu.clear();
    menu.addSectionHeader(JUCEApplication::getInstance()->getApplicationName());
    if (menuIconLeftClicked) {
        menu.addItem(1, "Preferences");
        menu.addSeparator();
        menu.addSectionHeader("Active Plugins");
        // Active plugins — directly from PluginChain (vector index = chain position)
        {
            for (int i = 0; i < pluginChain->size(); i++)
            {
                const auto& slot = (*pluginChain)[i];
                PopupMenu options;
                if (!slot.isFailed())
                {
                    bool isOpen = (slot.node != nullptr)
                        && PluginWindow::isWindowOpenFor(slot.node->nodeID);
                    options.addItem(INDEX_EDIT + i, "Edit", true, isOpen);
                }
                options.addItem(INDEX_BYPASS + i, "Bypass", true, slot.bypassed);
                options.addSeparator();
                options.addItem(INDEX_MOVE_UP + i, "Move Up", i > 0);
                options.addItem(INDEX_MOVE_DOWN + i, "Move Down", i < pluginChain->size() - 1);
                options.addSeparator();
                options.addItem(INDEX_DELETE + i, "Delete");

                // Emoji: 🔴 failed, ⚪ bypassed, 🟢 active
                String emoji;
                if (slot.isFailed())
                    emoji = String::fromUTF8(failedPluginEmoji);
                else if (slot.bypassed)
                    emoji = String::fromUTF8(bypassedPluginEmoji);
                else
                    emoji = String::fromUTF8(nonBypassedPluginEmoji);

                String displayText = emoji + " [" + String(i + 1) + "] " + slot.desc.name;

                PopupMenu::Item item(displayText);
                item.itemID = slot.isFailed() ? 0 : INDEX_EDIT + i;
                item.subMenu = std::make_unique<PopupMenu>(std::move(options));
                menu.addItem(std::move(item));
            }
        }
        menu.addItem(2, "Add plugins...");

        // Total latency display

        menu.addSeparator();
        // Presets section
        {
            menu.addSectionHeader("Presets");
            if (presetManager->getCurrentPresetFile().exists())
                menu.addItem(1, presetManager->getCurrentPresetFile().getFileName(), false);
            }
            bool hasPlugins = pluginChain->size() > 0;
            menu.addItem(INDEX_PRESET_NEW, "New", true);
            menu.addItem(INDEX_PRESET_SAVE, presetManager->isDirty() ? "Save*" : "Save", hasPlugins);
            menu.addItem(INDEX_PRESET_SAVE_AS, "Save As...", hasPlugins);

            PopupMenu loadMenu;
            loadMenu.addItem(INDEX_PRESET_LOAD_SELECT, "Select File...");
            presetFilePaths.clear();
            Array<File> presetFiles = PresetManager::getDefaultPresetDirectory()
                .findChildFiles(File::findFiles, false, "*.lhp");
            if (presetFiles.size() > 0)
                loadMenu.addSeparator();
            for (int i = 0; i < presetFiles.size(); i++)
            {
                presetFilePaths.add(presetFiles[i].getFullPathName());
                bool isCurrent = (presetManager->getCurrentPresetFile().getFullPathName() == presetFilePaths[i]);
                // Only show checkmark if the current preset is in the default directory
                if (isCurrent)
                {
                    bool inDefaultDir = presetManager->getCurrentPresetFile().getParentDirectory()
                                        == PresetManager::getDefaultPresetDirectory();
                    isCurrent = isCurrent && inDefaultDir;
                }
                loadMenu.addItem(INDEX_PRESET_LOAD_FILE + i,
                    presetFiles[i].getFileNameWithoutExtension(),
                    true,
                    isCurrent);
            }
            menu.addSubMenu("Load", loadMenu, true);
    }
    else
    {
        menu.addItem(1, "Quit");
        menu.addSeparator();
        menu.addItem(2, "Delete Plugin States");
#if !JUCE_MAC
        menu.addItem(3, "Invert Icon Color");
#endif
    }
#if JUCE_MAC || JUCE_LINUX
    menu.showMenuAsync(PopupMenu::Options().withTargetComponent(this), ModalCallbackFunction::forComponent(menuInvocationCallback, this));
#else
    if (x == 0 || y == 0)
    {
        POINT iconLocation;
        iconLocation.x = 0;
        iconLocation.y = 0;
        GetCursorPos(&iconLocation);
        x = iconLocation.x;
        y = iconLocation.y;
    }
    juce::Rectangle<int> rect(x, y, 1, 1);
    menu.showMenuAsync(PopupMenu::Options().withTargetScreenArea(rect), ModalCallbackFunction::forComponent(menuInvocationCallback, this));
#endif
}

void IconMenu::mouseDown(const MouseEvent& e)
{
#if JUCE_MAC || JUCE_LINUX
    Process::setDockIconVisible(true);
#endif
    Process::makeForegroundProcess();
    menuIconLeftClicked = e.mods.isLeftButtonDown();
    startTimer(50);
}

void IconMenu::menuInvocationCallback(int id, IconMenu* im)
{
    // Right click
    if ((!im->menuIconLeftClicked))
    {
        if (id == 1)
        {
            // Fade out and stop audio thread
            im->pluginChain->fadeOut();
            im->player.suspend(im->deviceManager);

            // Clear tray icon — prevents icon from persisting after exit
            {
                juce::Image clearImg(juce::Image::ARGB, 1, 1, true);
                clearImg.clear(clearImg.getBounds(), juce::Colours::transparentBlack);
                im->setIconImage(clearImg, clearImg);
            }

            im->pluginChain->saveToProperties(getAppProperties());
            return JUCEApplication::getInstance()->quit();
        }
        if (id == 2)
        {
            // Clear saved states and rebuild with defaults
            for (int i = 0; i < im->pluginChain->size(); i++)
                (*im->pluginChain)[i].state = MemoryBlock();

            im->pluginChain->fadeOut();
            im->player.suspend(im->deviceManager);
            // Close all plugin windows before rebuilding — the old
            // instances are destroyed inside loadAll() via graph.clear()
            PluginWindow::closeAllCurrentlyOpenWindows();
            im->pluginChain->loadAll();
            im->player.resume(im->deviceManager, im->graph);
            return;
        }
        if (id == 3)
        {
            String color = getAppProperties().getUserSettings()->getValue("icon");
            getAppProperties().getUserSettings()->setValue("icon", color.equalsIgnoreCase("black") ? "white" : "black");
            return im->setIcon();
        }
    }
#if JUCE_MAC || JUCE_LINUX
    // Click elsewhere
    if (id == 0 && !PluginWindow::containsActiveWindows())
        Process::setDockIconVisible(false);
#endif
    // Audio settings
    if (id == 1)
        im->showAudioSettings();
    // Reload
    if (id == 2)
        im->reloadPlugins();
    // Presets
    if (id == im->INDEX_PRESET_NEW)
    {
        im->presetManager->newPreset(
            [im] { im->pluginChain->fadeOut(); im->player.suspend(im->deviceManager); },
            [im] { im->player.resume(im->deviceManager, im->graph); });
        return;
    }
    if (id == im->INDEX_PRESET_SAVE)
    {
        im->saveCurrentPreset();
        return;
    }
    if (id == im->INDEX_PRESET_SAVE_AS)
    {
        FileChooser chooser("Save Preset As",
            PresetManager::getDefaultPresetDirectory().getChildFile("Untitled.lhp"),
            "*.lhp");
        if (chooser.browseForFileToSave(true))
        {
            File result = chooser.getResult();
            im->presetManager->savePresetToFile(result);
            im->presetManager->setCurrentPresetFile(result);
            getAppProperties().getUserSettings()->setValue("currentPresetPath",
                result.getFullPathName());
            getAppProperties().saveIfNeeded();
            im->presetManager->clearDirty();
            PluginWindow::updateAllTitlesAndToolbars(im);
        }
        return;
    }
    if (id == im->INDEX_PRESET_LOAD_SELECT)
    {
        FileChooser chooser("Load Preset",
            PresetManager::getDefaultPresetDirectory(), "*.lhp");
        if (chooser.browseForFileToOpen())
        {
            File result = chooser.getResult();
            im->presetManager->loadPresetFromFile(result,
                [im] { im->pluginChain->fadeOut(); im->player.suspend(im->deviceManager); },
                [im] { im->player.resume(im->deviceManager, im->graph); });
        }
        return;
    }
    if (id >= im->INDEX_PRESET_LOAD_FILE && id < im->INDEX_PRESET_LOAD_FILE + 1000000)
    {
        int index = id - im->INDEX_PRESET_LOAD_FILE;
        if (index >= 0 && index < im->presetFilePaths.size())
        {
            File presetFile(im->presetFilePaths[index]);
            if (presetFile.exists())
                im->presetManager->loadPresetFromFile(presetFile,
                    [im] { im->pluginChain->fadeOut(); im->player.suspend(im->deviceManager); },
                    [im] { im->player.resume(im->deviceManager, im->graph); });
        }
        return;
    }
    // Plugins
    if (id > 2)
    {
        // Delete plugin
        if (id >= im->INDEX_DELETE && id < im->INDEX_DELETE + 1000000)
        {
            int index = id - im->INDEX_DELETE;

            im->pluginChain->remove(index);
            im->presetManager->markDirty();
            PluginWindow::updateAllTitlesAndToolbars(im);
        }
        // Add plugin
        else if (im->knownPluginList.getIndexChosenByMenu(id) > -1)
        {
            PluginDescription plugin = *im->knownPluginList.getType(im->knownPluginList.getIndexChosenByMenu(id));

            im->pluginChain->add(plugin);
            im->presetManager->markDirty();
            PluginWindow::updateAllTitlesAndToolbars(im);
        }
        // Bypass plugin
        else if (id >= im->INDEX_BYPASS && id < im->INDEX_BYPASS + 1000000)
        {
            int index = id - im->INDEX_BYPASS;

            im->pluginChain->toggleBypass(index);
            PluginWindow::updateAllTitlesAndToolbars(im);
        }
        // Show / close active plugin GUI (toggle)
        else if (id >= im->INDEX_EDIT && id < im->INDEX_EDIT + 1000000)
        {
            int editIndex = id - im->INDEX_EDIT;
            if (editIndex >= 0 && editIndex < im->pluginChain->size())
            {
                auto& slot = (*im->pluginChain)[editIndex];
                if (slot.isFailed())
                {
                    NativeMessageBox::showMessageBoxAsync(MessageBoxIconType::WarningIcon,
                        "Plugin Load Failed",
                        "The plugin \"" + slot.desc.name + "\" failed to load.\n\n" + slot.errorMessage);
                }
                else if (slot.node)
                {
                    if (PluginWindow::isWindowOpenFor(slot.node->nodeID))
                    {
                        PluginWindow::closeCurrentlyOpenWindowsFor(slot.node->nodeID);
                    }
                    else
                    {
                        if (PluginWindow* const w = PluginWindow::getWindowFor(slot.node, PluginWindow::Normal, im))
                            w->forceToFront();
                    }
                }
            }
        }
        // Move plugin up the list
        else if (id >= im->INDEX_MOVE_UP && id < im->INDEX_MOVE_UP + 1000000)
        {
            int index = id - im->INDEX_MOVE_UP;

            im->pluginChain->moveUp(index);
            im->presetManager->markDirty();
            PluginWindow::updateAllTitlesAndToolbars(im);
        }
        // Move plugin down the list
        else if (id >= im->INDEX_MOVE_DOWN && id < im->INDEX_MOVE_DOWN + 1000000)
        {
            int index = id - im->INDEX_MOVE_DOWN;

            im->pluginChain->moveDown(index);
            im->presetManager->markDirty();
            PluginWindow::updateAllTitlesAndToolbars(im);
        }
    }
}

void IconMenu::togglePluginBypass(int timeSortedIndex)
{
    pluginChain->toggleBypass(timeSortedIndex);
    PluginWindow::updateAllTitlesAndToolbars(this);
}

void IconMenu::movePluginUp(int timeSortedIndex)
{
    pluginChain->moveUp(timeSortedIndex);
    presetManager->markDirty();
    PluginWindow::updateAllTitlesAndToolbars(this);
}

void IconMenu::movePluginDown(int timeSortedIndex)
{
    pluginChain->moveDown(timeSortedIndex);
    // See comment in movePluginUp() — same reasoning applies.
    presetManager->markDirty();
    PluginWindow::updateAllTitlesAndToolbars(this);
}

bool IconMenu::isBypassed(int timeSortedIndex)
{
    if (timeSortedIndex < 0 || timeSortedIndex >= pluginChain->size())
        return false;
    return (*pluginChain)[timeSortedIndex].bypassed;
}

void IconMenu::saveCurrentPreset()
{
    if (pluginChain->size() == 0)
        return;

    if (presetManager->getCurrentPresetFile().exists())
    {
        presetManager->savePresetToFile(presetManager->getCurrentPresetFile());
        presetManager->clearDirty();
        PluginWindow::updateAllTitlesAndToolbars(this);
    }
    else
    {
        FileChooser chooser("Save Preset As",
            PresetManager::getDefaultPresetDirectory().getChildFile("Untitled.lhp"),
            "*.lhp");
        if (chooser.browseForFileToSave(true))
        {
            File result = chooser.getResult();
            presetManager->savePresetToFile(result);
            presetManager->setCurrentPresetFile(result);
            presetManager->clearDirty();
            getAppProperties().getUserSettings()->setValue("currentPresetPath",
                result.getFullPathName());
            getAppProperties().saveIfNeeded();
            PluginWindow::updateAllTitlesAndToolbars(this);
        }
    }
}

void IconMenu::triggerAudioDeviceRecovery()
{
    // Debounce: don't retry more than once every 3 seconds, max 5 attempts.
    if (deviceRecentlyRecovered || deviceRecoveryRetryCount >= maxDeviceRecoveryRetries)
        return;

    deviceRecentlyRecovered = true;
    ++deviceRecoveryRetryCount;

    // Suspend audio callbacks
    deviceManager.removeAudioCallback (&player);
    player.setProcessor (nullptr);

    // Save the current device state XML and try to re-initialise with it
    if (auto state = deviceManager.createStateXml())
    {
        deviceManager.closeAudioDevice();
        String error = deviceManager.initialise (256, 256, state.get(), false);

        if (error.isEmpty())
        {
            // Success — reconnect the graph and resume
            player.setProcessor (&graph);
            deviceManager.addAudioCallback (&player);
            deviceRecoveryRetryCount = 0;

            // Persist recovered state to global
            if (auto stableState = deviceManager.createStateXml())
            {
                getAppProperties().getUserSettings()->setValue ("audioDeviceState",
                    stableState.get());
                getAppProperties().getUserSettings()->saveIfNeeded();
            }
        }
        else
        {
            // Still failing — leave the device closed; the UI will show the error
            lastDeviceError = error;
        }
    }
    else
    {
        // No saved state to restore — try opening a default device
        deviceManager.closeAudioDevice();
        String error = deviceManager.initialiseWithDefaultDevices (256, 256);
        if (error.isEmpty())
        {
            player.setProcessor (&graph);
            deviceManager.addAudioCallback (&player);
            deviceRecoveryRetryCount = 0;
        }
    }

    // Release the debounce lock after 3 seconds
    Timer::callAfterDelay (3000, [this]
    {
        deviceRecentlyRecovered = false;
    });
}

void IconMenu::markPresetDirty()
{
    if (presetManager == nullptr)
        return;

    if (!presetManager->isDirty())
    {
        presetManager->markDirty();
        PluginWindow::updateAllTitlesAndToolbars(this);
    }
}


void IconMenu::showAudioSettings()
{
    AudioSettingsComponent audioSettingsComp (deviceManager, player, *pluginChain,
                                              lastDeviceError);

    DialogWindow::LaunchOptions o;
    o.content.setNonOwned (&audioSettingsComp);
    o.dialogTitle                   = "Audio Settings";
    o.componentToCentreAround       = this;
    o.escapeKeyTriggersCloseButton  = true;
    o.useNativeTitleBar             = false;
    o.dialogBackgroundColour        = LookAndFeel::getDefaultLookAndFeel().findColour (DocumentWindow::backgroundColourId);
    o.resizable                     = false;

    o.runModal ();

    // Persist per-type state for ALL device types configured this session
    for (auto& [type, state] : audioSettingsComp.getPerTypeState())
    {
        if (state)
            getAppProperties().getUserSettings()->setValue (
                "audioDeviceState_" + type, state.get());
    }
    // Also persist a snapshot of the current device to the generic key
    if (auto stateAfter = deviceManager.createStateXml())
        getAppProperties().getUserSettings()->setValue ("audioDeviceState",
            stateAfter.get());

    // enableFade is a user preference — always persist
    getAppProperties().getUserSettings()->setValue ("enableFade",
        audioSettingsComp.isFadeEnabled());
    getAppProperties().getUserSettings()->saveIfNeeded();
}

void IconMenu::reloadPlugins()
{
    if (pluginListWindow == nullptr)
        pluginListWindow.reset(new PluginListWindow(*this, formatManager));
    pluginListWindow->toFront(true);
}


