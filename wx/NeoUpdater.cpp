#include "NeoUpdater.hpp"
#include "NeoWxUi.hpp"
#include "SimpleJson.hpp"

#include <wx/app.h>
#include <wx/base64.h>
#include <wx/ffile.h>
#include <wx/filename.h>
#include <wx/gauge.h>
#include <wx/stdpaths.h>
#include <wx/timer.h>
#include <wx/webrequest.h>
#include <wx/wfstream.h>
#include <wx/zipstrm.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
#elif !defined(__APPLE__)
#include <spawn.h>
#include <unistd.h>
#endif

namespace neoupdate {
namespace {
namespace fs = std::filesystem;

std::array<int, 3> versionNumbers(wxString version) {
    if (version.StartsWith("v")) version = version.Mid(1);
    std::istringstream text(wxui::toStd(version));
    text.exceptions(std::ios::failbit | std::ios::badbit);
    std::array<int, 3> numbers;
    char separator;
    text >> numbers[0] >> separator >> numbers[1] >> separator >> numbers[2];
    return numbers;
}

wxString packageSuffix() {
#if defined(_WIN32)
    return "-windows-x86_64.zip";
#elif defined(__APPLE__)
#if defined(__aarch64__) || defined(__arm64__)
    return "-macos-arm64.zip";
#else
    return "-macos-x86_64.zip";
#endif
#else
    return "-linux-x86_64.zip";
#endif
}

#if !defined(_WIN32)
std::string shellQuote(const fs::path& path) {
    auto value = neosettings::pathToWx(path);
    value.Replace("'", "'\\''");
    return "'" + wxui::toStd(value) + "'";
}
#endif

#if defined(_WIN32)
wxString powershellQuote(const fs::path& path) {
    auto value = neosettings::pathToWx(path);
    value.Replace("'", "''");
    return "'" + value + "'";
}

std::string powershellCommand(const wxString& script) {
    const auto utf16 = script.ToStdWstring();
    return "powershell.exe -NoProfile -NonInteractive -EncodedCommand " +
        wxui::toStd(wxBase64Encode(utf16.data(), utf16.size() * sizeof(wchar_t)));
}

std::string archiveHash(const fs::path& archive) {
    BCRYPT_ALG_HANDLE provider{};
    if (BCryptOpenAlgorithmProvider(&provider, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        throw std::runtime_error("Could not initialize SHA-256 verification.");
    auto closeProvider = [](void* handle) { BCryptCloseAlgorithmProvider(handle, 0); };
    std::unique_ptr<void, decltype(closeProvider)> providerOwner(provider, closeProvider);
    BCRYPT_HASH_HANDLE hash{};
    if (BCryptCreateHash(provider, &hash, nullptr, 0, nullptr, 0, 0) < 0)
        throw std::runtime_error("Could not initialize SHA-256 verification.");
    std::unique_ptr<void, decltype(&BCryptDestroyHash)> hashOwner(hash, BCryptDestroyHash);
    std::ifstream file(archive, std::ios::binary);
    std::array<unsigned char, 64 * 1024> buffer;
    while (file.read(reinterpret_cast<char*>(buffer.data()), buffer.size()), file.gcount()) {
        if (BCryptHashData(hash, buffer.data(), static_cast<ULONG>(file.gcount()), 0) < 0)
            throw std::runtime_error("Could not verify the update download.");
    }
    if (!file.eof()) throw std::runtime_error("Could not read the update download.");
    std::array<unsigned char, 32> digest;
    if (BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0)
        throw std::runtime_error("Could not verify the update download.");
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(digest.size() * 2);
    for (const auto byte : digest) {
        result += hex[byte >> 4];
        result += hex[byte & 15];
    }
    return result;
}
#else
std::string runCommand(const std::string& command) {
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) throw std::runtime_error("Could not start update preparation.");
    std::string output;
    std::array<char, 4096> buffer;
    while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe)) output += buffer.data();
    const auto result = pclose(pipe);
    if (result != 0) throw std::runtime_error("Update preparation failed: " + output);
    return output;
}
#endif

fs::path prepareUpdate(const fs::path& archive, const fs::path& stage,
                       const wxString& application, const wxString& expectedHash) {
#if defined(_WIN32)
    const auto hash = archiveHash(archive);
#elif defined(__APPLE__)
    const auto hash = runCommand("/usr/bin/shasum -a 256 -- " + shellQuote(archive) + " 2>&1");
#else
    const auto hash = runCommand("sha256sum -- " + shellQuote(archive) + " 2>&1");
#endif
    const auto actualHash = wxString::FromUTF8(hash).BeforeFirst(' ').Strip().Right(64).Lower();
    if (actualHash != expectedHash.Lower())
        throw std::runtime_error("The update download does not match its SHA-256 checksum.");

#if defined(__APPLE__)
    const auto contents = stage / "contents";
    runCommand("/usr/bin/ditto -x -k " + shellQuote(archive) + " " + shellQuote(contents) + " 2>&1");
    return contents / neosettings::pathFromWx(application + ".app");
#else
    wxFFileInputStream file(neosettings::pathToWx(archive));
    wxZipInputStream zip(file);
    std::unique_ptr<wxZipEntry> entry;
    while (entry.reset(zip.GetNextEntry()), entry) {
#if defined(_WIN32)
        const bool program = entry->GetName() == application + ".exe";
#else
        const bool program = entry->GetName().StartsWith(application + "-") &&
                             entry->GetName().EndsWith(".AppImage");
#endif
        if (!program) continue;
        const auto binary = stage / "replacement";
        wxFFileOutputStream output(neosettings::pathToWx(binary));
        zip.Read(output);
        const bool written = output.IsOk();
        if (!output.Close() || !written || !zip.Eof())
            throw std::runtime_error("Could not extract the update.");
#if !defined(_WIN32)
        fs::permissions(binary, fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec |
                                fs::perms::group_read | fs::perms::group_exec |
                                fs::perms::others_read | fs::perms::others_exec);
#endif
        return binary;
    }
    throw std::runtime_error("The update ZIP does not contain the application.");
#endif
}

class Updater final : public wxDialog {
public:
    Updater(wxFrame& frame, wxString application, wxString version, PrepareRestart prepareRestart)
        : wxDialog(&frame, wxID_ANY, application + " Updates", wxDefaultPosition, wxDefaultSize,
                   wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
          frame_(frame), application_(std::move(application)), version_(std::move(version)),
          prepareRestart_(std::move(prepareRestart)),
          session_(wxWebSession::New()), timer_(this) {
        auto* root = new wxBoxSizer(wxVERTICAL);
        root->Add(new wxStaticText(this, wxID_ANY, application_ + " " + version_), 0, wxALL, FromDIP(12));
        status_ = new wxStaticText(this, wxID_ANY, "Check for a newer release.");
        root->Add(status_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));
        notes_ = new wxTextCtrl(this, wxID_ANY, {}, wxDefaultPosition, FromDIP(wxSize(480, 230)),
                                wxTE_MULTILINE | wxTE_READONLY);
        root->Add(notes_, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));
        progress_ = new wxGauge(this, wxID_ANY, 1000, wxDefaultPosition, FromDIP(wxSize(-1, 10)));
        root->Add(progress_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));
        auto* buttons = new wxBoxSizer(wxHORIZONTAL);
        auto* releases = new wxButton(this, wxID_ANY, "Release Page");
        action_ = new wxButton(this, wxID_ANY, "Download Update");
        action_->Disable();
        auto* close = new wxButton(this, wxID_CLOSE, "Close");
        buttons->Add(releases, 0, wxRIGHT, FromDIP(8));
        buttons->AddStretchSpacer();
        buttons->Add(action_, 0, wxRIGHT, FromDIP(8));
        buttons->Add(close);
        root->Add(buttons, 0, wxEXPAND | wxALL, FromDIP(12));
        SetSizerAndFit(root);
        wxui::configureResponsiveWindow(*this, wxSize(560, 430), wxSize(420, 300));
        releases->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            wxLaunchDefaultBrowser("https://github.com/vrifftech/" + application_ + "/releases");
        });
        action_->Bind(wxEVT_BUTTON, [this, close](wxCommandEvent&) {
            try {
                if (phase_ != Phase::Ready) { download(); return; }
                const auto readyStatus = status_->GetLabel();
                status_->SetLabel("Waiting for the current operation to finish before restarting...");
                status_->Wrap(FromDIP(490));
                close->SetLabel("Hide");
                action_->Disable();
                Layout();
                auto decision = [this, close, readyStatus](bool proceed) {
                    status_->SetLabel(readyStatus);
                    close->SetLabel("Close");
                    action_->Enable();
                    Layout();
                    if (!proceed) return;
                    startup_ = false;
                    try { restart(); }
                    catch (const std::exception& error) { failed(wxui::toWx(error.what())); }
                };
                if (prepareRestart_) prepareRestart_(std::move(decision));
                else decision(true);
            }
            catch (const std::exception& error) { failed(error.what()); }
        });
        close->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { dismiss(); });
        Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent&) { dismiss(); });
        Bind(wxEVT_WEBREQUEST_STATE, &Updater::onRequest, this);
        Bind(wxEVT_TIMER, &Updater::onTimer, this);
        frame_.Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent& event) {
            if (busy() && event.CanVeto()) {
                closing_ = true;
                if (phase_ == Phase::Preparing) {
                    status_->SetLabel("Finishing update preparation before closing...");
                    present();
                } else request_.Cancel();
                event.Veto();
            } else event.Skip();
        });
    }

    ~Updater() override {
        if (busy() && phase_ != Phase::Preparing) request_.Cancel();
        if (preparing_.valid()) preparing_.wait();
        if (!installing_) cleanup();
    }

    void check(bool startup) {
        startup_ = startup;
        if (busy() || phase_ == Phase::Ready) { present(); return; }
        cleanup();
        phase_ = Phase::Checking;
        action_->Disable();
        action_->SetLabel("Download Update");
        notes_->Clear();
        status_->SetLabel("Checking for updates...");
        if (!startup_) present();
        try { fetch("https://api.github.com/repos/vrifftech/" + application_ + "/releases/latest"); }
        catch (const std::exception& error) { failed(wxui::toWx(error.what())); }
    }

