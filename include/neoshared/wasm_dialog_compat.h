#pragma once

#if defined(__EMSCRIPTEN__)

#include <wx/arrstr.h>
#include <wx/button.h>
#include <wx/dirdlg.h>
#include <wx/filedlg.h>
#include <wx/filefn.h>
#include <wx/filepicker.h>
#include <wx/filename.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/textctrl.h>
#include <wx/validate.h>
#include <wx/window.h>

namespace neoshared::wasm {

// Synchronous-looking browser dialogs backed by Emscripten Asyncify. They are
// intended as a compatibility fallback for legacy wxWidgets handlers. New code
// should prefer the explicit callback API used by NeoERF.
class FileDialog final : public wxDialog {
public:
    FileDialog(wxWindow* parent,
               const wxString& message = wxFileSelectorPromptStr,
               const wxString& defaultDir = wxEmptyString,
               const wxString& defaultFile = wxEmptyString,
               const wxString& wildcard = wxFileSelectorDefaultWildcardStr,
               long style = wxFD_DEFAULT_STYLE,
               const wxPoint& pos = wxDefaultPosition,
               const wxSize& size = wxDefaultSize,
               const wxString& name = wxFileDialogNameStr);
    ~FileDialog() override;

    int ShowModal() override;

    wxString GetPath() const;
    void GetPaths(wxArrayString& paths) const;
    wxString GetFilename() const;
    void GetFilenames(wxArrayString& files) const;
    wxString GetDirectory() const;
    int GetFilterIndex() const;

    void SetPath(const wxString& path);
    void SetDirectory(const wxString& directory);
    void SetFilename(const wxString& filename);
    void SetWildcard(const wxString& wildcard);
    void SetFilterIndex(int filterIndex);
    void SetMessage(const wxString& message);

private:
    wxString message_;
    wxString directory_;
    wxString filename_;
    wxString wildcard_;
    long style_{wxFD_DEFAULT_STYLE};
    int filterIndex_{0};
    wxArrayString paths_;
    bool publishOnDestroy_{false};
};

class DirDialog final : public wxDialog {
public:
    DirDialog(wxWindow* parent,
              const wxString& message = wxDirSelectorPromptStr,
              const wxString& defaultPath = wxEmptyString,
              long style = wxDD_DEFAULT_STYLE,
              const wxPoint& pos = wxDefaultPosition,
              const wxSize& size = wxDefaultSize,
              const wxString& name = wxDirDialogNameStr);

    int ShowModal() override;
    wxString GetPath() const;
    void SetPath(const wxString& path);
    void SetMessage(const wxString& message);

private:
    wxString message_;
    wxString path_;
    long style_{wxDD_DEFAULT_STYLE};
};

class FilePickerCtrl final : public wxPanel {
public:
    FilePickerCtrl() = default;
    FilePickerCtrl(wxWindow* parent,
                   wxWindowID id,
                   const wxString& path = wxEmptyString,
                   const wxString& message = wxFileSelectorPromptStr,
                   const wxString& wildcard = wxFileSelectorDefaultWildcardStr,
                   const wxPoint& pos = wxDefaultPosition,
                   const wxSize& size = wxDefaultSize,
                   long style = wxFLP_DEFAULT_STYLE,
                   const wxValidator& validator = wxDefaultValidator,
                   const wxString& name = wxFilePickerCtrlNameStr);

    bool Create(wxWindow* parent,
                wxWindowID id,
                const wxString& path = wxEmptyString,
                const wxString& message = wxFileSelectorPromptStr,
                const wxString& wildcard = wxFileSelectorDefaultWildcardStr,
                const wxPoint& pos = wxDefaultPosition,
                const wxSize& size = wxDefaultSize,
                long style = wxFLP_DEFAULT_STYLE,
                const wxValidator& validator = wxDefaultValidator,
                const wxString& name = wxFilePickerCtrlNameStr);

