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
#include <filesystem>
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
        return lowerAscii(lhs.path.u8string()) < lowerAscii(rhs.path.u8string());
    });
    return directories;
}

class OpenGameDirectoryMenu final {
public:
    OpenGameDirectoryMenu(wxWindow& owner, wxMenu& menu)
        : owner_(owner), menu_(menu) {
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
                neosettings::toWx(directory.path.u8string()));
            item->Enable(directory.exists);

            const int id = item->GetId();
            entries_.push_back({id, directory.path});
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
        std::filesystem::path path;
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

        if (!isDirectoryPath(it->path)) {
            wxString message = "The saved game directory no longer exists:\n\n";
            message += neosettings::toWx(it->path.u8string());
            message += "\n\nUse Manage Game Directories to update it.";
            wxMessageBox(message,
                         "Game Directory Missing",
                         wxOK | wxICON_WARNING,
                         &owner_);
            refresh();
            return;
        }

        if (!wxLaunchDefaultApplication(neosettings::toWx(it->path.u8string()))) {
            wxString message = "The system file manager could not open:\n\n";
            message += neosettings::toWx(it->path.u8string());
            wxMessageBox(message,
                         "Unable to Open Game Directory",
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
    int manageId_ = wxID_NONE;
};

inline std::unique_ptr<OpenGameDirectoryMenu> appendOpenGameDirectoryMenu(
    wxWindow& owner,
    wxMenu& parent,
    const wxString& label = "Open Game &Directory") {
    auto* submenu = new wxMenu();
    parent.AppendSubMenu(submenu, label,
                         "Open a saved game install folder in the system file manager");
    return std::make_unique<OpenGameDirectoryMenu>(owner, *submenu);
}

} // namespace neogames