private:
    enum class Phase { Idle, Checking, Available, Checksum, Downloading, Preparing, Ready };

    bool busy() const {
        return phase_ == Phase::Checking || phase_ == Phase::Checksum ||
               phase_ == Phase::Downloading || phase_ == Phase::Preparing;
    }

    void present() {
        wxui::applyTheme(this, wxui::currentDarkMode());
        Show(); Raise();
    }

    void dismiss() {
        Hide();
        startup_ = true;
        if (phase_ == Phase::Checking || phase_ == Phase::Checksum || phase_ == Phase::Downloading)
            request_.Cancel();
    }

    void cleanup() {
        if (!stage_.empty()) {
            std::error_code error;
            fs::remove_all(stage_, error);
            stage_.clear();
        }
    }

    void failed(const wxString& message) {
        phase_ = Phase::Idle;
        timer_.Stop();
        action_->Disable();
        progress_->SetValue(0);
        status_->SetLabel("Update failed: " + message);
        status_->Wrap(FromDIP(490));
        Layout();
        if (!startup_) {
            present();
            wxMessageBox(message, application_ + " Update", wxOK | wxICON_ERROR, this);
        }
    }

    void fetch(const wxString& url, bool file = false) {
        request_ = session_.CreateRequest(this, url);
        if (!request_.IsOk()) throw std::runtime_error("Could not initialize HTTPS downloads.");
        request_.SetHeader("User-Agent", application_ + "/" + version_);
        request_.SetStorage(file ? wxWebRequest::Storage_File : wxWebRequest::Storage_Memory);
        request_.Start();
        timer_.Start(150);
    }

    void onRequest(wxWebRequestEvent& event) {
        if (closing_ && (event.GetState() == wxWebRequest::State_Cancelled ||
                         event.GetState() == wxWebRequest::State_Unauthorized ||
                         event.GetState() == wxWebRequest::State_Failed ||
                         event.GetState() == wxWebRequest::State_Completed)) {
            closing_ = false;
            phase_ = Phase::Idle;
            timer_.Stop();
            frame_.CallAfter([frame = &frame_] { frame->Close(); });
            return;
        }
        if (event.GetState() == wxWebRequest::State_Cancelled) {
            phase_ = Phase::Idle; timer_.Stop(); return;
        }
        if (event.GetState() == wxWebRequest::State_Failed) {
            failed(event.GetErrorDescription()); return;
        }
        if (event.GetState() == wxWebRequest::State_Unauthorized) {
            failed("The update server requires authorization."); return;
        }
        if (event.GetState() != wxWebRequest::State_Completed) return;
        try {
            const auto response = request_.GetResponse();
            if (response.GetStatus() != 200)
                throw std::runtime_error("GitHub returned HTTP " + std::to_string(response.GetStatus()) + ".");
            if (phase_ == Phase::Checking) {
                const auto release = neojson::parse(wxui::toStd(response.AsString()));
                const auto remote = wxui::toWx(release.at("tag_name").asString());
                notes_->ChangeValue(wxui::toWx(release.at("body").asString()));
                timer_.Stop(); progress_->SetValue(0);
                if (versionNumbers(remote) <= versionNumbers(version_)) {
                    phase_ = Phase::Idle; status_->SetLabel("You have the latest release."); return;
                }
                downloadUrl_.clear(); checksumUrl_.clear();
                wxString package;
                const auto& assets = release.at("assets").asArray();
                for (const auto& asset : assets) {
                    const auto name = wxui::toWx(asset.at("name").asString());
                    if (name.EndsWith(packageSuffix())) {
                        package = name;
                        downloadUrl_ = wxui::toWx(asset.at("browser_download_url").asString());
                    }
                }
                for (const auto& asset : assets)
                    if (wxui::toWx(asset.at("name").asString()) == package + ".sha256")
                        checksumUrl_ = wxui::toWx(asset.at("browser_download_url").asString());
                if (downloadUrl_.empty() || checksumUrl_.empty())
                    throw std::runtime_error("This release has no update ZIP and checksum for this platform.");
                phase_ = Phase::Available;
                status_->SetLabel(remote + " is available. Download it, then restart to install.");
                action_->Enable(); present();
            } else if (phase_ == Phase::Checksum) {
                std::istringstream checksum(wxui::toStd(response.AsString()));
                std::string hash; checksum >> hash; expectedHash_ = wxui::toWx(hash);
                phase_ = Phase::Downloading;
                status_->SetLabel("Downloading update...");
                fetch(downloadUrl_, true);
            } else if (phase_ == Phase::Downloading) {
                const auto archive = stage_ / "update.zip";
                fs::rename(neosettings::pathFromWx(event.GetDataFile()), archive);
                phase_ = Phase::Preparing;
                status_->SetLabel("Verifying and preparing update...");
                preparing_ = std::async(std::launch::async, prepareUpdate,
                    archive, stage_, application_, expectedHash_);
            }
        } catch (const std::exception& error) { failed(wxui::toWx(error.what())); }
    }

    void download() {
        startup_ = false;
        target_ = neosettings::pathFromWx(wxStandardPaths::Get().GetExecutablePath());
#if defined(__APPLE__)
        target_ = target_.parent_path().parent_path().parent_path();
        if (target_.extension() != ".app") throw std::runtime_error("Run the installed .app bundle to update it.");
#elif !defined(_WIN32)
        wxString appimage; wxGetEnv("APPIMAGE", &appimage);
        target_ = neosettings::pathFromWx(appimage);
#endif
        const auto temporary = wxFileName::CreateTempFileName(
            neosettings::pathToWx(target_.parent_path() / ".neo-update-"));
        if (temporary.empty())
#if defined(__APPLE__)
            throw std::runtime_error("Cannot prepare an update beside this application.\n\n"
                "If you opened it from Downloads or a disk image, use Finder to move the .app to a writable "
                "folder such as Applications in your home folder, then relaunch it and try again.");
#else
            throw std::runtime_error("Cannot prepare an update in the application's folder.");
#endif
        stage_ = neosettings::pathFromWx(temporary);
        fs::remove(stage_); fs::create_directory(stage_);
        session_.SetTempDir(neosettings::pathToWx(stage_));
        phase_ = Phase::Checksum; action_->Disable();
        status_->SetLabel("Reading update checksum...");
        fetch(checksumUrl_);
    }

    void onTimer(wxTimerEvent&) {
        if (phase_ == Phase::Preparing) {
            progress_->Pulse();
            if (preparing_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
            try {
                prepared_ = preparing_.get();
                phase_ = Phase::Ready; timer_.Stop(); progress_->SetValue(1000);
                status_->SetLabel("Update ready. Restart to install; open documents will use their usual save prompts.");
                status_->Wrap(FromDIP(490));
                action_->SetLabel("Restart && Install"); action_->Enable(); Layout();
            } catch (const std::exception& error) { failed(wxui::toWx(error.what())); }
            if (closing_) {
                closing_ = false;
                frame_.CallAfter([frame = &frame_] { frame->Close(); });
            }
        } else if (phase_ == Phase::Downloading) {
            const auto total = request_.GetBytesExpectedToReceive();
            if (total > 0) progress_->SetValue(static_cast<int>(1000 * request_.GetBytesReceived() / total));
            else progress_->Pulse();
        } else progress_->Pulse();
    }

    void restart() {
#if defined(_WIN32)
        const auto script = stage_ / "install.ps1";
        const wxString text = R"(param([int]$ParentId, [string]$Source, [string]$Target, [string]$Work)
$ErrorActionPreference = 'Stop'
try {
    Add-Type @'
using System.Runtime.InteropServices;
public static class NeoUpdateNative {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, ExactSpelling = true, SetLastError = true)]
    public static extern bool MoveFileExW(string source, string target, uint flags);
}
'@
    Wait-Process -Id $ParentId -ErrorAction SilentlyContinue
    # Same-volume replacement, with MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH.
    if (![NeoUpdateNative]::MoveFileExW($Source, $Target, 9)) {
        throw [System.ComponentModel.Win32Exception]::new([System.Runtime.InteropServices.Marshal]::GetLastWin32Error())
    }
    Start-Process -FilePath $Target -WorkingDirectory ([System.IO.Path]::GetDirectoryName($Target))
    Remove-Item -LiteralPath $Work -Recurse -Force
} catch {
    $message = "The update could not be completed.`r`n`r`n" + $_.Exception.Message + "`r`n`r`nInstaller folder: " + $Work
    $message | Set-Content -LiteralPath (Join-Path $Work 'install-error.txt') -Encoding UTF8 -ErrorAction Continue
    Add-Type -AssemblyName System.Windows.Forms
    [void][System.Windows.Forms.MessageBox]::Show($message, 'NeoTools Update', 'OK', 'Error')
    exit 1
}
)";
        wxFFile file(neosettings::pathToWx(script), "wb");
        if (!file.Write(text) || !file.Close()) throw std::runtime_error("Could not write the update installer.");
        const wxString command = "& " + powershellQuote(script) + " -ParentId " +
            std::to_string(wxGetProcessId()) + " -Source " + powershellQuote(prepared_) +
            " -Target " + powershellQuote(target_) + " -Work " + powershellQuote(stage_);
        const auto encoded = wxui::toWx(powershellCommand(command));
        std::vector<std::wstring> args = {L"powershell.exe", L"-NoProfile", L"-NonInteractive",
            L"-ExecutionPolicy", L"Bypass", L"-EncodedCommand", encoded.AfterLast(' ').ToStdWstring()};
