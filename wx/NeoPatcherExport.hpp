#pragma once

#include "NeoWindowPlacement.hpp"
#include "NeoWxUi.hpp"
#include "TslPatcher.hpp"

#include <wx/button.h>
#include <wx/clipbrd.h>
#include <wx/dataobj.h>
#include <wx/dialog.h>
#include <wx/radiobut.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include <algorithm>
#include <exception>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace wxui {

enum class PatcherOutputMode {
    WriteToIni,
    Fragment,
};

class PatcherOutputModeDialog final : public wxDialog {
public:
    explicit PatcherOutputModeDialog(wxWindow* parent)
        : wxDialog(parent,
                   wxID_ANY,
                   "Patcher Output",
                   wxDefaultPosition,
                   wxDefaultSize,
                   wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER) {
        auto* root = new wxBoxSizer(wxVERTICAL);
        auto* heading = new wxStaticText(
            this,
            wxID_ANY,
            "Choose what to do with the generated TSLPatcher/HoloPatcher instructions:");
        root->Add(heading, 0, wxEXPAND | wxALL, FromDIP(12));

        writeToIni_ = new wxRadioButton(
            this,
            wxID_ANY,
            "Write to INI",
            wxDefaultPosition,
            wxDefaultSize,
            wxRB_GROUP);
        fragment_ = new wxRadioButton(
            this,
            wxID_ANY,
            "Fragment (copy or save as new INI)");
        writeToIni_->SetValue(true);

        root->Add(writeToIni_, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));
        root->Add(fragment_, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));
        root->Add(CreateStdDialogButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));

        SetSizer(root);
        root->Fit(this);
        neowindow::configureResponsiveWindow(*this, wxSize(560, 220), wxSize(420, 180));
        applyTheme(this, currentDarkMode());
    }

    PatcherOutputMode selectedMode() const noexcept {
        return fragment_ != nullptr && fragment_->GetValue()
            ? PatcherOutputMode::Fragment
            : PatcherOutputMode::WriteToIni;
    }

private:
    wxRadioButton* writeToIni_ = nullptr;
    wxRadioButton* fragment_ = nullptr;
};

inline std::optional<PatcherOutputMode> choosePatcherOutputMode(wxWindow* parent) {
    PatcherOutputModeDialog dialog(parent);
    if (dialog.ShowModal() != wxID_OK) return std::nullopt;
    return dialog.selectedMode();
}

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
          project_(project),
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
                "Save INI fragment as new file",
                {},
                "fragment.ini");
            if (!output) return;

            try {
                const auto report = neotsl::writeFragment(project_, *output);
                saveButton_->SetLabel("Save Another INI...");
                wxMessageBox(
                    "Saved the generated fragment to:\n" + neosettings::pathToWx(report.iniPath),
                    "Fragment Saved",
                    wxOK | wxICON_INFORMATION,
                    this);
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

    const neotsl::PatchProject& project_;
    std::string fragment_;
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
    IniFragmentDialog dialog(
        parent,
        title,
        project,
        companionFiles);
    dialog.ShowModal();
}

} // namespace wxui
