//
//  PluginChain.cpp
//  Light Host
//
//  Unified plugin effect chain management.
//  Replaces the old activePluginList + getTimeSortedList() approach.
//

#include "PluginChain.hpp"
#include "PluginWindow.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <climits>

//==============================================================================
PluginChain::PluginChain (AudioProcessorGraph& graphRef,
                          AudioPluginFormatManager& fmRef,
                          AudioStream& audioStreamRef)
    : graph (graphRef), formatManager (fmRef), audioStream (audioStreamRef)
{
}

PluginChain::~PluginChain()
{
}

//==============================================================================
// Chain operations
//==============================================================================

int PluginChain::add (const PluginDescription& desc)
{
    fadeOut();

    PluginSlot slot;
    slot.desc = desc;
    slot.bypassed = false;

    String errorMessage;
    auto instance = formatManager.createPluginInstance (desc,
        graph.getSampleRate(), graph.getBlockSize(), errorMessage);

    if (instance == nullptr)
    {
        slot.errorMessage = errorMessage;
        slot.node = nullptr;
    }
    else
    {
        instance->setRateAndBufferSizeDetails (graph.getSampleRate(), graph.getBlockSize());

        // Capture default state for persistence
        instance->getStateInformation (slot.state);

        slot.node = graph.addNode (std::move (instance), std::nullopt);
    }

    chain.push_back (std::move (slot));
    connectChain();
    fadeIn();
    return (int) chain.size() - 1;
}

bool PluginChain::remove (int index)
{
    if (index < 0 || index >= (int) chain.size())
        return false;

    fadeOut();

    auto& slot = chain[(size_t) index];

    // Close the plugin's window before removing the node
    if (slot.node != nullptr)
    {
        PluginWindow::closeCurrentlyOpenWindowsFor (slot.node->nodeID);
        graph.removeNode (slot.node->nodeID);
    }

    chain.erase (chain.begin() + index);
    connectChain();
    fadeIn();
    return true;
}

bool PluginChain::moveUp (int index)
{
    if (index <= 0 || index >= (int) chain.size())
        return false;

    fadeOut();
    std::swap (chain[(size_t) index], chain[(size_t) (index - 1)]);
    connectChain();
    fadeIn();
    return true;
}

bool PluginChain::moveDown (int index)
{
    if (index < 0 || index >= (int) chain.size() - 1)
        return false;

    fadeOut();
    std::swap (chain[(size_t) index], chain[(size_t) (index + 1)]);
    connectChain();
    fadeIn();
    return true;
}

void PluginChain::toggleBypass (int index)
{
    if (index < 0 || index >= (int) chain.size())
        return;

    fadeOut();

    auto& slot = chain[(size_t) index];
    slot.bypassed = !slot.bypassed;

    connectChain();
    fadeIn();
}

void PluginChain::clear()
{
    // Step 1: Close all plugin editor windows.  For standard (non-VST3)
    // editors this destroys the ToolbarComponent and its child editor
    // component, which nulls the SafePointer in ToolbarComponent and
    // calls editorBeingDeleted() on the AudioProcessor.  VST3 editors
    // (VST3PluginWindow in JUCE 8) may survive this step because the
    // VST3 plugin's IPlugView retains a COM reference that prevents
    // complete teardown — those are handled in Step 2.
    PluginWindow::closeAllCurrentlyOpenWindows();

    // Step 2: Explicitly delete any editors that survived window
    // closure.  The ToolbarComponent's SafePointer<Component> member
    // auto-nulls when the editor component is destroyed, so the
    // PluginWindow destructor's listener-removal path is safe even
    // when the editor has already been freed.
    for (auto& node : graph.getNodes())
        if (auto* editor = node->getProcessor()->getActiveEditor())
            delete editor;

    chain.clear();
}

//==============================================================================

void PluginChain::fadeOut()
{
    if (audioStream.fadeEnabled)
    {
        audioStream.fadeTo (0.0f, AudioStream::fadeRampMs);
        MessageManager::getInstance()->runDispatchLoopUntil (AudioStream::fadeRampMs + 10);
    }
    else
    {
        audioStream.setGainImmediately (0.0f);
    }

    int totalLatencySamples = getTotalPluginLatencySamples();
    if (totalLatencySamples > 0)
    {
        double sr = graph.getSampleRate();
        if (sr > 0)
        {
            int drainMs = (int) (totalLatencySamples / sr * 1000.0);
            if (audioStream.fadeEnabled)
                drainMs += AudioStream::fadeExtraMs;
            MessageManager::getInstance()->runDispatchLoopUntil (drainMs);
        }
        else if (audioStream.fadeEnabled)
        {
            MessageManager::getInstance()->runDispatchLoopUntil (AudioStream::fadeExtraMs);
        }
    }
}

