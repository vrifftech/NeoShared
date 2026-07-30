#pragma once

#include <wx/treectrl.h>

#include <string>
#include <utility>
#include <vector>

namespace neotree {

// View-only state for a wxTreeCtrl that is rebuilt when the active document
// changes. Keys are supplied by the application and must remain stable for as
// long as the represented document structure remains unchanged.
struct TreeViewState {
    std::vector<std::string> expandedKeys;
    std::string selectedKey;
    std::string firstVisibleKey;
    bool initialized = false;

    void reset() {
        expandedKeys.clear();
        selectedKey.clear();
        firstVisibleKey.clear();
        initialized = false;
    }
};

namespace detail {

template <typename KeyForItem>
void captureExpandedItems(wxTreeCtrl& tree,
                          const wxTreeItemId& item,
                          KeyForItem&& keyForItem,
                          std::vector<std::string>& expandedKeys) {
    if (!item.IsOk()) return;

    const std::string key = keyForItem(item);
    if (!key.empty() && tree.IsExpanded(item)) expandedKeys.push_back(key);

    wxTreeItemIdValue cookie;
    wxTreeItemId child = tree.GetFirstChild(item, cookie);
    while (child.IsOk()) {
        captureExpandedItems(tree, child, keyForItem, expandedKeys);
        child = tree.GetNextChild(item, cookie);
    }
}

} // namespace detail

template <typename KeyForItem>
void captureTreeViewState(wxTreeCtrl& tree,
                          TreeViewState& state,
                          KeyForItem&& keyForItem) {
    state.expandedKeys.clear();
    detail::captureExpandedItems(
        tree,
        tree.GetRootItem(),
        keyForItem,
        state.expandedKeys);

    state.selectedKey.clear();
    const wxTreeItemId selection = tree.GetSelection();
    if (selection.IsOk()) state.selectedKey = keyForItem(selection);

    state.firstVisibleKey.clear();
    const wxTreeItemId firstVisible = tree.GetFirstVisibleItem();
    if (firstVisible.IsOk()) state.firstVisibleKey = keyForItem(firstVisible);

    state.initialized = true;
}

struct TreeRestoreResult {
    bool selectionRestored = false;
    bool firstVisibleRestored = false;
};

template <typename ResolveItem>
TreeRestoreResult restoreTreeViewState(wxTreeCtrl& tree,
                                       const TreeViewState& state,
                                       ResolveItem&& resolveItem) {
    TreeRestoreResult result;
    if (!state.initialized) return result;

    // captureTreeViewState() records expanded items in parent-before-child
    // order. This is important for lazily materialized trees.
    for (const std::string& key : state.expandedKeys) {
        const wxTreeItemId item = resolveItem(key);
        if (item.IsOk()) tree.Expand(item);
    }

    if (!state.selectedKey.empty()) {
        const wxTreeItemId item = resolveItem(state.selectedKey);
        if (item.IsOk()) {
            tree.SelectItem(item);
            result.selectionRestored = true;
        }
    }

    if (!state.firstVisibleKey.empty()) {
        const wxTreeItemId item = resolveItem(state.firstVisibleKey);
        if (item.IsOk()) {
            tree.ScrollTo(item);
            result.firstVisibleRestored = true;
        }
    }

    return result;
}

} // namespace neotree
