#pragma once

#include "NeoGameInstallResolver.hpp"
#include "NeoWindowPlacement.hpp"

#include <wx/button.h>
#include <wx/choicdlg.h>
#include <wx/dirdlg.h>
#include <wx/filedlg.h>
#include <wx/listctrl.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textdlg.h>
#include <wx/wx.h>
#include <wx/wrapsizer.h>

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
        : wxDialog(parent, wxID_ANY, "Saved Directories", wxDefaultPosition, wxDefaultSize,
                   wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
          allowedGameIds_(std::move(allowedGameIds)) {
        buildLayout();
        refreshList();
    }

private:
    enum class RowKind {
        Placeholder,
        Installation,
        CustomDirectory,
    };

    struct Row {
        RowKind kind = RowKind::Placeholder;
        std::string gameId;
        GameInstall install;
        CustomDirectory directory;
    };

    void buildLayout() {
        auto* root = new wxBoxSizer(wxVERTICAL);
        wxString introText;
        if (allowedGameIds_.size() == 1u) {
            const GameDefinition* game = findGame(allowedGameIds_.front());
            const std::string name = game ? game->displayName : allowedGameIds_.front();
            introText = neosettings::toWx(
                "Saved " + name +
                " installations and custom directories are shared by all Neo tools. "
                "Installations provide TLK, override, and resource-root resolution. "
                "Custom directories are global named bookmarks for opening files only.");
        } else {
            introText =
                "Saved game installations and custom directories are shared by all Neo tools. "
                "Installations provide TLK, override, and resource-root resolution. "
                "Custom directories may point anywhere and are global named bookmarks for opening files only.";
        }
#if defined(__EMSCRIPTEN__)
        introText =
            "Persistent installation and directory registration is unavailable in the browser. "
            "Open individual resources explicitly for the current session.";
#endif
        auto* intro = new wxStaticText(this, wxID_ANY, introText);
        intro->Wrap(FromDIP(820));
        root->Add(intro, 0, wxEXPAND | wxALL, FromDIP(10));

        list_ = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                               wxLC_REPORT | wxLC_SINGLE_SEL | wxLC_HRULES | wxLC_VRULES);
        list_->AppendColumn("Scope", wxLIST_FORMAT_LEFT, FromDIP(190));
        list_->AppendColumn("Kind", wxLIST_FORMAT_LEFT, FromDIP(130));
        list_->AppendColumn("Name", wxLIST_FORMAT_LEFT, FromDIP(180));
        list_->AppendColumn("Active", wxLIST_FORMAT_LEFT, FromDIP(65));
        list_->AppendColumn("Status", wxLIST_FORMAT_LEFT, FromDIP(105));
        list_->AppendColumn("Directory", wxLIST_FORMAT_LEFT, FromDIP(320));
        list_->AppendColumn("TLK", wxLIST_FORMAT_LEFT, FromDIP(250));
        list_->AppendColumn("Override/Data", wxLIST_FORMAT_LEFT, FromDIP(250));
        root->Add(list_, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(10));

        auto* actions = new wxWrapSizer(wxHORIZONTAL);
        addInstallButton_ = new wxButton(this, wxID_ANY, "Add Install...");
        addDirectoryButton_ = new wxButton(this, wxID_ANY, "Add Directory...");
        changeButton_ = new wxButton(this, wxID_ANY, "Change Path...");
        browseTlkButton_ = new wxButton(this, wxID_ANY, "Browse TLK...");
        renameButton_ = new wxButton(this, wxID_ANY, "Rename...");
        setActiveButton_ = new wxButton(this, wxID_ANY, "Set Active");
        rescanSelectedButton_ = new wxButton(this, wxID_ANY, "Rescan Selected");
        rescanAllButton_ = new wxButton(this, wxID_ANY, "Rescan All");
        clearButton_ = new wxButton(this, wxID_ANY, "Clear Selected");
        auto* close = new wxButton(this, wxID_CLOSE, "Close");

        for (wxButton* button : {addInstallButton_, addDirectoryButton_, changeButton_,
                                 browseTlkButton_, renameButton_, setActiveButton_,
                                 rescanSelectedButton_, rescanAllButton_, clearButton_}) {
            actions->Add(button, 0, wxRIGHT | wxBOTTOM, FromDIP(6));
        }
        root->Add(actions, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(10));

        auto* closeRow = new wxBoxSizer(wxHORIZONTAL);
        closeRow->AddStretchSpacer();
        closeRow->Add(close, 0);
        root->Add(closeRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));

        SetSizer(root);
        neowindow::configureResponsiveWindow(*this, wxSize(1420, 640), wxSize(760, 420));
        CentreOnParent();
        neowindow::constrainWindowToDisplay(*this);

        addInstallButton_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { onAddInstall(); });
        addDirectoryButton_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { onAddDirectory(); });
        changeButton_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { onChangePath(); });
        browseTlkButton_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { onBrowseTlk(); });
        renameButton_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { onRename(); });
        setActiveButton_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { onSetActive(); });
        rescanSelectedButton_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { onRescanSelected(); });
        rescanAllButton_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { onRescanAll(); });
        clearButton_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { onClearSelected(); });
        close->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CLOSE); });
        list_->Bind(wxEVT_LIST_ITEM_ACTIVATED, [this](wxListEvent&) { onRename(); });
        list_->Bind(wxEVT_LIST_ITEM_SELECTED, [this](wxListEvent&) { updateActionState(); });
        list_->Bind(wxEVT_LIST_ITEM_DESELECTED, [this](wxListEvent&) { updateActionState(); });
    }

    long selectedRowIndex() const {
        return list_ ? list_->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED) : -1;
    }

    const Row* selectedRow() const {
        const long index = selectedRowIndex();
        if (index < 0 || static_cast<std::size_t>(index) >= rows_.size()) return nullptr;
        return &rows_[static_cast<std::size_t>(index)];
    }

    std::optional<GameInstall> selectedInstall() const {
        const Row* row = selectedRow();
        if (row == nullptr || row->kind != RowKind::Installation ||
            row->install.installId.empty()) {
            return std::nullopt;
        }
        return row->install;
    }

    std::optional<CustomDirectory> selectedCustomDirectory() const {
        const Row* row = selectedRow();
        if (row == nullptr || row->kind != RowKind::CustomDirectory ||
            row->directory.directoryId.empty()) {
            return std::nullopt;
        }
        return row->directory;
    }

    const GameDefinition* selectedGame() const {
        const Row* row = selectedRow();
        if (row == nullptr || row->gameId.empty()) return nullptr;
        return findGame(row->gameId);
    }

    const GameDefinition* chooseGameForInstall() {
        std::vector<const GameDefinition*> choices;
        for (const auto& game : knownGames()) {
            if (isAllowedGameId(allowedGameIds_, game.id)) choices.push_back(&game);
        }
        if (choices.empty()) return nullptr;
        if (choices.size() == 1u) return choices.front();

        wxArrayString labels;
        int initial = 0;
        const GameDefinition* selected = selectedGame();
        for (std::size_t i = 0; i < choices.size(); ++i) {
            labels.Add(neosettings::toWx(choices[i]->displayName));
            if (selected != nullptr && selected->id == choices[i]->id) {
                initial = static_cast<int>(i);
            }
        }
        wxSingleChoiceDialog dialog(this, "Add an installation for which game?",
                                    "Choose Game", labels);
        dialog.SetSelection(initial);
        if (dialog.ShowModal() != wxID_OK) return nullptr;
        const int selection = dialog.GetSelection();
        if (selection == wxNOT_FOUND || static_cast<std::size_t>(selection) >= choices.size()) {
            return nullptr;
        }
        return choices[static_cast<std::size_t>(selection)];
    }

    void showInvalidInstallMessage(const GameDefinition& game,
                                   const std::filesystem::path& path) const {
        wxString message = "The selected directory is not a valid ";
        message += neosettings::toWx(game.displayName);
        message += " installation:\n\n";
        message += neosettings::pathToWx(path);
        message += "\n\nSelect the game root containing ";
        message += neosettings::toWx(installationRequirementText(game));
        message += ". Folder names such as Override, StreamWaves, lips, modules, or data do not identify an installation.";
        wxMessageBox(message, "Invalid Game Installation", wxOK | wxICON_WARNING,
                     const_cast<GameDirectoryDialog*>(this));
    }

    void refreshList(const std::string& selectGameId = {},
                     const std::string& selectEntryId = {},
                     bool selectCustom = false) {
        if (list_ == nullptr) return;
        list_->DeleteAllItems();
        rows_.clear();

        for (const CustomDirectory& directory : resolver().settings().readCustomDirectories()) {
            Row row;
            row.kind = RowKind::CustomDirectory;
            row.directory = directory;
            rows_.push_back(std::move(row));
        }

        for (const auto& game : knownGames()) {
            if (!isAllowedGameId(allowedGameIds_, game.id)) continue;
            auto installs = resolver().settings().readAll(game);
            if (installs.empty()) {
                Row row;
                row.kind = RowKind::Placeholder;
                row.gameId = game.id;
                rows_.push_back(std::move(row));
            } else {
                for (auto& install : installs) {
                    Row row;
                    row.kind = RowKind::Installation;
                    row.gameId = game.id;
                    row.install = std::move(install);
                    rows_.push_back(std::move(row));
                }
            }
        }

        long selectRow = -1;
        for (std::size_t i = 0; i < rows_.size(); ++i) {
            const Row& row = rows_[i];
            const long item = list_->InsertItem(static_cast<long>(i), wxEmptyString);
            if (row.kind == RowKind::CustomDirectory) {
                const bool exists = isDirectoryPath(row.directory.path);
                list_->SetItem(item, 0, "All tools");
                list_->SetItem(item, 1, "Custom directory");
                list_->SetItem(item, 2, neosettings::toWx(row.directory.displayName));
                list_->SetItem(item, 4, exists ? "available" : "missing");
                list_->SetItem(item, 5, neosettings::pathToWx(row.directory.path));
                if (selectCustom && row.directory.directoryId == selectEntryId) {
                    selectRow = item;
                }
                continue;
            }

            const GameDefinition* game = findGame(row.gameId);
            const std::string gameName = game ? game->displayName : row.gameId;
            list_->SetItem(item, 0, neosettings::toWx(gameName));
            if (row.kind == RowKind::Placeholder) {
                list_->SetItem(item, 2, "(not configured)");
                list_->SetItem(item, 4, "not configured");
                continue;
            }

            const std::string active = game
                ? resolver().settings().activeInstallId(game->id).value_or(std::string{})
                : std::string{};
            const bool isActive = !active.empty() && active == row.install.installId;
            list_->SetItem(item, 1, "Installation");
            list_->SetItem(item, 2, neosettings::toWx(row.install.displayName));
            list_->SetItem(item, 3, isActive ? "Yes" : "");
            list_->SetItem(item, 4, neosettings::toWx(
                row.install.status.empty() ? "not found" : row.install.status));
            list_->SetItem(item, 5, neosettings::pathToWx(row.install.installPath));
            list_->SetItem(item, 6, neosettings::pathToWx(row.install.tlkPath));
            const std::string resourceRoot = !row.install.overridePath.empty()
                ? neosettings::pathToUtf8(row.install.overridePath)
                : neosettings::pathToUtf8(row.install.dataRootPath);
            list_->SetItem(item, 7, neosettings::toWx(resourceRoot));
            if (!selectCustom && row.gameId == selectGameId &&
                (selectEntryId.empty() || row.install.installId == selectEntryId)) {
                selectRow = item;
            }
        }

        if (selectRow >= 0) {
            list_->SetItemState(selectRow, wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED,
                                wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED);
            list_->EnsureVisible(selectRow);
        }
        updateActionState();
    }

    void updateActionState() {
#if defined(__EMSCRIPTEN__)
        for (wxButton* button : {addInstallButton_, addDirectoryButton_, changeButton_,
                                 browseTlkButton_, renameButton_, setActiveButton_,
                                 rescanSelectedButton_, rescanAllButton_, clearButton_}) {
            if (button != nullptr) button->Enable(false);
        }
#else
        const Row* row = selectedRow();
        const bool installation = row != nullptr && row->kind == RowKind::Installation;
        const bool custom = row != nullptr && row->kind == RowKind::CustomDirectory;
        const bool configured = installation || custom;
        if (addInstallButton_ != nullptr) addInstallButton_->Enable(true);
        if (addDirectoryButton_ != nullptr) addDirectoryButton_->Enable(true);
        if (changeButton_ != nullptr) changeButton_->Enable(configured);
        if (browseTlkButton_ != nullptr) browseTlkButton_->Enable(installation);
        if (renameButton_ != nullptr) renameButton_->Enable(configured);
        if (setActiveButton_ != nullptr) setActiveButton_->Enable(installation);
        if (rescanSelectedButton_ != nullptr) rescanSelectedButton_->Enable(installation);
        if (rescanAllButton_ != nullptr) rescanAllButton_->Enable(true);
        if (clearButton_ != nullptr) clearButton_->Enable(configured);
#endif
    }

    void onAddInstall() {
#if defined(__EMSCRIPTEN__)
        return;
#else
        const GameDefinition* game = chooseGameForInstall();
        if (game == nullptr) return;
        wxDirDialog dialog(this,
                           neosettings::toWx("Choose install folder for " + game->displayName),
                           wxEmptyString, wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
        if (dialog.ShowModal() != wxID_OK) return;
        const auto path = neosettings::pathFromWx(dialog.GetPath());
        const auto install = resolver().rememberUserInstall(game->id, path);
        if (install.installId.empty()) {
            showInvalidInstallMessage(*game, path);
            return;
        }
        refreshList(install.id, install.installId, false);
#endif
    }

    void onAddDirectory() {
#if defined(__EMSCRIPTEN__)
        return;
#else
        wxDirDialog directoryDialog(this, "Choose a directory to save", wxEmptyString,
                                    wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
        if (directoryDialog.ShowModal() != wxID_OK) return;
        const auto path = neosettings::pathFromWx(directoryDialog.GetPath());
        std::string defaultName = neosettings::pathToUtf8(path.filename());
        if (defaultName.empty()) defaultName = "Custom Directory";
        wxTextEntryDialog nameDialog(this, "Directory name:", "Add Directory",
                                     neosettings::toWx(defaultName));
        if (nameDialog.ShowModal() != wxID_OK) return;
        const std::string name = neosettings::toStd(nameDialog.GetValue());
        if (name.empty()) {
            wxMessageBox("The directory name cannot be empty.", "Add Directory",
                         wxOK | wxICON_INFORMATION, this);
            return;
        }
        const auto entry = resolver().rememberCustomDirectory(path, name);
        if (entry.directoryId.empty()) {
            wxMessageBox("The selected directory could not be saved.", "Add Directory",
                         wxOK | wxICON_WARNING, this);
            return;
        }
        refreshList({}, entry.directoryId, true);
#endif
    }

    void onChangePath() {
#if defined(__EMSCRIPTEN__)
        return;
#else
        if (const auto custom = selectedCustomDirectory()) {
            wxDirDialog dialog(this, "Choose replacement directory",
                               neosettings::pathToWx(custom->path),
                               wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
            if (dialog.ShowModal() != wxID_OK) return;
            const auto updated = resolver().rememberCustomDirectory(
                neosettings::pathFromWx(dialog.GetPath()), custom->displayName,
                custom->directoryId);
            if (updated.directoryId.empty()) {
                wxMessageBox("The selected directory could not be saved.", "Change Directory",
                             wxOK | wxICON_WARNING, this);
                return;
            }
            refreshList({}, updated.directoryId, true);
            return;
        }

        const auto selected = selectedInstall();
        if (!selected) return;
        const GameDefinition* game = findGame(selected->id);
        if (game == nullptr) return;
        wxDirDialog dialog(this,
                           neosettings::toWx("Choose install folder for " + game->displayName),
                           neosettings::pathToWx(selected->installPath),
                           wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
        if (dialog.ShowModal() != wxID_OK) return;
        const auto path = neosettings::pathFromWx(dialog.GetPath());
        const auto updated = resolver().rememberUserInstall(
            game->id, path,
            selected->explicitTlk ? selected->tlkPath : std::filesystem::path{},
            selected->displayName, selected->installId);
        if (updated.installId.empty()) {
            showInvalidInstallMessage(*game, path);
            return;
        }
        refreshList(updated.id, updated.installId, false);
#endif
    }

    void onBrowseTlk() {
#if defined(__EMSCRIPTEN__)
        return;
#else
        const auto selected = selectedInstall();
        if (!selected) return;
        const GameDefinition* game = findGame(selected->id);
        if (game == nullptr) return;
        wxFileDialog dialog(this,
                            neosettings::toWx("Choose TLK file for " + game->displayName),
                            wxEmptyString, wxEmptyString,
                            "TLK files (*.tlk)|*.tlk|All files (*.*)|*.*",
                            wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (dialog.ShowModal() != wxID_OK) return;
        const auto install = resolver().rememberUserTlk(
            game->id, neosettings::pathFromWx(dialog.GetPath()),
            selected->installId, selected->displayName);
        if (install.installId.empty()) {
            wxMessageBox("The selected TLK file could not be registered.",
                         "Invalid TLK File", wxOK | wxICON_WARNING, this);
            return;
        }
        refreshList(install.id, install.installId, false);
#endif
    }

    void onRename() {
        if (const auto custom = selectedCustomDirectory()) {
            wxTextEntryDialog dialog(this, "Directory name:", "Rename Directory",
                                     neosettings::toWx(custom->displayName));
            if (dialog.ShowModal() != wxID_OK) return;
            const std::string name = neosettings::toStd(dialog.GetValue());
            if (name.empty()) {
                wxMessageBox("The name cannot be empty.", "Saved Directories",
                             wxOK | wxICON_INFORMATION, this);
                return;
            }
            if (!resolver().settings().renameCustomDirectory(custom->directoryId, name)) {
                wxMessageBox("The selected directory could not be renamed.",
                             "Saved Directories", wxOK | wxICON_ERROR, this);
                return;
            }
            refreshList({}, custom->directoryId, true);
            return;
        }

        const auto selected = selectedInstall();
        if (!selected) return;
        wxTextEntryDialog dialog(this, "Install name:", "Rename Install",
                                 neosettings::toWx(selected->displayName));
        if (dialog.ShowModal() != wxID_OK) return;
        const std::string name = neosettings::toStd(dialog.GetValue());
        if (name.empty()) {
            wxMessageBox("The name cannot be empty.", "Saved Directories",
                         wxOK | wxICON_INFORMATION, this);
            return;
        }
        if (!resolver().settings().renameInstall(selected->id, selected->installId, name)) {
            wxMessageBox("The selected installation could not be renamed.",
                         "Saved Directories", wxOK | wxICON_ERROR, this);
            return;
        }
        refreshList(selected->id, selected->installId, false);
    }

    void onSetActive() {
        const auto selected = selectedInstall();
        if (!selected) return;
        if (!resolver().settings().setActiveInstall(selected->id, selected->installId)) {
            const GameDefinition* game = findGame(selected->id);
            if (game != nullptr && !selected->installPath.empty()) {
                showInvalidInstallMessage(*game, selected->installPath);
            } else {
                wxMessageBox(
                    "The selected entry has neither a valid installation root nor a readable explicit TLK file.",
                    "Cannot Activate Entry", wxOK | wxICON_WARNING, this);
            }
            return;
        }
        refreshList(selected->id, selected->installId, false);
    }

    void onRescanSelected() {
        const auto selected = selectedInstall();
        if (!selected) return;
        const GameDefinition* game = findGame(selected->id);
        if (game == nullptr) return;
        resolver().resolveInstalls(
            *game,
            selected->installPath.empty()
                ? std::optional<std::filesystem::path>{}
                : std::optional<std::filesystem::path>{selected->installPath},
            true);
        refreshList(game->id, selected->installId, false);
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
        if (const auto custom = selectedCustomDirectory()) {
            resolver().settings().clearCustomDirectory(custom->directoryId);
            refreshList();
            return;
        }
        const auto selected = selectedInstall();
        if (!selected) return;
        resolver().settings().clearInstall(selected->id, selected->installId);
        refreshList(selected->id);
    }

    wxListCtrl* list_ = nullptr;
    wxButton* addInstallButton_ = nullptr;
    wxButton* addDirectoryButton_ = nullptr;
    wxButton* changeButton_ = nullptr;
    wxButton* browseTlkButton_ = nullptr;
    wxButton* renameButton_ = nullptr;
    wxButton* setActiveButton_ = nullptr;
    wxButton* rescanSelectedButton_ = nullptr;
    wxButton* rescanAllButton_ = nullptr;
    wxButton* clearButton_ = nullptr;
    std::vector<Row> rows_;
    GameDirectoryGameIds allowedGameIds_;
};

inline void showGameDirectoriesDialog(wxWindow* parent,
                                      GameDirectoryGameIds allowedGameIds = {}) {
    GameDirectoryDialog dialog(parent, std::move(allowedGameIds));
    dialog.ShowModal();
}

} // namespace neogames
