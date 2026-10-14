// GIDS Initialization Tool
//
// Initializes a card where the GIDS applet has just been installed. Without
// argument it shows a dialog ; with --pin it runs from the command line.

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <bcrypt.h>
#include <strsafe.h>
#include "resource.h"
#include "version.h"
#include "DarkMode.h"
#include "Gids.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "bcrypt.lib")

#define HEADER_MARGIN_PX    10
#define HEADER_TITLE        L"Initialize GIDS card"
#define TIMER_MONITOR       1
#define MONITOR_PERIOD_MS   1000

static HINSTANCE    g_hInst = NULL;
static SCARDCONTEXT g_hContext = 0;
static HBITMAP      g_hBmpLogo = NULL;
static int          g_cxBmp = 0;
static int          g_cyBmp = 0;
static HFONT        g_hFontTitle = NULL;
static HFONT        g_hFontMono = NULL;
// the generated admin key has been copied or saved
static BOOL         g_fAdminKeyKept = FALSE;
// to detect a reader or a card change
static PWSTR        g_szReaders = NULL;
static DWORD        g_dwLastEventState = 0;

static VOID BytesToHex(__in_bcount(cbData) const BYTE* pbData, __in DWORD cbData, __out_ecount(cbData * 2 + 1) PWSTR szHex)
{
	static const WCHAR digits[] = L"0123456789ABCDEF";
	for (DWORD i = 0; i < cbData; i++)
	{
		szHex[i * 2] = digits[pbData[i] >> 4];
		szHex[i * 2 + 1] = digits[pbData[i] & 0x0F];
	}
	szHex[cbData * 2] = L'\0';
}

// exactly cbData bytes, spaces ignored
static BOOL HexToBytes(__in PCWSTR szHex, __out_bcount(cbData) PBYTE pbData, __in DWORD cbData)
{
	DWORD dwNibble = 0;
	for (; *szHex; szHex++)
	{
		WCHAR c = *szHex;
		BYTE b;
		if (c == L' ') continue;
		if (c >= L'0' && c <= L'9') b = (BYTE) (c - L'0');
		else if (c >= L'a' && c <= L'f') b = (BYTE) (c - L'a' + 10);
		else if (c >= L'A' && c <= L'F') b = (BYTE) (c - L'A' + 10);
		else return FALSE;
		if (dwNibble >= cbData * 2) return FALSE;
		if (dwNibble % 2 == 0)
			pbData[dwNibble / 2] = (BYTE) (b << 4);
		else
			pbData[dwNibble / 2] |= b;
		dwNibble++;
	}
	return dwNibble == cbData * 2;
}

// 4 to 16 printable ASCII characters : what the minidriver sends as it is
static BOOL IsValidPin(__in PCWSTR szPin)
{
	size_t cchPin = lstrlenW(szPin);
	if (cchPin < GIDS_PIN_MIN_SIZE || cchPin > GIDS_PIN_MAX_SIZE)
	{
		return FALSE;
	}
	for (size_t i = 0; i < cchPin; i++)
	{
		if (szPin[i] < 0x20 || szPin[i] > 0x7E)
		{
			return FALSE;
		}
	}
	return TRUE;
}

static VOID SetRandomHex(__in HWND hEdit, __in DWORD cbData)
{
	BYTE  pbData[GIDS_ADMIN_KEY_SIZE];
	WCHAR szHex[GIDS_ADMIN_KEY_SIZE * 2 + 1];
	if (BCRYPT_SUCCESS(BCryptGenRandom(NULL, pbData, cbData, BCRYPT_USE_SYSTEM_PREFERRED_RNG)))
	{
		BytesToHex(pbData, cbData, szHex);
		SetWindowTextW(hEdit, szHex);
	}
	SecureZeroMemory(pbData, sizeof(pbData));
	SecureZeroMemory(szHex, sizeof(szHex));
}

static PCWSTR StateText(__in GIDS_CARD_STATE state)
{
	switch (state)
	{
	case GidsNoReader:       return L"The reader is not available.";
	case GidsNoCard:         return L"Insert a card in the reader.";
	case GidsCardMute:       return L"The card does not answer or is used by another program.";
	case GidsNotGids:        return L"There is no GIDS applet on this card. Install it first.";
	case GidsNotInitialized: return L"\x2714 GIDS applet ready to be initialized.";
	case GidsInitialized:    return L"This card is already initialized.";
	}
	return L"";
}

