#pragma once
#include <string>
#include <d2d1.h>
#include <d2d1helper.h>
#include "ImageTypes.h"

#include "EditState.h"
extern AppConfig g_config;

extern void RequestRepaint(QuickView::PaintLayer layer);

enum class OSDPosition { Bottom, Top, TopRight };

struct OSDState {
    std::wstring Message;
    std::wstring MessageLeft;  // For compare mode
    std::wstring MessageRight; // For compare mode
    bool IsCompareOSD = false;
    DWORD StartTime = 0;
    DWORD Duration = 1000;
    bool IsError = false;
    bool IsWarning = false;
    D2D1_COLOR_F CustomColor = D2D1::ColorF(D2D1::ColorF::Black, 0.0f);
    OSDPosition Position = OSDPosition::Bottom;

    void Show(HWND hwnd, const std::wstring& msg, bool error = false, bool warning = false, D2D1_COLOR_F color = D2D1::ColorF(D2D1::ColorF::White), OSDPosition pos = OSDPosition::Bottom, DWORD durationMs = 1000) {
        if (!g_config.ShowOSD) return;
        Message = msg;
        IsCompareOSD = false;
        StartTime = GetTickCount();
        IsError = error;
        IsWarning = warning;
        CustomColor = color;
        Position = pos;
        Duration = durationMs;
        if (hwnd) {
            SetTimer(hwnd, 994, 30, nullptr);
            RequestRepaint(QuickView::PaintLayer::Dynamic);
        }
    }

    void ShowCompare(HWND hwnd, const std::wstring& left, const std::wstring& right, D2D1_COLOR_F color = D2D1::ColorF(D2D1::ColorF::White), DWORD durationMs = 1000) {
        if (!g_config.ShowOSD) return;
        MessageLeft = left;
        MessageRight = right;
        Message = L"COMPARE"; // Dummy to trigger visibility
        IsCompareOSD = true;
        StartTime = GetTickCount();
        IsError = false;
        IsWarning = false;
        CustomColor = color;
        Position = OSDPosition::Bottom;
        Duration = durationMs;
        if (hwnd) {
            SetTimer(hwnd, 994, 30, nullptr);
            RequestRepaint(QuickView::PaintLayer::Dynamic);
        }
    }

    bool IsVisible() const {
        return (!Message.empty() || IsCompareOSD) && (GetTickCount() - StartTime) < Duration;
    }
};
