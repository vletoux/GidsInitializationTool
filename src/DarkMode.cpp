// Dark mode shared by the EIDVirtual programs - see DarkMode.h

// the projects target Windows XP headers (_WIN32_WINNT=0x501) ; dark mode needs the recent ones
#undef _WIN32_WINNT
#undef WINVER
#undef NTDDI_VERSION
#define _WIN32_WINNT 0x0A00
#define WINVER 0x0A00
#define NTDDI_VERSION 0x0A000000
#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <vssym32.h>
#include <tchar.h>
#include <strsafe.h>
#include "version.h"
#include "DarkMode.h"

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")

#define DARK_BACKGROUND		RGB(0x20, 0x20, 0x20)
#define DARK_CONTROL		RGB(0x2B, 0x2B, 0x2B)
#define DARK_TEXT			RGB(0xE8, 0xE8, 0xE8)

// set on the controls that were switched to a dark variant, so that only what
// was done is undone : SetWindowTheme(NULL, NULL) on a control never switched
// would lose its transparent background
static const TCHAR DARK_CONTROL_PROP[] = TEXT("EIDDarkControl");

BOOL IsDarkModeEnabled()
{
	DWORD dwValue = 1; // default when the value is missing : light
	DWORD dwSize = sizeof(dwValue);
	if (RegGetValue(HKEY_CURRENT_USER,
		TEXT("Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize"),
		TEXT("AppsUseLightTheme"), RRF_RT_REG_DWORD, NULL, &dwValue, &dwSize) != ERROR_SUCCESS)
	{
		return FALSE;
	}
	return dwValue == 0;
}

static DWORD GetWindowsBuildNumber()
{
	static DWORD dwBuild = 0;
	if (!dwBuild)
	{
		typedef LONG (WINAPI *RtlGetVersionProc)(PRTL_OSVERSIONINFOW);
		HMODULE hNtdll = GetModuleHandle(TEXT("ntdll.dll"));
		RtlGetVersionProc pRtlGetVersion = hNtdll ? (RtlGetVersionProc) GetProcAddress(hNtdll, "RtlGetVersion") : NULL;
		RTL_OSVERSIONINFOW vi = { sizeof(vi) };
		if (pRtlGetVersion && pRtlGetVersion(&vi) == 0)
		{
			dwBuild = vi.dwBuildNumber;
		}
	}
	return dwBuild;
}

// Without this opt-in, the "DarkMode_*" visual styles (buttons, edit boxes,
// scrollbars...) are ignored. uxtheme exports it by ordinal only : 135 is
// AllowDarkModeForApp(BOOL) on 1809 and SetPreferredAppMode(AllowDark) since
// 1903 ; both take 1 to allow.
static VOID AllowDarkModeForApp()
{
	static BOOL fDone = FALSE;
	if (fDone)
	{
		return;
	}
	fDone = TRUE;
	if (GetWindowsBuildNumber() < 17763)
	{
		return;
	}
	HMODULE hUxtheme = GetModuleHandle(TEXT("uxtheme.dll"));
	typedef int (WINAPI *SetPreferredAppModeProc)(int);
	SetPreferredAppModeProc pSetPreferredAppMode = hUxtheme ? (SetPreferredAppModeProc) GetProcAddress(hUxtheme, MAKEINTRESOURCEA(135)) : NULL;
	if (pSetPreferredAppMode)
	{
		pSetPreferredAppMode(1);
	}
}

VOID ApplyDarkTitleBar(__in HWND hWnd)
{
	BOOL fDark = IsDarkModeEnabled();
	// DWMWA_USE_IMMERSIVE_DARK_MODE is 20 since Windows 10 20H1/Windows 11 ;
	// earlier 1809-1909 builds used the (then undocumented) value 19. A
	// build that doesn't recognize either just ignores it (E_INVALIDARG).
	if (FAILED(DwmSetWindowAttribute(hWnd, 20, &fDark, sizeof(fDark))))
	{
		DwmSetWindowAttribute(hWnd, 19, &fDark, sizeof(fDark));
	}
}

COLORREF GetThemedBackgroundColor()
{
	return IsDarkModeEnabled() ? DARK_BACKGROUND : RGB(0xFF, 0xFF, 0xFF);
}

COLORREF GetThemedTextColor()
{
	return IsDarkModeEnabled() ? DARK_TEXT : RGB(0x00, 0x00, 0x00);
}

