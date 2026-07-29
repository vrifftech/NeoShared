#pragma once

#include <wx/display.h>
#include <wx/toplevel.h>
#include <wx/utils.h>

#include <algorithm>

namespace neowindow {

inline wxRect fallbackClientArea() {
    const wxRect area = wxGetClientDisplayRect();
    if (area.GetWidth() > 0 && area.GetHeight() > 0) return area;
    return wxRect(0, 0, 1280, 720);
}

inline wxRect displayClientArea(const wxTopLevelWindow& window,
                                const wxRect* requestedRect = nullptr) {
    int displayIndex = wxNOT_FOUND;

    if (requestedRect != nullptr && requestedRect->GetWidth() > 0 && requestedRect->GetHeight() > 0) {
        const wxPoint centre(
            requestedRect->GetX() + requestedRect->GetWidth() / 2,
            requestedRect->GetY() + requestedRect->GetHeight() / 2);
        displayIndex = wxDisplay::GetFromPoint(centre);
    }

    if (displayIndex == wxNOT_FOUND) {
        displayIndex = wxDisplay::GetFromWindow(const_cast<wxTopLevelWindow*>(&window));
    }
    if (displayIndex == wxNOT_FOUND) {
        displayIndex = wxDisplay::GetFromPoint(wxGetMousePosition());
    }
    if (displayIndex == wxNOT_FOUND && wxDisplay::GetCount() > 0) {
        displayIndex = 0;
    }

    if (displayIndex != wxNOT_FOUND) {
        wxDisplay display(static_cast<unsigned int>(displayIndex));
        if (display.IsOk()) {
            const wxRect area = display.GetClientArea();
            if (area.GetWidth() > 0 && area.GetHeight() > 0) return area;
        }
    }

    return fallbackClientArea();
}

inline int horizontalMarginPixels(const wxTopLevelWindow& window, int marginDip) {
    return std::max(0, window.FromDIP(wxSize(marginDip, 0)).GetWidth());
}

inline int verticalMarginPixels(const wxTopLevelWindow& window, int marginDip) {
    return std::max(0, window.FromDIP(wxSize(0, marginDip)).GetHeight());
}

inline wxSize maximumWindowSize(const wxTopLevelWindow& window,
                                const wxRect& area,
                                int marginDip) {
    const int marginX = horizontalMarginPixels(window, marginDip);
    const int marginY = verticalMarginPixels(window, marginDip);
    return wxSize(
        std::max(1, area.GetWidth() - 2 * marginX),
        std::max(1, area.GetHeight() - 2 * marginY));
}

inline wxSize clampSize(const wxSize& requested,
                        const wxSize& minimum,
                        const wxSize& maximum) {
    const int minWidth = std::min(std::max(1, minimum.GetWidth()), maximum.GetWidth());
    const int minHeight = std::min(std::max(1, minimum.GetHeight()), maximum.GetHeight());
    return wxSize(
        std::clamp(requested.GetWidth(), minWidth, maximum.GetWidth()),
        std::clamp(requested.GetHeight(), minHeight, maximum.GetHeight()));
}

inline wxRect clampRectToArea(const wxRect& requested,
                              const wxRect& area,
                              const wxSize& minimum,
                              int marginX,
                              int marginY) {
    const wxRect insetArea(
        area.GetX() + marginX,
        area.GetY() + marginY,
        std::max(1, area.GetWidth() - 2 * marginX),
        std::max(1, area.GetHeight() - 2 * marginY));

    const wxSize size = clampSize(requested.GetSize(), minimum, insetArea.GetSize());
    const int maximumX = insetArea.GetRight() - size.GetWidth() + 1;
    const int maximumY = insetArea.GetBottom() - size.GetHeight() + 1;

    int x = requested.GetX();
    int y = requested.GetY();
    if (x < insetArea.GetX() || x > maximumX) {
        x = insetArea.GetX() + std::max(0, (insetArea.GetWidth() - size.GetWidth()) / 2);
    }
    if (y < insetArea.GetY() || y > maximumY) {
        y = insetArea.GetY() + std::max(0, (insetArea.GetHeight() - size.GetHeight()) / 2);
    }

    x = std::clamp(x, insetArea.GetX(), std::max(insetArea.GetX(), maximumX));
    y = std::clamp(y, insetArea.GetY(), std::max(insetArea.GetY(), maximumY));
    return wxRect(x, y, size.GetWidth(), size.GetHeight());
}

inline wxSize responsiveMinimumSize(const wxTopLevelWindow& window,
                                    const wxSize& minimumDip,
                                    const wxRect& area,
                                    int marginDip = 12) {
    const wxSize requested = window.FromDIP(minimumDip);
    const wxSize floor = window.FromDIP(wxSize(320, 240));
    const wxSize maximum = maximumWindowSize(window, area, marginDip);
    return clampSize(requested, wxSize(
        std::min(floor.GetWidth(), maximum.GetWidth()),
        std::min(floor.GetHeight(), maximum.GetHeight())), maximum);
}

inline void configureResponsiveWindow(wxTopLevelWindow& window,
                                      const wxSize& preferredDip,
                                      const wxSize& minimumDip,
                                      int marginDip = 12) {
    const wxRect area = displayClientArea(window);
    const wxSize minimum = responsiveMinimumSize(window, minimumDip, area, marginDip);
    const wxSize maximum = maximumWindowSize(window, area, marginDip);
    const wxSize preferred = clampSize(window.FromDIP(preferredDip), minimum, maximum);

    window.SetMinSize(minimum);

    const int marginX = horizontalMarginPixels(window, marginDip);
    const int marginY = verticalMarginPixels(window, marginDip);
    const wxRect desired(
        area.GetX() + std::max(marginX, (area.GetWidth() - preferred.GetWidth()) / 2),
        area.GetY() + std::max(marginY, (area.GetHeight() - preferred.GetHeight()) / 2),
        preferred.GetWidth(),
        preferred.GetHeight());
    window.SetSize(clampRectToArea(desired, area, minimum, marginX, marginY));
}

inline void applyRestoredWindowRect(wxTopLevelWindow& window,
                                    const wxRect& requested,
                                    int marginDip = 8) {
    const wxRect area = displayClientArea(window, &requested);
    const wxSize currentMinimum = window.GetMinSize();
    const wxSize fallbackMinimum = window.FromDIP(wxSize(320, 240));
    const wxSize minimum(
        currentMinimum.GetWidth() > 0 ? currentMinimum.GetWidth() : fallbackMinimum.GetWidth(),
        currentMinimum.GetHeight() > 0 ? currentMinimum.GetHeight() : fallbackMinimum.GetHeight());
    const int marginX = horizontalMarginPixels(window, marginDip);
    const int marginY = verticalMarginPixels(window, marginDip);
    window.SetSize(clampRectToArea(requested, area, minimum, marginX, marginY));
}

inline void constrainWindowToDisplay(wxTopLevelWindow& window, int marginDip = 8) {
    if (window.IsMaximized() || window.IsIconized()) return;
    applyRestoredWindowRect(window, window.GetRect(), marginDip);
}

} // namespace neowindow
