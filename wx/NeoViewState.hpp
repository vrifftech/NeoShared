#pragma once

#include "NeoDocumentTabs.hpp"
#include "NeoSettings.hpp"

#include <wx/aui/auibook.h>
#include <wx/event.h>
#include <wx/eventfilter.h>
#include <wx/grid.h>
#include <wx/listctrl.h>
#include <wx/settings.h>
#include <wx/statusbr.h>
#include <wx/treectrl.h>
#include <wx/window.h>
#include <wx/wupdlock.h>

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace neoview {

constexpr double kDefaultFontScale = 1.0;
constexpr double kMinFontScale = 0.75;
constexpr double kMaxFontScale = 2.0;
constexpr int kFontScaleStepPercent = 10;
constexpr double kFontScaleStep = static_cast<double>(kFontScaleStepPercent) / 100.0;

inline double clampFontScale(double scale) {
    if (!std::isfinite(scale)) return kDefaultFontScale;
    return std::clamp(scale, kMinFontScale, kMaxFontScale);
}

inline int fontScalePercent(double scale) {
    return static_cast<int>(std::lround(clampFontScale(scale) * 100.0));
}

inline double steppedFontScale(double scale, int steps) {
    if (steps == 0) return clampFontScale(scale);
    const int minimum = static_cast<int>(std::lround(kMinFontScale * 100.0));
    const int maximum = static_cast<int>(std::lround(kMaxFontScale * 100.0));
    const int next = std::clamp(fontScalePercent(scale) + steps * kFontScaleStepPercent,
                                minimum, maximum);
    return static_cast<double>(next) / 100.0;
}

// Mouse events are not command events and do not reliably bubble from grids,
// lists, trees, and text controls to their parent frame. This process-wide
// filter scopes itself to one top-level frame so Ctrl+wheel zoom behaves the
// same regardless of which child control owns the mouse.
class FontScaleWheelFilter final : public wxEventFilter {
public:
    FontScaleWheelFilter() = default;
    FontScaleWheelFilter(const FontScaleWheelFilter&) = delete;
    FontScaleWheelFilter& operator=(const FontScaleWheelFilter&) = delete;

    ~FontScaleWheelFilter() override { detach(); }

    void attach(wxWindow* root, std::function<void(int)> callback) {
        detach();
        root_ = root;
        callback_ = std::move(callback);
        reset();
        if (root_ != nullptr && callback_) {
            wxEvtHandler::AddFilter(this);
            attached_ = true;
        }
    }

    void detach() {
        if (attached_) wxEvtHandler::RemoveFilter(this);
        attached_ = false;
        root_ = nullptr;
        callback_ = {};
        reset();
    }

    void reset() noexcept {
        wheelRemainder_ = 0;
        wheelDirection_ = 0;
    }

    int FilterEvent(wxEvent& rawEvent) override {
        if (!attached_ || root_ == nullptr || root_->IsBeingDeleted() ||
            rawEvent.GetEventType() != wxEVT_MOUSEWHEEL) {
            return Event_Skip;
        }

        auto* source = wxDynamicCast(rawEvent.GetEventObject(), wxWindow);
        if (source == nullptr || source->IsBeingDeleted() ||
            wxGetTopLevelParent(source) != root_) {
            return Event_Skip;
        }

        auto& event = static_cast<wxMouseEvent&>(rawEvent);
        if (!event.ControlDown() || event.GetWheelAxis() != wxMOUSE_WHEEL_VERTICAL) {
            reset();
            return Event_Skip;
        }

        const int rotation = event.GetWheelRotation();
        if (rotation == 0) return Event_Skip;

        const int direction = rotation > 0 ? 1 : -1;
        if (wheelDirection_ != 0 && wheelDirection_ != direction) {
            wheelRemainder_ = 0;
        }
        wheelDirection_ = direction;

        const int wheelDelta = event.GetWheelDelta() > 0 ? event.GetWheelDelta() : 120;
        wheelRemainder_ += rotation;

        const int steps = wheelRemainder_ / wheelDelta;
        if (steps == 0) {
            // This is still a zoom gesture. Consume the partial high-resolution
            // wheel delta so the focused grid/tree/list does not scroll.
            return Event_Processed;
        }

        wheelRemainder_ -= steps * wheelDelta;
        callback_(steps);
        return Event_Processed;
    }

private:
    wxWindow* root_ = nullptr;
    std::function<void(int)> callback_;
    int wheelRemainder_ = 0;
    int wheelDirection_ = 0;
    bool attached_ = false;
};


enum class TextFilterMode {
    Contains,
    DoesNotContain,
    Equals,
    NotEquals,
    StartsWith,
    EndsWith,
    Blank,
    NotBlank
};

struct ColumnFilter {
    std::size_t logicalColumn = 0;
    std::string columnLabel;
    std::string term;
    TextFilterMode mode = TextFilterMode::Contains;
    bool enabled = true;
};

inline std::string lowercaseAscii(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return text;
}

inline std::string trimmedCopy(std::string text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

inline bool containsInsensitive(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return true;
    return lowercaseAscii(haystack).find(lowercaseAscii(needle)) != std::string::npos;
}

inline bool startsWithInsensitive(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return true;
    const auto h = lowercaseAscii(haystack);
    const auto n = lowercaseAscii(needle);
    return h.size() >= n.size() && h.compare(0, n.size(), n) == 0;
}

inline bool endsWithInsensitive(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return true;
    const auto h = lowercaseAscii(haystack);
    const auto n = lowercaseAscii(needle);
    return h.size() >= n.size() && h.compare(h.size() - n.size(), n.size(), n) == 0;
}

inline bool equalsInsensitive(const std::string& lhs, const std::string& rhs) {
    return lowercaseAscii(lhs) == lowercaseAscii(rhs);
}

inline bool matchesColumnFilterText(const std::string& value, const ColumnFilter& filter) {
    if (!filter.enabled) return true;
    const std::string term = filter.term;
    switch (filter.mode) {
    case TextFilterMode::Contains: return containsInsensitive(value, term);
    case TextFilterMode::DoesNotContain: return !containsInsensitive(value, term);
    case TextFilterMode::Equals: return equalsInsensitive(value, term);
    case TextFilterMode::NotEquals: return !equalsInsensitive(value, term);
    case TextFilterMode::StartsWith: return startsWithInsensitive(value, term);
    case TextFilterMode::EndsWith: return endsWithInsensitive(value, term);
    case TextFilterMode::Blank: return trimmedCopy(value).empty();
    case TextFilterMode::NotBlank: return !trimmedCopy(value).empty();
    }
    return true;
}

inline const char* filterModeLabel(TextFilterMode mode) {
    switch (mode) {
    case TextFilterMode::Contains: return "contains";
    case TextFilterMode::DoesNotContain: return "does not contain";
    case TextFilterMode::Equals: return "equals";
    case TextFilterMode::NotEquals: return "does not equal";
    case TextFilterMode::StartsWith: return "starts with";
    case TextFilterMode::EndsWith: return "ends with";
    case TextFilterMode::Blank: return "is blank";
    case TextFilterMode::NotBlank: return "is not blank";
    }
    return "matches";
}

struct DocumentViewState {
    // GUI-only text filter/search state. It must never be used as file data.
    // This object is intentionally tab-local; future tabs should own one instance per document tab.
    std::string filterTerm;
    std::vector<ColumnFilter> columnFilters;

    // Visual-to-logical mappings. Editing code should translate through these before touching the model.
    std::vector<std::size_t> visualToLogicalRows;
    std::vector<std::size_t> visualToLogicalColumns;

    // Per-document cursor/selection state. Apps can use logical or visual fields according to control type.
    int selectedVisualRow = 0;
    int selectedVisualColumn = 0;
    int selectedLogicalRow = 0;
    int selectedLogicalColumn = 0;
    std::optional<std::size_t> primarySelection;
    std::optional<std::size_t> secondarySelection;
    std::string selectedPath;

    // Per-document sort/view preferences. Changing these is view state only and must not dirty the document.
    int sortColumn = 0;
    bool sortAscending = true;
    bool searchResultsActive = false;
    std::string preferredViewMode;

    void resetForNewDocument() {
        filterTerm.clear();
        columnFilters.clear();
        visualToLogicalRows.clear();
        visualToLogicalColumns.clear();
        selectedVisualRow = 0;
        selectedVisualColumn = 0;
        selectedLogicalRow = 0;
        selectedLogicalColumn = 0;
        primarySelection.reset();
        secondarySelection.reset();
        selectedPath.clear();
        sortColumn = 0;
        sortAscending = true;
        searchResultsActive = false;
        preferredViewMode.clear();
    }
};

inline bool isValidPermutation(const std::vector<std::size_t>& mapping, std::size_t count) {
    if (mapping.size() != count) return false;
    std::vector<bool> seen(count, false);
    for (const auto logical : mapping) {
        if (logical >= count || seen[logical]) return false;
        seen[logical] = true;
    }
    return true;
}

inline void setIdentityRows(DocumentViewState& state, std::size_t rowCount) {
    state.visualToLogicalRows.clear();
    state.visualToLogicalRows.reserve(rowCount);
    for (std::size_t row = 0; row < rowCount; ++row) state.visualToLogicalRows.push_back(row);
}

inline void setIdentityColumns(DocumentViewState& state, std::size_t columnCount) {
    state.visualToLogicalColumns.clear();
    state.visualToLogicalColumns.reserve(columnCount);
    for (std::size_t col = 0; col < columnCount; ++col) state.visualToLogicalColumns.push_back(col);
}

inline void setRowsFromLogicalRows(DocumentViewState& state, std::vector<std::size_t> logicalRows) {
    state.visualToLogicalRows = std::move(logicalRows);
}

inline void ensureRowsInRange(DocumentViewState& state, std::size_t rowCount) {
    state.visualToLogicalRows.erase(std::remove_if(state.visualToLogicalRows.begin(), state.visualToLogicalRows.end(),
                                                   [rowCount](std::size_t logical) { return logical >= rowCount; }),
                                    state.visualToLogicalRows.end());
}

inline void ensureIdentityRows(DocumentViewState& state, std::size_t rowCount) {
    if (!isValidPermutation(state.visualToLogicalRows, rowCount)) setIdentityRows(state, rowCount);
}

inline void ensureCompleteColumnMapping(DocumentViewState& state, std::size_t columnCount) {
    std::vector<std::size_t> next;
    next.reserve(columnCount);
    std::vector<bool> seen(columnCount, false);
    for (const auto logical : state.visualToLogicalColumns) {
        if (logical < columnCount && !seen[logical]) {
            next.push_back(logical);
            seen[logical] = true;
        }
    }
    for (std::size_t logical = 0; logical < columnCount; ++logical) {
        if (!seen[logical]) next.push_back(logical);
    }
    state.visualToLogicalColumns = std::move(next);
}

inline void ensureIdentityColumns(DocumentViewState& state, std::size_t columnCount) {
    ensureCompleteColumnMapping(state, columnCount);
}

inline void removeColumnFiltersOutsideRange(DocumentViewState& state, std::size_t columnCount) {
    state.columnFilters.erase(std::remove_if(state.columnFilters.begin(), state.columnFilters.end(),
                                             [columnCount](const ColumnFilter& filter) { return filter.logicalColumn >= columnCount; }),
                              state.columnFilters.end());
}

inline std::size_t logicalRowForVisual(const DocumentViewState& state, int visualRow) {
    if (visualRow < 0 || static_cast<std::size_t>(visualRow) >= state.visualToLogicalRows.size()) {
        throw std::out_of_range("visual row is outside the current view");
    }
    return state.visualToLogicalRows[static_cast<std::size_t>(visualRow)];
}

inline std::size_t logicalColumnForVisual(const DocumentViewState& state, std::size_t visualColumn) {
    if (visualColumn >= state.visualToLogicalColumns.size()) {
        throw std::out_of_range("visual column is outside the current view");
    }
    return state.visualToLogicalColumns[visualColumn];
}

inline std::size_t logicalColumnForVisual(const DocumentViewState& state, int visualColumn) {
    if (visualColumn < 0) {
        throw std::out_of_range("visual column is outside the current view");
    }
    return logicalColumnForVisual(state, static_cast<std::size_t>(visualColumn));
}

inline int visualRowForLogical(const DocumentViewState& state, std::size_t logicalRow) {
    for (std::size_t i = 0; i < state.visualToLogicalRows.size(); ++i) {
        if (state.visualToLogicalRows[i] == logicalRow) return static_cast<int>(i);
    }
    return -1;
}

inline int visualColumnForLogical(const DocumentViewState& state, std::size_t logicalColumn) {
    for (std::size_t i = 0; i < state.visualToLogicalColumns.size(); ++i) {
        if (state.visualToLogicalColumns[i] == logicalColumn) return static_cast<int>(i);
    }
    return -1;
}

inline bool moveVisualColumn(DocumentViewState& state, int fromVisual, int toVisual) {
    if (fromVisual < 0 || toVisual < 0) return false;
    auto from = static_cast<std::size_t>(fromVisual);
    auto to = static_cast<std::size_t>(toVisual);
    if (from >= state.visualToLogicalColumns.size() || to >= state.visualToLogicalColumns.size() || from == to) return false;
    const auto logical = state.visualToLogicalColumns[from];
    state.visualToLogicalColumns.erase(state.visualToLogicalColumns.begin() + static_cast<std::ptrdiff_t>(from));
    state.visualToLogicalColumns.insert(state.visualToLogicalColumns.begin() + static_cast<std::ptrdiff_t>(to), logical);
    return true;
}

inline const ColumnFilter* findColumnFilter(const DocumentViewState& state, std::size_t logicalColumn) {
    for (const auto& filter : state.columnFilters) {
        if (filter.logicalColumn == logicalColumn && filter.enabled) return &filter;
    }
    return nullptr;
}

inline ColumnFilter* findColumnFilter(DocumentViewState& state, std::size_t logicalColumn) {
    for (auto& filter : state.columnFilters) {
        if (filter.logicalColumn == logicalColumn) return &filter;
    }
    return nullptr;
}

inline bool hasActiveColumnFilters(const DocumentViewState& state) {
    for (const auto& filter : state.columnFilters) {
        if (filter.enabled) return true;
    }
    return false;
}

inline bool hasAnyFilter(const DocumentViewState& state) {
    return !state.filterTerm.empty() || hasActiveColumnFilters(state);
}

inline void clearColumnFilter(DocumentViewState& state, std::size_t logicalColumn) {
    state.columnFilters.erase(std::remove_if(state.columnFilters.begin(), state.columnFilters.end(),
                                             [logicalColumn](const ColumnFilter& filter) { return filter.logicalColumn == logicalColumn; }),
                              state.columnFilters.end());
}

inline void clearColumnFilters(DocumentViewState& state) {
    state.columnFilters.clear();
}

inline void clearAllFilters(DocumentViewState& state) {
    state.filterTerm.clear();
    state.columnFilters.clear();
}

inline void setColumnFilter(DocumentViewState& state, ColumnFilter filter) {
    filter.term = trimmedCopy(filter.term);
    clearColumnFilter(state, filter.logicalColumn);
    if ((filter.mode == TextFilterMode::Blank || filter.mode == TextFilterMode::NotBlank || !filter.term.empty()) && filter.enabled) {
        state.columnFilters.push_back(std::move(filter));
    }
}

inline bool rowPassesColumnFilters(const DocumentViewState& state,
                                   const std::function<std::string(std::size_t logicalColumn)>& cellAt) {
    for (const auto& filter : state.columnFilters) {
        if (!filter.enabled) continue;
        if (!matchesColumnFilterText(cellAt(filter.logicalColumn), filter)) return false;
    }
    return true;
}

inline std::string columnFilterSummary(const DocumentViewState& state) {
    std::string summary;
    for (const auto& filter : state.columnFilters) {
        if (!filter.enabled) continue;
        if (!summary.empty()) summary += "; ";
        const std::string label = filter.columnLabel.empty() ? ("Column " + std::to_string(filter.logicalColumn)) : filter.columnLabel;
        summary += label;
        summary += " ";
        summary += filterModeLabel(filter.mode);
        if (filter.mode != TextFilterMode::Blank && filter.mode != TextFilterMode::NotBlank) {
            summary += " \"" + filter.term + "\"";
        }
    }
    return summary;
}

// Reapply pixel-based metrics (grid row heights, status-bar minimum height,
// and the fixed document tab strip) after the platform has completed a DPI
// transition. Point-size fonts scale automatically, but these explicit DIP
// measurements must be recalculated for the new monitor.
inline void bindFontScaleDpiRefresh(wxWindow* window, std::function<void()> refresh) {
#if wxCHECK_VERSION(3, 1, 3)
    if (window == nullptr || !refresh) return;
    window->Bind(wxEVT_DPI_CHANGED,
                 [window, refresh = std::move(refresh)](wxDPIChangedEvent& event) {
                     event.Skip();
                     window->CallAfter([window, refresh]() {
                         if (!window->IsBeingDeleted()) refresh();
                     });
                 });
#else
    (void)window;
    (void)refresh;
#endif
}

inline wxFont scaledFontLike(const wxFont& currentFont, double scale) {
    const wxFont base = wxSystemSettings::GetFont(wxSYS_DEFAULT_GUI_FONT);
    int basePointSize = base.IsOk() ? base.GetPointSize() : 10;
    if (basePointSize <= 0 && currentFont.IsOk()) basePointSize = currentFont.GetPointSize();
    if (basePointSize <= 0) basePointSize = 10;

    wxFont result = currentFont.IsOk() ? currentFont : base;
    result.SetPointSize(std::max(6, static_cast<int>(std::lround(static_cast<double>(basePointSize) * clampFontScale(scale)))));
    return result;
}

inline void applyGridFontScale(wxGrid& grid, double scale) {
    const wxFont font = scaledFontLike(grid.GetFont(), scale);
    grid.SetFont(font);
    grid.SetDefaultCellFont(font);
    grid.SetLabelFont(font);

    const int point = std::max(6, font.GetPointSize());
    const int rowHeight = std::max(grid.FromDIP(18), grid.FromDIP(point + 10));
    const int labelHeight = std::max(grid.FromDIP(22), grid.FromDIP(point + 14));
    grid.SetDefaultRowSize(rowHeight, true);
    grid.SetColLabelSize(labelHeight);
    grid.Refresh(false);
}

inline void applyListFontScale(wxListCtrl& list, double scale) {
    list.SetFont(scaledFontLike(list.GetFont(), scale));
    list.Refresh(false);
}

inline void applyTreeFontScale(wxTreeCtrl& tree, double scale) {
    tree.SetFont(scaledFontLike(tree.GetFont(), scale));
    tree.Refresh(false);
}

inline void applyStatusBarFontScale(wxStatusBar& status, double scale) {
    const wxFont font = scaledFontLike(status.GetFont(), scale);
    status.SetFont(font);
    const int minimumHeight = std::max(status.FromDIP(20),
                                       status.GetCharHeight() + status.FromDIP(8));
    status.SetMinHeight(minimumHeight);
    status.SetMinSize(wxSize(-1, minimumHeight));
    status.Refresh(false);
}

inline void applyWindowFontScale(wxWindow& window, double scale) {
    window.SetFont(scaledFontLike(window.GetFont(), scale));
}

inline void applyDocumentTabStripFontScale(wxAuiNotebook& notebook, double scale) {
    notebook.SetFont(scaledFontLike(notebook.GetFont(), scale));
    neotabs::updateDocumentTabStripLayout(&notebook);
}

inline void applyFontScaleRecursive(wxWindow* window, double scale) {
    if (window == nullptr) return;

    // Document notebooks are deliberately only tab strips. Recursing into
    // their empty placeholder pages makes wxAuiNotebook recalculate a page
    // best size and can abruptly expand the notebook on Windows while zooming.
    if (auto* notebook = wxDynamicCast(window, wxAuiNotebook)) {
        if (neotabs::isDocumentTabStrip(notebook)) {
            applyDocumentTabStripFontScale(*notebook, scale);
            return;
        }
    }

    // Composite native controls own implementation children that should not
    // be recursively restyled. Scale their public surface and stop there.
    if (auto* grid = wxDynamicCast(window, wxGrid)) {
        applyGridFontScale(*grid, scale);
        return;
    }
    if (auto* list = wxDynamicCast(window, wxListCtrl)) {
        applyListFontScale(*list, scale);
        return;
    }
    if (auto* tree = wxDynamicCast(window, wxTreeCtrl)) {
        applyTreeFontScale(*tree, scale);
        return;
    }
    if (auto* status = wxDynamicCast(window, wxStatusBar)) {
        applyStatusBarFontScale(*status, scale);
        return;
    }

    applyWindowFontScale(*window, scale);

    wxWindowList& children = window->GetChildren();
    for (wxWindowList::compatibility_iterator node = children.GetFirst(); node; node = node->GetNext()) {
        if (auto* child = node->GetData()) applyFontScaleRecursive(child, scale);
    }

    if (window->GetSizer() != nullptr) window->Layout();
}

inline void applyFontScale(wxWindow* window, double scale) {
    if (window == nullptr) return;

    wxWindow* const focused = wxWindow::FindFocus();
    const bool restoreFocus =
        focused != nullptr && wxGetTopLevelParent(focused) == wxGetTopLevelParent(window);

    // Apply the complete scale change as one visual transaction. Refresh only
    // after the locker has thawed the frame so native controls never paint an
    // intermediate geometry.
    {
        wxWindowUpdateLocker updateLocker(window);
        applyFontScaleRecursive(window, scale);
        window->Layout();
    }
    window->Refresh(false);

    if (restoreFocus && wxWindow::FindFocus() != focused &&
        !focused->IsBeingDeleted() && focused->IsEnabled() && focused->IsShownOnScreen()) {
        focused->SetFocus();
    }
}

} // namespace neoview