void PluginChain::fadeIn()
{
    audioStream.setGainImmediately (0.0f);
    if (audioStream.fadeEnabled)
        audioStream.fadeTo (1.0f, AudioStream::fadeRampMs);
    else
        audioStream.setGainImmediately (1.0f);
}

int PluginChain::getTotalPluginLatencySamples() const
{
    int total = 0;
    for (const auto& slot : chain)
        if (slot.node != nullptr && !slot.bypassed && !slot.isFailed())
            total += slot.node->getProcessor()->getLatencySamples();
    return total;
}

//==============================================================================
// Graph management
//==============================================================================

void PluginChain::loadAll()
{
    // Assumes audio is paused and the chain has been cleared
    // (PluginWindow::closeAllCurrentlyOpenWindows was already called
    // in clear()).  Just reset the graph — old nodes are gone.
    graph.clear();

    const AudioProcessorGraph::NodeID INPUT (1000000);
    const AudioProcessorGraph::NodeID OUTPUT (INPUT.uid + 1);
    const int CHANNEL_ONE = 0;
    const int CHANNEL_TWO = 1;

    // Rebuild IO nodes
    graph.addNode (std::make_unique<AudioProcessorGraph::AudioGraphIOProcessor>(
        AudioProcessorGraph::AudioGraphIOProcessor::audioInputNode), INPUT);
    graph.addNode (std::make_unique<AudioProcessorGraph::AudioGraphIOProcessor>(
        AudioProcessorGraph::AudioGraphIOProcessor::audioOutputNode), OUTPUT);

    // Build plugin nodes
    for (int i = 0; i < (int) chain.size(); i++)
    {
        auto& slot = chain[(size_t) i];

        // Skip previously failed plugins
        if (slot.isFailed())
        {
            slot.node = nullptr;
            continue;
        }

        String errorMessage;
        auto instance = formatManager.createPluginInstance (slot.desc,
            graph.getSampleRate(), graph.getBlockSize(), errorMessage);

        if (instance == nullptr)
        {
            slot.errorMessage = errorMessage;
            slot.node = nullptr;
            continue;
        }

        instance->setRateAndBufferSizeDetails (graph.getSampleRate(), graph.getBlockSize());

        // Restore saved state if available
        if (slot.hasSavedState())
        {
            instance->setStateInformation (slot.state.getData(), slot.state.getSize());
        }
        // Note: slot.state intentionally NOT re-captured here.  The state
        // decoded from the preset XML is retained as-is; the next call to
        // savePresetToFile will call getStateInformation on each running
        // processor.  This avoids doubling the memory/ time for plugins
        // with large state (samplers, convolution IRs, etc.).

        slot.node = graph.addNode (std::move (instance),
            AudioProcessorGraph::NodeID (i + 1));
    }

    // Reconnect
    connectChain();
}

//==============================================================================
// Internal connection logic
//==============================================================================

void PluginChain::connectChain()
{
    const AudioProcessorGraph::NodeID INPUT (1000000);
    const AudioProcessorGraph::NodeID OUTPUT (INPUT.uid + 1);
    const int CHANNEL_ONE = 0;
    const int CHANNEL_TWO = 1;

    // Remove all existing connections
    auto connections = graph.getConnections();
    for (auto& c : connections)
        graph.removeConnection (c);

    auto conn = [&](AudioProcessorGraph::NodeID src, int sc,
                     AudioProcessorGraph::NodeID dst, int dc)
    {
        graph.addConnection ({ {src, sc}, {dst, dc} });
    };

    if (chain.empty())
    {
        conn (INPUT, CHANNEL_ONE, OUTPUT, CHANNEL_ONE);
        conn (INPUT, CHANNEL_TWO, OUTPUT, CHANNEL_TWO);
        return;
    }

    AudioProcessorGraph::NodeID lastId;
    bool hasInputConnected = false;

    for (int i = 0; i < (int) chain.size(); i++)
    {
        const auto& slot = chain[(size_t) i];

        // Failed or bypassed plugins are skipped in the audio path
        if (slot.isFailed() || slot.bypassed || slot.node == nullptr)
            continue;

        auto nodeId = slot.node->nodeID;

        if (!hasInputConnected)
        {
            conn (INPUT, CHANNEL_ONE, nodeId, CHANNEL_ONE);
            conn (INPUT, CHANNEL_TWO, nodeId, CHANNEL_TWO);
            hasInputConnected = true;
        }
        else
        {
            conn (lastId, CHANNEL_ONE, nodeId, CHANNEL_ONE);
            conn (lastId, CHANNEL_TWO, nodeId, CHANNEL_TWO);
        }

        lastId = nodeId;
    }

    if (!hasInputConnected)
    {
        // All plugins are bypassed/failed — pass audio straight through
        conn (INPUT, CHANNEL_ONE, OUTPUT, CHANNEL_ONE);
        conn (INPUT, CHANNEL_TWO, OUTPUT, CHANNEL_TWO);
    }
    else if (lastId.uid != 0)
    {
        conn (lastId, CHANNEL_ONE, OUTPUT, CHANNEL_ONE);
        conn (lastId, CHANNEL_TWO, OUTPUT, CHANNEL_TWO);
    }
}