#elif defined(__APPLE__)
        const auto script = stage_ / "install.sh";
        const wxString text = R"(#!/bin/sh
set -u
parent=$1
source=$2
target=$3
work=$4
report_error() {
    printf 'Could not %s.\n%s\n' "$1" "$2" > "$work/install-error.txt"
    /usr/bin/osascript -l JavaScript -e 'function run(args) {
        var app = Application.currentApplication();
        app.includeStandardAdditions = true;
        app.displayDialog("Could not " + args[0] + ".\n\n" + args[1] + "\n\nInstaller folder: " + args[2],
            {withTitle: "NeoTools Update", withIcon: "stop", buttons: ["OK"], defaultButton: "OK"});
    }' "$1" "$2" "$work"
    exit 1
}
while kill -0 "$parent" 2>/dev/null; do sleep 0.2; done
# RENAME_SWAP exchanges both bundles atomically on the staging volume.
error=$(/usr/bin/osascript -l JavaScript -e 'function run(args) { ObjC.import("stdio");
    if ($.renamex_np(args[0], args[1], 2) !== 0) throw new Error("Could not replace the application bundle.");
}' "$source" "$target" 2>&1) || report_error 'replace the application bundle' "$error"
error=$(/usr/bin/open -n "$target" 2>&1) || report_error 'restart the application' "$error"
error=$(rm -rf -- "$work" 2>&1) || report_error 'remove the update files' "$error"
)";
        wxFFile file(neosettings::pathToWx(script), "wb");
        if (!file.Write(text) || !file.Close()) throw std::runtime_error("Could not write the update installer.");
        std::vector<std::wstring> args = {L"/bin/sh", neosettings::pathToWx(script).ToStdWstring(),
            std::to_wstring(wxGetProcessId()), neosettings::pathToWx(prepared_).ToStdWstring(),
            neosettings::pathToWx(target_).ToStdWstring(), neosettings::pathToWx(stage_).ToStdWstring()};