COLORREF GetThemedTitleColor()
{
	// the same blue used in light mode reads poorly on a dark background
	return IsDarkModeEnabled() ? RGB(0x66, 0x99, 0xFF) : RGB(0x00, 0x33, 0x99);
}

COLORREF GetThemedControlColor()
{
	return IsDarkModeEnabled() ? DARK_CONTROL : GetSysColor(COLOR_WINDOW);
}

HBRUSH GetThemedDialogBrush()
{
	static HBRUSH hbrDark = NULL;
	if (IsDarkModeEnabled())
	{
		if (!hbrDark)
		{
			hbrDark = CreateSolidBrush(DARK_BACKGROUND);
		}
		return hbrDark;
	}
	return (HBRUSH) GetStockObject(WHITE_BRUSH);
}

static HBRUSH GetDarkControlBrush()
{
	static HBRUSH hbr = NULL;
	if (!hbr)
	{
		hbr = CreateSolidBrush(DARK_CONTROL);
	}
	return hbr;
}

// The logo (dark strokes + blue on white) is treated as a color painted over a
// white sheet : the coverage of each pixel is recovered from its lightest
// channel, and the color is painted again over the dark background. The dark
// strokes would vanish, so they are lightened.
HBITMAP GetThemedLogo(__in HBITMAP hLogo)
{
	static HBITMAP hSource = NULL;
	static HBITMAP hDark = NULL;
	if (!hLogo || !IsDarkModeEnabled())
	{
		return hLogo;
	}
	if (hSource == hLogo && hDark)
	{
		return hDark;
	}
	BITMAP bm;
	if (!GetObject(hLogo, sizeof(bm), &bm) || bm.bmWidth <= 0 || bm.bmHeight <= 0)
	{
		return hLogo;
	}
	BITMAPINFO bi = { 0 };
	bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bi.bmiHeader.biWidth = bm.bmWidth;
	bi.bmiHeader.biHeight = -bm.bmHeight; // top-down
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = 32;
	bi.bmiHeader.biCompression = BI_RGB;
	DWORD cPixels = (DWORD) bm.bmWidth * (DWORD) bm.bmHeight;
	PBYTE pBits = (PBYTE) LocalAlloc(LMEM_FIXED, cPixels * 4);
	HDC hdc = GetDC(NULL);
	HBITMAP hResult = NULL;
	if (pBits && hdc && GetDIBits(hdc, hLogo, 0, bm.bmHeight, pBits, &bi, DIB_RGB_COLORS))
	{
		const int bgR = GetRValue(DARK_BACKGROUND), bgG = GetGValue(DARK_BACKGROUND), bgB = GetBValue(DARK_BACKGROUND);
		for (DWORD i = 0; i < cPixels; i++)
		{
			PBYTE p = pBits + i * 4; // B G R X
			int b = p[0], g = p[1], r = p[2];
			int lightest = r > g ? (r > b ? r : b) : (g > b ? g : b);
			int darkest = r < g ? (r < b ? r : b) : (g < b ? g : b);
			int a = 255 - darkest; // coverage over white
			if (a <= 0)
			{
				p[0] = (BYTE) bgB; p[1] = (BYTE) bgG; p[2] = (BYTE) bgR;
				continue;
			}
			// the color painted at that coverage
			int cr = 255 - (255 - r) * 255 / a;
			int cg = 255 - (255 - g) * 255 / a;
			int cb = 255 - (255 - b) * 255 / a;
			if (cr < 0) cr = 0;
			if (cg < 0) cg = 0;
			if (cb < 0) cb = 0;
			// low saturation and dark : the text of the logo, shown light
			if (lightest - darkest < 48 && (cr + cg + cb) / 3 < 100)
			{
				cr = cg = cb = GetRValue(DARK_TEXT);
			}
			p[2] = (BYTE) ((bgR * (255 - a) + cr * a) / 255);
			p[1] = (BYTE) ((bgG * (255 - a) + cg * a) / 255);
			p[0] = (BYTE) ((bgB * (255 - a) + cb * a) / 255);
		}
		hResult = CreateCompatibleBitmap(hdc, bm.bmWidth, bm.bmHeight);
		if (hResult && !SetDIBits(hdc, hResult, 0, bm.bmHeight, pBits, &bi, DIB_RGB_COLORS))
		{
			DeleteObject(hResult);
			hResult = NULL;
		}
	}
	if (hdc)
	{
		ReleaseDC(NULL, hdc);
	}
	if (pBits)
	{
		LocalFree(pBits);
	}
	if (!hResult)
	{
		return hLogo;
	}
	if (hDark)
	{
		DeleteObject(hDark);
	}
	hSource = hLogo;
	hDark = hResult;
	return hDark;
}

