#pragma once

#include "NeoSettings.hpp"

#include <wx/wx.h>
#include <wx/textctrl.h>
#include <wx/statusbr.h>
#include <wx/dcbuffer.h>
#include <wx/statbox.h>
#include <wx/settings.h>
#include <wx/scrolwin.h>
#include <wx/grid.h>
#include <wx/config.h>
#include <wx/dirdlg.h>
#include <wx/filedlg.h>
#include <wx/listctrl.h>
#include <wx/textdlg.h>
#include <wx/spinctrl.h>
#include <wx/treectrl.h>

#if defined(__WXMSW__) && !wxCHECK_VERSION(3, 3, 3)
#error "Neo Tools Windows GUIs require wxWidgets 3.3.3 or newer. Use the neoshared vcpkg overlay."
#endif

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace wxui {

inline wxString toWx(const std::string& text) {
    return wxString::FromUTF8(text.c_str());
}

inline std::string toStd(const wxString& text) {
    const wxScopedCharBuffer buffer = text.ToUTF8();
    return buffer ? std::string(buffer.data()) : std::string();
}

struct ThemePalette {
    wxColour frame;
    wxColour panel;
    wxColour field;
    wxColour fieldAlt;
    wxColour button;
    wxColour text;
    wxColour mutedText;
    wxColour gridLine;
    wxColour selection;
    wxColour selectionText;
};

inline bool& activeDarkMode() {
    static bool enabled = false;
    return enabled;
}

inline bool currentDarkMode() {
    return activeDarkMode();
}

inline ThemePalette themePalette(bool darkMode) {
    if (darkMode) {
        return ThemePalette{
            wxColour(24, 24, 27),
            wxColour(32, 33, 36),
            wxColour(18, 18, 21),
            wxColour(33, 33, 36),
            wxColour(44, 44, 48),
            wxColour(238, 238, 238),
            wxColour(188, 188, 188),
            wxColour(72, 72, 78),
            wxColour(65, 105, 160),
            wxColour(255, 255, 255)
        };
    }
    const wxColour face = wxSystemSettings::GetColour(wxSYS_COLOUR_BTNFACE);
    return ThemePalette{
        face,
        face,
        wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW),
        wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW),
        face,
        wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT),
        wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT),
        wxSystemSettings::GetColour(wxSYS_COLOUR_3DSHADOW),
        wxSystemSettings::GetColour(wxSYS_COLOUR_HIGHLIGHT),
        wxSystemSettings::GetColour(wxSYS_COLOUR_HIGHLIGHTTEXT)
    };
}

// wxGrid normally paints cell contents first and grid lines in a separate
// pass. On some wxMSW/Windows 10 configurations, a small invalidation around
// the mouse cursor can repaint the cell without reliably restoring that later
// line pass. Paint each separator as part of its owning cell instead so any
// cell repaint restores the same right and bottom edges deterministically.
class PersistentGridCellRenderer final : public wxGridCellStringRenderer {
public:
    wxGridCellRenderer* Clone() const override {
        return new PersistentGridCellRenderer();
    }

    void Draw(wxGrid& grid,
              wxGridCellAttr& attr,
              wxDC& dc,
              const wxRect& rect,
              int row,
              int col,
              bool isSelected) override {
        wxGridCellStringRenderer::Draw(grid, attr, dc, rect, row, col, isSelected);

        if (rect.GetWidth() <= 0 || rect.GetHeight() <= 0) {
            return;
        }

        const wxPen previousPen = dc.GetPen();
        const int right = rect.GetRight();
        const int bottom = rect.GetBottom();

        dc.SetPen(grid.GetColGridLinePen(col));
        dc.DrawLine(right, rect.GetTop(), right, bottom + 1);

        dc.SetPen(grid.GetRowGridLinePen(row));
        dc.DrawLine(rect.GetLeft(), bottom, right + 1, bottom);

        dc.SetPen(previousPen);
    }
};

