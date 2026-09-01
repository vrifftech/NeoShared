#if defined(__EMSCRIPTEN__)

#define NEOSHARED_WASM_DIALOG_NO_REMAP
#include <neoshared/wasm_dialog_compat.h>

#include <emscripten.h>

#include <algorithm>
#include <cstdlib>
#include <string>
#include <utility>

namespace {

std::string Utf8(const wxString& value) {
    const wxScopedCharBuffer bytes = value.utf8_str();
    return bytes ? std::string(bytes.data()) : std::string();
}

wxString FromUtf8(const char* value) {
    return value ? wxString::FromUTF8(value) : wxString();
}

wxArrayString SplitPaths(const char* value) {
    wxArrayString result;
    if (!value || !*value) return result;
    std::string text(value);
    std::size_t begin = 0;
    while (begin <= text.size()) {
        const std::size_t end = text.find('\n', begin);
        const std::string item = text.substr(
            begin, end == std::string::npos ? std::string::npos : end - begin);
        if (!item.empty()) result.Add(wxString::FromUTF8(item));
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return result;
}

EM_JS(void, neo_wasm_io_init, (), {
    if (globalThis.NeoWasmIO === Module.neoToolsBrowserFiles) return;
    if (!Module.neoToolsBrowserFiles) {
        console.error('[NeoTools] Shared browser-file bridge is unavailable.');
        globalThis.NeoWasmIO = null;
        return;
    }
    // Compatibility dialogs delegate to the canonical NeoBrowserFiles bridge.
    // No second file map, timer set, FS hook, or download implementation exists.
    globalThis.NeoWasmIO = Module.neoToolsBrowserFiles;
});

EM_ASYNC_JS(char*, neo_wasm_pick_files, (const char* wildcardPtr, int multiple), {
    const state = Module.neoToolsBrowserFiles || null;
    globalThis.NeoWasmIO = state;
    const wildcard = UTF8ToString(wildcardPtr || 0);

    function resultString(paths) {
        const text = (paths || []).join(String.fromCharCode(10));
        const size = lengthBytesUTF8(text) + 1;
        const ptr = _malloc(size);
        stringToUTF8(text, ptr, size);
        return ptr;
    }

    function browserAccept(value) {
        const result = [];
        const regex = new RegExp('[*][.]([A-Za-z0-9_+-]+)', 'g');
        let match;
        while ((match = regex.exec(value)) !== null) {
            const extension = '.' + match[1].toLowerCase();
            if (!result.includes(extension)) result.push(extension);
        }
        return result.join(',');
    }

    if (!state || typeof state.chooseAndImportFiles !== 'function') {
        console.error('[NeoTools] Shared file picker is unavailable.');
        return resultString([]);
    }
    try {
        const paths = await state.chooseAndImportFiles({
            accept: browserAccept(wildcard),
            multiple: multiple !== 0
        });
        return resultString(paths || []);
    } catch (error) {
        console.error('[NeoTools] Browser file selection failed:', error);
        return resultString([]);
    }
});

EM_ASYNC_JS(char*, neo_wasm_pick_save,
            (const char* suggestedPtr, const char* wildcardPtr), {
    const state = Module.neoToolsBrowserFiles || null;
    globalThis.NeoWasmIO = state;
    const suggested = UTF8ToString(suggestedPtr || 0) || 'download.bin';
    const wildcard = UTF8ToString(wildcardPtr || 0);

    function resultString(path) {
        const text = String(path || '');
        const size = lengthBytesUTF8(text) + 1;
        const ptr = _malloc(size);
        stringToUTF8(text, ptr, size);
        return ptr;
    }

    function pickerTypes() {
        const extensions = [];
        const regex = new RegExp('[*][.]([A-Za-z0-9_+-]+)', 'g');
        let match;
        while ((match = regex.exec(wildcard)) !== null) {
            const extension = '.' + match[1].toLowerCase();
            if (!extensions.includes(extension)) extensions.push(extension);
        }
        return extensions.length ? [{
            description: 'Supported files',
            accept: { 'application/octet-stream': extensions }
        }] : undefined;
    }

    if (!state || typeof state.createWritableFile !== 'function') {
        console.error('[NeoTools] Shared save-file bridge is unavailable.');
        return resultString('');
    }

    let handle = null;
    let chosenName = suggested;
    if (typeof window.showSaveFilePicker === 'function') {
        try {
            const options = { suggestedName: suggested };
            const types = pickerTypes();
            if (types) options.types = types;
            handle = await window.showSaveFilePicker(options);
            chosenName = handle.name || suggested;
        } catch (error) {
            if (error && error.name === 'AbortError') return resultString('');
            console.warn('[NeoTools] Native save picker failed; using a download name prompt.', error);
        }
    }
    if (!handle) {
        const response = window.prompt('Save file as', chosenName);
        if (response === null) return resultString('');
        chosenName = response || chosenName;
    }

    try {
        return resultString(state.createWritableFile(chosenName, handle));
    } catch (error) {
        console.error('[NeoTools] Unable to prepare the browser output file:', error);
        return resultString('');
    }
});

EM_ASYNC_JS(char*, neo_wasm_pick_directory, (const char* messagePtr, int allowCreate), {
    const state = Module.neoToolsBrowserFiles || null;
    globalThis.NeoWasmIO = state;
    const message = UTF8ToString(messagePtr || 0);

    function resultString(path) {
        const text = String(path || '');
        const size = lengthBytesUTF8(text) + 1;
        const ptr = _malloc(size);
        stringToUTF8(text, ptr, size);
        return ptr;
    }

    if (!state || typeof state.chooseAndImportDirectory !== 'function') {
        console.error('[NeoTools] Shared directory picker is unavailable.');
        return resultString('');
    }
    try {
        const path = await state.chooseAndImportDirectory({
            title: message,
            allowCreate: allowCreate !== 0
        });
        return resultString(path || '');
    } catch (error) {
        console.error('[NeoTools] Browser directory selection failed:', error);
        return resultString('');
    }
});

EM_JS(void, neo_wasm_publish_file, (const char* pathPtr), {
    const state = Module.neoToolsBrowserFiles || null;
    globalThis.NeoWasmIO = state;
    const path = UTF8ToString(pathPtr || 0);
    if (state && path && typeof state.scheduleWritablePath === 'function') {
        state.scheduleWritablePath(path);
    }
});

EM_JS(void, neo_wasm_register_output_file, (const char* pathPtr, const char* namePtr), {
    const state = Module.neoToolsBrowserFiles || null;
    globalThis.NeoWasmIO = state;
    const path = UTF8ToString(pathPtr || 0);
    if (!state || !path) return;
    const directory = PATH.dirname(path);
    try { FS.mkdirTree(directory); } catch (_) {}
    state.registerWritablePath(
        path, UTF8ToString(namePtr || 0) || PATH.basename(path), null);
    // Preserve any selected directory handle while allowing sibling sidecars.
    state.registerWritableDirectory(directory, null);
});

EM_JS(void, neo_wasm_register_output_directory, (const char* pathPtr), {
    const state = Module.neoToolsBrowserFiles || null;
    globalThis.NeoWasmIO = state;
    const path = UTF8ToString(pathPtr || 0);
    if (!state || !path) return;
    try { FS.mkdirTree(path); } catch (_) {}
    state.registerWritableDirectory(path, null);
});

EM_JS(void, neo_wasm_release_path, (const char* pathPtr), {
    const state = Module.neoToolsBrowserFiles || null;
    globalThis.NeoWasmIO = state;
    const path = UTF8ToString(pathPtr || 0);
    if (state && path && typeof state.releasePath === 'function') state.releasePath(path);
});

EM_JS(void, neo_wasm_release_directory, (const char* pathPtr), {
    const state = Module.neoToolsBrowserFiles || null;
    globalThis.NeoWasmIO = state;
    const path = UTF8ToString(pathPtr || 0);
    if (state && path && typeof state.releaseDirectory === 'function') {
        state.releaseDirectory(path);
    }
});

}  // namespace

extern "C" EMSCRIPTEN_KEEPALIVE void neo_wasm_dialog_compat_link_anchor() {}

namespace neoshared::wasm {

FileDialog::FileDialog(wxWindow* parent,
                       const wxString& message,
                       const wxString& defaultDir,
                       const wxString& defaultFile,
                       const wxString& wildcard,
                       long style,
                       const wxPoint& pos,
                       const wxSize& size,
                       const wxString& name)
    : wxDialog(),
      message_(message),
      directory_(defaultDir),
      filename_(defaultFile),
      wildcard_(wildcard),
      style_(style) {
    (void)parent;
    (void)pos;
    (void)size;
    (void)name;
}

int FileDialog::ShowModal() {
    neo_wasm_io_init();
    paths_.Clear();

    char* raw = nullptr;
    if ((style_ & wxFD_SAVE) != 0) {
        wxString suggested = filename_;
        if (suggested.empty()) {
            suggested = "download.bin";
        }
        const std::string suggestedUtf8 = Utf8(suggested);
        const std::string wildcardUtf8 = Utf8(wildcard_);
        raw = neo_wasm_pick_save(suggestedUtf8.c_str(), wildcardUtf8.c_str());
        if (raw && *raw) paths_.Add(FromUtf8(raw));
    } else {
        const std::string wildcardUtf8 = Utf8(wildcard_);
        raw = neo_wasm_pick_files(wildcardUtf8.c_str(), (style_ & wxFD_MULTIPLE) != 0 ? 1 : 0);
        paths_ = SplitPaths(raw);
    }
    std::free(raw);

    if (paths_.IsEmpty()) {
        return wxID_CANCEL;
    }
    const wxFileName selected(paths_[0]);
    directory_ = selected.GetPath();
    filename_ = selected.GetFullName();
    return wxID_OK;
}

wxString FileDialog::GetPath() const { return paths_.IsEmpty() ? wxString() : paths_[0]; }
void FileDialog::GetPaths(wxArrayString& paths) const { paths = paths_; }
wxString FileDialog::GetFilename() const { return filename_; }
void FileDialog::GetFilenames(wxArrayString& files) const {
    files.Clear();
    for (const wxString& path : paths_) files.Add(wxFileName(path).GetFullName());
}
wxString FileDialog::GetDirectory() const { return directory_; }
int FileDialog::GetFilterIndex() const { return filterIndex_; }
void FileDialog::SetPath(const wxString& path) {
    paths_.Clear();
    if (!path.empty()) paths_.Add(path);
    const wxFileName file(path);
    directory_ = file.GetPath();
    filename_ = file.GetFullName();
    if ((style_ & wxFD_SAVE) != 0 && !path.empty()) {
        RegisterOutputFile(path, filename_);
    }
}
void FileDialog::SetDirectory(const wxString& directory) { directory_ = directory; }
void FileDialog::SetFilename(const wxString& filename) { filename_ = filename; }
void FileDialog::SetWildcard(const wxString& wildcard) { wildcard_ = wildcard; }
void FileDialog::SetFilterIndex(int filterIndex) { filterIndex_ = std::max(0, filterIndex); }
void FileDialog::SetMessage(const wxString& message) { message_ = message; }

DirDialog::DirDialog(wxWindow* parent,
                     const wxString& message,
                     const wxString& defaultPath,
                     long style,
                     const wxPoint& pos,
                     const wxSize& size,
                     const wxString& name)
    : wxDialog(), message_(message), path_(defaultPath), style_(style) {
    (void)parent;
    (void)pos;
    (void)size;
    (void)name;
}

int DirDialog::ShowModal() {
    neo_wasm_io_init();
    const std::string messageUtf8 = Utf8(message_);
    const bool allowCreate = (style_ & wxDD_DIR_MUST_EXIST) == 0;
    char* raw = neo_wasm_pick_directory(messageUtf8.c_str(), allowCreate ? 1 : 0);
    path_ = FromUtf8(raw);
    std::free(raw);
    return path_.empty() ? wxID_CANCEL : wxID_OK;
}
wxString DirDialog::GetPath() const { return path_; }
void DirDialog::SetPath(const wxString& path) {
    path_ = path;
    if ((style_ & wxDD_DIR_MUST_EXIST) == 0 && !path_.empty()) {
        RegisterOutputDirectory(path_);
    }
}
void DirDialog::SetMessage(const wxString& message) { message_ = message; }

FilePickerCtrl::FilePickerCtrl(wxWindow* parent,
                               wxWindowID id,
                               const wxString& path,
                               const wxString& message,
                               const wxString& wildcard,
                               const wxPoint& pos,
                               const wxSize& size,
                               long style,
                               const wxValidator& validator,
                               const wxString& name) {
    Create(parent, id, path, message, wildcard, pos, size, style, validator, name);
}

bool FilePickerCtrl::Create(wxWindow* parent,
                            wxWindowID id,
                            const wxString& path,
                            const wxString& message,
                            const wxString& wildcard,
                            const wxPoint& pos,
                            const wxSize& size,
                            long style,
                            const wxValidator& validator,
                            const wxString& name) {
    if (!wxPanel::Create(parent, id, pos, size, wxTAB_TRAVERSAL, name)) return false;
    message_ = message;
    wildcard_ = wildcard;
    pickerStyle_ = style;
    auto* sizer = new wxBoxSizer(wxHORIZONTAL);
    text_ = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
                           wxTE_READONLY, validator);
    browse_ = new wxButton(this, wxID_ANY, "Browse...");
    sizer->Add(text_, 1, wxEXPAND | wxRIGHT, 4);
    sizer->Add(browse_, 0, wxEXPAND);
    SetSizer(sizer);
    browse_->Bind(wxEVT_BUTTON, &FilePickerCtrl::OnBrowse, this);
    SetPath(path);
    return true;
}

wxString FilePickerCtrl::GetPath() const { return text_ ? text_->GetValue() : wxString(); }
void FilePickerCtrl::SetPath(const wxString& path) {
    if (text_) text_->ChangeValue(path);
    if ((pickerStyle_ & wxFLP_SAVE) != 0 && !path.empty()) {
        RegisterOutputFile(path, wxFileName(path).GetFullName());
    }
}
void FilePickerCtrl::SetInitialDirectory(const wxString& directory) { initialDirectory_ = directory; }
wxTextCtrl* FilePickerCtrl::GetTextCtrl() const { return text_; }
void FilePickerCtrl::OnBrowse(wxCommandEvent&) {
    long dialogStyle = (pickerStyle_ & wxFLP_SAVE) ? wxFD_SAVE | wxFD_OVERWRITE_PROMPT : wxFD_OPEN | wxFD_FILE_MUST_EXIST;
    FileDialog dialog(this, message_, initialDirectory_, wxFileName(GetPath()).GetFullName(), wildcard_, dialogStyle);
    if (dialog.ShowModal() != wxID_OK) return;
    SetPath(dialog.GetPath());
    wxFileDirPickerEvent changed(wxEVT_FILEPICKER_CHANGED, this, GetId(), GetPath());
    ProcessWindowEvent(changed);
}

DirPickerCtrl::DirPickerCtrl(wxWindow* parent,
                             wxWindowID id,
                             const wxString& path,
                             const wxString& message,
                             const wxPoint& pos,
                             const wxSize& size,
                             long style,
                             const wxValidator& validator,
                             const wxString& name) {
    Create(parent, id, path, message, pos, size, style, validator, name);
}

bool DirPickerCtrl::Create(wxWindow* parent,
                           wxWindowID id,
                           const wxString& path,
                           const wxString& message,
                           const wxPoint& pos,
                           const wxSize& size,
                           long style,
                           const wxValidator& validator,
                           const wxString& name) {
    if (!wxPanel::Create(parent, id, pos, size, wxTAB_TRAVERSAL, name)) return false;
    message_ = message;
    initialDirectory_ = path;
    pickerStyle_ = style;
    auto* sizer = new wxBoxSizer(wxHORIZONTAL);
    text_ = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
                           wxTE_READONLY, validator);
    browse_ = new wxButton(this, wxID_ANY, "Browse...");
    sizer->Add(text_, 1, wxEXPAND | wxRIGHT, 4);
    sizer->Add(browse_, 0, wxEXPAND);
    SetSizer(sizer);
    browse_->Bind(wxEVT_BUTTON, &DirPickerCtrl::OnBrowse, this);
    SetPath(path);
    return true;
}

