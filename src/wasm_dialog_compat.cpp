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
    if (!value || !*value) {
        return result;
    }
    std::string text(value);
    std::size_t begin = 0;
    while (begin <= text.size()) {
        const std::size_t end = text.find('\n', begin);
        const std::string item = text.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
        if (!item.empty()) {
            result.Add(wxString::FromUTF8(item));
        }
        if (end == std::string::npos) {
            break;
        }
        begin = end + 1;
    }
    return result;
}

EM_JS(void, neo_wasm_io_init, (), {
    if (globalThis.NeoWasmIO) return;

    const state = {
        files: new Map(),
        directories: [],
        timers: new Map(),
        sequence: 0,

        cleanName(name) {
            const source = String(name || 'download.bin');
            let cleaned = '';
            for (let index = 0; index < source.length; ++index) {
                const code = source.charCodeAt(index);
                cleaned += (code === 0 || code === 10 || code === 13 ||
                            code === 47 || code === 92) ? '_' : source[index];
            }
            return cleaned || 'download.bin';
        },

        mkdir(path) {
            FS.mkdirTree(path);
        },

        uniqueRoot(prefix) {
            this.sequence += 1;
            return '/' + prefix + '/' + Date.now().toString(36) + '-' + this.sequence.toString(36);
        },

        registerFile(path, handle, name) {
            const existing = this.files.get(path);
            this.files.set(path, {
                handle: handle || (existing ? existing.handle : null),
                name: this.cleanName(name || (existing ? existing.name : PATH.basename(path)))
            });
        },

        registerDirectory(root, handle) {
            root = String(root || '');
            while (root.length > 1 && root.charCodeAt(root.length - 1) === 47) {
                root = root.slice(0, -1);
            }
            if (!root) root = '/';
            const existing = this.directories.find(entry => entry.root === root);
            this.directories = this.directories.filter(entry => entry.root !== root);
            this.directories.push({ root, handle: handle || (existing ? existing.handle : null) });
        },

        matchDirectory(path) {
            let best = null;
            for (const entry of this.directories) {
                if (path === entry.root || path.startsWith(entry.root + '/')) {
                    if (!best || entry.root.length > best.root.length) best = entry;
                }
            }
            return best;
        },

        async directoryFileHandle(entry, relative) {
            let directory = entry.handle;
            const parts = relative.split('/').filter(Boolean);
            const filename = parts.pop();
            for (const part of parts) {
                directory = await directory.getDirectoryHandle(part, { create: true });
            }
            return directory.getFileHandle(filename, { create: true });
        },

        download(path, name) {
            let bytes;
            try { bytes = FS.readFile(path); } catch (_) { return; }
            const copy = bytes.slice ? bytes.slice() : new Uint8Array(bytes);
            if (Module.neoToolsBrowserFiles &&
                typeof Module.neoToolsBrowserFiles.prepareDownloadBytes === 'function') {
                Module.neoToolsBrowserFiles.prepareDownloadBytes(
                    copy, this.cleanName(name || PATH.basename(path)));
                return;
            }
            const blob = new Blob([copy], { type: 'application/octet-stream' });
            const url = URL.createObjectURL(blob);
            const anchor = document.createElement('a');
            anchor.href = url;
            anchor.download = this.cleanName(name || PATH.basename(path));
            anchor.style.display = 'none';
            document.body.appendChild(anchor);
            anchor.click();
            anchor.remove();
            setTimeout(() => URL.revokeObjectURL(url), 1000);
        },

        async flush(path) {
            let bytes;
            try { bytes = FS.readFile(path); } catch (_) { return; }

            const file = this.files.get(path);
            if (file) {
                if (file.handle) {
                    try {
                        const writable = await file.handle.createWritable();
                        await writable.write(bytes);
                        await writable.close();
                        return;
                    } catch (error) {
                        console.warn('Neo WASM: file-handle write failed; downloading instead.', error);
                    }
                }
                this.download(path, file.name);
                return;
            }

            const directory = this.matchDirectory(path);
            if (directory) {
                let relative = path.slice(directory.root.length);
                while (relative.length && relative.charCodeAt(0) === 47) {
                    relative = relative.slice(1);
                }
                if (!relative) return;
                if (directory.handle) {
                    try {
                        const handle = await this.directoryFileHandle(directory, relative);
                        const writable = await handle.createWritable();
                        await writable.write(bytes);
                        await writable.close();
                        return;
                    } catch (error) {
                        console.warn('Neo WASM: directory-handle write failed; downloading instead.', error);
                    }
                }
                this.download(path, PATH.basename(path));
            }
        },

        schedule(path) {
            if (!this.files.has(path) && !this.matchDirectory(path)) return;
            const old = this.timers.get(path);
            if (old) clearTimeout(old);
            this.timers.set(path, setTimeout(() => {
                this.timers.delete(path);
                this.flush(path);
            }, 20));
        },

        consume(path) {
            const timer = this.timers.get(path);
            if (timer) clearTimeout(timer);
            this.timers.delete(path);
        }
    };

    globalThis.NeoWasmIO = state;

    const originalClose = FS.close.bind(FS);
    FS.close = function(stream) {
        let path = '';
        let writable = false;
        try {
            path = stream.path || (stream.node ? FS.getPath(stream.node) : '');
            writable = ((stream.flags || 0) & 3) !== 0;
        } catch (_) {}
        const result = originalClose(stream);
        if (path && writable) state.schedule(path);
        return result;
    };

    const originalRename = FS.rename.bind(FS);
    FS.rename = function(oldPath, newPath) {
        const destination = state.files.get(newPath) || null;
        const source = state.files.get(oldPath) || null;
        const result = originalRename(oldPath, newPath);
        if (destination) state.files.set(newPath, destination);
        else if (source) {
            state.files.delete(oldPath);
            state.files.set(newPath, source);
        }
        state.schedule(newPath);
        return result;
    };
});

