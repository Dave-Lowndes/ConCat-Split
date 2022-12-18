#include <afxmt.h>
#include <afxwin.h>
#include <windowsx.h>
#include <vector>
#include <optional>
#include "CommonDlg.h"
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

void CommonDlg::UIEnable() noexcept
{
	/* Re-enable the control windows */
	for ( const auto it : this->vhDisabledCtrls )
	{
		::EnableWindow( it, true );
	}

	/* Change the caption of the Cancel button back */
	{
		CString sClose( MAKEINTRESOURCE( IDS_CLOSE ) );
		SetDlgItemText( IDCANCEL, sClose );
	}

	/* Re-enable closing the dialog box */
	this->GetSystemMenu( FALSE )->EnableMenuItem( SC_CLOSE, MF_BYCOMMAND | MF_ENABLED );
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
				(GetWindowLongPtr( hWnd, GWL_STYLE ) & BS_GROUPBOX) != BS_GROUPBOX :
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

void CommonDlg::UIDisable() noexcept
{
	/* Move the keyboard focus to the cancel button before we disable anything */
	this->GotoDlgCtrl( GetDlgItem( IDCANCEL ) );

	/* Disable the currently enabled child windows to prevent interaction */
	EnumChildWindows( this->m_hWnd, EnumChildProcToDisableSomeControls, reinterpret_cast<LPARAM>( &this->vhDisabledCtrls ) );

	/* Change the caption of the Close button to Cancel - it now functions to cancel the operation */
	{
		CString sCancel( MAKEINTRESOURCE( IDS_CANCEL ) );
		SetDlgItemText( IDCANCEL, sCancel );
	}

	/* Need to disable closing the dialog box */
	this->GetSystemMenu( FALSE )->EnableMenuItem( SC_CLOSE, MF_BYCOMMAND | MF_DISABLED | MF_GRAYED );
}

void TMBHandler( HWND hDlg, LPARAM lParam ) noexcept
{
	CThreadMessageBoxParams* pmbp = reinterpret_cast<CThreadMessageBoxParams*>(lParam);

	pmbp->RetVal = MessageBox( hDlg, pmbp->pText, pmbp->pCaption, pmbp->Type );
	SetEvent( g_hTMBEvent );
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

int ThreadMessageBox( HWND hParent, LPCTSTR lpText, LPCTSTR lpCaption, UINT Type ) noexcept
{
	CThreadMessageBoxParams mbp{ .pText = lpText, .pCaption = lpCaption, .Type = Type, .RetVal = 0 };

	PostMessage( hParent, UWM_TMB, 0, reinterpret_cast<LPARAM>(&mbp) );
	WaitForSingleObject( g_hTMBEvent, INFINITE );

	return mbp.RetVal;
}

void CommonDlg::OnWorkerFinished( std::span<const int> CtrlsToDisable, LPCTSTR szMsgBoxCaption ) noexcept
{
	/* Calculate the time it's taken to do the operation */
	UINT64 PeriodInMS = m_tim.GetTimeToNow();
	PeriodInMS = m_tim.AsMilliSeconds( PeriodInMS );

	/* Do the common operations to re-enable the UI aspects that were disabled during this operation */
	{
		UIEnable();

		/* Disable these to match the original state */
		for ( auto id : CtrlsToDisable )
		{
			GetDlgItem( id )->EnableWindow( false );
		}

		// Reset the progress control position
//		GetDlgItem( IDC_PROGRESS )->PostMessage( PBM_SETPOS, 0, 0 );
	}

	if ( m_tim.InUse() )
	{
		CString sMsg;
		sMsg.Format( _T( "The operation took: %I64u.%I64u seconds" ), PeriodInMS / 1'000, PeriodInMS % 1'000 );
		MessageBox( sMsg, szMsgBoxCaption, MB_OK );
	}
	else
	{
		/* If we've not canceled the operation */
		if ( !g_bCancel )
		{
			DisplayOperationCompleteMessage();

			/* Close the dialog - we're all done */
			EndDialog( IDCANCEL );
		}
	}
}

LRESULT CommonDlg::OnThreadMessageBox( WPARAM, LPARAM lParam )
{
	TMBHandler( this->m_hWnd, lParam );
	return 0;
}

LRESULT CommonDlg::OnUpdateProgress( int FileNum, LPCTSTR szPath )
{
	// FileNum is 0 based, so offset to show the number from 1 in the UI
	SetDlgItemInt( IDC_FNUM, FileNum + 1, false );

	{
		LPCTSTR pPath = PathFindFileName( szPath );

		{
			HWND hCtrl = GetDlgItem( IDC_CURRFILE )->m_hWnd;
			HDC hDC = ::GetDC( hCtrl );
			RECT rcCtrl;
			::GetWindowRect( hCtrl, &rcCtrl );
			HFONT hFont = GetWindowFont( hCtrl );
			/* It's best to see the RHS of the text because the numbers that change are at the RHS */
			{
				const int CtrlWidth = rcCtrl.right - rcCtrl.left;

				/* Create a memory DC with the same attributes as the control */
				HDC hMemDC = CreateCompatibleDC( hDC );
				HFONT hOldFont = static_cast<HFONT>( SelectObject( hMemDC, hFont ) );

				/* Exclude characters from the start of the string until it'll fit the control's width */
				for ( int Len = static_cast<int>( wcslen( pPath ) ); pPath < &szPath[_MAX_PATH]; pPath++, Len-- )
				{
					SIZE size;
					GetTextExtentPoint32( hMemDC, pPath, Len, &size );
					const auto Excess = size.cx - CtrlWidth;
					if ( Excess <= 0 )
					{
						break;
					}
				}

				SelectObject( hMemDC, hOldFont );

				DeleteDC( hMemDC );
			}

			::ReleaseDC( hCtrl, hDC );
		}

		SetDlgItemText( IDC_CURRFILE, pPath );
	}

	return 0;
}

void CommonDlg::OnSysCommand( UINT nID, LPARAM lParam )
{
	if ( (nID & 0x0FFF0) == SC_CLOSE )
	{
		/* If the default OK button is disabled, we must be doing the operation, so don't close */
		if ( !GetDlgItem( IDOK )->IsWindowEnabled() )
		{
			return;
		}
	}

	CDialog::OnSysCommand( nID, lParam );
}

void CommonDlg::OnClickedAbout()
{
	AboutHandler( this->m_hWnd,
					m_RegData,
					AfxGetResourceHandle(),
					szConcatAppName,
					_T("Thanks for using Concat/Split"),
					szRegistryKey,
					AfxGetInstanceHandle(),
					ProductCode::Concat,
					IDS_CLOSE_FOR_REG );
}

void CommonDlg::OnCancel()
{
	/* If we're in the middle of the operation, this is used to cancel the operation */
	/* If the default OK button is disabled, we must be doing the operation */
	if ( !GetDlgItem( IDOK )->IsWindowEnabled() )
	{
		InterlockedExchange( &g_bCancel, TRUE );
	}
	else
	{
		// Closes the dialog
		__super::OnCancel();
	}
}

BEGIN_MESSAGE_MAP( CommonDlg, CDialog )
	ON_WM_SYSCOMMAND()
	ON_BN_CLICKED( IDC_ABOUT, &CommonDlg::OnClickedAbout )
	ON_MESSAGE( UWM_TMB, OnThreadMessageBox )
END_MESSAGE_MAP()
