// Access to a GIDS applet - see Gids.h
//
// The initialization is the one of gids_initialize() in OpenSC (card-gids.c).
// Until the activation the applet is in its creation state and accepts any
// command without authentication.

#include <windows.h>
#include <winscard.h>
#include "Gids.h"

#pragma comment(lib, "winscard.lib")

#define SW_OK                   0x9000
#define MAX_RESPONSE            258

static const BYTE GIDS_AID[] = { 0xA0, 0x00, 0x00, 0x03, 0x97, 0x42, 0x54, 0x46, 0x59 };

// DO of the master file (in the EF A000), present once the card is initialized
#define MF_FI                   0xA000
#define MF_DO                   0xDF1F
#define CARDAPPS_FI             0xA010
#define CARDAPPS_DO             0xDF21
#define CARDCF_FI               0xA010
#define CARDCF_DO               0xDF22
#define CMAP_FI                 0xA010
#define CMAP_DO                 0xDF23
#define CARDID_FI               0xA012
#define CARDID_DO               0xDF20
#define GIDS_APPLET_EFID        0x3FFF
#define GIDS_PUT_KEY_DO         0x70

typedef struct _GIDS_CARD
{
	SCARDHANDLE hCard;
	DWORD       dwProtocol;
} GIDS_CARD;

// sends an APDU and handles 61xx (GET RESPONSE) and 6Cxx (wrong Le) : the
// caller only sees the final status word
static LONG Transmit(
	__in GIDS_CARD* pCard,
	__in_bcount(cbCommand) const BYTE* pbCommand,
	__in DWORD cbCommand,
	__out_bcount_opt(*pcbData) PBYTE pbData,
	__inout_opt PDWORD pcbData,
	__out PWORD pwSW)
{
	LPCSCARD_IO_REQUEST pci = pCard->dwProtocol == SCARD_PROTOCOL_T1 ? SCARD_PCI_T1 : SCARD_PCI_T0;
	BYTE  command[5 + 255 + 1];
	BYTE  response[MAX_RESPONSE];
	DWORD cbResponse = sizeof(response);
	DWORD cbData = 0;
	DWORD cbDataMax = (pbData && pcbData) ? *pcbData : 0;

	if (cbCommand > sizeof(command))
	{
		return SCARD_E_INVALID_PARAMETER;
	}
	CopyMemory(command, pbCommand, cbCommand);
	*pwSW = 0;

	for (int iteration = 0; iteration < 32; iteration++)
	{
		cbResponse = sizeof(response);
		LONG lReturn = SCardTransmit(pCard->hCard, pci, command, cbCommand, NULL, response, &cbResponse);
		if (lReturn != SCARD_S_SUCCESS)
		{
			SecureZeroMemory(command, sizeof(command));
			return lReturn;
		}
		if (cbResponse < 2)
		{
			SecureZeroMemory(command, sizeof(command));
			return SCARD_F_COMM_ERROR;
		}
		BYTE sw1 = response[cbResponse - 2];
		BYTE sw2 = response[cbResponse - 1];
		DWORD cbChunk = cbResponse - 2;
		if (cbChunk > 0)
		{
			if (cbData + cbChunk > cbDataMax && pbData)
			{
				SecureZeroMemory(command, sizeof(command));
				return SCARD_E_INSUFFICIENT_BUFFER;
			}
			if (pbData)
			{
				CopyMemory(pbData + cbData, response, cbChunk);
			}
			cbData += cbChunk;
		}
		if (sw1 == 0x61)
		{
			// more data available
			BYTE getResponse[] = { 0x00, 0xC0, 0x00, 0x00, sw2 };
			CopyMemory(command, getResponse, sizeof(getResponse));
			cbCommand = sizeof(getResponse);
			continue;
		}
		if (sw1 == 0x6C && cbCommand >= 5)
		{
			// wrong Le : same command with the length given by the card
			command[cbCommand - 1] = sw2;
			continue;
		}
		*pwSW = (WORD) ((sw1 << 8) | sw2);
		break;
	}
	SecureZeroMemory(command, sizeof(command));
	if (pcbData)
	{
		*pcbData = cbData;
	}
	return SCARD_S_SUCCESS;
}