// the error, from the status word or the WinSCard code
static VOID FormatInitError(__in const GIDS_INIT_RESULT* pResult, __out_ecount(cchMessage) PWSTR szMessage, __in DWORD cchMessage)
{
	if (pResult->lSCardError != SCARD_S_SUCCESS)
	{
		WCHAR szError[256] = L"";
		FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, pResult->lSCardError, 0, szError, ARRAYSIZE(szError), NULL);
		StringCchPrintfW(szMessage, cchMessage, L"The initialization failed while %s.\n\n0x%08X %s",
			pResult->szFailedStep, pResult->lSCardError, szError);
	}
	else if (pResult->wStatusWord == 0x9000)
	{
		StringCchPrintfW(szMessage, cchMessage, L"The initialization failed while %s.", pResult->szFailedStep);
	}
	else
	{
		StringCchPrintfW(szMessage, cchMessage, L"The initialization failed while %s.\n\nThe card returned %04X (%s).",
			pResult->szFailedStep, pResult->wStatusWord, GidsStatusWordText(pResult->wStatusWord));
	}
}

static LONG EnsureContext()
{
	if (g_hContext && SCardIsValidContext(g_hContext) == SCARD_S_SUCCESS)
	{
		return SCARD_S_SUCCESS;
	}
	if (g_hContext)
	{
		SCardReleaseContext(g_hContext);
		g_hContext = 0;
	}
	return SCardEstablishContext(SCARD_SCOPE_USER, NULL, NULL, &g_hContext);
}

// readers as a multi-string, NULL when there is none - free it with SCardFreeMemory
static PWSTR ListReaders()
{
	PWSTR szReaders = NULL;
	DWORD cchReaders = SCARD_AUTOALLOCATE;
	if (EnsureContext() != SCARD_S_SUCCESS)
	{
		return NULL;
	}
	if (SCardListReadersW(g_hContext, NULL, (PWSTR) &szReaders, &cchReaders) != SCARD_S_SUCCESS)
	{
		return NULL;
	}
	return szReaders;
}

static BOOL SameMultiString(__in_opt PCWSTR sz1, __in_opt PCWSTR sz2)
{
	if (!sz1 || !sz2)
	{
		return sz1 == sz2;
	}
	for (;;)
	{
		if (lstrcmpW(sz1, sz2) != 0) return FALSE;
		if (!*sz1) return TRUE;
		sz1 += lstrlenW(sz1) + 1;
		sz2 += lstrlenW(sz2) + 1;
	}
}

static VOID GetSelectedReader(__in HWND hDlg, __out_ecount(cchReader) PWSTR szReader, __in DWORD cchReader)
{
	HWND hCombo = GetDlgItem(hDlg, IDC_READER);
	int iItem = ComboBox_GetCurSel(hCombo);
	szReader[0] = L'\0';
	if (iItem >= 0 && ComboBox_GetLBTextLen(hCombo, iItem) < (int) cchReader)
	{
		ComboBox_GetLBText(hCombo, iItem, szReader);
	}
}

static VOID UpdateStatus(__in HWND hDlg)
{
	WCHAR szReader[256];
	GIDS_CARD_STATE state = GidsNoReader;
	GetSelectedReader(hDlg, szReader, ARRAYSIZE(szReader));
	if (szReader[0] && EnsureContext() == SCARD_S_SUCCESS)
	{
		SCARD_READERSTATEW readerState = { 0 };
		readerState.szReader = szReader;
		readerState.dwCurrentState = SCARD_STATE_UNAWARE;
		if (SCardGetStatusChangeW(g_hContext, 0, &readerState, 1) == SCARD_S_SUCCESS)
		{
			g_dwLastEventState = readerState.dwEventState;
		}
		state = GidsProbe(g_hContext, szReader);
	}
	SetDlgItemTextW(hDlg, IDC_STATUS, szReader[0] ? StateText(state) : L"Connect a smart card reader.");
	EnableWindow(GetDlgItem(hDlg, IDOK), state == GidsNotInitialized);
}