inline void configureStableGridRendering(wxGrid& grid) {
    // Native grid lines are a second paint pass. The renderer above owns the
    // separators instead, keeping them inside the cell's invalidated rectangle.
    grid.EnableGridLines(false);
    grid.SetDefaultRenderer(new PersistentGridCellRenderer());

    // Keep resizing available from the row/column labels, but do not switch to
    // resize cursors while traversing the one-pixel separators in the cell area.
    grid.DisableDragGridSize();
}


class ThemedStatusBar final : public wxStatusBar {
public:
    explicit ThemedStatusBar(wxWindow* parent, int fields = 1)
        : wxStatusBar(parent, wxID_ANY, wxSTB_DEFAULT_STYLE | wxFULL_REPAINT_ON_RESIZE) {
        const int safeFields = std::max(1, fields);
        wxStatusBar::SetFieldsCount(safeFields);
        labels_.assign(static_cast<std::size_t>(safeFields), wxString{});
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        Bind(wxEVT_PAINT, &ThemedStatusBar::onPaint, this);
        Bind(wxEVT_ERASE_BACKGROUND, &ThemedStatusBar::onEraseBackground, this);
        Bind(wxEVT_SIZE, &ThemedStatusBar::onSize, this);
        applyPalette();
    }

    void setDarkMode(bool enabled) {
        darkMode_ = enabled;
        applyPalette();
        Refresh(false);
        Update();
    }

    void SetStatusText(const wxString& text, int number = 0) {
        if (number < 0) {
            return;
        }
        ensureLabelCount(number + 1);
        labels_[static_cast<std::size_t>(number)] = text;

        // Keep native status-bar text empty.  On MSW the native status-bar
        // control can repaint labels using the system foreground after the
        // theme pass, which produces black footer text in dark mode.
        wxStatusBar::SetStatusText(wxString{}, number);
        Refresh(false);
        Update();
    }

private:
    void ensureLabelCount(int minimum) {
        if (minimum <= 0) {
            return;
        }
        if (minimum > GetFieldsCount()) {
            wxStatusBar::SetFieldsCount(minimum);
        }
        if (static_cast<std::size_t>(minimum) > labels_.size()) {
            labels_.resize(static_cast<std::size_t>(minimum));
        }
    }

    void applyPalette() {
        const ThemePalette palette = themePalette(darkMode_);
        SetBackgroundColour(palette.button);
        SetForegroundColour(palette.text);
    }

    void onEraseBackground(wxEraseEvent&) {
        // The themed status bar is fully painted in onPaint().
    }

    void onSize(wxSizeEvent& event) {
        event.Skip();
        Refresh(false);
    }

    void onPaint(wxPaintEvent&) {
        const ThemePalette palette = themePalette(darkMode_);
        wxAutoBufferedPaintDC dc(this);
        const wxRect client = GetClientRect();

        dc.SetBackground(wxBrush(palette.button));
        dc.Clear();
        dc.SetPen(wxPen(palette.button));
        dc.SetBrush(wxBrush(palette.button));
        dc.DrawRectangle(client.GetX(), client.GetY(), client.GetWidth(), client.GetHeight());

        dc.SetFont(GetFont());
        dc.SetTextForeground(palette.text);
        dc.SetPen(wxPen(palette.gridLine));
        if (client.GetHeight() > 1) {
            dc.DrawLine(client.GetLeft(), client.GetTop(), client.GetRight(), client.GetTop());
        }

        const int fields = std::max(1, GetFieldsCount());
        ensureLabelCount(fields);
        for (int field = 0; field < fields; ++field) {
            wxRect rect;
            if (!GetFieldRect(field, rect)) {
                continue;
            }

            if (field > 0) {
                dc.SetPen(wxPen(palette.gridLine));
                dc.DrawLine(rect.GetLeft(), rect.GetTop() + FromDIP(3),
                            rect.GetLeft(), rect.GetBottom() - FromDIP(3));
            }

            const wxString& text = labels_[static_cast<std::size_t>(field)];
            if (text.empty()) {
                continue;
            }

            wxRect textRect = rect;
            textRect.Deflate(FromDIP(6), 0);
            const wxSize extent = dc.GetTextExtent(text);
            int y = textRect.GetTop() + std::max(0, (textRect.GetHeight() - extent.GetHeight()) / 2);
            if (y < textRect.GetTop()) {
                y = textRect.GetTop();
            }

            dc.SetClippingRegion(textRect);
            dc.DrawText(text, textRect.GetLeft(), y);
            dc.DestroyClippingRegion();
        }
    }

