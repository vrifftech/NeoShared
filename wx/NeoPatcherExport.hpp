#pragma once

#include "NeoWindowPlacement.hpp"
#include "NeoWxUi.hpp"
#include "TslPatcher.hpp"

#include <wx/button.h>
#include <wx/clipbrd.h>
#include <wx/dataobj.h>
#include <wx/dialog.h>
#include <wx/radiobut.h>
#include <wx/statbox.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include <algorithm>
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
inline constexpr unsigned kPatcherExportUiApiVersion = 3u;

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
    // Set only for WriteToIni. Fragment deliberately has no destination before
    // the preview is generated.
    std::filesystem::path iniPath;

    bool writesToIni() const noexcept {
        return mode == PatcherOutputMode::WriteToIni;
    }
};

class PatcherOutputDialog final : public wxDialog {
public:
    PatcherOutputDialog(wxWindow* parent,
                        const std::filesystem::path& initialDirectory = {},
                        const std::string& defaultFile = "changes.ini")
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
        writeToIni_->SetValue(true);
        root->Add(writeToIni_, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(12));

        auto* writeDescription = new wxStaticText(
            this,
            wxID_ANY,
            "Select an exact existing or new .ini file. Generated instructions are merged into that file, and required payloads are staged beside it.");
        writeDescription->Wrap(FromDIP(660));
        root->Add(writeDescription, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(32));

        auto* iniBox = new wxStaticBoxSizer(wxVERTICAL, this, "Installer INI");
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
        root->Add(iniBox, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));

        fragment_ = new wxRadioButton(
            this,
            wxID_ANY,
            "Fragment (preview, copy, or save as a new INI)");
        root->Add(fragment_, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(12));

#if defined(__EMSCRIPTEN__)
        // The wxWidgets browser port can import or download individual files,
        // but it cannot yet perform one atomic installer-directory transaction.
        // A Write-to-INI export could otherwise download the INI while silently
        // omitting companion payloads. Keep the audited text-only Fragment flow
        // available and make the package-aware desktop boundary explicit.
        writeToIni_->SetValue(false);
        writeToIni_->Enable(false);
        fragment_->SetValue(true);
        auto* browserNote = new wxStaticText(
            this,
            wxID_ANY,
            "Write to INI is disabled in the browser build because an installer INI and its companion payload files must be updated together. Use Fragment here, or use a desktop build for package-aware Write to INI output.");
        browserNote->Wrap(FromDIP(660));
        root->Add(browserNote, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(32));
#endif

        auto* fragmentDescription = new wxStaticText(
            this,
            wxID_ANY,
            "No destination is requested before generation. Fragment mode shows the exact text, creates no payloads, omits [Settings], and never merges into or overwrites an existing INI.");
        fragmentDescription->Wrap(FromDIP(660));
        root->Add(fragmentDescription, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(32));

        auto* buttons = CreateStdDialogButtonSizer(wxOK | wxCANCEL);
        root->Add(buttons, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));

        SetSizer(root);
        root->Fit(this);
        neowindow::configureResponsiveWindow(*this, wxSize(760, 430), wxSize(520, 340));
        applyTheme(this, currentDarkMode());

        writeToIni_->Bind(wxEVT_RADIOBUTTON, &PatcherOutputDialog::onModeChanged, this);
        fragment_->Bind(wxEVT_RADIOBUTTON, &PatcherOutputDialog::onModeChanged, this);
        browseButton_->Bind(wxEVT_BUTTON, &PatcherOutputDialog::onBrowse, this);
        Bind(wxEVT_BUTTON, &PatcherOutputDialog::onAccept, this, wxID_OK);
        updateModeControls();
    }

    PatcherOutputSelection selection() const {
        PatcherOutputSelection result;
        result.mode = fragment_ != nullptr && fragment_->GetValue()
            ? PatcherOutputMode::Fragment
            : PatcherOutputMode::WriteToIni;
        if (result.writesToIni()) result.iniPath = selectedIni_;
        return result;
    }

private:
    void onModeChanged(wxCommandEvent&) {
        updateModeControls();
    }

    void updateModeControls() {
        const bool enabled = writeToIni_ != nullptr && writeToIni_->GetValue();
        if (iniPath_ != nullptr) iniPath_->Enable(enabled);
        if (browseButton_ != nullptr) browseButton_->Enable(enabled);
    }

    bool selectIniPath() {
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
    }

    void onBrowse(wxCommandEvent&) {
        (void)selectIniPath();
    }

    void onAccept(wxCommandEvent&) {
        if (writeToIni_ != nullptr && writeToIni_->GetValue() && selectedIni_.empty()) {
            wxMessageBox(
                "Write to INI requires a specific installer .ini file. Select an existing INI or enter a new INI filename.",
                "Select Installer INI",
                wxOK | wxICON_INFORMATION,
                this);
            if (!selectIniPath()) return;
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
};

// This is the only selection entry point automatic patch exporters should use.
// It always presents both output modes and, for Write to INI, returns an exact
// .ini path rather than a package directory.
inline std::optional<PatcherOutputSelection> choosePatcherOutput(
    wxWindow* parent,
    const std::filesystem::path& initialDirectory = {},
    const std::string& defaultFile = "changes.ini") {
    PatcherOutputDialog dialog(parent, initialDirectory, defaultFile);
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