#else
        // Keep the GUI alive to report errors while replacing the running AppImage.
        const bool exitOnClose = wxTheApp->GetExitOnFrameDelete();
        frame_.RemoveChild(this);
        wxTheApp->SetExitOnFrameDelete(false);
        if (!frame_.Close()) {
            frame_.AddChild(this);
            wxTheApp->SetExitOnFrameDelete(exitOnClose);
            return;
        }
        Hide();
        try {
            fs::rename(prepared_, target_);
            fs::current_path(target_.parent_path());
            for (const auto* name : {"APPIMAGE", "APPDIR", "ARGV0", "OWD", "LD_LIBRARY_PATH", "LD_PRELOAD"})
                unsetenv(name);
            auto executable = target_.string();
            char* const argv[] = {executable.data(), nullptr};
            pid_t process;
            posix_spawn_file_actions_t actions;
            int error = posix_spawn_file_actions_init(&actions);
            if (error != 0)
                throw std::system_error(error, std::generic_category(), "Could not prepare the application restart");
            error = posix_spawn_file_actions_addclosefrom_np(&actions, 3);
            if (error == 0)
                error = posix_spawn(&process, executable.c_str(), &actions, nullptr, argv, environ);
            posix_spawn_file_actions_destroy(&actions);
            if (error != 0)
                throw std::system_error(error, std::generic_category(),
                    "The update was installed, but the application could not be restarted. Open it manually");
        } catch (const std::exception& error) {
            wxMessageBox(wxui::toWx(error.what()), application_ + " Update", wxOK | wxICON_ERROR, this);
        }
        Destroy();
        wxTheApp->ExitMainLoop();
