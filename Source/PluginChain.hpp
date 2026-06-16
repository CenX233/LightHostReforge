//
//  PluginChain.hpp
//  Light Host
//
//  Unified plugin effect chain management.
//  Replaces the old activePluginList + pluginNodeIDs + getTimeSortedList() approach
//  with a single std::vector<PluginSlot> as the source of truth.
//
//  Vector index = chain position = menu display position.
//
//  Failed plugins are kept in the chain with node == nullptr and
//  errorMessage set, displayed as 🔴 in the menu.
//

#ifndef PluginChain_hpp
#define PluginChain_hpp

#include <juce_audio_processors/juce_audio_processors.h>
#include "AudioStream.hpp"
#include <vector>

using namespace juce;

//==============================================================================
/**
    A single slot in the plugin effect chain.

    - `desc`:  PluginDescription identifying the plugin (name, version, format).
    - `node`:  Graph node pointer. nullptr if the plugin failed to load.
    - `errorMessage`:  Non-empty if plugin failed to load (the error string).
    - `bypassed`:  Current bypass state (persisted across restarts).
    - `state`:  Cached plugin parameter state from getStateInformation().
*/
struct PluginSlot
{
    PluginDescription desc;
    AudioProcessorGraph::Node::Ptr node;
    String errorMessage;
    bool bypassed = false;
    MemoryBlock state;

    bool isFailed() const noexcept          { return node == nullptr && errorMessage.isNotEmpty(); }
    bool hasSavedState() const noexcept     { return state.getSize() > 0; }
};

//==============================================================================
/**
    Manages the plugin effect chain as a single ordered list of PluginSlots.

    Owns no audio resources itself; receives references to the graph,
    format manager, etc.  The caller (IconMenu) is responsible for
    pausing/resuming the audio callback around chain operations.
*/
class PluginChain
{
public:
    PluginChain (AudioProcessorGraph& graphRef,
                 AudioPluginFormatManager& fmRef,
                 AudioStream& audioStreamRef);

    ~PluginChain();

    //==============================================================================
    /// @name Chain operations
    //@{

    /** Add a plugin to the end of the chain.
        Creates the AudioProcessor and graph node, or records failure.
        @return the index of the new slot.
    */
    int add (const PluginDescription& desc);

    /** Remove a plugin from the chain by index.
        Closes its window (if open) and removes its graph node.
    */
    bool remove (int index);

    /** Directly append a pre-built PluginSlot (for migration from old format).
        Does NOT create a graph node or rebuild connections.
    */
    void addSlot (PluginSlot&& slot)   { chain.push_back (std::move (slot)); }

    /** Move a plugin up one position in the chain. */
    bool moveUp (int index);

    /** Move a plugin down one position in the chain. */
    bool moveDown (int index);

    /** Toggle bypass state for the plugin at \a index.
        If the plugin has no graph node (failed), only the recorded state changes.
    */
    void toggleBypass (int index);

    /** Clear the chain (all slots removed). */
    void clear();

    //@}
    //==============================================================================
    /// @name Access
    //@{

    int size() const noexcept                       { return (int) chain.size(); }

    PluginSlot&       operator[] (int index)        { return chain[(size_t) index]; }
    const PluginSlot& operator[] (int index) const  { return chain[(size_t) index]; }

    //@}
    //==============================================================================
    /// @name Graph management
    //@{

    /** Full graph rebuild from the current chain.
        Destroys all existing nodes and creates fresh ones.
        The caller must pause the audio callback before calling this.
    */
    void loadAll();

    //@}
    //==============================================================================
    /// @name Queries
    //@{

    /** Find the chain slot index for a given graph NodeID.
        Returns -1 if not found.
    */
    int getSlotIndexForNode (AudioProcessorGraph::NodeID nodeId) const;

    /** Find the time-sorted chain position (0-based) for a graph NodeID.
        This is the position displayed in window titles and menus.
    */
    int getChainPositionForNode (AudioProcessorGraph::NodeID nodeId) const;

    //@}
    //==============================================================================
    /// @name Persistence
    //@{

    /** Load the chain from app properties (key "pluginChain").
        If the key does not exist, the chain remains empty.
    */
    void loadFromProperties (ApplicationProperties& props);

    /** Save the chain to app properties (key "pluginChain"). */
    void saveToProperties (ApplicationProperties& props);

    /** Create an XML element representing the current chain for presets.
        The returned element has tag "pluginchain".
    */
    std::unique_ptr<XmlElement> createPresetXml() const;

    /** Replace the current chain from a preset XML element
        (tag "pluginchain" as created by createPresetXml).
    */
    void loadFromPresetXml (const XmlElement* xml);

    void fadeOut();
    void fadeIn();

    /** Returns the sum of latency samples from all non-bypassed, non-failed
        plugin processors in the chain. */
    int getTotalPluginLatencySamples() const;

    //@}

private:
    //==============================================================================
    void connectChain();

    std::vector<PluginSlot> chain;

    AudioProcessorGraph&        graph;
    AudioPluginFormatManager&   formatManager;
    AudioStream&                audioStream;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginChain)
};

#endif // PluginChain_hpp
