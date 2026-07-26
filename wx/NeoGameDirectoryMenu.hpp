#pragma once

#include "NeoGameDirectoryDialog.hpp"
#include "NeoGameInstallResolver.hpp"
#include "NeoSettings.hpp"

#include <wx/event.h>
#include <wx/menu.h>
#include <wx/msgdlg.h>
#include <wx/utils.h>
#include <wx/window.h>

#include <algorithm>
#include <exception>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace neogames {

struct SavedGameDirectory {
    std::string gameId;
    std::string gameName;
    std::string installId;
    std::string installName;
    std::filesystem::path path;
    bool active = false;
    bool exists = false;
};

// Applications that need more than the selected path (for example, NeoERF's
// resource-name profile) receive the complete saved-game selection. The path-
// only callback remains available below for tools without game-specific state.
using OpenGameDirectoryAction = std::function<void(const SavedGameDirectory&)>;
using OpenGameFileDialog = std::function<void(const std::filesystem::path&)>;

inline std::vector<SavedGameDirectory> savedGameDirectories() {
    std::vector<SavedGameDirectory> directories;
    const GamePathSettings settings;

    for (const GameDefinition& game : knownGames()) {
        const std::string activeId = settings.activeInstallId(game.id).value_or(std::string{});
        for (const GameInstall& install : settings.readAll(game)) {
            if (install.installPath.empty()) continue;

            SavedGameDirectory directory;
            directory.gameId = game.id;
            directory.gameName = game.displayName;
            directory.installId = install.installId;
            directory.installName = install.displayName.empty()
                                        ? defaultInstallName(game, install.installPath)
                                        : install.displayName;
            directory.path = neosettings::normalizedPath(install.installPath);
            directory.active = !activeId.empty() && install.installId == activeId;
            directory.exists = isDirectoryPath(directory.path);
            directories.push_back(std::move(directory));
        }
    }

    std::stable_sort(directories.begin(), directories.end(), [](const SavedGameDirectory& lhs,
                                                                 const SavedGameDirectory& rhs) {
        if (lhs.gameName != rhs.gameName) return lowerAscii(lhs.gameName) < lowerAscii(rhs.gameName);
        if (lhs.active != rhs.active) return lhs.active;
        if (lhs.installName != rhs.installName) return lowerAscii(lhs.installName) < lowerAscii(rhs.installName);
        return lowerAscii(neosettings::pathToUtf8(lhs.path)) < lowerAscii(neosettings::pathToUtf8(rhs.path));
    });
    return directories;
}

class OpenGameDirectoryMenu final {
public:
    OpenGameDirectoryMenu(wxWindow& owner, wxMenu& menu, OpenGameDirectoryAction action)
        : owner_(owner), menu_(menu), action_(std::move(action)) {
        owner_.Bind(wxEVT_MENU_OPEN, &OpenGameDirectoryMenu::onMenuOpen, this);
        refresh();
    }

    ~OpenGameDirectoryMenu() {
        owner_.Unbind(wxEVT_MENU_OPEN, &OpenGameDirectoryMenu::onMenuOpen, this);
        unbindItemHandlers();
    }

    OpenGameDirectoryMenu(const OpenGameDirectoryMenu&) = delete;
    OpenGameDirectoryMenu& operator=(const OpenGameDirectoryMenu&) = delete;

    void refresh() {
        unbindItemHandlers();
        clearMenu();

        const std::vector<SavedGameDirectory> directories = savedGameDirectories();
        for (const SavedGameDirectory& directory : directories) {
            std::string label = directory.active ? "[Active] " : std::string{};
            label += directory.gameName;
            if (!directory.installName.empty() && directory.installName != directory.gameName) {
                label += " - " + directory.installName;
            }
            if (!directory.exists) label += " (missing)";
            label = neosettings::ellipsizeMiddle(label, 100);

            wxMenuItem* item = menu_.Append(
                wxID_ANY,
                neosettings::toWx(neosettings::escapeMenuLabel(label)),
                neosettings::pathToWx(directory.path));
            item->Enable(directory.exists);

            const int id = item->GetId();
            entries_.push_back({id, directory});
            owner_.Bind(wxEVT_MENU, &OpenGameDirectoryMenu::onOpenDirectory, this, id);
        }

        if (entries_.empty()) {
            wxMenuItem* empty = menu_.Append(wxID_ANY, "No saved game directories");
            empty->Enable(false);
        }

        menu_.AppendSeparator();
        wxMenuItem* manage = menu_.Append(wxID_ANY, "&Manage Game Directories...");
        manageId_ = manage->GetId();
        owner_.Bind(wxEVT_MENU, &OpenGameDirectoryMenu::onManageDirectories, this, manageId_);
    }

private:
    struct BoundEntry {
        int id = wxID_NONE;
        SavedGameDirectory directory;
    };

