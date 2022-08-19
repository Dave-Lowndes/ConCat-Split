#include <afxmt.h>
#include <afxwin.h>
#include <windowsx.h>
#include <CommCtrl.h>
#include <process.h>
#include <atlstr.h>
#include <algorithm>
#include <filesystem>
#include <string>
#include <thread>
#include <memory>

#include "CommonDlg.h"
#include "CSelPlusReg.h"
#include "ConcatThreadData.h"
#include "Globals.h"
#include "chandle.h"
#include "resource.h"
#include "CheckForUpdate.h"
#include "ConcatDlg.h"

using std::wstring;
namespace fs = std::filesystem;
using std::unique_ptr;

extern void __stdcall ConcatControlThread_Reader( unique_ptr<ConcatThreadData> ctd );

void CConcatDlg::ConcatEm( HWND hProgress, LPCTSTR szToName )
{
	{
		auto pctd( make_unique<ConcatThreadData>( m_Files, szToName, hProgress, this->m_hWnd ) );

		/* Disable all the UI components while the thread runs */
		UIDisable();

		/* Clear the cancel flag */
		InterlockedExchange( &g_bCancel, FALSE );

		std::thread cctr( ConcatControlThread_Reader, std::move( pctd ) );
		// Don't wait for the thread to finish
		cctr.detach();
	}
}

static bool FileExists( LPCTSTR pFileName ) noexcept
{
	bool bExists = false;

	CFileHandle hFile( CreateFile( pFileName, GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL ) );

	if ( hFile.IsValid() )
	{
		/* We can read the file, so it must exist */
		bExists = true;
	}
	else
	{
		const DWORD dwError = GetLastError();

		/* If it's not this error, then the file exists, but for some reason we can't open it */
		if ( dwError != ERROR_FILE_NOT_FOUND )
		{
			bExists = true;
		}
	}

	return(bExists);
}

BEGIN_MESSAGE_MAP( CConcatDlg, CommonDlg )
	ON_MESSAGE( UWM_WORKER_FINISHED, OnWorkerFinished )
	ON_MESSAGE( UWM_UPDATE_PROGRESS, OnUpdateProgress )
	ON_WM_MEASUREITEM()
	ON_WM_DRAWITEM()
	ON_WM_SYSCOMMAND()
	ON_LBN_SELCHANGE( IDC_COPY_LIST, &CConcatDlg::OnSelchangeCopyList )
	ON_COMMAND_RANGE( IDC_UP, IDC_DOWN, OnUpDown )
END_MESSAGE_MAP()