    wxString GetPath() const;
    void SetPath(const wxString& path);
    void SetInitialDirectory(const wxString& directory);
    wxTextCtrl* GetTextCtrl() const;

private:
    void OnBrowse(wxCommandEvent& event);
    wxTextCtrl* text_{nullptr};
    wxButton* browse_{nullptr};
    wxString message_;
    wxString wildcard_;
    wxString initialDirectory_;
    long pickerStyle_{wxFLP_DEFAULT_STYLE};
};

class DirPickerCtrl final : public wxPanel {
public:
    DirPickerCtrl() = default;
    DirPickerCtrl(wxWindow* parent,
                  wxWindowID id,
                  const wxString& path = wxEmptyString,
                  const wxString& message = wxDirSelectorPromptStr,
                  const wxPoint& pos = wxDefaultPosition,
                  const wxSize& size = wxDefaultSize,
                  long style = wxDIRP_DEFAULT_STYLE,
                  const wxValidator& validator = wxDefaultValidator,
                  const wxString& name = wxDirPickerCtrlNameStr);

    bool Create(wxWindow* parent,
                wxWindowID id,
                const wxString& path = wxEmptyString,
                const wxString& message = wxDirSelectorPromptStr,
                const wxPoint& pos = wxDefaultPosition,
                const wxSize& size = wxDefaultSize,
                long style = wxDIRP_DEFAULT_STYLE,
                const wxValidator& validator = wxDefaultValidator,
                const wxString& name = wxDirPickerCtrlNameStr);

    wxString GetPath() const;
    void SetPath(const wxString& path);
    void SetInitialDirectory(const wxString& directory);
    wxTextCtrl* GetTextCtrl() const;

private:
    void OnBrowse(wxCommandEvent& event);
    wxTextCtrl* text_{nullptr};
    wxButton* browse_{nullptr};
    wxString message_;
    wxString initialDirectory_;
    long pickerStyle_{wxDIRP_DEFAULT_STYLE};
};

wxString FileSelector(const wxString& message = wxFileSelectorPromptStr,
                      const wxString& defaultPath = wxEmptyString,
                      const wxString& defaultFilename = wxEmptyString,
                      const wxString& defaultExtension = wxEmptyString,
                      const wxString& wildcard = wxFileSelectorDefaultWildcardStr,
                      int flags = 0,
                      wxWindow* parent = nullptr,
                      int x = wxDefaultCoord,
                      int y = wxDefaultCoord);

wxString FileSelectorEx(const wxString& message,
                        const wxString& defaultPath = wxEmptyString,
                        const wxString& defaultFilename = wxEmptyString,
                        int* defaultExtension = nullptr,
                        const wxString& wildcard = wxFileSelectorDefaultWildcardStr,
                        int flags = 0,
                        wxWindow* parent = nullptr,
                        int x = wxDefaultCoord,
                        int y = wxDefaultCoord);

wxString LoadFileSelector(const wxString& what,
                          const wxString& extension,
                          const wxString& defaultName = wxEmptyString,
                          wxWindow* parent = nullptr);
wxString SaveFileSelector(const wxString& what,
                          const wxString& extension,
                          const wxString& defaultName = wxEmptyString,
                          wxWindow* parent = nullptr);
wxString DirSelector(const wxString& message = wxDirSelectorPromptStr,
                     const wxString& defaultPath = wxEmptyString,
                     long style = wxDD_DEFAULT_STYLE,
                     const wxPoint& pos = wxDefaultPosition,
                     wxWindow* parent = nullptr);

// Explicitly flush a virtual path to its retained browser handle or a download.
void PublishFile(const wxString& path);

// Register browser output destinations that are supplied programmatically
// rather than selected through the compatibility picker. Re-registering a path
// preserves any real browser file or directory handle already attached to it.
void RegisterOutputFile(const wxString& path,
                        const wxString& downloadName = wxEmptyString);
void RegisterOutputDirectory(const wxString& path);

}  // namespace neoshared::wasm

#ifndef NEOSHARED_WASM_DIALOG_NO_REMAP
#define wxFileDialog ::neoshared::wasm::FileDialog
#define wxDirDialog ::neoshared::wasm::DirDialog
#define wxFilePickerCtrl ::neoshared::wasm::FilePickerCtrl
#define wxDirPickerCtrl ::neoshared::wasm::DirPickerCtrl
#define wxFileSelector ::neoshared::wasm::FileSelector
#define wxFileSelectorEx ::neoshared::wasm::FileSelectorEx
#define wxLoadFileSelector ::neoshared::wasm::LoadFileSelector
#define wxSaveFileSelector ::neoshared::wasm::SaveFileSelector
#define wxDirSelector ::neoshared::wasm::DirSelector
#endif

#endif  // __EMSCRIPTEN__
