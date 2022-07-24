#include <afxmt.h>
#include <vector>
#include <optional>
#include "CommonDlgData.h"
#include "Globals.h"
#include "resource.h"
#include "CommonThreadData.h"
#include "CheckForUpdate.h"
#include "AboutDlg.h"
#include "RegKeyRegistryFuncs.h"
#include "AboutDlgResourceIds.h"

using std::vector;
using std::optional;

bool bUpdateChecked = false;
// The signalling event for the ThreadMessageBox facility
HANDLE g_hTMBEvent;

LARGE_INTEGER CPerfTimer::m_Freq;

void CommonDlgData::UIEnable( HWND hParentWnd ) noexcept
{
	/* Re-enable the control windows */
	for ( const auto it : this->vhDisabledCtrls )
	{
		EnableWindow( it, true );
	}

	/* Change the caption of the Cancel button back */
	{
		TCHAR szCancel[30];

		LoadString( g_hResInst, IDS_CLOSE, szCancel, _countof( szCancel ) );
		SetDlgItemText( hParentWnd, IDCANCEL, szCancel );
	}

	/* Re-enable closing the dialog box */
	EnableMenuItem( ::GetSystemMenu( hParentWnd, FALSE ), SC_CLOSE, MF_BYCOMMAND | MF_ENABLED );
}

static BOOL CALLBACK EnumChildProcToDisableSomeControls( HWND hWnd, LPARAM lParam )
{
	if ( IsWindowEnabled( hWnd ) )
	{
		// Don't disable the Cancel/Close button (that's used to cancel the operation prematurely)
		if ( GetWindowLongPtr( hWnd, GWLP_ID ) != IDCANCEL )
		{
			// See https://docs.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-createwindoww for the predefined class names
			TCHAR szClass[20];
			const int NumChars = GetClassName( hWnd, szClass, _countof( szClass ) );

			const bool bIsButtonCtrl = (NumChars == sizeof( "Button" ) - 1) && (_wcsicmp( szClass, _T( "Button" ) ) == 0);

			const bool bDisableThisCtrl = bIsButtonCtrl ?
				// Don't disable any group boxes (buttons) - it causes a visual quirk with the overlaid check box
				(GetWindowLong( hWnd, GWL_STYLE ) & BS_GROUPBOX) != BS_GROUPBOX :
				// Don't disable static (label) controls
				!((NumChars == sizeof( "STATIC" ) - 1) && (_wcsicmp( szClass, _T( "STATIC" ) ) == 0));

			if ( bDisableThisCtrl )
			{
				EnableWindow( hWnd, false );

				/* Add the window handle to the vector that I use to re-enable them */
				auto pvControls = reinterpret_cast<vector<HWND> *>(lParam);

				pvControls->push_back( hWnd );
			}
		}
	}

	return TRUE;
}

void CommonDlgData::UIDisable( HWND hParentWnd ) noexcept
{
	/* Move the keyboard focus to the cancel button before we disable anything */
	SendMessage( hParentWnd, WM_NEXTDLGCTL, (WPARAM) GetDlgItem( hParentWnd, IDCANCEL ), TRUE );

	/* Disable the currently enabled child windows to prevent interaction */
	EnumChildWindows( hParentWnd, EnumChildProcToDisableSomeControls, (LPARAM) &this->vhDisabledCtrls );

	/* Change the caption of the Close button to Cancel - it now functions to cancel the operation */
	{
		TCHAR szCancel[30];

		LoadString( g_hResInst, IDS_CANCEL, szCancel, _countof( szCancel ) );
		SetDlgItemText( hParentWnd, IDCANCEL, szCancel );
	}

	/* Need to disable closing the dialog box */
	EnableMenuItem( ::GetSystemMenu( hParentWnd, FALSE ), SC_CLOSE, MF_BYCOMMAND | MF_DISABLED | MF_GRAYED );
}

void TMBHandler( HWND hDlg, LPARAM lParam ) noexcept
{
	CThreadMessageBoxParams* pmbp = reinterpret_cast<CThreadMessageBoxParams*>(lParam);

	pmbp->RetVal = MessageBox( hDlg, pmbp->pText, pmbp->pCaption, pmbp->Type );
	SetEvent( g_hTMBEvent );
}