wxString DirPickerCtrl::GetPath() const { return text_ ? text_->GetValue() : wxString(); }
void DirPickerCtrl::SetPath(const wxString& path) {
    if (text_) text_->ChangeValue(path);
    if ((pickerStyle_ & wxDIRP_DIR_MUST_EXIST) == 0 && !path.empty()) {
        RegisterOutputDirectory(path);
    }
}
void DirPickerCtrl::SetInitialDirectory(const wxString& directory) { initialDirectory_ = directory; }
wxTextCtrl* DirPickerCtrl::GetTextCtrl() const { return text_; }
void DirPickerCtrl::OnBrowse(wxCommandEvent&) {
    long dialogStyle = wxDD_DEFAULT_STYLE;
    if ((pickerStyle_ & wxDIRP_DIR_MUST_EXIST) != 0) dialogStyle |= wxDD_DIR_MUST_EXIST;
    DirDialog dialog(this, message_, GetPath().empty() ? initialDirectory_ : GetPath(), dialogStyle);
    if (dialog.ShowModal() != wxID_OK) return;
    SetPath(dialog.GetPath());
    wxFileDirPickerEvent changed(wxEVT_DIRPICKER_CHANGED, this, GetId(), GetPath());
    ProcessWindowEvent(changed);
}

wxString FileSelector(const wxString& message,
                      const wxString& defaultPath,
                      const wxString& defaultFilename,
                      const wxString& defaultExtension,
                      const wxString& wildcard,
                      int flags,
                      wxWindow* parent,
                      int x,
                      int y) {
    (void)x;
    (void)y;
    wxString filename = defaultFilename;
    if (!defaultExtension.empty() && !filename.empty() && wxFileName(filename).GetExt().empty()) {
        filename += "." + defaultExtension;
    }
    FileDialog dialog(parent, message, defaultPath, filename, wildcard, flags);
    return dialog.ShowModal() == wxID_OK ? dialog.GetPath() : wxString();
}