BOOL CConcatDlg::OnInitDialog()
{
	CommonDlg::OnInitDialog();

	{
		bREGISTERED = IsRegisteredAndValid( m_RegData, ProductCode::Concat );

		// sort them before adding to the list box to ensure they're in some order - hopefully the optimum one given the names the split operation will have used.
//		if ( bREGISTERED )
		{
			/* sort the filenames before adding them to the listbox */
			std::ranges::sort( m_Files );
		}

		const HWND hList = GetDlgItem( IDC_COPY_LIST )->m_hWnd;

		/* Displaying the path items using ellipsis notation */
		/* Add all the files to the list box */
		for ( const auto& itName : m_Files )
		{
			ListBox_AddString( hList, itName.c_str() );
		}

		/* Copy the first name to be the destination name */
		fs::path toPath{ m_Files.at( 0 ) };

		//		if ( bREGISTERED )
		{
			/* If the file has an extension > 3 characters long, and numerical,
			 * it's likely to be one of the split files created by Split.
			 * Therefore, attempt to "guess" the correct start name
			 */

			 /* Any numerical entries at the end of the extension are
			  * likely to have been created by Split, so remove them.
			  * The number of digits at the end are likely to be related
			  * to the number of selected files, so just remove those many.
			  */
			const size_t NumDigits = NumberOfCharactersToDisplayValue( m_Files.size() );

			// If there's a file extension
			wstring sNameExt = toPath.has_extension() ?
				/* use it */
				toPath.extension() :
				/* No extension, use the filename instead */
				toPath.filename();

			auto ExtLen = sNameExt.length();

			// At this point, expect ExtLen to be > NumDigits so that the following loop conditions are safe
			if ( ExtLen > NumDigits )
			{
				for ( size_t indx = ExtLen - 1; indx >= ExtLen - NumDigits; --indx )
				{
					/* If the extension character is a digit */
					if ( isdigit( sNameExt[indx] ) )
					{
						/* Remove it */
						sNameExt.erase( indx, 1 );
					}
					else
					{
						/* Stop at the first non-numerical character */
						break;
					}
				}
			}

			// Rebuild the name with the numerics removed from either the extension or filename.
			if ( toPath.has_extension() )
			{
				toPath.replace_extension( sNameExt );
			}
			else
			{
				toPath.replace_filename( sNameExt );
			}
		}

		SetDlgItemText( IDC_TO, toPath.c_str() );
	}
	{
		HWND hUp, hDown;

		hUp = GetDlgItem( IDC_UP )->m_hWnd;
		hDown = GetDlgItem( IDC_DOWN )->m_hWnd;

		/* Assign the bitmaps for the up/down buttons */
		auto hInstance = AfxGetInstanceHandle();

		HICON hU = (HICON) LoadImage( hInstance, MAKEINTRESOURCE( IDI_UP ), IMAGE_ICON, 0, 0, LR_DEFAULTCOLOR );
		HICON hD = (HICON) LoadImage( hInstance, MAKEINTRESOURCE( IDI_DOWN ), IMAGE_ICON, 0, 0, LR_DEFAULTCOLOR );

		::SendMessage( hUp, BM_SETIMAGE, IMAGE_ICON, (LPARAM) /*(DWORD)*/ hU );
		::SendMessage( hDown, BM_SETIMAGE, IMAGE_ICON, (LPARAM) /*(DWORD)*/ hD );

		/* Initially the up & down buttons are disabled */
		::EnableWindow( hUp, FALSE );
		::EnableWindow( hDown, FALSE );
	}

	//if ( !bREGISTERED )
	//{
	//	/* Disable the explanatory text so shareware users know this isn't meant to be there */
	//	EnableWindow( GetDlgItem( hDlg, IDC_EXPLANATION ), FALSE );
	//}

	{
// Not needed		CoInitialize( NULL );

		/* Make the file entry edit field autocomplete */
		SHAutoComplete( GetDlgItem( IDC_TO )->m_hWnd, SHACF_FILESYSTEM );
	}

	this->CenterWindow();

	/* Silent check for updates - once per instantiation */
	{
		if ( !bUpdateChecked )
		{
			bUpdateChecked = true;

			RegCheckData rcd( m_RegData, AfxGetInstanceHandle(), ProductCode::Concat );
			CheckForUpdate( this->m_hWnd, true, rcd );
		}
	}

	return TRUE;  // return TRUE unless you set the focus to a control
	// EXCEPTION: OCX Property Pages should return FALSE
}

static const int CtrlsToEnableWhileOperating[]{ IDC_CONCATING, IDC_CURRFILE, IDC_FNUM };

LRESULT CConcatDlg::OnWorkerFinished( WPARAM, LPARAM )
{
	CommonDlg::OnWorkerFinished( CtrlsToEnableWhileOperating, szConcatAppName );
	return 0;
}

LRESULT CConcatDlg::OnUpdateProgress( WPARAM wParam, LPARAM )
{
	/* wParam is the index into the files array */
	const int indx = (int) wParam;

	CommonDlg::OnUpdateProgress( indx, m_Files.at( indx ).c_str() );
	return 0;
}

#if 0
void CConcatDlg::OnMeasureItem( int nIDCtl, LPMEASUREITEMSTRUCT lpMeasureItemStruct )
{
	if ( wParam == IDC_COPY_LIST )
	{
		LPMEASUREITEMSTRUCT lpmi = (LPMEASUREITEMSTRUCT) lParam;

		{
			HWND hListWnd = GetDlgItem( hDlg, IDC_COPY_LIST );
			HFONT hFont = GetWindowFont( hListWnd );
			HDC hDC = GetDC( hListWnd );
			HFONT hOldFont = SelectObject( hDC, hFont );

			TEXTMETRIC tm;

			GetTextMetrics( hDC, &tm );

			lpmi->itemHeight = tm.tmHeight + tm.tmExternalLeading;   // height of single item in list box menu, in pixels 

			SelectObject( hDC, hOldFont );
			ReleaseDC( hListWnd, hDC );
		}

		return TRUE;
	}
	else
	{
		return FALSE;
	}

	CommonDlgData::OnMeasureItem( nIDCtl, lpMeasureItemStruct );
}
#endif // 0

