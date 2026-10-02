#pragma once
#include "NeoWxUi.hpp"
#include <neoshared/ResourceDocument.hpp>
#include <wx/menu.h>
#include <wx/panel.h>
#include <functional>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

// Small, compile-time hosting boundary. No plugin loader and no second wxApp.
namespace neomodules {
inline constexpr unsigned kPanelApiVersion = 3;
class Panel;
struct Context {
    bool embedded = false;
    bool compact = false;
    std::function<void(const wxString&)> titleChanged;
    std::function<void()> closeRequested;
    std::function<void(const std::filesystem::path&, const Panel*)> validateOutput;
    // Resolves a game resource from the host session without retaining archive internals.
    // Embedded viewers use this for companion resources such as MDX and TPC.
    std::function<std::optional<neoshared::ResourceDocument>(const std::string&, std::uint16_t)> resolveResource;

};
class Panel : public wxPanel {
public:
    Panel(wxWindow* parent, Context context = {})
        : wxPanel(parent, wxID_ANY), context_(std::move(context)) {}
    // Transfer once, before destroying the panel. The receiving frame owns the
    // menus, including menus later nested inside its own menu bar.
    std::unique_ptr<wxMenuBar> takeMenus() { return std::move(pendingMenus_); }
    bool ownsCommand(int id) const {
        for (auto* menu : menuRoots_) if (menu->FindItem(id)) return true;
        return false;
    }
    bool commandEnabled(int id) const {
        for(auto* menu:menuRoots_) if(auto* item=menu->FindItem(id)) return item->IsEnabled();
        return false;
    }
    bool dispatch(wxEvent& event) { return GetEventHandler()->ProcessEventLocally(event); }
    virtual std::vector<std::filesystem::path> openPaths() const { return {}; }
    virtual bool canClose() = 0;
    virtual void setAppearance(bool dark, double fontScale) = 0;
protected:
    Context context_;
    void validateHostOutput(const std::filesystem::path& path) const {
        if (context_.validateOutput) context_.validateOutput(path, this);
    }
    void setModuleMenus(wxMenuBar* bar) {
        pendingMenus_.reset(bar);
        menuRoots_.clear();
        for (std::size_t i=0; i<bar->GetMenuCount(); ++i) menuRoots_.push_back(bar->GetMenu(i));
    }
    void enableModuleCommand(int id, bool enabled) {
        for (auto* menu : menuRoots_) if (auto* item=menu->FindItem(id)) { item->Enable(enabled); return; }
    }
    void createModuleStatusBar(int fields) {
        if (!GetSizer()) throw std::logic_error("Module UI must have a sizer before its status bar");
        status_=new wxui::ThemedStatusBar(this, fields);
        GetSizer()->Add(status_,0,wxEXPAND);
    }
    wxStatusBar* moduleStatusBar() const { return status_; }
    void setModuleStatusText(const wxString& text, int field=0) { if(status_) status_->SetStatusText(text,field); }
    void setModuleStatusText(const std::string& text,int field=0) { setModuleStatusText(wxui::toWx(text),field); }
    void setModuleStatusText(const char* text,int field=0) { setModuleStatusText(wxString::FromUTF8(text),field); }
    void setModuleTitle(const wxString& title) { if(context_.titleChanged) context_.titleChanged(title); }
    void requestModuleClose() { if(context_.closeRequested) context_.closeRequested(); }
private:
    std::unique_ptr<wxMenuBar> pendingMenus_;
    std::vector<wxMenu*> menuRoots_;
    wxui::ThemedStatusBar* status_{};
};
// Frame commands do not naturally travel DOWN into a child panel. Routing is
// local-only to avoid bubbling the same event back into the host indefinitely.
inline bool routeCommand(const std::vector<Panel*>& panels, wxCommandEvent& event) {
    for(auto* panel:panels) if(panel && panel->ownsCommand(event.GetId())) {
        wxCommandEvent copy(event); copy.SetEventObject(panel);
        return panel->dispatch(copy);
    }
    return false;
}
inline void routeMenuOpen(const std::vector<Panel*>& panels, wxMenuEvent& event) {
    for(auto* panel:panels) if(panel) { wxMenuEvent copy(event); panel->dispatch(copy); }
}
} // namespace neomodules