static LONG SelectGids(__in GIDS_CARD* pCard, __out PWORD pwSW)
{
	BYTE command[5 + sizeof(GIDS_AID) + 1] = { 0x00, 0xA4, 0x04, 0x00, (BYTE) sizeof(GIDS_AID) };
	CopyMemory(command + 5, GIDS_AID, sizeof(GIDS_AID));
	BYTE  fci[MAX_RESPONSE];
	DWORD cbFci = sizeof(fci);
	return Transmit(pCard, command, sizeof(command), fci, &cbFci, pwSW);
}

// the master file exists only once the card is initialized
static LONG GetMasterFile(__in GIDS_CARD* pCard, __out PWORD pwSW)
{
	BYTE command[] = { 0x00, 0xCB, (BYTE) (MF_FI >> 8), (BYTE) MF_FI, 0x04, 0x5C, 0x02, (BYTE) (MF_DO >> 8), (BYTE) MF_DO, 0x00 };
	BYTE  data[1024];
	DWORD cbData = sizeof(data);
	return Transmit(pCard, command, sizeof(command), data, &cbData, pwSW);
}

static LONG Connect(__in SCARDCONTEXT hContext, __in PCWSTR szReader, __out GIDS_CARD* pCard)
{
	pCard->hCard = 0;
	pCard->dwProtocol = 0;
	return SCardConnectW(hContext, szReader, SCARD_SHARE_SHARED,
		SCARD_PROTOCOL_T0 | SCARD_PROTOCOL_T1, &pCard->hCard, &pCard->dwProtocol);
}

GIDS_CARD_STATE GidsProbe(__in SCARDCONTEXT hContext, __in PCWSTR szReader)
{
	SCARD_READERSTATEW state = { 0 };
	state.szReader = szReader;
	state.dwCurrentState = SCARD_STATE_UNAWARE;
	LONG lReturn = SCardGetStatusChangeW(hContext, 0, &state, 1);
	if (lReturn != SCARD_S_SUCCESS || (state.dwEventState & (SCARD_STATE_UNKNOWN | SCARD_STATE_UNAVAILABLE)))
	{
		return GidsNoReader;
	}
	if (!(state.dwEventState & SCARD_STATE_PRESENT))
	{
		return GidsNoCard;
	}
	if (state.dwEventState & (SCARD_STATE_MUTE | SCARD_STATE_EXCLUSIVE))
	{
		return GidsCardMute;
	}

	GIDS_CARD card;
	if (Connect(hContext, szReader, &card) != SCARD_S_SUCCESS)
	{
		return GidsCardMute;
	}
	GIDS_CARD_STATE result = GidsCardMute;
	if (SCardBeginTransaction(card.hCard) == SCARD_S_SUCCESS)
	{
		WORD sw;
		if (SelectGids(&card, &sw) != SCARD_S_SUCCESS)
		{
			result = GidsCardMute;
		}
		else if (sw != SW_OK)
		{
			result = GidsNotGids;
		}
		else if (GetMasterFile(&card, &sw) != SCARD_S_SUCCESS)
		{
			result = GidsCardMute;
		}
		else
		{
			result = sw == SW_OK ? GidsInitialized : GidsNotInitialized;
		}
		SCardEndTransaction(card.hCard, SCARD_LEAVE_CARD);
	}
	SCardDisconnect(card.hCard, SCARD_LEAVE_CARD);
	return result;
}

// records the failing step
static BOOL Step(
	__in GIDS_CARD* pCard,
	__in PCWSTR szStep,
	__in_bcount(cbCommand) const BYTE* pbCommand,
	__in DWORD cbCommand,
	__inout GIDS_INIT_RESULT* pResult)
{
	WORD sw = 0;
	LONG lReturn = Transmit(pCard, pbCommand, cbCommand, NULL, NULL, &sw);
	if (lReturn != SCARD_S_SUCCESS || sw != SW_OK)
	{
		pResult->szFailedStep = szStep;
		pResult->wStatusWord = sw;
		pResult->lSCardError = lReturn;
		return FALSE;
	}
	return TRUE;
}