// fills the list and selects the first reader whose card waits for its
// initialization, or keeps the current selection
static VOID UpdateReaders(__in HWND hDlg)
{
	HWND hCombo = GetDlgItem(hDlg, IDC_READER);
	WCHAR szSelected[256];
	GetSelectedReader(hDlg, szSelected, ARRAYSIZE(szSelected));

	if (g_szReaders)
	{
		SCardFreeMemory(g_hContext, g_szReaders);
	}
	g_szReaders = ListReaders();

	ComboBox_ResetContent(hCombo);
	int iSelected = -1;
	int iNew = -1;
	for (PCWSTR szReader = g_szReaders; szReader && *szReader; szReader += lstrlenW(szReader) + 1)
	{
		int iItem = ComboBox_AddString(hCombo, szReader);
		if (lstrcmpW(szReader, szSelected) == 0)
		{
			iSelected = iItem;
		}
		if (iNew < 0 && GidsProbe(g_hContext, szReader) == GidsNotInitialized)
		{
			iNew = iItem;
		}
	}
	if (iSelected < 0) iSelected = iNew;
	if (iSelected < 0 && ComboBox_GetCount(hCombo) > 0) iSelected = 0;
	ComboBox_SetCurSel(hCombo, iSelected);
	UpdateStatus(hDlg);
}

// timer : a reader plugged / unplugged or a card inserted / removed
static VOID MonitorReaders(__in HWND hDlg)
{
	PWSTR szReaders = ListReaders();
	BOOL fReadersChanged = !SameMultiString(szReaders, g_szReaders);
	if (szReaders)
	{
		SCardFreeMemory(g_hContext, szReaders);
	}
	if (fReadersChanged)
	{
		UpdateReaders(hDlg);
		return;
	}
	WCHAR szReader[256];
	GetSelectedReader(hDlg, szReader, ARRAYSIZE(szReader));
	if (!szReader[0])
	{
		return;
	}
	SCARD_READERSTATEW readerState = { 0 };
	readerState.szReader = szReader;
	readerState.dwCurrentState = g_dwLastEventState & ~SCARD_STATE_CHANGED;
	if (SCardGetStatusChangeW(g_hContext, 0, &readerState, 1) == SCARD_S_SUCCESS
		&& (readerState.dwEventState & SCARD_STATE_CHANGED))
	{
		UpdateStatus(hDlg);
	}
}