#endif
#if defined(_WIN32) || defined(__APPLE__)
        std::vector<const wchar_t*> argv;
        for (const auto& arg : args) argv.push_back(arg.c_str());
        argv.push_back(nullptr);
        wxExecuteEnv environment;
        environment.cwd = neosettings::pathToWx(target_.parent_path());
        const auto installer = wxExecute(argv.data(),
            wxEXEC_ASYNC | wxEXEC_HIDE_CONSOLE | wxEXEC_MAKE_GROUP_LEADER, nullptr, &environment);
        if (installer <= 0)
            throw std::runtime_error("Could not start the update installer. The installed application was not changed.");
        Hide();
        installing_ = true;
        if (!frame_.Close()) {
            wxKill(installer, wxSIGKILL, nullptr, wxKILL_CHILDREN);
            installing_ = false;
            present();
        }
#endif
    }

    wxFrame& frame_;
    wxString application_, version_, downloadUrl_, checksumUrl_, expectedHash_;
    PrepareRestart prepareRestart_;
    wxWebSession session_;
    wxWebRequest request_;
    wxTimer timer_;
    wxStaticText* status_{};
    wxTextCtrl* notes_{};
    wxGauge* progress_{};
    wxButton* action_{};
    Phase phase_{Phase::Idle};
    bool startup_{}, installing_{}, closing_{};
    fs::path stage_, target_, prepared_;
    std::future<fs::path> preparing_;
};
} // namespace