EM_ASYNC_JS(char*, neo_wasm_pick_files, (const char* wildcardPtr, int multiple), {
    neo_wasm_io_init();
    const state = globalThis.NeoWasmIO;
    const wildcard = UTF8ToString(wildcardPtr || 0);

    function resultString(paths) {
        const text = paths.join(String.fromCharCode(10));
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
        return extensions.length ? [{ description: 'Supported files', accept: { 'application/octet-stream': extensions } }] : undefined;
    }

    try {
        if (typeof window.showOpenFilePicker === 'function') {
            const handles = await window.showOpenFilePicker({ multiple: !!multiple, types: pickerTypes() });
            const root = state.uniqueRoot('neo-upload');
            state.mkdir(root);
            const paths = [];
            for (const handle of handles) {
                const file = await handle.getFile();
                const name = state.cleanName(file.name);
                const path = root + '/' + name;
                FS.writeFile(path, new Uint8Array(await file.arrayBuffer()));
                state.registerFile(path, handle, name);
                paths.push(path);
            }
            state.registerDirectory(root, null);
            return resultString(paths);
        }
    } catch (error) {
        if (error && error.name === 'AbortError') return resultString([]);
        console.warn('Neo WASM: native open picker failed; using file input.', error);
    }

    const files = await new Promise(resolve => {
        const input = document.createElement('input');
        input.type = 'file';
        input.multiple = !!multiple;
        const extensions = [];
        const regex = new RegExp('[*][.]([A-Za-z0-9_+-]+)', 'g');
        let match;
        while ((match = regex.exec(wildcard)) !== null) extensions.push('.' + match[1]);
        if (extensions.length) input.accept = extensions.join(',');
        input.style.display = 'none';
        document.body.appendChild(input);
        let settled = false;
        const finish = value => {
            if (settled) return;
            settled = true;
            input.remove();
            resolve(value);
        };
        input.addEventListener('change', () => finish(Array.from(input.files || [])), { once: true });
        input.addEventListener('cancel', () => finish([]), { once: true });
        window.addEventListener('focus', () => setTimeout(() => finish(Array.from(input.files || [])), 350), { once: true });
        input.click();
    });

    const root = state.uniqueRoot('neo-upload');
    state.mkdir(root);
    const paths = [];
    for (const file of files) {
        const name = state.cleanName(file.name);
        const path = root + '/' + name;
        FS.writeFile(path, new Uint8Array(await file.arrayBuffer()));
        state.registerFile(path, null, name);
        paths.push(path);
    }
    state.registerDirectory(root, null);
    return resultString(paths);
});