static int GetTextOverrun( HDC hDC, HFONT hFont, RECT* rect, LPCTSTR pName ) noexcept
{
	/* Create a memory DC with the same attributes as the control */
	HDC hMemDC{ CreateCompatibleDC( hDC ) };
	HFONT hOldFont{ (HFONT) SelectObject( hMemDC, hFont ) };

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


void CConcatDlg::OnDrawItem( int nIDCtl, LPDRAWITEMSTRUCT lpdis )
{
	if ( nIDCtl == IDC_COPY_LIST )
	{
		/* If there are no list box items, skip this message. */
		if ( lpdis->itemID != -1 )
		{
			/* Select inverse colours */
			COLORREF OldTextCol, OldBkCol;
			OldTextCol = OldBkCol = 0;	// To shut up the cowpiler
			bool bColourChanged = false;

			/* What colours do the new controls use when they don't have the focus, but keep selection? */
			if ( lpdis->itemState & ODS_SELECTED )
			{
#if 0
				/* Differentiate between selected with & without the focus */
				if ( lpdis->itemState & ODS_FOCUS )
#endif
				{
					OldTextCol = SetTextColor( lpdis->hDC, GetSysColor( COLOR_HIGHLIGHTTEXT ) );
					OldBkCol = SetBkColor( lpdis->hDC, GetSysColor( COLOR_HIGHLIGHT ) );
				}
#if 0
				else
				{
					/* Selected, but doesn't have the focus */
					OldTextCol = SetTextColor( lpdis->hDC, GetSysColor( COLOR_HIGHLIGHTTEXT ) );
					OldBkCol = SetBkColor( lpdis->hDC, GetSysColor( COLOR_BTNSHADOW ) );
				}
#endif
				bColourChanged = true;
			}

			TCHAR szBuffer[_MAX_PATH];

			/* Display the text associated with the item. */
			ListBox_GetText( lpdis->hwndItem, lpdis->itemID, szBuffer );

			RECT txtrc;

			txtrc = lpdis->rcItem;

			//				const int x = LOWORD(GetDialogBaseUnits());
			//				InflateRect( &txtrc, -x, 0 );

			const int Excess = GetTextOverrun( lpdis->hDC, GetWindowFont( GetDlgItem( IDC_COPY_LIST )->m_hWnd ), &txtrc, szBuffer );

			TEXTMETRIC tm;
			GetTextMetrics( lpdis->hDC, &tm );

			const int y = (lpdis->rcItem.bottom + lpdis->rcItem.top - tm.tmHeight) / 2;

			ExtTextOut( lpdis->hDC, lpdis->rcItem.left - Excess, y, ETO_CLIPPED | ETO_OPAQUE, &lpdis->rcItem, szBuffer, (UINT) wcslen( szBuffer ), NULL );

			/* Deselect inverse colours */
			if ( bColourChanged )
			{
				SetTextColor( lpdis->hDC, OldTextCol );
				SetBkColor( lpdis->hDC, OldBkCol );
			}

			if ( lpdis->itemState & ODS_FOCUS )
			{
				DrawFocusRect( lpdis->hDC, &lpdis->rcItem );
			}
		}

//		return TRUE;
	}
	else
	{
		CommonDlg::OnDrawItem( nIDCtl, lpdis );
//		return FALSE;
	}
}


void CConcatDlg::OnSelchangeCopyList()
{
	/* Has the user changed something in the list box? */
	{
		HWND hUp, hDown;

		hUp = GetDlgItem( IDC_UP )->m_hWnd;
		hDown = GetDlgItem( IDC_DOWN )->m_hWnd;

		//				if ( bREGISTERED )
		{
			CListBox* plb = static_cast<CListBox*>(GetDlgItem( IDC_COPY_LIST ));
			const int indx = plb->GetCurSel();

			/* Enable/Disable the up/down buttons accordingly */
			const BOOL bUpEnabled = indx != 0;
			const BOOL bDownEnabled = indx != (int) (m_Files.size() - 1);

			{
				/* If we're disabling a control that currently has the focus, we must move the focus */
				if ( !bUpEnabled && (::GetFocus() == hUp) )
				{
					/* Next control */
					NextDlgCtrl();
				}

				::EnableWindow( hUp, bUpEnabled );

				if ( !bDownEnabled && (::GetFocus() == hDown) )
				{
					/* Previous control */
					PrevDlgCtrl();
				}

				::EnableWindow( hDown, bDownEnabled );
			}
		}
		//else
		//{
		//	EnableWindow( hUp, false );
		//	EnableWindow( hDown, false );
		//}
	}
}

void CConcatDlg::OnUpDown( UINT nID )
{
	HWND hList = GetDlgItem( IDC_COPY_LIST )->m_hWnd;

	//			if ( bREGISTERED )
					/* Move the currently selected item in the list box 1 position up or down the list */
	{
		const int CurItem = ListBox_GetCurSel( hList );

		const int Offset = nID == IDC_UP ? 1 : -1;

		{
			const size_t pos = CurItem - Offset;

			// Shorthand references to file names [pos] & [CurItem]
			auto& FileNameAtPos = m_Files.at( pos );
			auto& FileNameAtCurItem = m_Files.at( CurItem );

			std::swap( FileNameAtPos, FileNameAtCurItem );

			/* Re-display the item in the list box */
			ListBox_InsertString( hList, pos, FileNameAtPos.c_str() );
			ListBox_DeleteString( hList, pos + 1 );

			ListBox_InsertString( hList, CurItem, FileNameAtCurItem.c_str() );
			ListBox_DeleteString( hList, CurItem + 1 );

			/* Reset the current item */
			ListBox_SetCurSel( hList, pos );
		}
	}

	/* Simulate the notification to do the logic of the up/down buttons */
	PostMessage( WM_COMMAND, MAKEWPARAM( IDC_COPY_LIST, LBN_SELCHANGE ), (LPARAM) hList );
}


void CConcatDlg::OnOK()
{
	/* Was the shift key down (to start timing)? */
	const bool bShiftKeyDown = GetKeyState( VK_SHIFT ) & 0x8000 ? true : false;

	/* Get the filename from the edit field */
	CString sToName;
	GetDlgItemText( IDC_TO, sToName );

	/* Check that the string is a valid file name */
	// *** TO BE DONE???
	{
		BOOL bOk = FALSE;

		/* If the file already exists, ask the user if they want to delete it */
		if ( FileExistsAndWritable( sToName ) )
		{
			/* File already exists message */
			CString sFmt( MAKEINTRESOURCE( IDS_ALREADY_EXISTSPROMPT ) );

			CString sMsg;
			sMsg.Format( sFmt, static_cast<LPCTSTR>( sToName ) );

			if ( IDYES == MessageBox( sMsg, szConcatAppName, MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2 ) )
			{
				bOk = TRUE;
			}
		}
		else
		{
			/* Get the current error code, because checking if the file exists will change it */
			const DWORD dwError = GetLastError();

			if ( FileExists( sToName ) )
			{
				LPVOID lpMsgBuf;

				FormatMessage(
					FORMAT_MESSAGE_ALLOCATE_BUFFER |
					FORMAT_MESSAGE_FROM_SYSTEM |
					FORMAT_MESSAGE_IGNORE_INSERTS,
					NULL,
					dwError,
					MAKELANGID( LANG_NEUTRAL, SUBLANG_DEFAULT ),
					(LPTSTR) &lpMsgBuf,
					0,
					NULL );

				CString sFmt( MAKEINTRESOURCE( IDS_CANT_ACCESS ) );
				CString sMsg;
				sMsg.Format( sFmt/*_T("The file you have specified '%s' can't be used.\n\n%s")*/, static_cast<LPCTSTR>(sToName), static_cast<LPCTSTR>(lpMsgBuf) );

				MessageBox( sMsg, szConcatAppName, MB_OK | MB_ICONERROR );

				// Free the buffer.
				LocalFree( lpMsgBuf );
			}
			else
			{
				/* The file does not exist, it's ok to continue */
				bOk = TRUE;
			}
		}

		if ( bOk )
		{
			for ( auto id : CtrlsToEnableWhileOperating )
			{
				GetDlgItem( id )->EnableWindow( true );
			}
			//					EnableWindow( GetDlgItem( hDlg, IDC_PROGRESS ), true );

			if ( bShiftKeyDown )
			{
				/* Save the time at the start of the operation */
				m_tim.SetStartTimeNow();
			}
			else
			{
				m_tim.SetNotInUse();
			}

			ConcatEm( GetDlgItem( IDC_PROGRESS )->m_hWnd, sToName );
		}
	}

//	CommonDlgData::OnOK();
}

void CConcatDlg::DisplayOperationCompleteMessage()
{
	CString sMsg( MAKEINTRESOURCE( IDS_CONCAT_OK ) );
	MessageBox( sMsg, szConcatAppName, MB_OK );
}