    bool darkMode_ = false;
    std::vector<wxString> labels_;
};

inline wxStatusBar* createStatusBar(wxFrame& frame, int fields = 1) {
    auto* status = new ThemedStatusBar(&frame, fields);
    frame.SetStatusBar(status);
    status->setDarkMode(currentDarkMode());
    return status;
}

inline void applyStatusBarTheme(wxStatusBar& status, bool darkMode) {
    if (auto* themed = dynamic_cast<ThemedStatusBar*>(&status)) {
        themed->setDarkMode(darkMode);
    } else {
        const ThemePalette palette = themePalette(darkMode);
        status.SetBackgroundColour(palette.button);
        status.SetForegroundColour(palette.text);
        status.Refresh(false);
        status.Update();
    }
}

inline void setStatusText(wxFrame& frame, const wxString& text, int field = 0) {
    if (auto* status = frame.GetStatusBar()) {
        if (auto* themed = dynamic_cast<ThemedStatusBar*>(status)) {
            themed->SetStatusText(text, field);
            return;
        }
    }
    frame.SetStatusText(text, field);
}

inline void setStatusText(wxFrame& frame, const std::string& text, int field = 0) {
    setStatusText(frame, toWx(text), field);
}

inline void setStatusText(wxFrame& frame, const char* text, int field = 0) {
    setStatusText(frame, wxString::FromUTF8(text), field);
}

inline bool readDarkMode(const std::string& appName) {
    return neosettings::AppSettings(appName).darkMode();
}

inline void writeDarkMode(const std::string& appName, bool enabled) {
    neosettings::AppSettings(appName).setDarkMode(enabled);
}

inline void styleListRow(wxListCtrl& list, long row, bool darkMode) {
    const ThemePalette palette = themePalette(darkMode);
    list.SetItemTextColour(row, palette.text);
    list.SetItemBackgroundColour(row, darkMode && (row % 2) ? palette.fieldAlt : palette.field);
}

inline void applyListTheme(wxListCtrl& list, bool darkMode) {
    const ThemePalette palette = themePalette(darkMode);
    list.SetBackgroundColour(palette.field);
    list.SetForegroundColour(palette.text);
    list.SetTextColour(palette.text);
    for (long row = 0; row < list.GetItemCount(); ++row) {
        styleListRow(list, row, darkMode);
    }
    list.Refresh();
    list.Update();
}

inline void applyTreeTheme(wxTreeCtrl& tree, bool darkMode) {
    const ThemePalette palette = themePalette(darkMode);
    tree.SetBackgroundColour(palette.field);
    tree.SetForegroundColour(palette.text);
    tree.SetOwnBackgroundColour(palette.field);
    tree.SetOwnForegroundColour(palette.text);
    tree.Refresh();
    tree.Update();
}

inline void applyGridTheme(wxGrid& grid, bool darkMode) {
    const ThemePalette palette = themePalette(darkMode);
    {
        // wxGrid owns multiple internal paint windows. Batch the attribute
        // changes and let wxGrid invalidate those windows itself instead of
        // synchronously repainting each child during the recursive theme pass.
        wxGridUpdateLocker lock(&grid);
        grid.SetBackgroundColour(palette.field);
        grid.SetForegroundColour(palette.text);
        grid.SetDefaultCellBackgroundColour(palette.field);
        grid.SetDefaultCellTextColour(palette.text);
        grid.SetLabelBackgroundColour(darkMode ? palette.panel : wxSystemSettings::GetColour(wxSYS_COLOUR_BTNFACE));
        grid.SetLabelTextColour(palette.text);
        // Separators are painted by PersistentGridCellRenderer as part of
        // each cell, not by wxGrid's separate native grid-line pass.
        grid.EnableGridLines(false);
        grid.SetGridLineColour(palette.gridLine);
        grid.SetSelectionBackground(palette.selection);
        grid.SetSelectionForeground(palette.selectionText);
        for (int row = 0; row < grid.GetNumberRows(); ++row) {
            for (int col = 0; col < grid.GetNumberCols(); ++col) {
                grid.SetCellBackgroundColour(row, col, darkMode && (row % 2) ? palette.fieldAlt : palette.field);
                grid.SetCellTextColour(row, col, palette.text);
            }
        }
    }
    grid.ForceRefresh();
    if (wxWindow* cells = grid.GetGridWindow()) {
        cells->Refresh(false);
    }
}