// CREATE FILE with its FCP, then ACTIVATE FILE
static BOOL CreateGidsFile(
	__in GIDS_CARD* pCard,
	__in PCWSTR szStep,
	__in_bcount(cbFcp) const BYTE* pbFcp,
	__in BYTE cbFcp,
	__inout GIDS_INIT_RESULT* pResult)
{
	BYTE command[5 + 255] = { 0x00, 0xE0, 0x00, 0x00, cbFcp };
	CopyMemory(command + 5, pbFcp, cbFcp);
	static const BYTE activate[] = { 0x00, 0x44, 0x00, 0x00 };
	return Step(pCard, szStep, command, 5 + cbFcp, pResult)
		&& Step(pCard, szStep, activate, sizeof(activate), pResult);
}

// PUT DATA of a BER-TLV data object in an EF
static BOOL PutDO(
	__in GIDS_CARD* pCard,
	__in PCWSTR szStep,
	__in WORD wFileIdentifier,
	__in WORD wDataObject,
	__in_bcount_opt(cbData) const BYTE* pbData,
	__in DWORD cbData,
	__inout GIDS_INIT_RESULT* pResult)
{
	BYTE  command[5 + 255];
	DWORD cbTlv = 0;
	BYTE* tlv = command + 5;

	if (wDataObject > 0xFF)
	{
		tlv[cbTlv++] = (BYTE) (wDataObject >> 8);
	}
	tlv[cbTlv++] = (BYTE) wDataObject;
	if (cbData > 0x7F)
	{
		tlv[cbTlv++] = 0x81;
	}
	tlv[cbTlv++] = (BYTE) cbData;
	if (cbTlv + cbData > 255)
	{
		pResult->szFailedStep = szStep;
		pResult->lSCardError = SCARD_E_INVALID_PARAMETER;
		return FALSE;
	}
	if (cbData)
	{
		CopyMemory(tlv + cbTlv, pbData, cbData);
	}
	cbTlv += cbData;

	command[0] = 0x00;
	command[1] = 0xDB;
	command[2] = (BYTE) (wFileIdentifier >> 8);
	command[3] = (BYTE) wFileIdentifier;
	command[4] = (BYTE) cbTlv;
	BOOL fReturn = Step(pCard, szStep, command, 5 + cbTlv, pResult);
	SecureZeroMemory(command, sizeof(command));
	return fReturn;
}