//==============================================================================
// Queries
//==============================================================================

int PluginChain::getSlotIndexForNode (AudioProcessorGraph::NodeID nodeId) const
{
    for (int i = 0; i < (int) chain.size(); i++)
    {
        const auto& slot = chain[(size_t) i];
        if (slot.node != nullptr && slot.node->nodeID == nodeId)
            return i;
    }
    return -1;
}

int PluginChain::getChainPositionForNode (AudioProcessorGraph::NodeID nodeId) const
{
    // The chain position is the same as the slot index
    // (the vector IS the ordered chain)
    return getSlotIndexForNode (nodeId);
}

//==============================================================================
// Persistence
//==============================================================================

void PluginChain::loadFromProperties (ApplicationProperties& props)
{
    auto xml = props.getUserSettings()->getXmlValue ("pluginChain");
    if (xml != nullptr)
    {
        loadFromPresetXml (xml.get());
    }
}

void PluginChain::saveToProperties (ApplicationProperties& props)
{
    auto xml = createPresetXml();
    if (xml != nullptr)
    {
        props.getUserSettings()->setValue ("pluginChain", xml.get());
        props.saveIfNeeded();
    }
}

std::unique_ptr<XmlElement> PluginChain::createPresetXml() const
{
    auto root = std::make_unique<XmlElement> ("pluginchain");

    for (int i = 0; i < (int) chain.size(); i++)
    {
        const auto& slot = chain[(size_t) i];

        auto pluginXml = std::make_unique<XmlElement> ("plugin");
        pluginXml->setAttribute ("bypassed", slot.bypassed);

        if (slot.errorMessage.isNotEmpty())
            pluginXml->setAttribute ("error", slot.errorMessage);

        // Full PluginDescription via its built-in XML serialization
        if (auto descXml = slot.desc.createXml())
            pluginXml->addChildElement (descXml.release());

        if (slot.hasSavedState())
        {
            auto stateXml = std::make_unique<XmlElement> ("state");
            stateXml->addTextElement (slot.state.toBase64Encoding());
            pluginXml->addChildElement (stateXml.release());
        }

        root->addChildElement (pluginXml.release());
    }

    return root;
}

void PluginChain::loadFromPresetXml (const XmlElement* xml)
{
    jassert (xml != nullptr && xml->hasTagName ("pluginchain"));
    if (xml == nullptr || !xml->hasTagName ("pluginchain"))
        return;

    clear();

    for (auto* pluginXml = xml->getFirstChildElement();
         pluginXml != nullptr;
         pluginXml = pluginXml->getNextElement())
    {
        if (!pluginXml->hasTagName ("plugin"))
            continue;

        PluginSlot slot;
        slot.bypassed     = pluginXml->getBoolAttribute ("bypassed", false);
        slot.errorMessage = pluginXml->getStringAttribute ("error", "");

        // Restore PluginDescription from child description element
        if (auto* descXml = pluginXml->getFirstChildElement())
        {
            if (descXml->hasTagName ("PLUGIN"))
                slot.desc.loadFromXml (*descXml);
        }

        // Fallback: try old format attributes (name/format/version)
        if (slot.desc.name.isEmpty() && pluginXml->hasAttribute ("name"))
        {
            slot.desc.name             = pluginXml->getStringAttribute ("name");
            slot.desc.pluginFormatName = pluginXml->getStringAttribute ("format");
            slot.desc.version          = pluginXml->getStringAttribute ("version");
            slot.desc.fileOrIdentifier = pluginXml->getStringAttribute ("file", slot.desc.fileOrIdentifier);
            slot.desc.uniqueId         = (int) pluginXml->getIntAttribute ("uid", slot.desc.uniqueId);
        }

        // Restore state
        if (auto* stateXml = pluginXml->getChildByName ("state"))
        {
            String stateBase64 = stateXml->getAllSubText().trim();
            if (stateBase64.isNotEmpty())
                slot.state.fromBase64Encoding (stateBase64);
        }

        chain.push_back (std::move (slot));
    }
}
