#pragma once

#include "NeoWindowPlacement.hpp"
#include "NeoWxUi.hpp"
#include "TslPatcher.hpp"

#include <wx/button.h>
#include <wx/clipbrd.h>
#include <wx/choice.h>
#include <wx/dataobj.h>
#include <wx/dialog.h>
#include <wx/radiobut.h>
#include <wx/statbox.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/weakref.h>

#include <algorithm>
#include <cstdint>
#include <cctype>
#include <exception>
#include <filesystem>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace wxui {

// Increment this when the shared patch-export contract changes in a way that
// every consuming GUI must adopt. Tool repositories use this to fail clearly
// instead of silently building against a stale, folder-only neoshared checkout.
inline constexpr unsigned kPatcherExportUiApiVersion = 4u;

enum class PatcherOutputMode {
    WriteToIni,
    Fragment,
};

struct PatcherOutputSelection {
#if defined(__EMSCRIPTEN__)
    PatcherOutputMode mode = PatcherOutputMode::Fragment;
#else
    PatcherOutputMode mode = PatcherOutputMode::WriteToIni;
#endif
    // Set only for desktop WriteToIni. Fragment deliberately has no
    // destination before the preview is generated.
    std::filesystem::path iniPath;
#if defined(__EMSCRIPTEN__)
    // Browser Write-to-INI uses an opaque directory-handle session plus the
    // exact package-relative INI path selected or entered by the user.
    std::uint32_t browserDirectorySession = 0;
    std::string browserIniPath;
#endif

    bool writesToIni() const noexcept {
        return mode == PatcherOutputMode::WriteToIni;
    }
};