    void clearMenu() {
        while (menu_.GetMenuItemCount() > 0) {
            wxMenuItem* item = menu_.FindItemByPosition(0);
            if (item == nullptr) break;
            menu_.Destroy(item);
        }
    }

    void unbindItemHandlers() {
        for (const BoundEntry& entry : entries_) {
            owner_.Unbind(wxEVT_MENU, &OpenGameDirectoryMenu::onOpenDirectory, this, entry.id);
        }
        entries_.clear();

        if (manageId_ != wxID_NONE) {
            owner_.Unbind(wxEVT_MENU, &OpenGameDirectoryMenu::onManageDirectories, this, manageId_);
            manageId_ = wxID_NONE;
        }
    }

    void onMenuOpen(wxMenuEvent& event) {
        if (event.GetMenu() == &menu_) refresh();
        event.Skip();
    }

    void onOpenDirectory(wxCommandEvent& event) {
        const auto it = std::find_if(entries_.begin(), entries_.end(), [&](const BoundEntry& entry) {
            return entry.id == event.GetId();
        });
        if (it == entries_.end()) return;

        if (!isDirectoryPath(it->directory.path)) {
            wxString message = "The saved game directory no longer exists:\n\n";
            message += neosettings::pathToWx(it->directory.path);
            message += "\n\nUse Manage Game Directories to update it.";
            wxMessageBox(message,
                         "Game Directory Missing",
                         wxOK | wxICON_WARNING,
                         &owner_);
            refresh();
            return;
        }

        if (!action_) {
            wxMessageBox("This application did not configure a file picker for saved game directories.",
                         "Unable to Open from Game Directory",
                         wxOK | wxICON_ERROR,
                         &owner_);
            return;
        }

        try {
            action_(it->directory);
        } catch (const std::exception& ex) {
            wxString message = "The application could not open its file dialog:\n\n";
            message += neosettings::toWx(ex.what());
            wxMessageBox(message,
                         "Unable to Open from Game Directory",
                         wxOK | wxICON_ERROR,
                         &owner_);
        }
    }

    void onManageDirectories(wxCommandEvent&) {
        showGameDirectoriesDialog(&owner_);
        refresh();
    }

    wxWindow& owner_;
    wxMenu& menu_;
    std::vector<BoundEntry> entries_;
    OpenGameDirectoryAction action_;
    int manageId_ = wxID_NONE;
};

inline std::unique_ptr<OpenGameDirectoryMenu> appendOpenGameDirectoryMenu(
    wxWindow& owner,
    wxMenu& parent,
    OpenGameDirectoryAction action,
    const wxString& label = "Open Game &Directory") {
    auto* submenu = new wxMenu();
    parent.AppendSubMenu(submenu, label,
                         "Open a supported file from a saved game installation");
    return std::make_unique<OpenGameDirectoryMenu>(owner, *submenu, std::move(action));
}

inline std::unique_ptr<OpenGameDirectoryMenu> appendOpenGameDirectoryMenu(
    wxWindow& owner,
    wxMenu& parent,
    OpenGameFileDialog openFileDialog,
    const wxString& label = "Open Game &Directory") {
    OpenGameDirectoryAction action = [openFileDialog = std::move(openFileDialog)](
                                         const SavedGameDirectory& directory) {
        if (openFileDialog) openFileDialog(directory.path);
    };
    return appendOpenGameDirectoryMenu(owner, parent, std::move(action), label);
}

// Compatibility overload for independently versioned tool repositories.
// Updated tools pass an application-specific file-dialog callback above; this
// preserves the previous behavior only for an older consumer checked out
// against a newer neoshared revision during a staggered repository rollout.
inline std::unique_ptr<OpenGameDirectoryMenu> appendOpenGameDirectoryMenu(
    wxWindow& owner,
    wxMenu& parent,
    const wxString& label = "Open Game &Directory") {
    return appendOpenGameDirectoryMenu(
        owner,
        parent,
        OpenGameFileDialog{[&owner](const std::filesystem::path& directory) {
            if (wxLaunchDefaultApplication(neosettings::pathToWx(directory))) return;

            wxString message = "The system file manager could not open:\n\n";
            message += neosettings::pathToWx(directory);
            wxMessageBox(message,
                         "Unable to Open Game Directory",
                         wxOK | wxICON_ERROR,
                         &owner);
        }},
        label);
}

} // namespace neogames