// "DarkMode_DarkTheme" (recent Windows 11 builds) is the only dark variant of
// the Button class giving group boxes and radio buttons a light text. An
// unknown variant silently falls back to the light Button, so check the color.
static BOOL HasDarkThemeButtons()
{
	// only a success is remembered : a failed check must not disable it for
	// the rest of the session
	static BOOL fSupported = FALSE;
	if (!fSupported)
	{
		HTHEME hTheme = OpenThemeData(NULL, L"DarkMode_DarkTheme::Button");
		if (hTheme)
		{
			COLORREF color;
			if (SUCCEEDED(GetThemeColor(hTheme, BP_GROUPBOX, GBS_NORMAL, TMT_TEXTCOLOR, &color)) && GetGValue(color) > 0x80)
			{
				fSupported = TRUE;
			}
			CloseThemeData(hTheme);
		}
	}
	return fSupported;
}

static BOOL IsCheckBoxStyle(__in UINT uType)
{
	return uType == BS_CHECKBOX || uType == BS_AUTOCHECKBOX || uType == BS_3STATE || uType == BS_AUTO3STATE;
}

// switch a control to (or back from) the dark variant of its visual style
static VOID SetControlTheme(__in HWND hWnd, __in BOOL fDark, __in PCWSTR szDarkTheme)
{
	if (fDark)
	{
		SetProp(hWnd, DARK_CONTROL_PROP, (HANDLE) 1);
		SetWindowTheme(hWnd, szDarkTheme, szDarkTheme && szDarkTheme[0] == 0 ? L"" : NULL);
	}
	else if (RemoveProp(hWnd, DARK_CONTROL_PROP))
	{
		SetWindowTheme(hWnd, NULL, NULL);
	}
}