class PatcherOutputDialog final : public wxDialog {
public:
    PatcherOutputDialog(wxWindow* parent,
                        const std::filesystem::path& initialDirectory = {},
                        const std::string& defaultFile = "changes.ini",
                        bool allowBrowserPackageWrite = false)
        : wxDialog(parent,
                   wxID_ANY,
                   "Patcher Output",
                   wxDefaultPosition,
                   wxDefaultSize,
                   wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
          initialDirectory_(initialDirectory),
          defaultFile_(defaultFile.empty() ? "changes.ini" : defaultFile) {
        auto* root = new wxBoxSizer(wxVERTICAL);

        auto* heading = new wxStaticText(
            this,
            wxID_ANY,
            "Choose how to deliver the generated TSLPatcher/HoloPatcher instructions:");
        heading->Wrap(FromDIP(700));
        root->Add(heading, 0, wxEXPAND | wxALL, FromDIP(12));

        writeToIni_ = new wxRadioButton(
            this,
            wxID_ANY,
            "Write to a selected installer INI",
            wxDefaultPosition,
            wxDefaultSize,
            wxRB_GROUP);
        root->Add(writeToIni_, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(12));

        auto* writeDescription = new wxStaticText(
            this,
            wxID_ANY,
            "Select an exact existing or new .ini file. Generated instructions are merged into that file, and required payloads are staged beside it.");
        writeDescription->Wrap(FromDIP(660));
        root->Add(writeDescription, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(24));

        auto* iniBox = new wxStaticBoxSizer(wxVERTICAL, this, "Installer INI");
#if defined(__EMSCRIPTEN__)
        browserPackageWriteEnabled_ = allowBrowserPackageWrite;
        browserDirectorySupported_ = browserPackageWriteEnabled_ &&
            neobrowser::packageDirectoryAccessSupported();
#else
        (void)allowBrowserPackageWrite;
#endif
#if defined(__EMSCRIPTEN__)
        auto* folderRow = new wxBoxSizer(wxHORIZONTAL);
        browserDirectoryLabel_ = new wxStaticText(
            iniBox->GetStaticBox(), wxID_ANY, "No installer folder selected.");
        browseButton_ = new wxButton(
            iniBox->GetStaticBox(), wxID_ANY, "Select Installer Folder...");
        folderRow->Add(browserDirectoryLabel_, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
        folderRow->Add(browseButton_, 0);
        iniBox->Add(folderRow, 0, wxEXPAND | wxALL, FromDIP(8));

        auto* existingLabel = new wxStaticText(
            iniBox->GetStaticBox(), wxID_ANY, "Existing INIs in the selected folder:");
        iniBox->Add(existingLabel, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(8));
        browserIniChoice_ = new wxChoice(iniBox->GetStaticBox(), wxID_ANY);
        browserIniChoice_->SetToolTip(
            "Existing INIs are shown by package-relative path so same-named files in different option folders remain distinguishable.");
        iniBox->Add(browserIniChoice_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));

        auto* pathLabel = new wxStaticText(
            iniBox->GetStaticBox(), wxID_ANY,
            "Exact package-relative INI path (select one above or enter a new path):");
        iniBox->Add(pathLabel, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(8));
        iniPath_ = new wxTextCtrl(iniBox->GetStaticBox(), wxID_ANY);
        iniPath_->SetHint("changes.ini, full/changes.ini, or install_lite.ini");
        iniBox->Add(iniPath_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
#else
        auto* iniRow = new wxBoxSizer(wxHORIZONTAL);
        iniPath_ = new wxTextCtrl(
            iniBox->GetStaticBox(),
            wxID_ANY,
            wxEmptyString,
            wxDefaultPosition,
            wxDefaultSize,
            wxTE_READONLY);
        iniPath_->SetHint("Select a specific .ini file, such as full/changes.ini or lite/changes.ini");
        browseButton_ = new wxButton(iniBox->GetStaticBox(), wxID_ANY, "Select INI...");
        iniRow->Add(iniPath_, 1, wxEXPAND | wxRIGHT, FromDIP(8));
        iniRow->Add(browseButton_, 0);
        iniBox->Add(iniRow, 0, wxEXPAND | wxALL, FromDIP(8));
#endif
        root->Add(iniBox, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));

        fragment_ = new wxRadioButton(
            this,
            wxID_ANY,
            "Fragment (preview, copy, or save as a new INI)");
        root->Add(fragment_, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(12));

#if defined(__EMSCRIPTEN__)
        if (browserDirectorySupported_) {
            writeToIni_->SetValue(true);
            fragment_->SetValue(false);
            auto* browserNote = new wxStaticText(
                this,
                wxID_ANY,
                "The browser will request read/write access to one installer folder. NeoTools preflights the selected INI and same-name payloads, writes payloads first, and commits the INI last. The folder remains on your computer; it is not uploaded.");
            browserNote->Wrap(FromDIP(660));
            root->Add(browserNote, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(20));
        } else {
            writeToIni_->SetValue(false);
            writeToIni_->Enable(false);
            fragment_->SetValue(true);
            auto* browserNote = new wxStaticText(
                this,
                wxID_ANY,
                browserPackageWriteEnabled_
                    ? "This browser does not provide writable folder access. Fragment remains available; package-aware Write to INI requires a browser implementing the File System Access directory API or a desktop build."
                    : "Package-aware Write to INI is not enabled for this exporter in the browser build. Fragment remains available and creates no companion payloads.");
            browserNote->Wrap(FromDIP(660));
            root->Add(browserNote, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(20));
        }
#else
        writeToIni_->SetValue(true);
#endif

        auto* fragmentDescription = new wxStaticText(
            this,
            wxID_ANY,
            "No destination is requested before generation. Fragment mode shows the exact text, creates no payloads, omits [Settings], and never merges into or overwrites an existing INI.");
        fragmentDescription->Wrap(FromDIP(660));
        root->Add(fragmentDescription, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(24));

        auto* buttons = CreateStdDialogButtonSizer(wxOK | wxCANCEL);
        root->Add(buttons, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));

        SetSizer(root);
        root->Fit(this);
        neowindow::configureResponsiveWindow(*this, wxSize(800, 560), wxSize(540, 380));
        applyTheme(this, currentDarkMode());

        writeToIni_->Bind(wxEVT_RADIOBUTTON, &PatcherOutputDialog::onModeChanged, this);
        fragment_->Bind(wxEVT_RADIOBUTTON, &PatcherOutputDialog::onModeChanged, this);
        browseButton_->Bind(wxEVT_BUTTON, &PatcherOutputDialog::onBrowse, this);
#if defined(__EMSCRIPTEN__)
        browserIniChoice_->Bind(wxEVT_CHOICE, &PatcherOutputDialog::onBrowserIniChoice, this);
#endif
        Bind(wxEVT_BUTTON, &PatcherOutputDialog::onAccept, this, wxID_OK);
        updateModeControls();
    }

