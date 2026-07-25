#pragma once

#include <wx/aui/auibook.h>
#include <wx/event.h>
#include <wx/menu.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/string.h>
#include <wx/version.h>
#include <wx/window.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <string>

namespace neotabs {

constexpr std::size_t npos = static_cast<std::size_t>(-1);

inline const wxString& documentTabStripName() {
    static const wxString name = wxString::FromUTF8("NeoDocumentTabStrip");
    return name;
}

inline bool isDocumentTabStrip(const wxWindow* window) {
    return window != nullptr && window->GetName() == documentTabStripName();
}

// wxAuiNotebook indices are logical indices and can differ from the visible
// order after wxAUI_NB_TAB_MOVE. Page windows are stable identities, so all
// document-to-tab operations translate through the page pointer first.
inline int pageIndexFor(wxAuiNotebook* notebook, const wxWindow* page) {
    if (notebook == nullptr || page == nullptr) return wxNOT_FOUND;
    return notebook->FindPage(page);
}

inline wxWindow* pageForIndex(wxAuiNotebook* notebook, int index) {
    if (notebook == nullptr || index < 0 ||
        static_cast<std::size_t>(index) >= notebook->GetPageCount()) {
        return nullptr;
    }
    return notebook->GetPage(static_cast<std::size_t>(index));
}

inline wxWindow* currentPage(wxAuiNotebook* notebook) {
    return notebook == nullptr ? nullptr : notebook->GetCurrentPage();
}

// The applications use wxAuiNotebook as a document tab strip while rendering
// the active editor in controls below it. Keep the notebook page area at zero
// height so a best-size recalculation cannot expand the empty page region.
inline int documentTabStripHeight(wxAuiNotebook* notebook) {
    if (notebook == nullptr) return 0;
    notebook->SetTabCtrlHeight(-1);
    notebook->InvalidateBestSize();
    return std::max(notebook->FromDIP(24), notebook->GetHeightForPageHeight(0));
}

inline void updateDocumentTabStripLayout(wxAuiNotebook* notebook) {
    if (notebook == nullptr) return;

    wxWindow* const selectedPage = notebook->GetCurrentPage();
    const int height = documentTabStripHeight(notebook);
    if (height <= 0) return;

    // Placeholder pages carry identity only. Reassert their zero-height
    // contract after font, theme, DPI, page-add, and page-remove operations.
    for (std::size_t i = 0; i < notebook->GetPageCount(); ++i) {
        if (auto* page = notebook->GetPage(i)) {
            page->SetMinSize(wxSize(0, 0));
            page->SetMaxSize(wxSize(-1, 0));
        }
    }

    notebook->SetMinSize(wxSize(-1, height));
    notebook->SetMaxSize(wxSize(-1, height));
    if (auto* containingSizer = notebook->GetContainingSizer()) {
        containingSizer->SetItemMinSize(notebook, -1, height);
        if (auto* parent = notebook->GetParent();
            parent != nullptr && parent->GetSizer() != nullptr) {
            parent->Layout();
        }
    } else {
        // Fallback for a notebook configured before it is managed by a sizer.
        const wxSize current = notebook->GetSize();
        notebook->SetSize(wxSize(current.GetWidth(), height));
    }

    // A relayout must not switch documents. Restore by stable page identity,
    // not by a numeric index that may no longer match the visual tab order.
    const int selectedIndex = pageIndexFor(notebook, selectedPage);
    if (selectedIndex != wxNOT_FOUND && notebook->GetSelection() != selectedIndex) {
        notebook->ChangeSelection(static_cast<std::size_t>(selectedIndex));
    }

    notebook->Refresh(false);
}

inline void configureDocumentTabStrip(wxAuiNotebook* notebook) {
    if (notebook == nullptr) return;

    if (!isDocumentTabStrip(notebook)) {
        notebook->SetName(documentTabStripName());

#if wxCHECK_VERSION(3, 1, 3)
        // Let native DPI handling finish before rebuilding explicit DIP-based
        // metrics. The queued callback belongs to the notebook event handler
        // and is discarded with it during destruction.
        notebook->Bind(wxEVT_DPI_CHANGED, [notebook](wxDPIChangedEvent& event) {
            event.Skip();
            notebook->CallAfter([notebook]() {
                if (!notebook->IsBeingDeleted()) updateDocumentTabStripLayout(notebook);
            });
        });
#endif
    }

    updateDocumentTabStripLayout(notebook);
}

inline std::string pathToUtf8(const std::filesystem::path& path) {
#if defined(__cpp_lib_char8_t)
    const auto text = path.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
#else
    return path.u8string();
#endif
}

inline std::string displayNameForPath(const std::filesystem::path& path,
                                      const std::string& untitled) {
    if (!path.empty()) {
        const std::string leaf = pathToUtf8(path.filename());
        if (!leaf.empty()) return leaf;
        return pathToUtf8(path);
    }
    return untitled.empty() ? std::string("Untitled") : untitled;
}

inline wxString toWxUtf8(const std::string& text) {
    return wxString::FromUTF8(text.c_str());
}

inline wxString tabLabel(const std::string& displayName, bool dirty) {
    return toWxUtf8(std::string(dirty ? "*" : "") +
                    (displayName.empty() ? std::string("Untitled") : displayName));
}

inline void setTabLabel(wxAuiNotebook* notebook, const wxWindow* page,
                        const std::string& displayName, bool dirty) {
    const int index = pageIndexFor(notebook, page);
    if (index == wxNOT_FOUND) return;
    notebook->SetPageText(static_cast<std::size_t>(index), tabLabel(displayName, dirty));
}

inline wxWindow* addTabPage(wxAuiNotebook* notebook, const std::string& displayName,
                            bool dirty, bool select = true) {
    if (notebook == nullptr) return nullptr;
    configureDocumentTabStrip(notebook);

    auto* page = new wxPanel(notebook, wxID_ANY, wxDefaultPosition, wxSize(0, 0),
                             wxBORDER_NONE);
    page->SetMinSize(wxSize(0, 0));
    page->SetMaxSize(wxSize(-1, 0));

    // Add without selection events. The caller installs the returned page
    // identity in its DocumentTab before performing any model refresh.
    if (!notebook->AddPage(page, tabLabel(displayName, dirty), false)) {
        page->Destroy();
        return nullptr;
    }

    if (select) {
        const int index = pageIndexFor(notebook, page);
        if (index != wxNOT_FOUND) {
            notebook->ChangeSelection(static_cast<std::size_t>(index));
        }
    }

    updateDocumentTabStripLayout(notebook);
    return page;
}

inline bool changeSelectionToPage(wxAuiNotebook* notebook, const wxWindow* page) {
    const int index = pageIndexFor(notebook, page);
    if (index == wxNOT_FOUND) return false;
    notebook->ChangeSelection(static_cast<std::size_t>(index));
    return true;
}

inline bool deleteTabPage(wxAuiNotebook* notebook, const wxWindow* page) {
    const int index = pageIndexFor(notebook, page);
    if (index == wxNOT_FOUND) return false;
    if (!notebook->DeletePage(static_cast<std::size_t>(index))) return false;
    updateDocumentTabStripLayout(notebook);
    return true;
}

template <typename Documents>
inline std::size_t findDocumentIndexForPage(const Documents& documents,
                                            const wxWindow* page) {
    if (page == nullptr) return npos;
    for (std::size_t i = 0; i < documents.size(); ++i) {
        if (documents[i].tabPage == page) return i;
    }
    return npos;
}

inline void enableMenuItem(wxMenuBar* bar, int id, bool enable) {
    if (bar == nullptr) return;
    if (auto* item = bar->FindItem(id)) item->Enable(enable);
}

inline std::string closePromptText(const std::string& displayName) {
    return "The tab '" +
           (displayName.empty() ? std::string("Untitled") : displayName) +
           "' has unsaved changes. Close it anyway?";
}

} // namespace neotabs