void install(wxFrame& frame, const char* application, const char* version,
             PrepareRestart prepareRestart) {
#if !defined(_WIN32) && !defined(__APPLE__)
    wxString appimage;
    if (!wxGetEnv("APPIMAGE", &appimage)) return;
#endif
    auto* bar = frame.GetMenuBar();
    if (!bar) { bar = new wxMenuBar; frame.SetMenuBar(bar); }
    const auto index = bar->FindMenu("Help");
    auto* help = index == wxNOT_FOUND ? new wxMenu : bar->GetMenu(index);
    if (index == wxNOT_FOUND) bar->Append(help, "&Help");
    help->AppendSeparator();
    const auto checkId = help->Append(wxID_ANY, "Check for Updates...")->GetId();
    auto* startup = help->AppendCheckItem(wxID_ANY, "Check for updates on startup");
    const neosettings::ConfigStore settings(application);
    startup->Check(settings.readBool("Updates/CheckOnStartup", true));
    auto* updater = new Updater(frame, wxString::FromUTF8(application), wxString::FromUTF8(version),
                                std::move(prepareRestart));
    frame.Bind(wxEVT_MENU, [updater](wxCommandEvent&) { updater->check(false); }, checkId);
    frame.Bind(wxEVT_MENU, [settings](wxCommandEvent& event) {
        settings.writeBool("Updates/CheckOnStartup", event.IsChecked());
    }, startup->GetId());
    if (startup->IsChecked()) frame.CallAfter([updater] { updater->check(true); });
}
} // namespace neoupdate
