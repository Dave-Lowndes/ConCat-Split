#define NOMINMAX	// Prevent issue with gsl
#include <windows.h>
#include <windowsx.h>
#include <tchar.h>
#include "getvstr.h"
#include "..\..\RegGen\RegDecV3.h"
#include "tpregcommon.h"
#include "resource.h"

#define EMAIL _T("jdd@jddesign.co.uk")
#define URL _T("http://www.jddesign.co.uk/")

extern HINSTANCE g_hInstance;

#include "VerUtil.h"
#include <algorithm>
#include <locale>
#include "time_t_funcs.h"
using namespace std;

static constexpr char ToHexChar( BYTE nibbleValue ) noexcept
{
	return (nibbleValue >= 10) ?
		// A...F
		'A' + (nibbleValue - 10) :
		// 0...9
		'0' + nibbleValue;
}

void CheckForUpdate( HWND hParentWnd, const optional<CMyRegData> & myrdata )
{
	_int64 CurVer;
	{
		VS_FIXEDFILEINFO ffi;
		GetVerInfo( g_hInstance, ffi );

		CurVer = ffi.dwFileVersionMS;
		CurVer <<= 32;
		CurVer += ffi.dwFileVersionLS;
	}

	TCHAR szUpdaterPath[_MAX_PATH];

	GetModuleFileName( g_hInstance, szUpdaterPath, _countof( szUpdaterPath ) );

	TCHAR szDrive[_MAX_DRIVE];
	TCHAR szDir[_MAX_DIR];
	TCHAR szFName[_MAX_FNAME];
	TCHAR szExt[_MAX_EXT];

	if ( 0 == _tsplitpath_s( szUpdaterPath, szDrive, _countof(szDrive), szDir, _countof(szDir), szFName, _countof(szFName), szExt, _countof(szExt) ) )
	{
		/* Form the path to the updater application */
		_tmakepath_s( szUpdaterPath, _countof(szUpdaterPath), szDrive, szDir, _T("JDDPadUpdater"), _T(".exe") );

		/* Form the name of this component */
		TCHAR szCompName[_MAX_PATH];
		_tmakepath_s( szCompName, _countof( szCompName ), nullptr, nullptr, szFName, szExt );

		TCHAR szParams[200];

		{
			/* Param 0 = component name (DLL/EXE)
			* Param 1 - 1 == 64 bit, 0 == 32-bit
			* Param 2 = 64 bit value for the current version number
			* Param 3 - 1 = silent mode, 0 = interactive (shows error messages)
			* Param 4 - Registration Name
			* Param 5 - Unique User ID
			*/
			if ( myrdata.has_value() )
			{
				const wstring sUserName{ myrdata.value().GetRegisteredUserName() };
				const int uid{ myrdata.value().GetUniqueId() };

				wsprintf( szParams,
#ifdef _WIN64
					_T( "%s 1 %I64d %d \"%s\" %d" ),	// Name is quoted to prevent spaces in names being misinterpreted
#else
					_T( "%s 0 %I64d %d \"%s\" %d" ),
#endif
					szCompName,
					CurVer,
					hParentWnd == NULL ? 1 : 0  /* 1 or 0 - pass parent handle as NULL for silent mode */
					, sUserName.c_str(),
					uid );
			}
			else
			{
				// No registration details, so don't pass them
				wsprintf( szParams,
#ifdef _WIN64
					_T( "%s 1 %I64d %d" ),
#else
					_T( "%s 0 %I64d %d" ),
#endif
					szCompName,
					CurVer,
					hParentWnd == NULL ? 1 : 0  /* 1 or 0 - pass parent handle as NULL for silent mode */
				);
			}

		}

		SHELLEXECUTEINFO sei = { 0 };
		sei.cbSize = sizeof( sei );
		sei.hwnd = hParentWnd;
		sei.lpParameters = szParams;
		sei.lpFile = szUpdaterPath;
		sei.nShow = SW_NORMAL;
		sei.fMask = SEE_MASK_UNICODE;

		if ( hParentWnd != NULL )
		{
			sei.hMonitor = MonitorFromWindow( hParentWnd, MONITOR_DEFAULTTONEAREST );
			sei.fMask |= SEE_MASK_HMONITOR;
		}

		ShellExecuteEx( &sei );
	}
}

extern wstring GetWindowText( HWND hWnd );