static BOOL InitializeSteps(
	__in GIDS_CARD* pCard,
	__in PCSTR szPin,
	__in_bcount(GIDS_ADMIN_KEY_SIZE) const BYTE* pbAdminKey,
	__in_bcount(GIDS_SERIAL_SIZE) const BYTE* pbSerial,
	__inout GIDS_INIT_RESULT* pResult)
{
	// BER-TLV EFs (type 39) used as directories : the ACL (8C) applies to all their DO
	static const BYTE UserCreateDeleteDirAc[]    = { 0x62, 0x0C, 0x82, 0x01, 0x39, 0x83, 0x02, 0xA0, 0x00, 0x8C, 0x03, 0x03, 0x30, 0x00 };
	static const BYTE EveryoneReadUserWriteAc[]  = { 0x62, 0x0C, 0x82, 0x01, 0x39, 0x83, 0x02, 0xA0, 0x10, 0x8C, 0x03, 0x03, 0x30, 0x00 };
	static const BYTE UserWriteExecuteAc[]       = { 0x62, 0x0C, 0x82, 0x01, 0x39, 0x83, 0x02, 0xA0, 0x11, 0x8C, 0x03, 0x03, 0x30, 0xFF };
	static const BYTE EveryoneReadAdminWriteAc[] = { 0x62, 0x0C, 0x82, 0x01, 0x39, 0x83, 0x02, 0xA0, 0x12, 0x8C, 0x03, 0x03, 0x20, 0x00 };
	static const BYTE UserReadWriteAc[]          = { 0x62, 0x0C, 0x82, 0x01, 0x39, 0x83, 0x02, 0xA0, 0x13, 0x8C, 0x03, 0x03, 0x30, 0x30 };
	static const BYTE AdminReadWriteAc[]         = { 0x62, 0x0C, 0x82, 0x01, 0x39, 0x83, 0x02, 0xA0, 0x14, 0x8C, 0x03, 0x03, 0x20, 0x20 };
	// key file (type 18) of the 3DES admin key
	static const BYTE AdminKeyFile[] = { 0x62, 0x1A, 0x82, 0x01, 0x18, 0x83, 0x02, 0xB0, 0x80, 0x8C, 0x04, 0x87, 0x00, 0x20, 0xFF, 0xA5,
		0x0B, 0xA4, 0x09, 0x80, 0x01, 0x02, 0x83, 0x01, 0x80, 0x95, 0x01, 0xC0 };
	// file table of the minidriver (undocumented format) : mscp, cardid, cardapps, cardcf, mscp\cmapfile
	static const BYTE MasterFile[] = { 0x01,
		0x6d, 0x73, 0x63, 0x70, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xa0, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x63, 0x61, 0x72, 0x64, 0x69, 0x64, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0xdf, 0x00, 0x00, 0x12, 0xa0, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x63, 0x61, 0x72, 0x64, 0x61, 0x70, 0x70, 0x73, 0x00, 0x00, 0x00, 0x21, 0xdf, 0x00, 0x00, 0x10, 0xa0, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x63, 0x61, 0x72, 0x64, 0x63, 0x66, 0x00, 0x00, 0x00, 0x00, 0x00, 0x22, 0xdf, 0x00, 0x00, 0x10, 0xa0, 0x00, 0x00,
		0x6d, 0x73, 0x63, 0x70, 0x00, 0x00, 0x00, 0x00, 0x00, 0x63, 0x6d, 0x61, 0x70, 0x66, 0x69, 0x6c, 0x65, 0x00, 0x00, 0x00, 0x23, 0xdf, 0x00, 0x00, 0x10, 0xa0, 0x00, 0x00 };
	static const BYTE CardApps[] = { 0x6d, 0x73, 0x63, 0x70, 0x00, 0x00, 0x00, 0x00 };
	static const BYTE CardCf[]   = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
	static const BYTE SelectApplication[] = { 0x00, 0xA4, 0x00, 0x0C, 0x02, (BYTE) (GIDS_APPLET_EFID >> 8), (BYTE) GIDS_APPLET_EFID };
	static const BYTE Activate[] = { 0x00, 0x44, 0x00, 0x00 };

	// user PIN : CHANGE REFERENCE DATA without the old value
	{
		size_t cbPin = lstrlenA(szPin);
		BYTE command[5 + GIDS_PIN_MAX_SIZE] = { 0x00, 0x24, 0x01, 0x80, (BYTE) cbPin };
		if (cbPin < GIDS_PIN_MIN_SIZE || cbPin > GIDS_PIN_MAX_SIZE)
		{
			pResult->szFailedStep = L"setting the PIN";
			pResult->lSCardError = SCARD_E_INVALID_PARAMETER;
			return FALSE;
		}
		CopyMemory(command + 5, szPin, cbPin);
		BOOL fReturn = Step(pCard, L"setting the PIN", command, (DWORD) (5 + cbPin), pResult);
		SecureZeroMemory(command, sizeof(command));
		if (!fReturn)
		{
			return FALSE;
		}
	}

	if (!CreateGidsFile(pCard, L"creating the directory A000", UserCreateDeleteDirAc, sizeof(UserCreateDeleteDirAc), pResult)
		|| !CreateGidsFile(pCard, L"creating the directory A010", EveryoneReadUserWriteAc, sizeof(EveryoneReadUserWriteAc), pResult)
		|| !CreateGidsFile(pCard, L"creating the directory A011", UserWriteExecuteAc, sizeof(UserWriteExecuteAc), pResult)
		|| !CreateGidsFile(pCard, L"creating the directory A012", EveryoneReadAdminWriteAc, sizeof(EveryoneReadAdminWriteAc), pResult)
		|| !CreateGidsFile(pCard, L"creating the directory A013", UserReadWriteAc, sizeof(UserReadWriteAc), pResult)
		|| !CreateGidsFile(pCard, L"creating the directory A014", AdminReadWriteAc, sizeof(AdminReadWriteAc), pResult))
	{
		return FALSE;
	}

	// admin key
	if (!CreateGidsFile(pCard, L"creating the administrator key file", AdminKeyFile, sizeof(AdminKeyFile), pResult))
	{
		return FALSE;
	}
	{
		BYTE keyData[] = { 0x84, 0x01, 0x80,                 // key reference
			0xA5, 0x1F,                                      // key template
			0x87, 0x18,                                      // key value
			0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
			0x88, 0x03, 0xB0, 0x73, 0xDC };                  // key file
		CopyMemory(keyData + 7, pbAdminKey, GIDS_ADMIN_KEY_SIZE);
		BOOL fReturn = PutDO(pCard, L"setting the administrator key", GIDS_APPLET_EFID, GIDS_PUT_KEY_DO, keyData, sizeof(keyData), pResult);
		SecureZeroMemory(keyData, sizeof(keyData));
		if (!fReturn)
		{
			return FALSE;
		}
	}

	// files of the minidriver
	if (!PutDO(pCard, L"writing the master file", MF_FI, MF_DO, MasterFile, sizeof(MasterFile), pResult)
		|| !PutDO(pCard, L"writing cardapps", CARDAPPS_FI, CARDAPPS_DO, CardApps, sizeof(CardApps), pResult)
		|| !PutDO(pCard, L"writing cardcf", CARDCF_FI, CARDCF_DO, CardCf, sizeof(CardCf), pResult)
		|| !PutDO(pCard, L"writing cmapfile", CMAP_FI, CMAP_DO, NULL, 0, pResult)
		|| !PutDO(pCard, L"writing the serial number", CARDID_FI, CARDID_DO, pbSerial, GIDS_SERIAL_SIZE, pResult))
	{
		return FALSE;
	}

	// activation : the ACL are enforced from now on
	return Step(pCard, L"activating the card", SelectApplication, sizeof(SelectApplication), pResult)
		&& Step(pCard, L"activating the card", Activate, sizeof(Activate), pResult);
}

