// Access to a GIDS applet through WinSCard : state of the card and initialization
// (PIN, admin key, file system), the same sequence as gids-tool --initialize.

#pragma once

#include <windows.h>
#include <winscard.h>

#define GIDS_ADMIN_KEY_SIZE   24    // 3DES key
#define GIDS_SERIAL_SIZE      16    // card id of the minidriver
#define GIDS_PIN_MIN_SIZE     4
#define GIDS_PIN_MAX_SIZE     16

typedef enum _GIDS_CARD_STATE
{
	GidsNoReader,           // the reader is gone
	GidsNoCard,             // no card in the reader
	GidsCardMute,           // cannot be connected to, or used exclusively by another program
	GidsNotGids,            // no GIDS applet on the card
	GidsNotInitialized,     // the applet is installed and waits for its initialization
	GidsInitialized,        // already initialized
} GIDS_CARD_STATE;

GIDS_CARD_STATE GidsProbe(__in SCARDCONTEXT hContext, __in PCWSTR szReader);

// the step that failed (NULL on success) and why : status word or WinSCard error
typedef struct _GIDS_INIT_RESULT
{
	PCWSTR szFailedStep;
	WORD   wStatusWord;
	LONG   lSCardError;
} GIDS_INIT_RESULT;

BOOL GidsInitialize(
	__in SCARDCONTEXT hContext,
	__in PCWSTR szReader,
	__in PCSTR szPin,
	__in_bcount(GIDS_ADMIN_KEY_SIZE) const BYTE* pbAdminKey,
	__in_bcount(GIDS_SERIAL_SIZE) const BYTE* pbSerial,
	__out GIDS_INIT_RESULT* pResult);

PCWSTR GidsStatusWordText(__in WORD wStatusWord);