    PatcherOutputSelection selection() const {
        PatcherOutputSelection result;
        result.mode = fragment_ != nullptr && fragment_->GetValue()
            ? PatcherOutputMode::Fragment
            : PatcherOutputMode::WriteToIni;
        if (result.writesToIni()) {
#if defined(__EMSCRIPTEN__)
            result.browserDirectorySession = browserDirectorySession_;
            result.browserIniPath = selectedIniRelative_;
#else
            result.iniPath = selectedIni_;
#endif
        }
        return result;
    }

private:
    void onModeChanged(wxCommandEvent&) {
        updateModeControls();
    }

    void updateModeControls() {
        const bool writeSelected = writeToIni_ != nullptr && writeToIni_->GetValue();
#if defined(__EMSCRIPTEN__)
        const bool pathEnabled = writeSelected && browserDirectorySupported_ &&
            browserDirectorySession_ != 0 && !browserDirectoryRequestPending_;
        if (browserDirectoryLabel_ != nullptr) browserDirectoryLabel_->Enable(writeSelected);
        if (browserIniChoice_ != nullptr) browserIniChoice_->Enable(pathEnabled);
        if (iniPath_ != nullptr) iniPath_->Enable(pathEnabled);
        if (browseButton_ != nullptr) {
            browseButton_->Enable(
                writeSelected && browserDirectorySupported_ && !browserDirectoryRequestPending_);
        }
#else
        if (iniPath_ != nullptr) iniPath_->Enable(writeSelected);
        if (browseButton_ != nullptr) browseButton_->Enable(writeSelected);
#endif
    }

#if defined(__EMSCRIPTEN__)
    void completeBrowserDirectory(neobrowser::PackageDirectoryResult result) {
        browserDirectoryRequestPending_ = false;
        if (!result.error.empty()) {
            browserDirectoryLabel_->SetLabel("Installer folder selection failed.");
            updateModeControls();
            wxMessageBox(
                toWx(result.error),
                "Unable to Select Installer Folder",
                wxOK | wxICON_ERROR,
                this);
            return;
        }
        if (result.cancelled()) {
            browserDirectoryLabel_->SetLabel(
                browserDirectorySession_ == 0
                    ? wxString("No installer folder selected.")
                    : toWx("Selected folder: " + browserDirectoryName_));
            updateModeControls();
            return;
        }

        browserDirectorySession_ = result.sessionId;
        browserDirectoryName_ = result.displayName.empty()
            ? std::string("installer folder")
            : std::move(result.displayName);
        browserDirectoryLabel_->SetLabel(
            toWx("Selected folder: " + browserDirectoryName_));
        browserIniChoice_->Clear();
        browserIniPaths_ = std::move(result.iniPaths);
        for (const auto& path : browserIniPaths_) browserIniChoice_->Append(toWx(path));

        std::string selected = defaultFile_;
        if (!browserIniPaths_.empty()) {
            auto preferred = std::find_if(
                browserIniPaths_.begin(), browserIniPaths_.end(),
                [this](const std::string& path) {
                    std::string left = path;
                    std::string right = defaultFile_;
                    std::transform(left.begin(), left.end(), left.begin(), [](unsigned char ch) {
                        return static_cast<char>(std::tolower(ch));
                    });
                    std::transform(right.begin(), right.end(), right.begin(), [](unsigned char ch) {
                        return static_cast<char>(std::tolower(ch));
                    });
                    return left == right;
                });
            const std::size_t index = preferred == browserIniPaths_.end()
                ? 0u
                : static_cast<std::size_t>(preferred - browserIniPaths_.begin());
            browserIniChoice_->SetSelection(static_cast<int>(index));
            selected = browserIniPaths_[index];
        }
        selectedIniRelative_ = neobrowser::normalizePackageRelativePath(selected, true);
        iniPath_->ChangeValue(toWx(selectedIniRelative_));
        iniPath_->SetInsertionPointEnd();
        updateModeControls();
        Layout();
    }

