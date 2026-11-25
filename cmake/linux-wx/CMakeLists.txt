cmake_minimum_required(VERSION 3.24)
project(NeoLinuxWx LANGUAGES C CXX)

# Debian 12's wxWidgets package omits the updater's HTTPS backend.
find_package(CURL REQUIRED)
set(wxBUILD_SHARED ON CACHE BOOL "" FORCE)
set(wxUSE_WEBREQUEST ON CACHE BOOL "" FORCE)
set(wxUSE_WEBREQUEST_CURL ON CACHE BOOL "" FORCE)
set(wxUSE_WEBVIEW OFF CACHE BOOL "" FORCE)
set(wxUSE_MEDIACTRL OFF CACHE BOOL "" FORCE)

include(FetchContent)
FetchContent_Declare(wxwidgets
    URL https://github.com/wxWidgets/wxWidgets/releases/download/v3.3.3/wxWidgets-3.3.3.tar.bz2
    URL_HASH SHA256=81b09d6dd9f1ed9301f8c55a968a488d0491f264dc2bab19a7e407ac67009482
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(wxwidgets)