// The header of a list view can't be given a light text by its visual style :
// it is drawn here, from the custom draw notification the list view receives.
static LRESULT CALLBACK ListViewSubclassProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
{
	UNREFERENCED_PARAMETER(dwRefData);
	if (message == WM_NOTIFY && IsDarkModeEnabled())
	{
		LPNMHDR pnmh = (LPNMHDR) lParam;
		if (pnmh->code == NM_CUSTOMDRAW && pnmh->hwndFrom == ListView_GetHeader(hWnd))
		{
			LPNMCUSTOMDRAW pcd = (LPNMCUSTOMDRAW) lParam;
			switch (pcd->dwDrawStage)
			{
			case CDDS_PREPAINT:
				return CDRF_NOTIFYITEMDRAW | CDRF_NOTIFYPOSTPAINT;
			case CDDS_POSTPAINT:
				{
					// the part of the header right of the last column is painted by the
					// control itself, in light : paint it over
					int cItems = (int) SendMessage(pnmh->hwndFrom, HDM_GETITEMCOUNT, 0, 0);
					RECT rcFill = pcd->rc;
					RECT rcLast;
					if (cItems > 0 && SendMessage(pnmh->hwndFrom, HDM_GETITEMRECT, cItems - 1, (LPARAM) &rcLast))
					{
						rcFill.left = rcLast.right;
					}
					HBRUSH hbrFill = CreateSolidBrush(DARK_CONTROL);
					FillRect(pcd->hdc, &rcFill, hbrFill);
					DeleteObject(hbrFill);
				}
				return CDRF_DODEFAULT;
			case CDDS_ITEMPREPAINT:
				{
					TCHAR szText[128] = TEXT("");
					HDITEM hdi = { 0 };
					hdi.mask = HDI_TEXT | HDI_FORMAT;
					hdi.pszText = szText;
					hdi.cchTextMax = ARRAYSIZE(szText);
					SendMessage(pnmh->hwndFrom, HDM_GETITEM, pcd->dwItemSpec, (LPARAM) &hdi);
					HBRUSH hbr = CreateSolidBrush(DARK_CONTROL);
					FillRect(pcd->hdc, &pcd->rc, hbr);
					DeleteObject(hbr);
					HPEN hPen = CreatePen(PS_SOLID, 1, RGB(0x50, 0x50, 0x50));
					HGDIOBJ hOldPen = SelectObject(pcd->hdc, hPen);
					MoveToEx(pcd->hdc, pcd->rc.right - 1, pcd->rc.top, NULL);
					LineTo(pcd->hdc, pcd->rc.right - 1, pcd->rc.bottom);
					MoveToEx(pcd->hdc, pcd->rc.left, pcd->rc.bottom - 1, NULL);
					LineTo(pcd->hdc, pcd->rc.right, pcd->rc.bottom - 1);
					SelectObject(pcd->hdc, hOldPen);
					DeleteObject(hPen);
					HFONT hFont = (HFONT) SendMessage(pnmh->hwndFrom, WM_GETFONT, 0, 0);
					HGDIOBJ hOldFont = SelectObject(pcd->hdc, hFont ? hFont : GetStockObject(DEFAULT_GUI_FONT));
					SetBkMode(pcd->hdc, TRANSPARENT);
					SetTextColor(pcd->hdc, DARK_TEXT);
					RECT rcText = pcd->rc;
					rcText.left += 6;
					rcText.right -= 4;
					DrawText(pcd->hdc, szText, -1, &rcText, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
					SelectObject(pcd->hdc, hOldFont);
					return CDRF_SKIPDEFAULT;
				}
			}
		}
	}
	else if (message == WM_NCDESTROY)
	{
		RemoveWindowSubclass(hWnd, ListViewSubclassProc, uIdSubclass);
	}
	return DefSubclassProc(hWnd, message, wParam, lParam);
}

static BOOL CALLBACK ThemeControlEnumProc(__in HWND hWnd, __in LPARAM lParam)
{
	BOOL fDark = (BOOL) lParam;
	TCHAR szClass[32];
	if (!GetClassName(hWnd, szClass, ARRAYSIZE(szClass)))
	{
		return TRUE;
	}
	if (_tcsicmp(szClass, TEXT("Button")) == 0)
	{
		UINT uType = GetWindowLong(hWnd, GWL_STYLE) & BS_TYPEMASK;
		switch (uType)
		{
		case BS_GROUPBOX:
		case BS_RADIOBUTTON:
		case BS_AUTORADIOBUTTON:
		case BS_CHECKBOX:
		case BS_AUTOCHECKBOX:
		case BS_3STATE:
		case BS_AUTO3STATE:
			if (!fDark)
			{
				SetControlTheme(hWnd, FALSE, NULL);
			}
			else if (HasDarkThemeButtons())
			{
				SetControlTheme(hWnd, TRUE, L"DarkMode_DarkTheme");
			}
			else if (IsCheckBoxStyle(uType))
			{
				// light text for check boxes since Windows 10 1809
				SetControlTheme(hWnd, TRUE, L"DarkMode_Explorer");
			}
			else
			{
				// no visual style at all : the classic drawing takes its
				// colors from WM_CTLCOLORBTN / WM_CTLCOLORSTATIC
				SetControlTheme(hWnd, TRUE, L"");
			}
			break;
		default:
			SetControlTheme(hWnd, fDark, L"DarkMode_Explorer");
			break;
		}
	}
	else if (_tcsicmp(szClass, TEXT("Edit")) == 0 || _tcsicmp(szClass, TEXT("ComboBox")) == 0)
	{
		SetControlTheme(hWnd, fDark, L"DarkMode_CFD");
	}
	else if (_tcsicmp(szClass, WC_LISTVIEW) == 0 || _tcsicmp(szClass, WC_TREEVIEW) == 0)
	{
		SetControlTheme(hWnd, fDark, L"DarkMode_Explorer");
		if (_tcsicmp(szClass, WC_LISTVIEW) == 0)
		{
			ThemeListView(hWnd);
			if (fDark)
			{
				SetWindowSubclass(hWnd, ListViewSubclassProc, 1, 0);
			}
			else
			{
				RemoveWindowSubclass(hWnd, ListViewSubclassProc, 1);
			}
		}
	}
	else if (_tcsicmp(szClass, TEXT("RichEdit20W")) == 0 || _tcsicmp(szClass, TEXT("RichEdit20A")) == 0 || _tcsicmp(szClass, TEXT("ScrollBar")) == 0)
	{
		// scrollbars of the rich edit
		SetControlTheme(hWnd, fDark, L"DarkMode_Explorer");
	}
	return TRUE;
}

VOID ThemeListView(__in HWND hListView)
{
	if (!hListView)
	{
		return;
	}
	if (IsDarkModeEnabled())
	{
		ListView_SetBkColor(hListView, DARK_CONTROL);
		ListView_SetTextBkColor(hListView, DARK_CONTROL);
		ListView_SetTextColor(hListView, DARK_TEXT);
	}
	else
	{
		// CLR_DEFAULT is not a recognized sentinel for LVM_SETBKCOLOR & co :
		// the real system colors have to be set explicitly
		ListView_SetBkColor(hListView, GetSysColor(COLOR_WINDOW));
		ListView_SetTextBkColor(hListView, GetSysColor(COLOR_WINDOW));
		ListView_SetTextColor(hListView, GetSysColor(COLOR_WINDOWTEXT));
	}
	InvalidateRect(hListView, NULL, TRUE);
}

VOID ThemeDialogControls(__in HWND hDlg)
{
	BOOL fDark = IsDarkModeEnabled();
	if (fDark)
	{
		AllowDarkModeForApp();
	}
	ApplyDarkTitleBar(hDlg);
	EnumChildWindows(hDlg, ThemeControlEnumProc, (LPARAM) fDark);
	RedrawWindow(hDlg, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME | RDW_UPDATENOW);
}

static BOOL IsClass(__in HWND hWnd, __in PCTSTR szClass)
{
	TCHAR szName[32];
	return GetClassName(hWnd, szName, ARRAYSIZE(szName)) && _tcsicmp(szName, szClass) == 0;
}

BOOL DarkModeDialogProc(__in HWND hDlg, __in UINT message, __in WPARAM wParam, __in LPARAM lParam, __out INT_PTR* plResult)
{
	*plResult = FALSE;
	switch (message)
	{
	case WM_INITDIALOG:
		ThemeDialogControls(hDlg);
		return FALSE;
	case WM_SETTINGCHANGE:
		if (lParam != 0 && wcscmp((LPCWSTR) lParam, L"ImmersiveColorSet") == 0)
		{
			ThemeDialogControls(hDlg);
		}
		return FALSE;
	}
	// in light mode the dialog keeps the system colors
	if (!IsDarkModeEnabled())
	{
		return FALSE;
	}
	HDC hdc = (HDC) wParam;
	switch (message)
	{
	case WM_CTLCOLORDLG:
		*plResult = (INT_PTR) GetThemedDialogBrush();
		return TRUE;
	case WM_CTLCOLORBTN:
		SetBkMode(hdc, TRANSPARENT);
		*plResult = (INT_PTR) GetThemedDialogBrush();
		return TRUE;
	case WM_CTLCOLOREDIT:
	case WM_CTLCOLORLISTBOX:
		SetTextColor(hdc, DARK_TEXT);
		SetBkColor(hdc, DARK_CONTROL);
		*plResult = (INT_PTR) GetDarkControlBrush();
		return TRUE;
	case WM_CTLCOLORSTATIC:
		if (IsClass((HWND) lParam, TEXT("Edit")))
		{
			// read-only edit box
			SetTextColor(hdc, DARK_TEXT);
			SetBkColor(hdc, DARK_CONTROL);
			*plResult = (INT_PTR) GetDarkControlBrush();
			return TRUE;
		}
		SetTextColor(hdc, DARK_TEXT);
		if (GetProp((HWND) lParam, DARK_CONTROL_PROP))
		{
			// group box / check box / radio button : opaque, the frame of a
			// classic group box would otherwise cross its caption
			SetBkColor(hdc, DARK_BACKGROUND);
			SetBkMode(hdc, OPAQUE);
			*plResult = (INT_PTR) GetThemedDialogBrush();
			return TRUE;
		}
		SetBkMode(hdc, TRANSPARENT);
		*plResult = (INT_PTR) GetThemedDialogBrush();
		return TRUE;
	}
	return FALSE;
}

VOID GetVersionText(__out_ecount(cchBuffer) PWSTR szBuffer, __in DWORD cchBuffer)
{
	// %S : VER_PRODUCT_VERSION_STR is an ANSI string (with an embedded \0 for the version resource)
	StringCchPrintfW(szBuffer, cchBuffer, L"Version %S", VER_PRODUCT_VERSION_STR);
}

VOID GetCopyrightText(__out_ecount(cchBuffer) PWSTR szBuffer, __in DWORD cchBuffer)
{
	StringCchPrintfW(szBuffer, cchBuffer, L"\x00A9 %S MySmartLogon.com", VER_COPYRIGHT_YEARS_STR);
}