    void onBrowserIniChoice(wxCommandEvent&) {
        const int selection = browserIniChoice_ == nullptr
            ? wxNOT_FOUND
            : browserIniChoice_->GetSelection();
        if (selection == wxNOT_FOUND ||
            static_cast<std::size_t>(selection) >= browserIniPaths_.size()) {
            return;
        }
        selectedIniRelative_ = browserIniPaths_[static_cast<std::size_t>(selection)];
        iniPath_->ChangeValue(toWx(selectedIniRelative_));
        iniPath_->SetInsertionPointEnd();
    }
#endif

    bool selectIniPath() {
#if defined(__EMSCRIPTEN__)
        if (browserDirectoryRequestPending_) return false;
        browserDirectoryRequestPending_ = true;
        browserDirectoryLabel_->SetLabel("Selecting and scanning installer folder...");
        updateModeControls();
        wxWeakRef<PatcherOutputDialog> weakThis(this);
        neobrowser::requestPackageDirectory(
            [weakThis](neobrowser::PackageDirectoryResult result) mutable {
                if (!weakThis) return;
                weakThis->completeBrowserDirectory(std::move(result));
            });
        return false;
#else
        const auto selected = choosePatcherIniFile(
            this,
            "Select an existing installer INI or enter a new INI filename",
            selectedIni_.empty() ? initialDirectory_ : selectedIni_.parent_path(),
            selectedIni_.empty()
                ? defaultFile_
                : neosettings::pathToUtf8(selectedIni_.filename()));
        if (!selected) return false;
        selectedIni_ = *selected;
        iniPath_->SetValue(neosettings::pathToWx(selectedIni_));
        iniPath_->SetInsertionPointEnd();
        return true;
#endif
    }

    void onBrowse(wxCommandEvent&) {
        (void)selectIniPath();
    }

    void onAccept(wxCommandEvent&) {
        if (writeToIni_ != nullptr && writeToIni_->GetValue()) {
#if defined(__EMSCRIPTEN__)
            if (browserDirectoryRequestPending_) {
                wxMessageBox(
                    "Wait for installer-folder selection to finish.",
                    "Installer Folder Selection",
                    wxOK | wxICON_INFORMATION,
                    this);
                return;
            }
            if (browserDirectorySession_ == 0) {
                wxMessageBox(
                    "Write to INI requires an installer folder. Select the folder containing the target INI, or the folder in which a new INI should be created.",
                    "Select Installer Folder",
                    wxOK | wxICON_INFORMATION,
                    this);
                (void)selectIniPath();
                return;
            }
            try {
                selectedIniRelative_ = neobrowser::normalizePackageRelativePath(
                    toStd(iniPath_->GetValue()), true);
                iniPath_->ChangeValue(toWx(selectedIniRelative_));
            } catch (const std::exception& exception) {
                wxMessageBox(
                    toWx(exception.what()),
                    "Invalid Installer INI Path",
                    wxOK | wxICON_ERROR,
                    this);
                return;
            }
#else
            if (selectedIni_.empty()) {
                wxMessageBox(
                    "Write to INI requires a specific installer .ini file. Select an existing INI or enter a new INI filename.",
                    "Select Installer INI",
                    wxOK | wxICON_INFORMATION,
                    this);
                if (!selectIniPath()) return;
            }
#endif
        }
        EndModal(wxID_OK);
    }