EM_ASYNC_JS(char*, neo_wasm_pick_save, (const char* suggestedPtr, const char* wildcardPtr), {
    neo_wasm_io_init();
    const state = globalThis.NeoWasmIO;
    const wildcard = UTF8ToString(wildcardPtr || 0);

    function resultString(path) {
        const size = lengthBytesUTF8(path) + 1;
        const ptr = _malloc(size);
        stringToUTF8(path, ptr, size);
        return ptr;
    }

    const extensions = [];
    const regex = new RegExp('[*][.]([A-Za-z0-9_+-]+)', 'g');
    let match;
    while ((match = regex.exec(wildcard)) !== null) {
        const extension = '.' + match[1].toLowerCase();
        if (!extensions.includes(extension)) extensions.push(extension);
    }

    function hasExtension(name) {
        const dot = name.lastIndexOf('.');
        return dot > 0 && dot + 1 < name.length;
    }

    function appendDefaultExtension(name) {
        if (!extensions.length || hasExtension(name)) return name;
        return name + extensions[0];
    }

    let suggested = state.cleanName(UTF8ToString(suggestedPtr || 0) || 'download.bin');
    suggested = appendDefaultExtension(suggested);
    let chosenName = suggested;
    let handle = null;
    if (typeof window.showSaveFilePicker === 'function') {
        try {
            const options = { suggestedName: suggested };
            if (extensions.length) {
                options.types = [{ description: 'Supported files', accept: { 'application/octet-stream': extensions } }];
                options.excludeAcceptAllOption = true;
            }
            handle = await window.showSaveFilePicker(options);
            chosenName = state.cleanName(handle && handle.name ? handle.name : suggested);
            if (extensions.length && !hasExtension(chosenName)) {
                window.alert('Please include a filename extension, such as ' + extensions[0] + '.');
                return resultString('');
            }
        } catch (error) {
            if (error && error.name === 'AbortError') return resultString('');
            console.warn('Neo WASM: native save picker failed; using a download.', error);
            handle = null;
        }
    }

    if (!handle) {
        const entered = window.prompt('Save file as', suggested);
        if (entered === null) return resultString('');
        chosenName = appendDefaultExtension(state.cleanName(entered || suggested));
    }

    const root = state.uniqueRoot('neo-save');
    state.mkdir(root);
    const path = root + '/' + chosenName;
    state.registerFile(path, handle, chosenName);
    // A format may emit companion files (for example TPC/TXI). The selected
    // primary file keeps its retained handle; sibling outputs download.
    state.registerDirectory(root, null);
    return resultString(path);
});