static VOID DrawHeader(__in HWND hWndHeader, __in HDC hdc)
{
	RECT rc;
	GetClientRect(hWndHeader, &rc);

	HBRUSH hbrBackground = CreateSolidBrush(GetThemedBackgroundColor());
	if (hbrBackground)
	{
		FillRect(hdc, &rc, hbrBackground);
		DeleteObject(hbrBackground);
	}

	if (g_hBmpLogo)
	{
		HDC hdcMem = CreateCompatibleDC(hdc);
		if (hdcMem)
		{
			HGDIOBJ hOld = SelectObject(hdcMem, GetThemedLogo(g_hBmpLogo));
			BitBlt(hdc, rc.right - g_cxBmp - HEADER_MARGIN_PX, (rc.bottom - g_cyBmp) / 2, g_cxBmp, g_cyBmp, hdcMem, 0, 0, SRCCOPY);
			SelectObject(hdcMem, hOld);
			DeleteDC(hdcMem);
		}
	}

	HGDIOBJ hOldFont = SelectObject(hdc, g_hFontTitle ? g_hFontTitle : GetStockObject(DEFAULT_GUI_FONT));
	SetBkMode(hdc, TRANSPARENT);
	SetTextColor(hdc, GetThemedTitleColor());
	RECT rcText = rc;
	rcText.left += HEADER_MARGIN_PX;
	DrawTextW(hdc, HEADER_TITLE, -1, &rcText, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

	// discrete version, top right
	WCHAR szVersion[64];
	LOGFONTW lfSmall = { 0 };
	GetVersionText(szVersion, ARRAYSIZE(szVersion));
	lfSmall.lfHeight = -9;
	StringCchCopyW(lfSmall.lfFaceName, ARRAYSIZE(lfSmall.lfFaceName), L"Segoe UI");
	HFONT hFontSmall = CreateFontIndirectW(&lfSmall);
	SelectObject(hdc, hFontSmall ? hFontSmall : GetStockObject(DEFAULT_GUI_FONT));
	SetTextColor(hdc, IsDarkModeEnabled() ? RGB(0x90, 0x90, 0x90) : RGB(0x80, 0x80, 0x80));
	RECT rcVersion = rc;
	rcVersion.top += 2;
	rcVersion.right -= 4;
	DrawTextW(hdc, szVersion, -1, &rcVersion, DT_RIGHT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
	SelectObject(hdc, hOldFont);
	if (hFontSmall)
	{
		DeleteObject(hFontSmall);
	}
}

// the text is kept out of the clipboard history and of the cloud clipboard
static VOID CopyToClipboard(__in HWND hDlg, __in PCWSTR szText)
{
	size_t cbText = (lstrlenW(szText) + 1) * sizeof(WCHAR);
	if (!OpenClipboard(hDlg))
	{
		return;
	}
	EmptyClipboard();
	HGLOBAL hText = GlobalAlloc(GMEM_MOVEABLE, cbText);
	if (hText)
	{
		CopyMemory(GlobalLock(hText), szText, cbText);
		GlobalUnlock(hText);
		if (!SetClipboardData(CF_UNICODETEXT, hText))
		{
			GlobalFree(hText);
		}
	}
	UINT uExclude = RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing");
	HGLOBAL hExclude = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
	if (uExclude && hExclude)
	{
		*(PDWORD) GlobalLock(hExclude) = 0;
		GlobalUnlock(hExclude);
		if (!SetClipboardData(uExclude, hExclude))
		{
			GlobalFree(hExclude);
		}
	}
	CloseClipboard();
}

static BOOL SaveAdminKey(__in HWND hDlg)
{
	WCHAR szFile[MAX_PATH] = L"GIDS admin key.txt";
	WCHAR szAdminKey[GIDS_ADMIN_KEY_SIZE * 2 + 1] = L"";
	WCHAR szSerial[GIDS_SERIAL_SIZE * 2 + 1] = L"";
	OPENFILENAMEW ofn = { sizeof(OPENFILENAMEW) };
	ofn.hwndOwner = hDlg;
	ofn.lpstrFilter = L"Text files (*.txt)\0*.txt\0All files\0*.*\0";
	ofn.lpstrFile = szFile;
	ofn.nMaxFile = ARRAYSIZE(szFile);
	ofn.lpstrDefExt = L"txt";
	ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
	if (!GetSaveFileNameW(&ofn))
	{
		return FALSE;
	}
	GetDlgItemTextW(hDlg, IDC_ADMINKEY, szAdminKey, ARRAYSIZE(szAdminKey));
	GetDlgItemTextW(hDlg, IDC_SERIAL, szSerial, ARRAYSIZE(szSerial));

	CHAR szContent[512];
	StringCchPrintfA(szContent, ARRAYSIZE(szContent),
		"GIDS card\r\nSerial number: %S\r\nAdmin key: %S\r\n", szSerial, szAdminKey);
	SecureZeroMemory(szAdminKey, sizeof(szAdminKey));

	BOOL fReturn = FALSE;
	HANDLE hFile = CreateFileW(szFile, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile != INVALID_HANDLE_VALUE)
	{
		DWORD cbWritten;
		fReturn = WriteFile(hFile, szContent, lstrlenA(szContent), &cbWritten, NULL);
		CloseHandle(hFile);
	}
	SecureZeroMemory(szContent, sizeof(szContent));
	if (!fReturn)
	{
		MessageBoxW(hDlg, L"The file could not be written.", HEADER_TITLE, MB_ICONERROR);
	}
	return fReturn;
}

static VOID OnInitializeCard(__in HWND hDlg)
{
	WCHAR szReader[256];
	WCHAR szPin[GIDS_PIN_MAX_SIZE + 2] = L"";
	WCHAR szPin2[GIDS_PIN_MAX_SIZE + 2] = L"";
	WCHAR szAdminKey[GIDS_ADMIN_KEY_SIZE * 2 + 16] = L"";
	WCHAR szSerial[GIDS_SERIAL_SIZE * 2 + 16] = L"";
	CHAR  szPinA[GIDS_PIN_MAX_SIZE + 1] = "";
	BYTE  pbAdminKey[GIDS_ADMIN_KEY_SIZE];
	BYTE  pbSerial[GIDS_SERIAL_SIZE];
	HWND  hFocus = NULL;
	PCWSTR szError = NULL;

	GetSelectedReader(hDlg, szReader, ARRAYSIZE(szReader));
	GetDlgItemTextW(hDlg, IDC_USERPIN, szPin, ARRAYSIZE(szPin));
	GetDlgItemTextW(hDlg, IDC_USERPIN2, szPin2, ARRAYSIZE(szPin2));
	GetDlgItemTextW(hDlg, IDC_ADMINKEY, szAdminKey, ARRAYSIZE(szAdminKey));
	GetDlgItemTextW(hDlg, IDC_SERIAL, szSerial, ARRAYSIZE(szSerial));

	if (!IsValidPin(szPin))
	{
		szError = L"The PIN must be 4 to 16 characters long (letters, digits or symbols, no accent).";
		hFocus = GetDlgItem(hDlg, IDC_USERPIN);
	}
	else if (lstrcmpW(szPin, szPin2) != 0)
	{
		szError = L"The two PINs are different.";
		hFocus = GetDlgItem(hDlg, IDC_USERPIN2);
	}
	else if (!HexToBytes(szAdminKey, pbAdminKey, sizeof(pbAdminKey)))
	{
		szError = L"The admin key must be 48 hexadecimal characters (24 bytes).";
		hFocus = GetDlgItem(hDlg, IDC_ADMINKEY);
	}
	else if (!HexToBytes(szSerial, pbSerial, sizeof(pbSerial)))
	{
		szError = L"The serial number must be 32 hexadecimal characters (16 bytes).";
		hFocus = GetDlgItem(hDlg, IDC_SERIAL);
	}
	else if (!g_fAdminKeyKept
		&& MessageBoxW(hDlg, L"The admin key has been neither copied nor saved.\n\nWithout it, a card whose PIN is blocked cannot be unblocked. Initialize the card anyway?",
			HEADER_TITLE, MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2) != IDYES)
	{
		hFocus = GetDlgItem(hDlg, IDC_SAVEADMINKEY);
	}
	else
	{
		GIDS_INIT_RESULT result;
		WideCharToMultiByte(CP_ACP, 0, szPin, -1, szPinA, sizeof(szPinA), NULL, NULL);
		HCURSOR hOldCursor = SetCursor(LoadCursor(NULL, IDC_WAIT));
		BOOL fSuccess = GidsInitialize(g_hContext, szReader, szPinA, pbAdminKey, pbSerial, &result);
		SetCursor(hOldCursor);
		if (fSuccess)
		{
			MessageBoxW(hDlg, L"The card has been initialized.\n\nIt can now receive certificates: enrollment, import of a PFX file or OpenSC.", HEADER_TITLE, MB_ICONINFORMATION);
			SetDlgItemTextW(hDlg, IDC_USERPIN, L"");
			SetDlgItemTextW(hDlg, IDC_USERPIN2, L"");
			SetRandomHex(GetDlgItem(hDlg, IDC_SERIAL), GIDS_SERIAL_SIZE);
		}
		else
		{
			WCHAR szMessage[512];
			FormatInitError(&result, szMessage, ARRAYSIZE(szMessage));
			MessageBoxW(hDlg, szMessage, HEADER_TITLE, MB_ICONERROR);
		}
		UpdateStatus(hDlg);
	}

	if (szError)
	{
		MessageBoxW(hDlg, szError, HEADER_TITLE, MB_ICONWARNING);
	}
	if (hFocus)
	{
		SetFocus(hFocus);
		Edit_SetSel(hFocus, 0, -1);
	}
	SecureZeroMemory(szPin, sizeof(szPin));
	SecureZeroMemory(szPin2, sizeof(szPin2));
	SecureZeroMemory(szPinA, sizeof(szPinA));
	SecureZeroMemory(szAdminKey, sizeof(szAdminKey));
	SecureZeroMemory(pbAdminKey, sizeof(pbAdminKey));
}

static VOID OnInitDialog(__in HWND hDlg)
{
	SendMessage(hDlg, WM_SETICON, ICON_BIG, (LPARAM) LoadIcon(g_hInst, MAKEINTRESOURCE(IDI_MYSMARTLOGON)));
	SendMessage(hDlg, WM_SETICON, ICON_SMALL, (LPARAM) LoadImage(g_hInst, MAKEINTRESOURCE(IDI_MYSMARTLOGON), IMAGE_ICON, 16, 16, 0));

	g_hBmpLogo = LoadBitmap(g_hInst, MAKEINTRESOURCE(IDB_BANNER));
	if (g_hBmpLogo)
	{
		BITMAP bm;
		GetObject(g_hBmpLogo, sizeof(bm), &bm);
		g_cxBmp = bm.bmWidth;
		g_cyBmp = bm.bmHeight;
	}

	HFONT hFont = (HFONT) SendMessage(hDlg, WM_GETFONT, 0, 0);
	LOGFONTW lf;
	if (hFont && GetObjectW(hFont, sizeof(lf), &lf))
	{
		LONG lHeight = lf.lfHeight;
		lf.lfWeight = FW_SEMIBOLD;
		lf.lfHeight = lHeight * 3 / 2;
		g_hFontTitle = CreateFontIndirectW(&lf);
		lf.lfWeight = FW_NORMAL;
		lf.lfHeight = lHeight;
		StringCchCopyW(lf.lfFaceName, ARRAYSIZE(lf.lfFaceName), L"Consolas");
		g_hFontMono = CreateFontIndirectW(&lf);
	}
	if (g_hFontMono)
	{
		SendDlgItemMessage(hDlg, IDC_ADMINKEY, WM_SETFONT, (WPARAM) g_hFontMono, FALSE);
		SendDlgItemMessage(hDlg, IDC_SERIAL, WM_SETFONT, (WPARAM) g_hFontMono, FALSE);
	}
	Edit_LimitText(GetDlgItem(hDlg, IDC_USERPIN), GIDS_PIN_MAX_SIZE);
	Edit_LimitText(GetDlgItem(hDlg, IDC_USERPIN2), GIDS_PIN_MAX_SIZE);
	Edit_LimitText(GetDlgItem(hDlg, IDC_ADMINKEY), GIDS_ADMIN_KEY_SIZE * 2);
	Edit_LimitText(GetDlgItem(hDlg, IDC_SERIAL), GIDS_SERIAL_SIZE * 2);

	WCHAR szCopyright[128];
	GetCopyrightText(szCopyright, ARRAYSIZE(szCopyright));
	SetDlgItemTextW(hDlg, IDC_COPYRIGHT, szCopyright);

	SetRandomHex(GetDlgItem(hDlg, IDC_ADMINKEY), GIDS_ADMIN_KEY_SIZE);
	SetRandomHex(GetDlgItem(hDlg, IDC_SERIAL), GIDS_SERIAL_SIZE);
	g_fAdminKeyKept = FALSE;

	UpdateReaders(hDlg);
	SetTimer(hDlg, TIMER_MONITOR, MONITOR_PERIOD_MS, NULL);
	SetFocus(GetDlgItem(hDlg, IDC_USERPIN));
}

static INT_PTR CALLBACK MainDialogProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
	INT_PTR lResult;
	if (DarkModeDialogProc(hDlg, message, wParam, lParam, &lResult))
	{
		return lResult;
	}
	switch (message)
	{
	case WM_INITDIALOG:
		OnInitDialog(hDlg);
		return FALSE;
	case WM_TIMER:
		if (wParam == TIMER_MONITOR)
		{
			MonitorReaders(hDlg);
		}
		return TRUE;
	case WM_DRAWITEM:
		if (wParam == IDC_HEADER)
		{
			DrawHeader(((LPDRAWITEMSTRUCT) lParam)->hwndItem, ((LPDRAWITEMSTRUCT) lParam)->hDC);
			return TRUE;
		}
		break;
	case WM_SETTINGCHANGE:
		InvalidateRect(GetDlgItem(hDlg, IDC_HEADER), NULL, TRUE);
		break;
	case WM_COMMAND:
		switch (LOWORD(wParam))
		{
		case IDOK:
			OnInitializeCard(hDlg);
			return TRUE;
		case IDCANCEL:
			KillTimer(hDlg, TIMER_MONITOR);
			EndDialog(hDlg, IDCANCEL);
			return TRUE;
		case IDC_REFRESH:
			UpdateReaders(hDlg);
			return TRUE;
		case IDC_READER:
			if (HIWORD(wParam) == CBN_SELCHANGE)
			{
				UpdateStatus(hDlg);
			}
			return TRUE;
		case IDC_GENADMINKEY:
			SetRandomHex(GetDlgItem(hDlg, IDC_ADMINKEY), GIDS_ADMIN_KEY_SIZE);
			g_fAdminKeyKept = FALSE;
			return TRUE;
		case IDC_GENSERIAL:
			SetRandomHex(GetDlgItem(hDlg, IDC_SERIAL), GIDS_SERIAL_SIZE);
			return TRUE;
		case IDC_COPYADMINKEY:
			{
				WCHAR szAdminKey[GIDS_ADMIN_KEY_SIZE * 2 + 1];
				GetDlgItemTextW(hDlg, IDC_ADMINKEY, szAdminKey, ARRAYSIZE(szAdminKey));
				CopyToClipboard(hDlg, szAdminKey);
				SecureZeroMemory(szAdminKey, sizeof(szAdminKey));
				g_fAdminKeyKept = TRUE;
			}
			return TRUE;
		case IDC_SAVEADMINKEY:
			if (SaveAdminKey(hDlg))
			{
				g_fAdminKeyKept = TRUE;
			}
			return TRUE;
		case IDC_ADMINKEY:
			// a key typed by the user is already known
			if (HIWORD(wParam) == EN_CHANGE && GetFocus() == (HWND) lParam)
			{
				g_fAdminKeyKept = TRUE;
			}
			return TRUE;
		}
		break;
	}
	return FALSE;
}

static HANDLE g_hOutput = NULL;
static BOOL   g_fConsole = FALSE;

// the output of the console that started the program, or the redirected one
static VOID OpenOutput()
{
	DWORD dwMode;
	g_hOutput = GetStdHandle(STD_OUTPUT_HANDLE);
	if (!g_hOutput || g_hOutput == INVALID_HANDLE_VALUE)
	{
		g_hOutput = NULL;
		if (AttachConsole(ATTACH_PARENT_PROCESS))
		{
			g_hOutput = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
			if (g_hOutput == INVALID_HANDLE_VALUE)
			{
				g_hOutput = NULL;
			}
		}
	}
	g_fConsole = g_hOutput && GetConsoleMode(g_hOutput, &dwMode);
}

static VOID Print(__in PCWSTR szFormat, ...)
{
	WCHAR szBuffer[1024];
	va_list args;
	va_start(args, szFormat);
	StringCchVPrintfW(szBuffer, ARRAYSIZE(szBuffer), szFormat, args);
	va_end(args);
	if (!g_hOutput)
	{
		return;
	}
	DWORD dwWritten;
	if (g_fConsole)
	{
		WriteConsoleW(g_hOutput, szBuffer, lstrlenW(szBuffer), &dwWritten, NULL);
	}
	else
	{
		CHAR szUtf8[2048];
		int cbUtf8 = WideCharToMultiByte(CP_UTF8, 0, szBuffer, -1, szUtf8, sizeof(szUtf8), NULL, NULL);
		if (cbUtf8 > 1)
		{
			WriteFile(g_hOutput, szUtf8, cbUtf8 - 1, &dwWritten, NULL);
		}
	}
}

static VOID PrintUsage()
{
	Print(L"\r\nGIDS Initialization Tool %S\r\n\r\n"
		L"Usage: InitializeGids.exe --pin <pin> [--admin-key <48 hex>] [--serial <32 hex>] [--reader <name>]\r\n\r\n"
		L"  --pin <pin>         user PIN, 4 to 16 characters\r\n"
		L"  --admin-key <hex>   3DES admin key, 24 bytes (default: random, printed)\r\n"
		L"  --serial <hex>      serial number, 16 bytes (default: random)\r\n"
		L"  --reader <name>     reader (default: the first card waiting for its initialization)\r\n\r\n"
		L"Without argument, the dialog is shown. The exit code is 0 on success.\r\n"
		L"From cmd or PowerShell, pipe the output (| more) to wait for the result.\r\n",
		VER_PRODUCT_VERSION_STR);
}

static int RunCommandLine(__in int argc, __in PWSTR* argv)
{
	PCWSTR szPin = NULL;
	PCWSTR szAdminKey = NULL;
	PCWSTR szSerial = NULL;
	PCWSTR szReader = NULL;
	BYTE   pbAdminKey[GIDS_ADMIN_KEY_SIZE];
	BYTE   pbSerial[GIDS_SERIAL_SIZE];
	CHAR   szPinA[GIDS_PIN_MAX_SIZE + 1] = "";
	WCHAR  szHex[GIDS_ADMIN_KEY_SIZE * 2 + 1];
	int    iReturn = 1;

	OpenOutput();
	for (int i = 1; i < argc; i++)
	{
		BOOL fValue = i + 1 < argc;
		if (lstrcmpiW(argv[i], L"--pin") == 0 && fValue) szPin = argv[++i];
		else if (lstrcmpiW(argv[i], L"--admin-key") == 0 && fValue) szAdminKey = argv[++i];
		else if (lstrcmpiW(argv[i], L"--serial") == 0 && fValue) szSerial = argv[++i];
		else if (lstrcmpiW(argv[i], L"--reader") == 0 && fValue) szReader = argv[++i];
		else
		{
			PrintUsage();
			return lstrcmpiW(argv[i], L"--help") == 0 || lstrcmpiW(argv[i], L"/?") == 0 ? 0 : 1;
		}
	}
	if (!szPin || !IsValidPin(szPin))
	{
		Print(L"The PIN must be 4 to 16 printable ASCII characters.\r\n");
		return 1;
	}
	if (szAdminKey ? !HexToBytes(szAdminKey, pbAdminKey, sizeof(pbAdminKey))
		: !BCRYPT_SUCCESS(BCryptGenRandom(NULL, pbAdminKey, sizeof(pbAdminKey), BCRYPT_USE_SYSTEM_PREFERRED_RNG)))
	{
		Print(L"The admin key must be 48 hexadecimal characters.\r\n");
		return 1;
	}
	if (szSerial ? !HexToBytes(szSerial, pbSerial, sizeof(pbSerial))
		: !BCRYPT_SUCCESS(BCryptGenRandom(NULL, pbSerial, sizeof(pbSerial), BCRYPT_USE_SYSTEM_PREFERRED_RNG)))
	{
		Print(L"The serial number must be 32 hexadecimal characters.\r\n");
		return 1;
	}
	WideCharToMultiByte(CP_ACP, 0, szPin, -1, szPinA, sizeof(szPinA), NULL, NULL);

	PWSTR szReaders = ListReaders();
	if (!szReader)
	{
		for (PCWSTR sz = szReaders; sz && *sz; sz += lstrlenW(sz) + 1)
		{
			if (GidsProbe(g_hContext, sz) == GidsNotInitialized)
			{
				szReader = sz;
				break;
			}
		}
	}
	if (!szReader)
	{
		Print(L"No card waiting for its initialization was found.\r\n");
	}
	else
	{
		GIDS_INIT_RESULT result;
		Print(L"Reader: %s\r\n", szReader);
		if (GidsInitialize(g_hContext, szReader, szPinA, pbAdminKey, pbSerial, &result))
		{
			BytesToHex(pbSerial, sizeof(pbSerial), szHex);
			Print(L"Serial number: %s\r\n", szHex);
			if (!szAdminKey)
			{
				BytesToHex(pbAdminKey, sizeof(pbAdminKey), szHex);
				Print(L"Admin key: %s\r\n", szHex);
			}
			Print(L"The card has been initialized.\r\n");
			iReturn = 0;
		}
		else
		{
			WCHAR szMessage[512];
			FormatInitError(&result, szMessage, ARRAYSIZE(szMessage));
			Print(L"%s\r\n", szMessage);
		}
	}
	if (szReaders)
	{
		SCardFreeMemory(g_hContext, szReaders);
	}
	SecureZeroMemory(szPinA, sizeof(szPinA));
	SecureZeroMemory(pbAdminKey, sizeof(pbAdminKey));
	SecureZeroMemory(szHex, sizeof(szHex));
	return iReturn;
}

int APIENTRY wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPWSTR lpCmdLine, _In_ int nCmdShow)
{
	UNREFERENCED_PARAMETER(hPrevInstance);
	UNREFERENCED_PARAMETER(lpCmdLine);
	UNREFERENCED_PARAMETER(nCmdShow);
	int iReturn;
	int argc = 0;
	PWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);

	g_hInst = hInstance;
	if (argv && argc > 1)
	{
		iReturn = RunCommandLine(argc, argv);
	}
	else
	{
		INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES };
		InitCommonControlsEx(&icc);
		DialogBoxW(hInstance, MAKEINTRESOURCEW(IDD_MAIN), NULL, MainDialogProc);
		iReturn = 0;
	}
	if (argv)
	{
		LocalFree(argv);
	}
	if (g_szReaders)
	{
		SCardFreeMemory(g_hContext, g_szReaders);
	}
	if (g_hContext)
	{
		SCardReleaseContext(g_hContext);
	}
	return iReturn;
}