    std::filesystem::path initialDirectory_;
    std::string defaultFile_;
    std::filesystem::path selectedIni_;
    wxRadioButton* writeToIni_ = nullptr;
    wxRadioButton* fragment_ = nullptr;
    wxTextCtrl* iniPath_ = nullptr;
    wxButton* browseButton_ = nullptr;
#if defined(__EMSCRIPTEN__)
    bool browserPackageWriteEnabled_ = false;
    bool browserDirectorySupported_ = false;
    bool browserDirectoryRequestPending_ = false;
    std::uint32_t browserDirectorySession_ = 0;
    std::string browserDirectoryName_;
    std::string selectedIniRelative_;
    std::vector<std::string> browserIniPaths_;
    wxStaticText* browserDirectoryLabel_ = nullptr;
    wxChoice* browserIniChoice_ = nullptr;
#endif
};
// This is the only selection entry point automatic patch exporters should use.
// It always presents both output modes and, for Write to INI, returns an exact
// .ini path rather than a package directory.
inline std::optional<PatcherOutputSelection> choosePatcherOutput(
    wxWindow* parent,
    const std::filesystem::path& initialDirectory = {},
    const std::string& defaultFile = "changes.ini",
    bool allowBrowserPackageWrite = false) {
    PatcherOutputDialog dialog(
        parent, initialDirectory, defaultFile, allowBrowserPackageWrite);
    if (dialog.ShowModal() != wxID_OK) return std::nullopt;
    return dialog.selection();
}

// Deliberately removed: a mode-only chooser allowed callers to fall back to a
// directory-based package writer. Automatic exporters must obtain the mode and
// exact INI path together through choosePatcherOutput().
inline std::optional<PatcherOutputMode> choosePatcherOutputMode(wxWindow*) = delete;

inline std::vector<std::string> patchProjectAssetNames(const neotsl::PatchProject& project) {
    std::vector<std::string> result;
    for (const auto& asset : project.assets) {
        if (asset.targetName.empty()) continue;
        if (std::find(result.begin(), result.end(), asset.targetName) == result.end()) {
            result.push_back(asset.targetName);
        }
    }
    return result;
}

class IniFragmentDialog final : public wxDialog {
public:
    IniFragmentDialog(wxWindow* parent,
                      const std::string& title,
                      const neotsl::PatchProject& project,
                      const std::vector<std::string>& companionFiles = {})
        : wxDialog(parent,
                   wxID_ANY,
                   toWx(title),
                   wxDefaultPosition,
                   wxDefaultSize,
                   wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
          fragment_(neotsl::writeIniFragmentText(project)) {
        auto* root = new wxBoxSizer(wxVERTICAL);

        auto* instructions = new wxStaticText(
            this,
            wxID_ANY,
            "Review the generated fragment below. Copy it to the clipboard, or save this exact text as a new INI file. Fragment mode does not merge into an existing INI or create companion package files.");
        instructions->Wrap(FromDIP(720));
        root->Add(instructions, 0, wxEXPAND | wxALL, FromDIP(10));

        if (!companionFiles.empty()) {
            std::ostringstream note;
            note << "Companion package files referenced by this fragment are not created when the fragment is saved";
            if (companionFiles.size() > 8u) {
                note << " (" << companionFiles.size() << " files; first 8 shown)";
            }
            note << ": ";
            const std::size_t shown = std::min<std::size_t>(companionFiles.size(), 8u);
            for (std::size_t index = 0; index < shown; ++index) {
                if (index != 0u) note << ", ";
                note << companionFiles[index];
            }
            auto* companion = new wxStaticText(this, wxID_ANY, toWx(note.str()));
            companion->Wrap(FromDIP(720));
            root->Add(companion, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));
        }

        text_ = new wxTextCtrl(
            this,
            wxID_ANY,
            toWx(fragment_),
            wxDefaultPosition,
            wxDefaultSize,
            wxTE_MULTILINE | wxTE_READONLY | wxTE_DONTWRAP | wxHSCROLL);
        wxFont font(wxFontInfo(10).Family(wxFONTFAMILY_TELETYPE));
        text_->SetFont(font);
        root->Add(text_, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));