BOOL GidsInitialize(
	__in SCARDCONTEXT hContext,
	__in PCWSTR szReader,
	__in PCSTR szPin,
	__in_bcount(GIDS_ADMIN_KEY_SIZE) const BYTE* pbAdminKey,
	__in_bcount(GIDS_SERIAL_SIZE) const BYTE* pbSerial,
	__out GIDS_INIT_RESULT* pResult)
{
	ZeroMemory(pResult, sizeof(*pResult));

	GIDS_CARD card;
	LONG lReturn = Connect(hContext, szReader, &card);
	if (lReturn != SCARD_S_SUCCESS)
	{
		pResult->szFailedStep = L"connecting to the card";
		pResult->lSCardError = lReturn;
		return FALSE;
	}
	lReturn = SCardBeginTransaction(card.hCard);
	if (lReturn != SCARD_S_SUCCESS)
	{
		pResult->szFailedStep = L"connecting to the card";
		pResult->lSCardError = lReturn;
		SCardDisconnect(card.hCard, SCARD_LEAVE_CARD);
		return FALSE;
	}

	BOOL fReturn = FALSE;
	WORD sw = 0;
	lReturn = SelectGids(&card, &sw);
	if (lReturn != SCARD_S_SUCCESS || sw != SW_OK)
	{
		pResult->szFailedStep = L"selecting the GIDS applet";
		pResult->wStatusWord = sw;
		pResult->lSCardError = lReturn;
	}
	else if (GetMasterFile(&card, &sw) == SCARD_S_SUCCESS && sw == SW_OK)
	{
		// never twice : the file system would be duplicated
		pResult->szFailedStep = L"checking the card (it is already initialized)";
		pResult->wStatusWord = sw;
	}
	else
	{
		fReturn = InitializeSteps(&card, szPin, pbAdminKey, pbSerial, pResult);
	}

	SCardEndTransaction(card.hCard, SCARD_LEAVE_CARD);
	// reset : the minidriver must not keep what it read from the blank card
	SCardDisconnect(card.hCard, fReturn ? SCARD_RESET_CARD : SCARD_LEAVE_CARD);
	return fReturn;
}

PCWSTR GidsStatusWordText(__in WORD wStatusWord)
{
	switch (wStatusWord)
	{
	case 0x6982: return L"security status not satisfied";
	case 0x6985: return L"conditions of use not satisfied";
	case 0x6A80: return L"incorrect data";
	case 0x6A82: return L"file or application not found";
	case 0x6A84: return L"not enough memory on the card";
	case 0x6A89: return L"the file already exists";
	case 0x6D00: return L"instruction not supported";
	case 0x6E00: return L"class not supported";
	case 0x6F00: return L"internal error of the applet";
	}
	return L"unexpected status";
}