int ModifyPathForControl( HDC hDC, HFONT hFont, RECT* rect, LPCTSTR pName ) noexcept
{
	/* Create a memory DC with the same attributes as the control */
	HDC hMemDC;
	hMemDC = CreateCompatibleDC( hDC );
	HFONT hOldFont = (HFONT) SelectObject( hMemDC, hFont );

	/* DrawText the string into the in-memory DC */
	DrawText( hMemDC, pName, -1, rect, DT_PATH_ELLIPSIS /*| DT_WORD_ELLIPSIS*/ | DT_MODIFYSTRING );

	/* Because DT_PATH_ELLIPSIS doesn't necessarily make the text fit, check it now
	 * and if it doesn't, make it right aligned so we can see the extension
	 * (which is the most important part here).
	 */
	SIZE size;

	GetTextExtentPoint32( hMemDC, pName, lstrlen( pName ), &size );

	int diff = size.cx - (rect->right - rect->left);
	if ( diff < 0 )
	{
		diff = 0;
	}

	SelectObject( hMemDC, hOldFont );

	DeleteDC( hMemDC );

	return diff;
}

constexpr size_t NumberOfCharactersToDisplayValue( size_t Value ) noexcept
{
	size_t DigitWidth = 0;

	do
	{
		Value /= 10;
		DigitWidth++;
	} while ( Value != 0 );

	return DigitWidth;
}

void CenterDialog( HWND hWnd ) noexcept
{
	HWND hWndParent = GetParent( hWnd );
	POINT   Point;
	RECT    DialogRect;
	RECT    ParentRect;
	int      nWidth;
	int      nHeight;

	// Get the size of the dialog box.
	GetWindowRect( hWnd, &DialogRect );

	// And the parent
	GetClientRect( hWndParent, &ParentRect );

	// Calculate the height and width for MoveWindow().
	nWidth = DialogRect.right - DialogRect.left;
	nHeight = DialogRect.bottom - DialogRect.top;

	// Find the center point and convert to screen coordinates.
	Point.x = (ParentRect.right/* - ParentRect.left*/) / 2;
	Point.y = (ParentRect.bottom/* - ParentRect.top*/) / 2;

	ClientToScreen( hWndParent, &Point );

	// Calculate the new X, Y starting point.
	Point.x -= nWidth / 2;
	Point.y -= nHeight / 2;

	// Move the window.
	MoveWindow( hWnd, Point.x, Point.y, nWidth, nHeight, FALSE );
}

void AboutHandler( HWND hDlg, const optional<CMyRegData>& RegData ) noexcept
{
	RegCheckData rcd( RegData, g_hInstance, ProductCode::Concat );

	DialogBoxParam( g_hResInst, MAKEINTRESOURCE( IDD_ABOUT_DLG ), hDlg, (DLGPROC) AboutDlg, reinterpret_cast<LPARAM>(&rcd) );

	if ( !bREGISTERED )
	{
		/* No point saving if there's nothing there! */
		if ( !rcd.sReturnedRegKey.empty() )
		{
			/* Store the entry to the registry ready for the next time */
			SaveMyRegistrationToTheRegistry( szRegistryKey, rcd.sReturnedRegKey );

			/* Message: If you've entered your registration details, press Close or Cancel to close the TouchPro dialog to have the changes take effect" */
			ResMessageBox( hDlg, IDS_CLOSE_FOR_REG, szAppName, MB_OK );
		}
	}
}

int ResMessageBox( HWND hWnd, int ResId, LPCTSTR pCaption, const int Flags ) noexcept
{
	TCHAR szMsg[256];

	::LoadString( g_hResInst, ResId, szMsg, _countof( szMsg ) );

	return(MessageBox( hWnd, szMsg, pCaption, Flags ));
}

int ThreadMessageBox( HWND hParent, LPCTSTR lpText, LPCTSTR lpCaption, UINT Type ) noexcept
{
	CThreadMessageBoxParams mbp;

	mbp.pText = lpText;
	mbp.pCaption = lpCaption;
	mbp.Type = Type;
	mbp.RetVal = 0;

	PostMessage( hParent, UWM_TMB, 0, reinterpret_cast<LPARAM>(&mbp) );
	WaitForSingleObject( g_hTMBEvent, INFINITE );

	return mbp.RetVal;
}