        auto* buttons = new wxBoxSizer(wxHORIZONTAL);
        saveButton_ = new wxButton(this, wxID_ANY, "Save as New INI...");
        copyButton_ = new wxButton(this, wxID_ANY, "Copy to Clipboard");
        auto* closeButton = new wxButton(this, wxID_CANCEL, "Close");
        buttons->AddStretchSpacer(1);
        buttons->Add(saveButton_, 0, wxRIGHT, FromDIP(8));
        buttons->Add(copyButton_, 0, wxRIGHT, FromDIP(8));
        buttons->Add(closeButton, 0);
        root->Add(buttons, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));

        SetSizer(root);
        neowindow::configureResponsiveWindow(*this, wxSize(860, 620), wxSize(520, 360));
        applyTheme(this, currentDarkMode());

        saveButton_->Bind(wxEVT_BUTTON, &IniFragmentDialog::onSave, this);
        copyButton_->Bind(wxEVT_BUTTON, &IniFragmentDialog::onCopy, this);
    }

private:
    void onSave(wxCommandEvent&) {
        for (;;) {
            const auto output = choosePatcherIniFile(
                this,
                "Save INI fragment as a new file",
                lastSaveDirectory_,
                "fragment.ini");
            if (!output) return;

            try {
                // Save the exact string shown in the preview rather than
                // regenerating it from the project.
                const auto report = neotsl::writeFragmentText(fragment_, *output);
#if defined(__EMSCRIPTEN__)
                if (!publishBrowserFile(report.iniPath, report.iniPath.filename().string())) {
                    throw std::runtime_error("The browser could not download the generated INI fragment.");
                }
#endif
                lastSaveDirectory_ = report.iniPath.parent_path();
                saveButton_->SetLabel("Save Another INI...");
#if defined(__EMSCRIPTEN__)
                wxMessageBox(
                    "Downloaded the generated fragment as:\n" + neosettings::pathToWx(report.iniPath.filename()),
                    "Fragment Downloaded",
                    wxOK | wxICON_INFORMATION,
                    this);
#else
                wxMessageBox(
                    "Saved the generated fragment to:\n" + neosettings::pathToWx(report.iniPath),
                    "Fragment Saved",
                    wxOK | wxICON_INFORMATION,
                    this);
#endif
                return;
            } catch (const std::exception& ex) {
                wxMessageBox(
                    toWx(ex.what()),
                    "Unable to Save Fragment",
                    wxOK | wxICON_ERROR,
                    this);
                // An existing path is not overwritten in Fragment mode. Let the
                // author choose another new filename without closing the preview.
            }
        }
    }

    void onCopy(wxCommandEvent&) {
        if (wxTheClipboard == nullptr || !wxTheClipboard->Open()) {
            wxMessageBox(
                "The clipboard could not be opened.",
                "Clipboard Error",
                wxOK | wxICON_ERROR,
                this);
            return;
        }

        const bool copied = wxTheClipboard->SetData(new wxTextDataObject(toWx(fragment_)));
        if (copied) wxTheClipboard->Flush();
        wxTheClipboard->Close();

        if (!copied) {
            wxMessageBox(
                "The INI fragment could not be copied to the clipboard.",
                "Clipboard Error",
                wxOK | wxICON_ERROR,
                this);
            return;
        }

        copyButton_->SetLabel("Copy Again");
    }

    std::string fragment_;
    std::filesystem::path lastSaveDirectory_;
    wxTextCtrl* text_ = nullptr;
    wxButton* saveButton_ = nullptr;
    wxButton* copyButton_ = nullptr;
};

inline void showIniFragmentDialog(wxWindow* parent,
                                  const std::string& title,
                                  const neotsl::PatchProject& project,
                                  const std::vector<std::string>& additionalCompanionFiles = {}) {
    std::vector<std::string> companionFiles = patchProjectAssetNames(project);
    for (const auto& name : additionalCompanionFiles) {
        if (name.empty()) continue;
        if (std::find(companionFiles.begin(), companionFiles.end(), name) == companionFiles.end()) {
            companionFiles.push_back(name);
        }
    }
    IniFragmentDialog dialog(parent, title, project, companionFiles);
    dialog.ShowModal();
}

} // namespace wxui