inline void applyTheme(wxWindow* window, bool darkMode) {
    activeDarkMode() = darkMode;
    if (window == nullptr) return;

    const ThemePalette palette = themePalette(darkMode);
    wxColour background = palette.panel;
    wxColour foreground = palette.text;

    if (dynamic_cast<wxFrame*>(window) != nullptr || dynamic_cast<wxDialog*>(window) != nullptr) {
        background = palette.frame;
    }
    if (dynamic_cast<wxPanel*>(window) != nullptr || dynamic_cast<wxScrolledWindow*>(window) != nullptr) {
        background = palette.panel;
    }
    if (dynamic_cast<wxTextCtrl*>(window) != nullptr || dynamic_cast<wxListCtrl*>(window) != nullptr) {
        background = palette.field;
    }
    if (dynamic_cast<wxButton*>(window) != nullptr || dynamic_cast<wxCheckBox*>(window) != nullptr ||
        dynamic_cast<wxChoice*>(window) != nullptr || dynamic_cast<wxComboBox*>(window) != nullptr ||
        dynamic_cast<wxRadioButton*>(window) != nullptr || dynamic_cast<wxSpinCtrl*>(window) != nullptr) {
        background = palette.button;
    }
    if (dynamic_cast<wxStaticBox*>(window) != nullptr || dynamic_cast<wxStaticText*>(window) != nullptr) {
        background = palette.panel;
    }

    window->SetBackgroundColour(background);
    window->SetForegroundColour(foreground);

    if (auto* text = dynamic_cast<wxTextCtrl*>(window)) {
        text->SetBackgroundColour(text->IsEditable() ? palette.field : (darkMode ? palette.fieldAlt : wxSystemSettings::GetColour(wxSYS_COLOUR_BTNFACE)));
        text->SetForegroundColour(palette.text);
    }
    if (auto* list = dynamic_cast<wxListCtrl*>(window)) {
        applyListTheme(*list, darkMode);
    }
    if (auto* grid = dynamic_cast<wxGrid*>(window)) {
        applyGridTheme(*grid, darkMode);
    }
    if (auto* tree = dynamic_cast<wxTreeCtrl*>(window)) {
        applyTreeTheme(*tree, darkMode);
    }
    if (auto* status = dynamic_cast<wxStatusBar*>(window)) {
        applyStatusBarTheme(*status, darkMode);
    }
    if (auto* frame = dynamic_cast<wxFrame*>(window)) {
        if (auto* status = frame->GetStatusBar()) {
            applyStatusBarTheme(*status, darkMode);
        }
    }

    // wxGrid child windows are implementation details with their own paint
    // pipeline. Recursively assigning colours to them can leave one-pixel
    // separators invalidated until the next mouse event on wxMSW.
    if (dynamic_cast<wxGrid*>(window) == nullptr) {
        for (wxWindowList::compatibility_iterator node = window->GetChildren().GetFirst(); node; node = node->GetNext()) {
            applyTheme(node->GetData(), darkMode);
        }
    }

    window->Refresh();
    window->Update();
}

inline void showError(wxWindow* parent, const std::exception& ex) {
    wxMessageBox(toWx(ex.what()), "Error", wxOK | wxICON_ERROR, parent);
}

inline void showMessage(wxWindow* parent, const std::string& title, const std::string& message) {
    wxMessageBox(toWx(message), toWx(title), wxOK | wxICON_INFORMATION, parent);
}

inline bool confirm(wxWindow* parent, const std::string& title, const std::string& message) {
    return wxMessageBox(toWx(message), toWx(title), wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION, parent) == wxYES;
}

