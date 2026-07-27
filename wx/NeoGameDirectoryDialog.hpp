#pragma once

#include "NeoGameInstallResolver.hpp"

#include <wx/button.h>
#include <wx/choice.h>
#include <wx/dirdlg.h>
#include <wx/filedlg.h>
#include <wx/listctrl.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/textdlg.h>
#include <wx/wx.h>

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace neogames {

using GameDirectoryGameIds = std::vector<std::string>;

inline bool isAllowedGameId(const GameDirectoryGameIds& allowedGameIds,
                            const std::string& gameId) {
    return allowedGameIds.empty() ||
           std::find(allowedGameIds.begin(), allowedGameIds.end(), gameId) != allowedGameIds.end();
}

class GameDirectoryDialog final : public wxDialog {
public:
    explicit GameDirectoryDialog(wxWindow* parent,
                                 GameDirectoryGameIds allowedGameIds = {})
        : wxDialog(parent, wxID_ANY, "Game Directories", wxDefaultPosition, wxDefaultSize,
                   wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
          allowedGameIds_(std::move(allowedGameIds)) {
        buildLayout();
        refreshList();
    }

private:
    void buildLayout() {
        auto* root = new wxBoxSizer(wxVERTICAL);
        wxString introText;
        if (allowedGameIds_.size() == 1) {
            const GameDefinition* game = findGame(allowedGameIds_.front());
            const std::string name = game ? game->displayName : allowedGameIds_.front();
            introText = neosettings::toWx(
                "Saved " + name +
                " installations are shared by the Neo tools. Multiple named installs, such as Steam or GOG, may be configured.");
        } else {
            introText =
                "Saved game directories are shared by all Neo tools to resolve TLK files, overrides, and resource roots. Each game can have multiple named installs, such as Steam, GOG, or K2 Test. Manual file opening still works when no game is configured.";
        }
        auto* intro = new wxStaticText(this, wxID_ANY, introText);
        intro->Wrap(FromDIP(760));
        root->Add(intro, 0, wxEXPAND | wxALL, FromDIP(10));

        list_ = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                               wxLC_REPORT | wxLC_SINGLE_SEL | wxLC_HRULES | wxLC_VRULES);
        list_->AppendColumn("Game", wxLIST_FORMAT_LEFT, FromDIP(220));
        list_->AppendColumn("Install Name", wxLIST_FORMAT_LEFT, FromDIP(180));
        list_->AppendColumn("Active", wxLIST_FORMAT_LEFT, FromDIP(70));
        list_->AppendColumn("Status", wxLIST_FORMAT_LEFT, FromDIP(85));
        list_->AppendColumn("Install Path", wxLIST_FORMAT_LEFT, FromDIP(300));
        list_->AppendColumn("TLK", wxLIST_FORMAT_LEFT, FromDIP(260));
        list_->AppendColumn("Override/Data", wxLIST_FORMAT_LEFT, FromDIP(260));
        root->Add(list_, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(10));

        auto* buttons = new wxBoxSizer(wxHORIZONTAL);
        auto* addInstall = new wxButton(this, wxID_ANY, "Add Install...");
        auto* changeInstall = new wxButton(this, wxID_ANY, "Change Install...");
        auto* browseTlk = new wxButton(this, wxID_ANY, "Browse TLK...");
        auto* rename = new wxButton(this, wxID_ANY, "Rename...");
        auto* setActive = new wxButton(this, wxID_ANY, "Set Active");
        auto* rescanSelected = new wxButton(this, wxID_ANY, "Rescan Selected");
        auto* rescanAll = new wxButton(this, wxID_ANY, "Rescan All");
        auto* clear = new wxButton(this, wxID_ANY, "Clear Selected");
        auto* close = new wxButton(this, wxID_CLOSE, "Close");
        buttons->Add(addInstall, 0, wxRIGHT, FromDIP(6));
        buttons->Add(changeInstall, 0, wxRIGHT, FromDIP(6));
        buttons->Add(browseTlk, 0, wxRIGHT, FromDIP(6));
        buttons->Add(rename, 0, wxRIGHT, FromDIP(6));
        buttons->Add(setActive, 0, wxRIGHT, FromDIP(6));
        buttons->Add(rescanSelected, 0, wxRIGHT, FromDIP(6));
        buttons->Add(rescanAll, 0, wxRIGHT, FromDIP(6));
        buttons->Add(clear, 0, wxRIGHT, FromDIP(6));
        buttons->AddStretchSpacer();
        buttons->Add(close, 0);
        root->Add(buttons, 0, wxEXPAND | wxALL, FromDIP(10));

        SetSizer(root);
        SetMinSize(FromDIP(wxSize(980, 440)));
        SetInitialSize(FromDIP(wxSize(1320, 620)));

        addInstall->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { onAddInstall(); });
        changeInstall->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { onChangeInstall(); });
        browseTlk->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { onBrowseTlk(); });
        rename->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { onRename(); });
        setActive->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { onSetActive(); });
        rescanSelected->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { onRescanSelected(); });
        rescanAll->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { onRescanAll(); });
        clear->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { onClearSelected(); });
        close->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CLOSE); });
        list_->Bind(wxEVT_LIST_ITEM_ACTIVATED, [this](wxListEvent&) { onRename(); });
    }

    long selectedRow() const {
        return list_ ? list_->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED) : -1;
    }

    std::optional<GameInstall> selectedInstall() const {
        const long row = selectedRow();
        if (row < 0 || static_cast<std::size_t>(row) >= rows_.size()) return std::nullopt;
        if (rows_[static_cast<std::size_t>(row)].installId.empty()) return std::nullopt;
        return rows_[static_cast<std::size_t>(row)];
    }

    const GameDefinition* selectedGame() const {
        const long row = selectedRow();
        if (row < 0 || static_cast<std::size_t>(row) >= rows_.size()) return nullptr;
        return findGame(rows_[static_cast<std::size_t>(row)].id);
    }

    const GameDefinition* requireGame() const {
        const GameDefinition* game = selectedGame();
        if (game == nullptr) {
            wxMessageBox("Select a game row first.", "Game Directories", wxOK | wxICON_INFORMATION, const_cast<GameDirectoryDialog*>(this));
        }
        return game;
    }

    std::optional<GameInstall> requireInstall() const {
        auto install = selectedInstall();
        if (!install) {
            wxMessageBox("Select a configured install row first.", "Game Directories", wxOK | wxICON_INFORMATION, const_cast<GameDirectoryDialog*>(this));
        }
        return install;
    }

    void refreshList(const std::string& selectGameId = {}, const std::string& selectInstallId = {}) {
        if (list_ == nullptr) return;
        list_->DeleteAllItems();
        rows_.clear();

        for (const auto& game : knownGames()) {
            if (!isAllowedGameId(allowedGameIds_, game.id)) continue;
            auto installs = resolver().settings().readAll(game);
            if (installs.empty()) {
                GameInstall missing;
                missing.id = game.id;
                missing.displayName = "";
                missing.status = "not configured";
                rows_.push_back(std::move(missing));
            } else {
                rows_.insert(rows_.end(), installs.begin(), installs.end());
            }
        }

        long selectRow = -1;
        for (std::size_t i = 0; i < rows_.size(); ++i) {
            const auto& row = rows_[i];
            const GameDefinition* game = findGame(row.id);
            const std::string gameName = game ? game->displayName : row.id;
            const std::string active = game ? resolver().settings().activeInstallId(game->id).value_or(std::string{}) : std::string{};
            const bool hasInstall = !row.installId.empty();
            const bool isActive = hasInstall && active == row.installId;
            const long item = list_->InsertItem(static_cast<long>(i), neosettings::toWx(gameName));
            list_->SetItem(item, 1, neosettings::toWx(hasInstall ? row.displayName : std::string("(not configured)")));
            list_->SetItem(item, 2, isActive ? "Yes" : "");
            list_->SetItem(item, 3, neosettings::toWx(row.status.empty() ? "not found" : row.status));
            list_->SetItem(item, 4, neosettings::pathToWx(row.installPath));
            list_->SetItem(item, 5, neosettings::pathToWx(row.tlkPath));
            const std::string root = !row.overridePath.empty()
                                         ? neosettings::pathToUtf8(row.overridePath)
                                         : neosettings::pathToUtf8(row.dataRootPath);
            list_->SetItem(item, 6, neosettings::toWx(root));
            if (selectRow < 0 && row.id == selectGameId &&
                (selectInstallId.empty() || row.installId == selectInstallId)) {
                selectRow = item;
            }
        }

        if (selectRow >= 0) {
            list_->SetItemState(selectRow, wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED,
                                wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED);
            list_->EnsureVisible(selectRow);
        }
    }

    void onAddInstall() {
        const GameDefinition* game = requireGame();
        if (game == nullptr) return;
        wxDirDialog dialog(this, neosettings::toWx("Choose install folder for " + game->displayName), wxEmptyString,
                           wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
        if (dialog.ShowModal() != wxID_OK) return;
        const auto path = std::filesystem::path(neosettings::toStd(dialog.GetPath()));
        const auto install = resolver().rememberUserInstall(game->id, path);
        refreshList(install.id, install.installId);
    }

    void onChangeInstall() {
        const GameDefinition* game = requireGame();
        if (game == nullptr) return;
        const auto selected = selectedInstall();
        wxDirDialog dialog(this, neosettings::toWx("Choose install folder for " + game->displayName), wxEmptyString,
                           wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
        if (dialog.ShowModal() != wxID_OK) return;
        const auto path = std::filesystem::path(neosettings::toStd(dialog.GetPath()));
        const auto install = resolver().rememberUserInstall(game->id, path,
                                                            selected ? selected->tlkPath : std::filesystem::path{},
                                                            selected ? selected->displayName : std::string{},
                                                            selected ? selected->installId : std::string{});
        refreshList(install.id, install.installId);
    }

    void onBrowseTlk() {
        const GameDefinition* game = requireGame();
        if (game == nullptr) return;
        const auto selected = selectedInstall();
        wxFileDialog dialog(this, neosettings::toWx("Choose TLK file for " + game->displayName), wxEmptyString,
                            wxEmptyString, "TLK files (*.tlk)|*.tlk|All files (*.*)|*.*",
                            wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (dialog.ShowModal() != wxID_OK) return;
        const auto install = resolver().rememberUserTlk(game->id, std::filesystem::path(neosettings::toStd(dialog.GetPath())),
                                                       selected ? selected->installId : std::string{},
                                                       selected ? selected->displayName : std::string{});
        refreshList(install.id, install.installId);
    }

    void onRename() {
        const auto selected = requireInstall();
        if (!selected) return;
        wxTextEntryDialog dialog(this, "Install name:", "Rename Install", neosettings::toWx(selected->displayName));
        if (dialog.ShowModal() != wxID_OK) return;
        const std::string name = neosettings::toStd(dialog.GetValue());
        if (name.empty()) {
            wxMessageBox("The install name cannot be empty.", "Game Directories", wxOK | wxICON_INFORMATION, this);
            return;
        }
        if (!resolver().settings().renameInstall(selected->id, selected->installId, name)) {
            wxMessageBox("The selected install could not be renamed.", "Game Directories", wxOK | wxICON_ERROR, this);
            return;
        }
        refreshList(selected->id, selected->installId);
    }

    void onSetActive() {
        const auto selected = requireInstall();
        if (!selected) return;
        resolver().settings().setActiveInstall(selected->id, selected->installId);
        refreshList(selected->id, selected->installId);
    }

    void onRescanSelected() {
        const GameDefinition* game = requireGame();
        if (game == nullptr) return;
        const auto selected = selectedInstall();
        resolver().resolveInstalls(*game, selected && !selected->installPath.empty()
                                              ? std::optional<std::filesystem::path>(selected->installPath)
                                              : std::nullopt,
                                   true);
        refreshList(game->id, selected ? selected->installId : std::string{});
    }

    void onRescanAll() {
        if (allowedGameIds_.empty()) {
            resolver().resolveAllInstalls(std::nullopt, true);
        } else {
            for (const std::string& gameId : allowedGameIds_) {
                if (const GameDefinition* game = findGame(gameId)) {
                    resolver().resolveInstalls(*game, std::nullopt, true);
                }
            }
        }
        refreshList();
    }

    void onClearSelected() {
        const auto selected = requireInstall();
        if (!selected) return;
        GamePathSettings{}.clearInstall(selected->id, selected->installId);
        refreshList(selected->id);
    }

    wxListCtrl* list_ = nullptr;
    std::vector<GameInstall> rows_;
    GameDirectoryGameIds allowedGameIds_;
};

inline void showGameDirectoriesDialog(wxWindow* parent,
                                      GameDirectoryGameIds allowedGameIds = {}) {
    GameDirectoryDialog dialog(parent, std::move(allowedGameIds));
    dialog.ShowModal();
}

} // namespace neogames
