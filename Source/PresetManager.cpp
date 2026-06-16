//
//  PresetManager.cpp
//  Light Host
//
//  Preset file I/O implementation.
//

#include "PresetManager.hpp"
#include "PluginChain.hpp"
#include "PluginWindow.h"
#include <vector>

//==============================================================================
PresetManager::PresetManager (PluginChain& chain,
                              ApplicationProperties& props)
    : pluginChain (chain), appProperties (props)
{
}

//==============================================================================
// File I/O
//==============================================================================

void PresetManager::savePresetToFile (File file)
{
    // Capture current state from running processors
    for (int i = 0; i < pluginChain.size(); i++)
    {
        auto& slot = pluginChain[i];
        if (slot.node != nullptr)
        {
            MemoryBlock stateBinary;
            slot.node->getProcessor()->getStateInformation (stateBinary);
            slot.state = stateBinary;
        }
    }

    // Get chain XML with plugin descriptions and state
    auto chainXml = pluginChain.createPresetXml();
    if (chainXml == nullptr)
        return;

    // Append window geometry to each plugin element.
    if (chainXml->hasTagName ("pluginchain"))
    {
        for (int i = 0; i < pluginChain.size() && i < chainXml->getNumChildElements(); i++)
        {
            auto& slot = pluginChain[i];
            auto* pluginEl = chainXml->getChildElement (i);
            if (pluginEl != nullptr)
            {
                pluginEl->setAttribute ("winX", (int) appProperties.getUserSettings()->getIntValue (getPluginKey ("winX", slot.desc), -1));
                pluginEl->setAttribute ("winY", (int) appProperties.getUserSettings()->getIntValue (getPluginKey ("winY", slot.desc), -1));
                pluginEl->setAttribute ("winW", (int) appProperties.getUserSettings()->getIntValue (getPluginKey ("winW", slot.desc), -1));
                pluginEl->setAttribute ("winH", (int) appProperties.getUserSettings()->getIntValue (getPluginKey ("winH", slot.desc), -1));
            }
        }
    }

    auto presetXml = std::make_unique<XmlElement> ("lighthostpreset");
    presetXml->addChildElement (chainXml.release());

    FileOutputStream outStream (file);
    if (outStream.openedOk())
    {
        outStream.setPosition (0);
        outStream.truncate();
        presetXml->writeTo (outStream);
    }

    dirty = false;
}

void PresetManager::loadPresetFromFile (File file,
                                        std::function<void()> suspendAudio,
                                        std::function<void()> resumeAudio)
{
    auto presetXml = XmlDocument::parse (file);
    if (presetXml == nullptr || !presetXml->hasTagName ("lighthostpreset"))
        return;

    // Fade out and suspend audio
    suspendAudio();

    // Load chain from preset XML — this calls clear() which closes all
    // plugin windows and destroys the old chain.
    if (auto* chainXml = presetXml->getChildByName ("pluginchain"))
    {
        pluginChain.loadFromPresetXml (chainXml);

        // Extract window geometry NOW, while the XML DOM is still alive.
        // Store in a lightweight vector so the XML can be freed before
        // loadAll() runs — this halves peak memory for presets with
        // large base64-encoded plugin state.
        struct WinGeo { int x, y, w, h; };
        std::vector<WinGeo> winGeos;
        for (auto* pluginEl = chainXml->getFirstChildElement ();
             pluginEl != nullptr;
             pluginEl = pluginEl->getNextElement ())
        {
            if (! pluginEl->hasTagName ("plugin"))  { winGeos.push_back ({ -1, -1, -1, -1 }); continue; }
            int wx = pluginEl->getIntAttribute ("winX", -1);
            int wy = pluginEl->getIntAttribute ("winY", -1);
            int ww = pluginEl->getIntAttribute ("winW", -1);
            int wh = pluginEl->getIntAttribute ("winH", -1);
            winGeos.push_back ({ wx, wy, ww, wh });
        }

        // Free the parsed XML tree before loadAll() — the base64 state
        // data in the DOM can be very large; keeping it alive during
        // plugin instantiation doubles peak memory.
        presetXml.reset();

        // Rebuild graph (instantiates plugins, restores their state)
        pluginChain.loadAll();

        // Apply window geometry to the newly-created plugin nodes
        for (int j = 0; j < (int) winGeos.size() && j < pluginChain.size(); ++j)
        {
            auto& g = winGeos[(size_t) j];
            if (g.x < 0 || g.y < 0 || g.w <= 0 || g.h <= 0)
                continue;

            auto& slot = pluginChain[j];
            if (slot.node != nullptr)
            {
                slot.node->properties.set (getLastXProp (PluginWindow::Normal), g.x);
                slot.node->properties.set (getLastYProp (PluginWindow::Normal), g.y);
                slot.node->properties.set (getLastWProp (PluginWindow::Normal), g.w);
                slot.node->properties.set (getLastHProp (PluginWindow::Normal), g.h);
            }
        }
    }
    else
    {
        // No chain XML — just rebuild an empty graph
        pluginChain.loadAll();
    }

    // Resume audio
    resumeAudio();

    currentPresetFile = file;
    appProperties.getUserSettings()->setValue ("currentPresetPath",
        file.getFullPathName());
    appProperties.saveIfNeeded();

    dirty = false;
}

//==============================================================================
// Default directory
//==============================================================================

File PresetManager::getDefaultPresetDirectory()
{
    File presetDir = File::getSpecialLocation (File::userApplicationDataDirectory)
        .getChildFile ("Light Host")
        .getChildFile ("Presets");
    if (!presetDir.exists())
        presetDir.createDirectory();
    return presetDir;
}

//==============================================================================
// New preset
//==============================================================================

void PresetManager::newPreset (std::function<void()> suspendAudio,
                               std::function<void()> resumeAudio)
{
    suspendAudio();
    pluginChain.clear();

    pluginChain.loadAll();
    resumeAudio();

    currentPresetFile = File();
    appProperties.getUserSettings()->removeValue ("currentPresetPath");
    appProperties.saveIfNeeded();

    dirty = false;
}
