// Dark mode shared by the EIDVirtual programs (wizard, manager, tracing).
//
// The programs are plain dialogs. A dialog procedure only has to forward its
// messages to DarkModeDialogProc first :
//
//	INT_PTR lResult;
//	if (DarkModeDialogProc(hWnd, message, wParam, lParam, &lResult))
//		return lResult;
//
// WM_INITDIALOG themes the dialog and its controls, WM_SETTINGCHANGE re-themes
// them when the user switches the system setting, and the WM_CTLCOLOR* messages
// are answered only in dark mode : in light mode the dialog keeps its default
// (system) look.

#pragma once

#include <windows.h>

// the "Choose your default app mode" setting of Windows (apps)
BOOL IsDarkModeEnabled();

// title bar of the window, following the setting
VOID ApplyDarkTitleBar(__in HWND hWnd);

// theme the controls of a dialog (and redraw it) - call it again when the
// setting changes. Done by DarkModeDialogProc.
VOID ThemeDialogControls(__in HWND hDlg);

// returns TRUE if the message has been fully handled and *plResult is the value
// the dialog procedure has to return ; always returns FALSE for the messages
// it only observes (WM_INITDIALOG, WM_SETTINGCHANGE)
BOOL DarkModeDialogProc(__in HWND hDlg, __in UINT message, __in WPARAM wParam, __in LPARAM lParam, __out INT_PTR* plResult);

// colors of the dialog surface / of the header panel (white in light mode)
COLORREF GetThemedBackgroundColor();
COLORREF GetThemedTextColor();
COLORREF GetThemedTitleColor();
// background of the edit controls, list views and tracing window
COLORREF GetThemedControlColor();
HBRUSH GetThemedDialogBrush();

// the logo of the headers is a dark drawing on a white background : in dark
// mode, returns a copy (cached) where the white is replaced by the dark
// background and the dark strokes are lightened. Returns hLogo itself in light
// mode. The returned bitmap must not be freed by the caller.
HBITMAP GetThemedLogo(__in HBITMAP hLogo);

// colors a ListView so its rows don't stay a plain white rectangle
VOID ThemeListView(__in HWND hListView);

// texts of the discrete version / copyright lines of the dialogs, generated
// from version.h by GenerateVersion.ps1 :
// "Version 2.0.0.118" and "(c) 2013-2026 MySmartLogon.com"
VOID GetVersionText(__out_ecount(cchBuffer) PWSTR szBuffer, __in DWORD cchBuffer);
VOID GetCopyrightText(__out_ecount(cchBuffer) PWSTR szBuffer, __in DWORD cchBuffer);