wxString FileSelectorEx(const wxString& message,
                        const wxString& defaultPath,
                        const wxString& defaultFilename,
                        int* defaultExtension,
                        const wxString& wildcard,
                        int flags,
                        wxWindow* parent,
                        int x,
                        int y) {
    FileDialog dialog(parent, message, defaultPath, defaultFilename, wildcard, flags,
                      wxPoint(x, y));
    if (defaultExtension) dialog.SetFilterIndex(*defaultExtension);
    if (dialog.ShowModal() != wxID_OK) return wxString();
    if (defaultExtension) *defaultExtension = dialog.GetFilterIndex();
    return dialog.GetPath();
}

wxString LoadFileSelector(const wxString& what,
                          const wxString& extension,
                          const wxString& defaultName,
                          wxWindow* parent) {
    const wxString wildcard = extension.empty() ? wxFileSelectorDefaultWildcardStr
                                                 : wxString::Format("*.%s", extension);
    return FileSelector("Load " + what, wxEmptyString, defaultName, extension, wildcard,
                        wxFD_OPEN | wxFD_FILE_MUST_EXIST, parent);
}

wxString SaveFileSelector(const wxString& what,
                          const wxString& extension,
                          const wxString& defaultName,
                          wxWindow* parent) {
    const wxString wildcard = extension.empty() ? wxFileSelectorDefaultWildcardStr
                                                 : wxString::Format("*.%s", extension);
    return FileSelector("Save " + what, wxEmptyString, defaultName, extension, wildcard,
                        wxFD_SAVE | wxFD_OVERWRITE_PROMPT, parent);
}