EM_ASYNC_JS(char*, neo_wasm_pick_directory, (const char* messagePtr, int allowCreate), {
    neo_wasm_io_init();
    const state = globalThis.NeoWasmIO;
    const message = UTF8ToString(messagePtr || 0).toLowerCase();

    function resultString(path) {
        const size = lengthBytesUTF8(path) + 1;
        const ptr = _malloc(size);
        stringToUTF8(path, ptr, size);
        return ptr;
    }

    async function importDirectory(handle, root) {
        async function walk(directory, destination) {
            for await (const [nameRaw, entry] of directory.entries()) {
                const name = state.cleanName(nameRaw);
                if (entry.kind === 'directory') {
                    const child = destination + '/' + name;
                    state.mkdir(child);
                    await walk(entry, child);
                } else {
                    const file = await entry.getFile();
                    FS.writeFile(destination + '/' + name, new Uint8Array(await file.arrayBuffer()));
                }
            }
        }
        await walk(handle, root);
    }

    if (typeof window.showDirectoryPicker === 'function') {
        try {
            const handle = await window.showDirectoryPicker({ mode: allowCreate ? 'readwrite' : 'read' });
            const root = state.uniqueRoot('neo-directory') + '/' + state.cleanName(handle.name || 'folder');
            state.mkdir(root);
            await importDirectory(handle, root);
            state.registerDirectory(root, handle);
            return resultString(root);
        } catch (error) {
            if (error && error.name === 'AbortError') return resultString('');
            console.warn('Neo WASM: native directory picker failed; using fallback.', error);
        }
    }

    const looksLikeOutput = !!allowCreate || /output|destination|export|save|write/.test(message);
    if (looksLikeOutput) {
        const root = state.uniqueRoot('neo-output');
        state.mkdir(root);
        state.registerDirectory(root, null);
        return resultString(root);
    }

    const files = await new Promise(resolve => {
        const input = document.createElement('input');
        input.type = 'file';
        input.multiple = true;
        input.webkitdirectory = true;
        input.setAttribute('directory', '');
        input.style.display = 'none';
        document.body.appendChild(input);
        let settled = false;
        const finish = value => {
            if (settled) return;
            settled = true;
            input.remove();
            resolve(value);
        };
        input.addEventListener('change', () => finish(Array.from(input.files || [])), { once: true });
        input.addEventListener('cancel', () => finish([]), { once: true });
        window.addEventListener('focus', () => setTimeout(() => finish(Array.from(input.files || [])), 350), { once: true });
        input.click();
    });
    if (!files.length) return resultString('');

    const firstRelative = files[0].webkitRelativePath || files[0].name;
    const folder = state.cleanName(firstRelative.split('/')[0] || 'folder');
    const root = state.uniqueRoot('neo-directory') + '/' + folder;
    state.mkdir(root);
    for (const file of files) {
        const relativeRaw = file.webkitRelativePath || file.name;
        const parts = relativeRaw.split('/').slice(1).map(state.cleanName);
        const relative = parts.length ? parts.join('/') : state.cleanName(file.name);
        const path = root + '/' + relative;
        state.mkdir(PATH.dirname(path));
        FS.writeFile(path, new Uint8Array(await file.arrayBuffer()));
    }
    state.registerDirectory(root, null);
    return resultString(root);
});

EM_JS(void, neo_wasm_publish_file, (const char* pathPtr), {
    neo_wasm_io_init();
    const path = UTF8ToString(pathPtr || 0);
    if (path) globalThis.NeoWasmIO.schedule(path);
});

EM_JS(void, neo_wasm_register_output_file, (const char* pathPtr, const char* namePtr), {
    neo_wasm_io_init();
    const state = globalThis.NeoWasmIO;
    const path = UTF8ToString(pathPtr || 0);
    if (!path) return;
    const directory = PATH.dirname(path);
    try { state.mkdir(directory); } catch (_) {}
    state.registerFile(path, null, UTF8ToString(namePtr || 0) || PATH.basename(path));
    // Programmatic save paths can also produce sidecars. Register the parent
    // as a fallback destination while preserving any retained directory handle.
    state.registerDirectory(directory, null);
});

EM_JS(void, neo_wasm_register_output_directory, (const char* pathPtr), {
    neo_wasm_io_init();
    const state = globalThis.NeoWasmIO;
    const path = UTF8ToString(pathPtr || 0);
    if (!path) return;
    try { state.mkdir(path); } catch (_) {}
    state.registerDirectory(path, null);
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

FileDialog::~FileDialog() {
    if (publishOnDestroy_ && paths_.GetCount() != 0) {
        PublishFile(paths_[0]);
    }
}

int FileDialog::ShowModal() {
    neo_wasm_io_init();
    paths_.Clear();
    publishOnDestroy_ = false;

    char* raw = nullptr;
    if ((style_ & wxFD_SAVE) != 0) {
        wxString suggested = filename_;
        if (suggested.empty()) {
            suggested = "download.bin";
        }
        const std::string suggestedUtf8 = Utf8(suggested);
        const std::string wildcardUtf8 = Utf8(wildcard_);
        raw = neo_wasm_pick_save(suggestedUtf8.c_str(), wildcardUtf8.c_str());
        if (raw && *raw) {
            paths_.Add(FromUtf8(raw));
            publishOnDestroy_ = true;
        }
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

}  // namespace neoshared::wasm

#endif  // __EMSCRIPTEN__