INT_PTR CALLBACK AboutDlg( HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam )
{
//	static bool bLinkVisited;
	static HFONT hHyperlinkFont;
	static const optional<CMyRegData> * myrdata;

	switch ( uMsg )
	{
	case WM_DESTROY:
		DeleteObject( hHyperlinkFont );
		hHyperlinkFont = NULL;
		break;

	case WM_INITDIALOG:
//		bLinkVisited = false;
	{
		// Reg data passed as lParam
		myrdata = reinterpret_cast<optional<CMyRegData>*>(lParam);

		hHyperlinkFont = NULL;

		/* Initialise the fields of the dialog with the version strings */
		TCHAR szVs[80];

		if ( GetVersionString( GVS_FileDescription, szVs, _countof( szVs ), g_hInstance ) )
		{
			SetDlgItemText( hDlg, IDC_FILE_DESC, szVs );
		}

		{
			VS_FIXEDFILEINFO ffi;
	
			GetVerInfo( g_hInstance, ffi );
	
			wsprintf( szVs, _T("%u.%u.%u.%u"),
						HIWORD( ffi.dwFileVersionMS ), LOWORD( ffi.dwFileVersionMS ),
						HIWORD( ffi.dwFileVersionLS ), LOWORD( ffi.dwFileVersionLS ) );
		}

		SetDlgItemText( hDlg, IDC_VERSION, szVs );

		if ( GetVersionString( GVS_LegalCopyright, szVs, _countof( szVs ), g_hInstance ) )
		{
			SetDlgItemText( hDlg, IDC_COPYRIGHT, szVs );
		}

		/* Registration fields */
		if ( IsRegistered( *myrdata, ProductCode::TouchPro ) )
		{
			/* Fill in the registered user name, number of users, and expiry date */
			const auto RegData = myrdata->value();
			const wstring UserName{ RegData.GetRegisteredUserName() };
			SetDlgItemText( hDlg, IDC_REG_USER, UserName.c_str() );
			SetDlgItemInt( hDlg, IDC_NUMBER_OF_USERS, RegData.GetNumberOfUsersLicenced(), TRUE );
			const time_t expDate = RegData.GetExpiryDate();

			if ( expDate == 0 )
			{
				// Perpetual licence
				SetDlgItemText( hDlg, IDC_EXPIRES_DATE, L"Perpetual" );
			}
			else
			{
				// Format in user's locale date
				wstring sDate = UnixTimeToWindowsFormattedDate( expDate, LOCALE_USER_DEFAULT, DATE_AUTOLAYOUT | DATE_LONGDATE );

				SetDlgItemText( hDlg, IDC_EXPIRES_DATE, sDate.c_str() );
			}

			// If the registration is valid, hide the entry fields
			if ( !myrdata->value().IsExpired() )
			{
				/* Disable the registration edit fields */
				EnableWindow( GetDlgItem( hDlg, IDC_REG_NUM ), FALSE );
				EnableWindow( GetDlgItem( hDlg, IDC_REG_MSG ), FALSE );

				/* Shrink the dialog to not show the registration stuff */
				RECT rect;

				GetWindowRect( hDlg, &rect );

				/* Preserve the original width of the dialog */
				const int width = rect.right - rect.left;

				/* Get the screen co-ordinates of the register message */
				GetWindowRect( GetDlgItem( hDlg, IDC_REG_MSG ), &rect );

				/* Convert to relative to the dialog */
				ScreenToClient( hDlg, reinterpret_cast<LPPOINT>(&rect.left) );
				ScreenToClient( hDlg, reinterpret_cast<LPPOINT>(&rect.right) );

				/* Setup to re-size the height of the dialog */
				rect.bottom = rect.top;
				rect.top = 0;

				/* Add in the height of the menu bar & frame */
				AdjustWindowRect( &rect, GetWindowLong( hDlg, GWL_STYLE ), false );

				/* Resize the dialog */
				SetWindowPos( hDlg, NULL, 0, 0, width, rect.bottom - rect.top, SWP_NOZORDER | SWP_NOMOVE | SWP_NOACTIVATE );
			}
		}
		else
		{
			/* Hide the "Registered To" text */
			ShowWindow( GetDlgItem( hDlg, IDC_REG_TO_TXT ), SW_HIDE );
		}
		return( TRUE );
	}

//	case WM_HELP:
//		WinHelp( hDlg, szHelpFile, HELP_CONTEXT, HIDC_ABOUT );
//		break;

	case WM_COMMAND:
		switch ( LOWORD( wParam ) )
		{
		case IDOK:
			/* If we're not already validly registered, see if there's anything been entered */
			if ( !IsRegisteredAndValid( *myrdata, ProductCode::TouchPro ) )
			{
				const wstring strRegNum = GetWindowText( GetDlgItem( hDlg, IDC_REG_NUM ) );

				/* No point saving if there's nothing there! */
				if ( !strRegNum.empty() )
				{
					/* Store the entry to the registry ready for the next time */
					SaveMyRegistrationToTheRegistry( strRegNum );
				}
				else
				{
					/* Return the same as though we'd pressed cancel */
					EndDialog( hDlg, IDCANCEL );
					return( TRUE );
				}
			}
			/* Drop through! */
			[[fallthrough]];

		case IDCANCEL:
			EndDialog( hDlg, LOWORD( wParam ) );
			return( TRUE );

		//case IDC_WEBPAGE:
		//	ShellExecute( hDlg, NULL, URL, NULL, NULL, SW_SHOWNORMAL );	
		//	return( TRUE );

		case IDC_EMAILUS:
			if ( STN_CLICKED == HIWORD( wParam ) )
			{
				TCHAR szMsg[100];
				TCHAR szIN[30];

				GetVersionString( GVS_InternalName, szIN, _countof( szIN ), g_hInstance );

				VS_FIXEDFILEINFO ffi;

				GetVerInfo( g_hInstance, ffi );

				wsprintf( szMsg,
							_T("mailto:") EMAIL
							_T( "?subject=%s V%u.%u.%u.%u" ),
							szIN,
							HIWORD( ffi.dwFileVersionMS ),
							LOWORD( ffi.dwFileVersionMS ),
							HIWORD( ffi.dwFileVersionLS ),
							LOWORD( ffi.dwFileVersionLS ) );

				ShellExecute( hDlg, nullptr, szMsg, nullptr, nullptr, SW_SHOWNORMAL );

//				bLinkVisited = true;
				InvalidateRect( reinterpret_cast<HWND>( lParam ), nullptr, true );
				return TRUE;
			}
			break;

		case IDC_WEB_HOME:
			if ( STN_CLICKED == HIWORD( wParam ) )
			{
				ShellExecute( hDlg, nullptr, URL, nullptr, nullptr, SW_SHOWNORMAL );

//				bLinkVisited = true;
				InvalidateRect( reinterpret_cast<HWND>( lParam ), nullptr, true );
				return TRUE;
			}
			break;

		case IDC_CHECK_FOR_UPDATE:
			if ( STN_CLICKED == HIWORD( wParam ) )
			{
				/* Invoke the updater application */
				CheckForUpdate( hDlg, *myrdata );

//				bLinkVisited = true;
				InvalidateRect( reinterpret_cast<HWND>( lParam ), nullptr, true );
				return TRUE;
			}
			break;

		default:
			break;
		}
		break;

	case WM_CTLCOLORSTATIC:
		{
		HWND hCtl = reinterpret_cast<HWND>( lParam );
		const LONG_PTR dwStyle = GetWindowLongPtr( hCtl, GWL_STYLE );
		if ( ( dwStyle & SS_NOTIFY ) == SS_NOTIFY )
//		if ( hCtl == GetDlgItem( hDlg, IDC_EMAILUS ) )
		{
			HDC hDC = reinterpret_cast<HDC>( wParam );

			HBRUSH hbr = NULL;

			if ( (dwStyle & 0xFF) <= SS_RIGHT )
			{
				// this is a text control: set up font and colors
				if ( hHyperlinkFont == NULL )
				{
					// first time init: create font
					LOGFONT lf;
					GetObject( GetWindowFont( hCtl ), sizeof(lf), &lf );
					lf.lfUnderline = TRUE;
					hHyperlinkFont = CreateFontIndirect( &lf );
				}

				// use underline font and visited/unvisited colors
				SelectObject( hDC, hHyperlinkFont );
				SetTextColor( hDC, /*bLinkVisited ? RGB(128,0,128) :*/ RGB(0,0,255) );
				SetBkMode( hDC, TRANSPARENT );

				// return hollow brush to preserve parent background color
				hbr = static_cast<HBRUSH>( ::GetStockObject( HOLLOW_BRUSH ) );
			}
			return reinterpret_cast<INT_PTR>( hbr );
		}
		}
		break;

	default:
		break;
	}

	return( FALSE );
}