wxString DirSelector(const wxString& message,
                     const wxString& defaultPath,
                     long style,
                     const wxPoint& pos,
                     wxWindow* parent) {
    DirDialog dialog(parent, message, defaultPath, style, pos);
    return dialog.ShowModal() == wxID_OK ? dialog.GetPath() : wxString();
}

void PublishFile(const wxString& path) {
    if (path.empty()) return;
    RegisterOutputFile(path, wxFileName(path).GetFullName());
    const std::string pathUtf8 = Utf8(path);
    neo_wasm_publish_file(pathUtf8.c_str());
}

void RegisterOutputFile(const wxString& path, const wxString& downloadName) {
    if (path.empty()) return;
    const std::string pathUtf8 = Utf8(path);
    const std::string nameUtf8 = Utf8(
        downloadName.empty() ? wxFileName(path).GetFullName() : downloadName);
    neo_wasm_register_output_file(pathUtf8.c_str(), nameUtf8.c_str());
}

void RegisterOutputDirectory(const wxString& path) {
    if (path.empty()) return;
    const std::string pathUtf8 = Utf8(path);
    neo_wasm_register_output_directory(pathUtf8.c_str());
}

void ReleasePath(const wxString& path) {
    if (path.empty()) return;
    const std::string pathUtf8 = Utf8(path);
    neo_wasm_release_path(pathUtf8.c_str());
}

void ReleaseDirectory(const wxString& path) {
    if (path.empty()) return;
    const std::string pathUtf8 = Utf8(path);
    neo_wasm_release_directory(pathUtf8.c_str());
}

}  // namespace neoshared::wasm

#endif  // __EMSCRIPTEN__