inline std::optional<std::string> promptText(wxWindow* parent,
                                             const std::string& title,
                                             const std::string& message,
                                             const std::string& defaultValue = {}) {
    wxTextEntryDialog dialog(parent, toWx(message), toWx(title), toWx(defaultValue));
    applyTheme(&dialog, currentDarkMode());
    if (dialog.ShowModal() != wxID_OK) {
        return std::nullopt;
    }
    return toStd(dialog.GetValue());
}

inline std::optional<std::filesystem::path> chooseOpenFile(
    wxWindow* parent,
    const std::string& title,
    const std::string& wildcard,
    const std::filesystem::path& initialDirectory = {}) {
    wxFileDialog dialog(parent, toWx(title), neosettings::pathToWx(initialDirectory), wxEmptyString, toWx(wildcard),
                        wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dialog.ShowModal() != wxID_OK) return std::nullopt;
    return neosettings::pathFromWx(dialog.GetPath());
}

inline std::vector<std::filesystem::path> chooseOpenFiles(
    wxWindow* parent,
    const std::string& title,
    const std::string& wildcard,
    const std::filesystem::path& initialDirectory = {}) {
    wxFileDialog dialog(parent, toWx(title), neosettings::pathToWx(initialDirectory), wxEmptyString, toWx(wildcard),
                        wxFD_OPEN | wxFD_FILE_MUST_EXIST | wxFD_MULTIPLE);
    if (dialog.ShowModal() != wxID_OK) return {};
    wxArrayString paths;
    dialog.GetPaths(paths);
    std::vector<std::filesystem::path> result;
    result.reserve(paths.size());
    for (const auto& path : paths) result.emplace_back(neosettings::pathFromWx(path));
    return result;
}

inline std::optional<std::filesystem::path> chooseSaveFile(wxWindow* parent,
                                                           const std::string& title,
                                                           const std::string& wildcard,
                                                           const std::string& defaultFile = {}) {
    wxFileDialog dialog(parent, toWx(title), wxEmptyString, toWx(defaultFile), toWx(wildcard),
                        wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    if (dialog.ShowModal() != wxID_OK) return std::nullopt;
    return neosettings::pathFromWx(dialog.GetPath());
}

inline std::optional<std::filesystem::path> chooseDirectory(wxWindow* parent,
                                                            const std::string& title) {
    wxDirDialog dialog(parent, toWx(title), wxEmptyString, wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
    if (dialog.ShowModal() != wxID_OK) return std::nullopt;
    return neosettings::pathFromWx(dialog.GetPath());
}

inline void setColumns(wxListCtrl& list, const std::vector<std::pair<std::string, int>>& columns) {
    list.ClearAll();
    long index = 0;
    for (const auto& column : columns) {
        list.InsertColumn(index, toWx(column.first), wxLIST_FORMAT_LEFT, column.second);
        ++index;
    }
}

inline void appendRow(wxListCtrl& list, const std::vector<std::string>& cells, long itemData = -1) {
    const long row = list.GetItemCount();
    list.InsertItem(row, cells.empty() ? wxString{} : toWx(cells.front()));
    for (std::size_t i = 1; i < cells.size(); ++i) {
        list.SetItem(row, static_cast<long>(i), toWx(cells[i]));
    }
    if (itemData >= 0) list.SetItemData(row, itemData);
    styleListRow(list, row, currentDarkMode());
}

inline long selectedRow(const wxListCtrl& list) {
    return list.GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
}

inline std::vector<long> selectedRows(const wxListCtrl& list) {
    std::vector<long> rows;
    long row = -1;
    while (true) {
        row = list.GetNextItem(row, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
        if (row == -1) break;
        rows.push_back(row);
    }
    return rows;
}

inline void selectRow(wxListCtrl& list, long row) {
    list.SetItemState(row, wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED,
                      wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED);
    list.EnsureVisible(row);
}

inline void selectAll(wxListCtrl& list) {
    for (long row = 0; row < list.GetItemCount(); ++row) {
        list.SetItemState(row, wxLIST_STATE_SELECTED, wxLIST_STATE_SELECTED);
    }
}

} // namespace wxui
