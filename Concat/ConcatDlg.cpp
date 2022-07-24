#include <afxmt.h>
#include <windowsx.h>
#include <CommCtrl.h>
#include <process.h>
#include <atlstr.h>
#include <algorithm>
#include <filesystem>
#include <string>

#include "CommonDlgData.h"
#include "CSelPlusReg.h"
#include "ConcatThreadData.h"
#include "Globals.h"
#include "chandle.h"
#include "resource.h"
#include "CheckForUpdate.h"

using std::wstring;
namespace fs = std::filesystem;

extern unsigned __stdcall ConcatControlThread_Reader( void* pParams );

class ConcatDlgData : public CommonDlgData
{
public:
	// Prevent inadvertent copying
	ConcatDlgData() = delete;
	ConcatDlgData( const ConcatDlgData& ) = delete;
	ConcatDlgData operator=( const ConcatDlgData& ) = delete;

	ConcatDlgData( const CSelPlusReg& spr ) : CommonDlgData( spr.m_RegData )
	{
		// Take a copy of the file names. A full copy is taken as the collection can be sorted/re-arranged.
		Files = spr.m_Files;
	}

	SELITEMS Files;		// The collection of file names that are being joined
};

static void ConcatEm( HWND hWnd, ConcatDlgData& dd, HWND hProgress, LPCTSTR szToName )
{
	{
		auto pctd = new ConcatThreadData( dd.Files, szToName, hProgress, hWnd );

		/* Disable all the UI components while the thread runs */
		dd.UIDisable( hWnd );

		/* Clear the cancel flag */
		InterlockedExchange( &g_bCancel, FALSE );

		UINT tid;
		::CHandle hThread( reinterpret_cast<HANDLE>(_beginthreadex( NULL, 0, ConcatControlThread_Reader, pctd, 0, &tid )) );
	}
	//else
	//{
	//	ResMessageBox( hWnd, IDS_FAIL_CREATE_TEMP /*
	//			_T("Failed to create temporary file.\n\n")
	//			_T("The directory path may be invalid, ")
	//			_T("you may not have access to that directory, or the disk may be full.")*/,
	//		szAppName, MB_OK | MB_ICONERROR );
	//}
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

INT_PTR CALLBACK ConcatDlg( HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam )
{
	/* Get this dialog's data class pointer - note this is not valid until first initialised in WM_INITDIALOG processing */
	ConcatDlgData* pcdd = reinterpret_cast<ConcatDlgData*>(::GetWindowLongPtr( hDlg, DWLP_USER ));

	switch ( message )
	{
		/* inter-thread MessageBox */
	case UWM_TMB:
		TMBHandler( hDlg, lParam );
		break;

	case UWM_WORKER_FINISHED:
		/* Calculate the time it's taken to do the split */
	{
		UINT64 PeriodInMS = pcdd->m_tim.GetTimeToNow();
		PeriodInMS = pcdd->m_tim.AsMilliSeconds( PeriodInMS );

		/* Do the common operations to re-enable the UI aspects that were disabled during this operation */
		{
			pcdd->UIEnable( hDlg );

			/* Disable this to match the original state */
			EnableWindow( GetDlgItem( hDlg, IDC_CONCATING ), false );
			EnableWindow( GetDlgItem( hDlg, IDC_CURRFILE ), false );
			EnableWindow( GetDlgItem( hDlg, IDC_FNUM ), false );
			PostMessage( GetDlgItem( hDlg, IDC_PROGRESS ), PBM_SETPOS, 0, 0 );
			//				EnableWindow( GetDlgItem( hDlg, IDC_PROGRESS ), false );
		}

		if ( pcdd->m_tim.InUse() )
		{
			CString sMsg;
			sMsg.Format( _T( "The operation took: %I64u.%I64u seconds" ), PeriodInMS / 1000, PeriodInMS % 1000 );
			MessageBox( hDlg, sMsg, szAppName, MB_OK );
		}
		else
		{
			/* If we've not canceled the operation */
			if ( !g_bCancel )
			{
				/* Close the dialog - we're all done */
				PostMessage( hDlg, WM_COMMAND, IDCANCEL, 0 );
			}
		}
	}
	break;

	/* Progress Message */
	case UWM_UPDATE_PROGRESS:
	{
		/* wParam is the index into the files array */
		const int indx = (int) wParam;

		SetDlgItemInt( hDlg, IDC_FNUM, indx + 1, false );

		{
			TCHAR szPath[_MAX_PATH];

			lstrcpyn( szPath, pcdd->Files.at( indx ).c_str(), _countof( szPath ) );
			LPTSTR pPath = PathFindFileName( szPath );

			{
				HWND hCtrl = GetDlgItem( hDlg, IDC_CURRFILE );
				HDC hDC = GetDC( hCtrl );
				RECT r;
				GetWindowRect( hCtrl, &r );
				HFONT hFont = GetWindowFont( hCtrl );
				int Excess = ModifyPathForControl( hDC, hFont, &r, pPath );
				/* It's best to see the RHS of the text */
				if ( Excess > 0 )
				{
					/* Do essentially what ModifyPathForControl does */

					const int CtrlWidth = r.right - r.left;

					/* Create a memory DC with the same attributes as the control */
					HDC hMemDC;
					hMemDC = CreateCompatibleDC( hDC );
					HFONT hOldFont = (HFONT) SelectObject( hMemDC, hFont );

					/* Remove characters from the start of the string until it'll fit the control width */
					++pPath;
					for ( int Len = lstrlen( pPath ); pPath < &szPath[_MAX_PATH]; pPath++, Len-- )
					{
						SIZE size;
						GetTextExtentPoint32( hMemDC, pPath, Len, &size );
						Excess = size.cx - CtrlWidth;
						if ( Excess <= 0 )
						{
							break;
						}
					}

					SelectObject( hMemDC, hOldFont );

					DeleteDC( hMemDC );
				}

				ReleaseDC( hCtrl, hDC );
			}

			SetDlgItemText( hDlg, IDC_CURRFILE, pPath );
		}
	}
	break;

	case WM_INITDIALOG:
	{
		// lParam is a pointer to the minimal data passed from the caller - the registration data and selected file names
		// Copy those parts to this dialog's class data
		pcdd = new ConcatDlgData( *((const CSelPlusReg*) lParam) );

		// Store the pointer to the data so that it's retrievable
		::SetWindowLongPtr( hDlg, DWLP_USER, reinterpret_cast<LONG_PTR>(pcdd) );

		bREGISTERED = IsRegisteredAndValid( pcdd->m_RegData, ProductCode::Concat );

		// sort them before adding to the list box to ensure they're in some order - hopefully the optimum one given the names the split operation will have used.
		if ( bREGISTERED )
		{
			/* sort the filenames before adding them to the listbox */
			std::ranges::sort( pcdd->Files );
		}

		const HWND hList = GetDlgItem( hDlg, IDC_COPY_LIST );

		/* Displaying the path items using ellipsis notation */
		/* Add all the files to the list box */
		for ( const auto& itName : pcdd->Files )
		{
			ListBox_AddString( hList, itName.c_str() );
		}

		/* Copy the first name to be the destination name */
		fs::path toPath{ pcdd->Files.at( 0 ) };

		if ( bREGISTERED )
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
			const size_t NumDigits = NumberOfCharactersToDisplayValue( pcdd->Files.size() );

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

		SetDlgItemText( hDlg, IDC_TO, toPath.c_str() );
	}
	{
		HWND hUp, hDown;

		hUp = GetDlgItem( hDlg, IDC_UP );
		hDown = GetDlgItem( hDlg, IDC_DOWN );

		/* Assign the bitmaps for the up/down buttons */
		HICON hU = (HICON) LoadImage( g_hInstance, MAKEINTRESOURCE( IDI_UP ), IMAGE_ICON, 0, 0, LR_DEFAULTCOLOR );
		HICON hD = (HICON) LoadImage( g_hInstance, MAKEINTRESOURCE( IDI_DOWN ), IMAGE_ICON, 0, 0, LR_DEFAULTCOLOR );

		SendMessage( hUp, BM_SETIMAGE, IMAGE_ICON, (LPARAM) /*(DWORD)*/ hU );
		SendMessage( hDown, BM_SETIMAGE, IMAGE_ICON, (LPARAM) /*(DWORD)*/ hD );

		/* Initially the up & down buttons are disabled */
		EnableWindow( hUp, FALSE );
		EnableWindow( hDown, FALSE );
	}

	if ( !bREGISTERED )
	{
		/* Disable the explanatory text so shareware users know this isn't meant to be there */
		EnableWindow( GetDlgItem( hDlg, IDC_EXPLANATION ), FALSE );
	}

	{
		CoInitialize( NULL );

		/* Make the file entry edit field autocomplete */
		SHAutoComplete( ::GetDlgItem( hDlg, IDC_TO ), SHACF_FILESYSTEM );
	}

	CenterDialog( hDlg );

	/* Silent check for updates - once per instantiation */
	{
		if ( !bUpdateChecked )
		{
			bUpdateChecked = true;

			RegCheckData rcd( *(pcdd->m_RegData), g_hInstance, ProductCode::Concat );
			CheckForUpdate( hDlg, true, rcd );
		}
	}
	break;

	case WM_DESTROY:
		CoUninitialize();
		delete pcdd;
		pcdd = nullptr;
		::SetWindowLongPtr( hDlg, DWLP_USER, reinterpret_cast<LONG_PTR>(pcdd) );
		break;

#if 0
	case WM_MEASUREITEM:
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
#endif
	case WM_DRAWITEM:
		if ( wParam == IDC_COPY_LIST )
		{
			LPDRAWITEMSTRUCT lpdis = (LPDRAWITEMSTRUCT) lParam;
			TCHAR szBuffer[_MAX_PATH];

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

				/* Display the text associated with the item. */
				ListBox_GetText( lpdis->hwndItem, lpdis->itemID, szBuffer );

				RECT txtrc;

				txtrc = lpdis->rcItem;

				//				const int x = LOWORD(GetDialogBaseUnits());
				//				InflateRect( &txtrc, -x, 0 );

				const int Excess = ModifyPathForControl( lpdis->hDC, GetWindowFont( GetDlgItem( hDlg, IDC_COPY_LIST ) ),
					&txtrc, szBuffer );

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

			return TRUE;
		}
		else
		{
			return FALSE;
		}

	case WM_SYSCOMMAND:
		if ( (wParam & 0x0FFF0) == SC_CLOSE )
		{
			/* If the default OK button is disabled, we must be doing the Split operation */
			if ( !IsWindowEnabled( GetDlgItem( hDlg, IDOK ) ) )
			{
				return TRUE;
			}
		}
		return FALSE;

	case WM_COMMAND:
		switch ( LOWORD( wParam ) )
		{
		case IDC_COPY_LIST:
		{
			/* Has the user changed something in the list box? */
			if ( HIWORD( wParam ) == LBN_SELCHANGE )
			{
				HWND hUp, hDown;

				hUp = GetDlgItem( hDlg, IDC_UP );
				hDown = GetDlgItem( hDlg, IDC_DOWN );

				if ( bREGISTERED )
				{
					const int indx = ListBox_GetCurSel( (HWND) lParam );

					/* Enable/Disable the up/down buttons accordingly */
					const BOOL bUpEnabled = indx != 0;
					const BOOL bDownEnabled = indx != (int) (pcdd->Files.size() - 1);

					{
						/* If we're disabling a control that currently has the focus, we must move the focus */
						if ( !bUpEnabled && (GetFocus() == hUp) )
						{
							/* Next control */
							FORWARD_WM_NEXTDLGCTL( hDlg, 0, FALSE, SendMessage );
						}

						EnableWindow( hUp, bUpEnabled );

						if ( !bDownEnabled && (GetFocus() == hDown) )
						{
							/* Previous control */
							FORWARD_WM_NEXTDLGCTL( hDlg, 1, FALSE, SendMessage );
						}

						EnableWindow( hDown, bDownEnabled );
					}
				}
				else
				{
					EnableWindow( hUp, false );
					EnableWindow( hDown, false );
				}
			}
		}
		break;

		case IDC_UP:
		case IDC_DOWN:
		{
			HWND hList = GetDlgItem( hDlg, IDC_COPY_LIST );

			if ( bREGISTERED )
				/* Move the currently selected item in the list box 1 position up or down the list */
			{
				const int CurItem = ListBox_GetCurSel( hList );

				const int Offset = LOWORD( wParam ) == IDC_UP ? 1 : -1;

				{
					const size_t pos = CurItem - Offset;

					// Shorthand references to file names [pos] & [CurItem]
					auto& FileNameAtPos = pcdd->Files.at( pos );
					auto& FileNameAtCurItem = pcdd->Files.at( CurItem );

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
			/* Simulate the notification to do the logic of the up/down buttons (plus this'll confuse the user with a hacked version) */
			PostMessage( hDlg, WM_COMMAND, MAKEWPARAM( IDC_COPY_LIST, LBN_SELCHANGE ), (LPARAM) hList );
		}
		break;

		case IDOK:
		{
			/* Was the shift key down (to start timing)? */
			const bool bShiftKeyDown = GetKeyState( VK_SHIFT ) & 0x8000 ? true : false;

			/* Get the filename from the edit field */
			TCHAR szToName[_MAX_PATH];
			GetDlgItemText( hDlg, IDC_TO, szToName, _countof( szToName ) );

			/* Check that the string is a valid file name */
			// *** TO BE DONE???
			{
				BOOL bOk = FALSE;

				/* If the file already exists, ask the user if they want to delete it */
				if ( FileExistsAndWritable( szToName ) )
				{
					/* File already exists message */
					TCHAR szFmtMsg[256];

					::LoadString( g_hResInst, IDS_ALREADY_EXISTSPROMPT, szFmtMsg, _countof( szFmtMsg ) );

					TCHAR szMsg[_MAX_PATH + _countof( szFmtMsg )];
					wsprintf( szMsg, szFmtMsg, szToName );

					if ( IDYES == MessageBox( hDlg, szMsg, szAppName, MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2 ) )
					{
						bOk = TRUE;
					}
				}
				else
				{
					/* Get the current error code, because checking if the file exists will change it */
					const DWORD dwError = GetLastError();

					if ( FileExists( szToName ) )
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

						TCHAR szMsg[1024];
						TCHAR szFmt[100];
						LoadString( g_hResInst, IDS_CANT_ACCESS, szFmt, _countof( szFmt ) );
						wsprintf( szMsg, szFmt/*_T("The file you have specified can't be used.\n\n%s")*/, (LPCTSTR) lpMsgBuf );

						MessageBox( hDlg, szMsg, szAppName, MB_OK | MB_ICONERROR );

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
					EnableWindow( GetDlgItem( hDlg, IDC_CONCATING ), true );
					EnableWindow( GetDlgItem( hDlg, IDC_CURRFILE ), true );
					EnableWindow( GetDlgItem( hDlg, IDC_FNUM ), true );
					//					EnableWindow( GetDlgItem( hDlg, IDC_PROGRESS ), true );

					if ( bShiftKeyDown )
					{
						/* Save the time at the start of the operation */
						pcdd->m_tim.SetStartTimeNow();
					}
					else
					{
						pcdd->m_tim.SetNotInUse();
					}

					ConcatEm( hDlg, *pcdd, GetDlgItem( hDlg, IDC_PROGRESS ), szToName );
				}
			}
		}
		break;

		case IDCANCEL:
			/* If we're in the middle of the concat operation, this is used to cancel the operation */
			/* If the default OK button is disabled, we must be doing the Split operation */
			if ( !IsWindowEnabled( GetDlgItem( hDlg, IDOK ) ) )
			{
				InterlockedExchange( &g_bCancel, TRUE );
			}
			else
			{
				EndDialog( hDlg, IDCANCEL );
			}
			break;

		case IDC_ABOUT:
			AboutHandler( hDlg, pcdd->m_RegData );
			break;

		default:
			/* We've not processed the message */
			return(FALSE);
		}
		break;

	default:
		/* We've not processed the message */
		return(FALSE);
	}

	/* We've processed the message */
	return(TRUE);
}

