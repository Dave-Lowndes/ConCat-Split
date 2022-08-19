#include <afxwin.h>
#include <afxmt.h>
#include <afxtaskdialog.h>
#include <windowsx.h>
#include <Shlwapi.h>
#include <string>
#include <string_view>
#include <process.h>
#include <vector>
#include <filesystem>
#include <span>
#include <strsafe.h>
#include <commdlg.h>
#include <format>
#include <memory>

#include "CommonDlg.h"
#include "CSelPlusReg.h"
#include "chandle.h"
#include "Globals.h"
#include "resource.h"
#include "SplitThreadData.h"
#include "CheckForUpdate.h"
#include "SplitDlg.h"

using std::wstring;
using std::string_view;
using std::vector;
using std::span;
namespace fs = std::filesystem;
using std::unique_ptr;

/// <summary>
/// Appends a fixed width number to the original name
/// </summary>
/// <param name="pOrigName">The file name</param>
/// <param name="FileNum">The number value to append</param>
/// <param name="NumCharsInFileNum">The number of characters to use to format the value</param>
/// <returns>e.g. "Original String001" if the number value is 1, and the number of characters is 3</returns>
static wstring CreateNumericalName( LPCTSTR pOrigName, const UINT FileNum, const size_t NumCharsInFileNum ) noexcept
{
	return wstring{ pOrigName + std::format( L"{:0{}d}", FileNum, NumCharsInFileNum ) };
}

DWORD CSplitDlg::CreateAndSizeDestinationFiles( vector<HandlePlusSize>& vFiles, size_t NumNumericChars, LONGLONG SrcFileSize ) noexcept
{
	DWORD dwError = ERROR_SUCCESS;

	// Set the collection size - that we're going to fill in
	vFiles.resize( m_NumFiles );
	bool bOverwriteAll = false;

	for ( UINT indx = 0;
		(indx < vFiles.size()) && (dwError == ERROR_SUCCESS) /*&& !g_bCancel Not now this is done before the work threads!*/;
		++indx )
	{
		auto& CurFile = vFiles[indx];

		/* Create the next file name */
		CurFile.sFName = CreateNumericalName( m_sToFileName.c_str(), indx + 1, NumNumericChars);

		CurFile.m_fh.Attach( CreateFile( CurFile.sFName.c_str(), GENERIC_WRITE | DELETE, 0, NULL,
			CREATE_NEW,
			FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL ) );

		/* Does the file already exist? */
		/* If it does, ask if we can overwrite it */
		if ( !CurFile.m_fh.IsValid() )
		{
			const DWORD dwErr = GetLastError();

			if ( dwErr == ERROR_FILE_EXISTS )
			{
				bool bOverwrite;

				if ( !bOverwriteAll )
				{
					CString sFmt( MAKEINTRESOURCE( IDS_OVERWRITE_PROMPT ) );

					CString sMsg;
					sMsg.Format( sFmt/*_T("The file \"%s\" already exists.\n\nDo you want to overwrite it?")*/, CurFile.sFName.c_str() );

					CTaskDialog dlg( sMsg, _T( "" ), szSplitAppName, TDCBF_YES_BUTTON | TDCBF_NO_BUTTON );
					dlg.SetDefaultCommandControl( IDNO );
					dlg.SetVerificationCheckboxText( CString(MAKEINTRESOURCE(IDS_YES_TO_ALL)) );
					dlg.SetVerificationCheckbox( bOverwriteAll );
					dlg.SetMainIcon( TD_WARNING_ICON );
					INT_PTR cmd = dlg.DoModal();

					if ( cmd == IDYES )
					{
						// Did the user choose to overwrite all?
						if ( dlg.GetVerificationCheckboxState() )
						{
							bOverwriteAll = true;
						}

						bOverwrite = true;
					}
					else
					{
						/* User's said "No" - there's no point in carrying on now */
						dwError = (DWORD) -1;	// Special value so we don't display another message box.

						bOverwrite = false;
					}
				}
				else
				{
					bOverwrite = true;
				}

				if ( bOverwrite )
				{
					CurFile.m_fh.Attach( CreateFile( CurFile.sFName.c_str(), GENERIC_WRITE, 0, NULL,
						CREATE_ALWAYS,
						FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL ) );
				}
			}
		}

		if ( CurFile.m_fh.IsValid() )
		{
			/* We can determine the sizes of the split files easily. All but the last
			 * one are the split size, and the last one is the remainder.
			 */
			CurFile.m_SizeToCopy = indx < m_NumFiles - 1 ?
										m_SplitSize :
										SrcFileSize - (m_NumFiles - 1) * m_SplitSize;

			/* Pre-size the file - to ensure we don't run out of space, and it may be faster overall */
			if ( SetFilePointerEx( CurFile.m_fh, *(reinterpret_cast<LARGE_INTEGER*>(&(CurFile.m_SizeToCopy))), NULL, FILE_BEGIN ) )
			{
				if ( SetEndOfFile( CurFile.m_fh ) )
				{
					/* Back to the start */
					if ( SetFilePointer( CurFile.m_fh, 0, NULL, FILE_BEGIN ) != INVALID_SET_FILE_POINTER )
					{
						/* All should be OK */
					}
					else
					{
						dwError = GetLastError();
					}
				}
				else
				{
					dwError = GetLastError();
				}
			}
			else
			{
				dwError = GetLastError();
			}
		}
		else
		{
			if ( dwError != -1 )
			{
				dwError = GetLastError();
			}
		}
	}

	return dwError;
}

extern unsigned __stdcall SplitControlThread_Reader( unique_ptr<SplitThreadData> ustd );

void CSplitDlg::SplitEm( HWND hWnd, HWND hProgress, bool bCreateBatchFile, bool bStartTiming )
{
	bool bContinue = true;

		/* Create the DOS batch file */
#ifdef ANSIBATCHOUTPUT
	CFileHandle hBatchFile;
#else
	FILE* fBatch = nullptr;
#endif

	if ( bCreateBatchFile )
	{
#ifdef ANSIBATCHOUTPUT
		hBatchFile.Attach( CreateFile( m_sBatchName.c_str(), GENERIC_WRITE, 0, NULL, CREATE_NEW,
			FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL ) );
		if ( !hBatchFile.IsValid() )
#else
		// Create the file for utf-8 ouput (w = write, x = fail if exists, t = text mode)
		_wfopen_s( &fBatch, osf.sBatchName.c_str(), L"wxt, ccs=UTF-8" );
		if ( fBatch == nullptr )
#endif
		{
#ifdef ANSIBATCHOUTPUT
			const DWORD dwErr = GetLastError();
			if ( dwErr == ERROR_FILE_EXISTS )
#else
			if ( errno == EEXIST )
#endif
			{
				CString sFmt(MAKEINTRESOURCE( IDS_OVERWRITE_BAT ));
				CString sMsg;
				sMsg.Format( sFmt/*_T("The batch file \"%s\" already exists.\n\nDo you want to overwrite it?")*/, m_sBatchName.c_str() );

				if ( IDYES == MessageBox( sMsg, szSplitAppName, MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2 ) )
				{
#ifdef ANSIBATCHOUTPUT
					hBatchFile.Attach( CreateFile( m_sBatchName.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
						FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL ) );
#else
					_wfopen_s( &fBatch, osf.sBatchName.c_str(), L"wt, ccs=UTF-8" );
#endif

#ifdef ANSIBATCHOUTPUT
					if ( !hBatchFile.IsValid() )
#else
					if ( fBatch == nullptr )
#endif
					{
						ResMessageBox( hWnd, IDS_FAIL_CREATE_BAT/*_T("Failed to create DOS batch file")*/, szSplitAppName, MB_OK | MB_ICONINFORMATION );

						bContinue = false;
					}
				}
				else
				{
					bContinue = false;
				}
			}
		}
	}

	if ( bContinue )
	{
		/* Open the original file for reading */
		CFileHandle hSrcFile( CreateFile( m_sSrcFileName.c_str(), GENERIC_READ, 0, NULL, OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL ) );

		if ( hSrcFile.IsValid() )
		{
			const auto NumChars = NumberOfCharactersToDisplayValue( m_NumFiles );

			/* Form the batch command */
#ifdef ANSIBATCHOUTPUT
			if ( hBatchFile.IsValid() )
#else
			if ( fBatch != nullptr )
#endif
			{
				// Write out the chcp command so that we handle unicode characters, also note the BOM character at the start!
#ifdef ANSIBATCHOUTPUT
				string_view cpCmd( "copy /b " );
				DWORD dwBytesWritten;
				WriteFile( hBatchFile, cpCmd.data(), static_cast<DWORD>(cpCmd.length() * sizeof( cpCmd[0] )), &dwBytesWritten, NULL );
#else
				wstring_view chcpCmd( L"chcp 65001\ncopy /b " );
				_fputts( chcpCmd.data(), fBatch );
#endif
			}

			LARGE_INTEGER SrcFileSize;
			if ( GetFileSizeEx( hSrcFile, &SrcFileSize ) )
			{
				UINT64 SrcRemaining;
				SrcRemaining = SrcFileSize.QuadPart;

				/* Initialise the progress control range */
				::PostMessage( hProgress, PBM_SETRANGE32, 0, PROGRESS_CTL_MAX );

				/* Fill in the data to pass to the thread */
				auto pstd( make_unique<SplitThreadData>( hProgress,
												hWnd,
												m_sSrcFileName,
												SrcFileSize.QuadPart,
												NumChars,
												hSrcFile.Detach(),
												SrcRemaining ) );
#ifdef ANSIBATCHOUTPUT
				pstd->hBatchFile.Attach( hBatchFile.Detach() );
#else
				ptd->fBatch = fBatch;
#endif

				auto dwError = CreateAndSizeDestinationFiles( pstd->vSplitFiles,
					pstd->NumNumericChars,
					pstd->SrcFileSize );

				if ( dwError == ERROR_SUCCESS )
				{
					/* Disable all the UI components while the thread runs */
					UIDisable();

					/* Was the shift key down (to start timing)? */
					if ( bStartTiming )
					{
						/* Save the time at the start of the operation */
						m_tim.SetStartTimeNow();
					}
					else
					{
						m_tim.SetNotInUse();
					}

					/* Clear the cancel flag */
					InterlockedExchange( &g_bCancel, FALSE );

					// Start the split worker thread, moving the unique_ptr data to ownership of the thread (so it deletes it)
					std::thread sctr( SplitControlThread_Reader, std::move( pstd ) );
					// Don't wait for the thread to finish
					sctr.detach();
				}
			}
		}
		else
		{
			/* Failed to open the source file :( */
			const DWORD dwError = GetLastError();

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

			CString sFmt( MAKEINTRESOURCE( IDS_FAIL_OPEN ) );
			CString sMsg;
			sMsg.Format( sFmt, m_sSrcFileName.c_str(), (LPCTSTR) lpMsgBuf );

			MessageBox( sMsg, szSplitAppName, MB_OK | MB_ICONERROR );

			// Free the buffer.
			LocalFree( lpMsgBuf );
		}
	}
}

struct SETTINGS
{
	UINT64 Size;
};

static void LoadSettings( SETTINGS & Settings ) noexcept
{
	CRegKey hk;
	auto Result = hk.Open( HKEY_CURRENT_USER, _T( "Software\\JD Design\\ConCat" ), KEY_READ );

	if ( ERROR_SUCCESS == Result )
	{
		DWORD dwSize{ sizeof( SETTINGS ) };

		/* Now read the settings entry */
		Result = hk.QueryBinaryValue( _T( "Settings" ), &Settings, &dwSize );
	}
}


static bool SaveSettings( SETTINGS Settings ) noexcept
{
	CRegKey hk;
	auto Result = hk.Open( HKEY_CURRENT_USER, _T( "Software\\JD Design\\ConCat" ), KEY_WRITE );

	if ( ERROR_SUCCESS == Result )
	{
		/* Now save the settings entry */
		Result = hk.SetBinaryValue( _T( "Settings" ), &Settings, sizeof( SETTINGS ) );
	}

	return Result == ERROR_SUCCESS;
}

static const struct PresetSizes
{
	const TCHAR* Description;
	unsigned __int64 Size;
}  PresetItemSizes[] =
{
	//				_T("360 KB"), (354*1024)
	//				,_T("720 KB"), (713*1024)
	//				,_T("1.2 MB"), (1185*1024)
	//				,
	_T( "1.4 MB" ), (1423 * 1024)

	,_T( "32 MB" ), (32 * 1024 * 1024)
	,_T( "64 MB" ), (64 * 1024 * 1024)
	,_T( "120 MB" ), (120 * 1024 * 1024)
	,_T( "128 MB" ), (128 * 1024 * 1024)
	,_T( "256 MB" ), (256 * 1024 * 1024)
	,_T( "512 MB" ), (512 * 1024 * 1024)

	,_T( "650 MB 74 min CD" ), (333000 * 2048)
	,_T( "700 MB 80 min CD" ), (360000 * 2048)
	,_T( "790 MB 90 min CD" ), (405000 * 2048)
	,_T( "870 MB 99 min CD" ), (445500 * 2048)

	,_T( "Max. FAT32 file (4 GB -1)" ), 4294967295// (2^32-1) bytes

	,_T( "4.7 GB DVD+R/RW" ), (2295104i64 * 2048)
	,_T( "4.7 GB DVD-R/RW" ), (2298496i64 * 2048)

	,_T( "25 GB Blu-ray single" ), 25025314816
	,_T( "50 GB Blu-ray dual" ), 50050629632
};

/* Array of sizes for the items in the split combo list - because the combo per-item data is limited to 32-bit */
static vector<UINT64> g_vItemSizes;

enum eDrives { CommonDriveSizes, LocalRemovableDriveSizes };

static void PopulateSizeList( HWND hCB, eDrives eType )
{
	/* Clear the combo list */
	ComboBox_ResetContent( hCB );

	/* And the associated item size list */
	g_vItemSizes.clear();

	switch ( eType )
	{
	case CommonDriveSizes:
	{
		g_vItemSizes.reserve( _countof( PresetItemSizes ) );

		/* Populate the combo with the standard sizes */
		for ( const auto & it : PresetItemSizes )
		{
			ComboBox_AddString( hCB, it.Description );
			g_vItemSizes.push_back( it.Size );
		}
	}
	break;

	case LocalRemovableDriveSizes:
		/* Populate the combo box with the sizes of any removable devices on the system */
		/* This may take a noticeable time, so show the hourglass */
	{
		HCURSOR hOldCur = SetCursor( LoadCursor( NULL, IDC_WAIT ) );

		/* Now enumerate the removable drives on the OS */
		TCHAR szDrive[sizeof( "X:\\" ) * 26 + 1];
		GetLogicalDriveStrings( _countof( szDrive ), szDrive );

		for ( LPCTSTR pDrive = szDrive; pDrive[0] != _T( '\0' ); pDrive += sizeof( "X:\\" ) )
		{
			if ( DRIVE_REMOVABLE == GetDriveType( pDrive ) )
			{
				/* It's a removable drive! */

				/* Now find how large it can accommodate */
				ULARGE_INTEGER FreeSpace, TotSpace;

				if ( GetDiskFreeSpaceEx( pDrive, &FreeSpace, &TotSpace, NULL ) )
				{
					TCHAR szSize[50];

					szSize[0] = pDrive[0];	// 'A'
					szSize[1] = pDrive[1];	// ':'
					szSize[2] = pDrive[2];	// '\'
					szSize[3] = _T( ' ' );

					StrFormatByteSizeW( FreeSpace.QuadPart, &szSize[4], _countof( szSize ) - 4 );
					ComboBox_AddString( hCB, szSize );
					g_vItemSizes.push_back( FreeSpace.QuadPart );
				}
			}
		}

		/* Restore the cursor */
		SetCursor( hOldCur );
	}
	break;
	}
}

static void MatchSizeToCBEntry( HWND hDlg, UINT64 SizeValue ) noexcept
{
	/* Select the item in the combo box that corresponds to the saved setting */
	const HWND hCB = GetDlgItem( hDlg, IDC_SIZE_CB );
	int NumItems = ComboBox_GetCount( hCB );
	for ( ; NumItems > 0; NumItems-- )
	{
		const UINT64 Size = g_vItemSizes[NumItems - 1];

		if ( Size == SizeValue )
		{
			/* A match - select this item */
			ComboBox_SetCurSel( hCB, NumItems - 1 );
			break;
		}
	}

	/* If we haven't found a match */
	if ( NumItems <= 0 )
	{
		/* Set the size in the edit field of the combo box */

		/* Convert the size to text */
		const auto sValue = std::to_wstring(SizeValue);

		/* Stick the text in the edit field */
		SetWindowText( hCB, sValue.c_str() );
	}
}

// Constructs a string to show in the UI like this: "filename.ext001...filename.ext199"
static void DisplayDestnFileNameRange( HWND hDlg, LPCTSTR pFName, const UINT NumFiles ) noexcept
{
	const auto NumChars = NumberOfCharactersToDisplayValue( NumFiles );

	/* Create the first file name */
	fs::path pat{ CreateNumericalName( pFName, 1, NumChars ) };

	/* Display the destn path */
	SetDlgItemText( hDlg, IDC_DEST_PATH, pat.parent_path().c_str() );

	/* Compose the first file name */
	wstring strFNameRange = pat.filename();

	/* There's no point showing a "to" filename if we've only got a single one! */
	if ( NumFiles > 1 )
	{
		strFNameRange += _T( '…' );

		fs::path pat2{ CreateNumericalName( pFName, NumFiles, NumChars ) };

		strFNameRange += pat2.filename();
	}

	SetDlgItemText( hDlg, IDC_DEST_NAME, strFNameRange.c_str() );
}

static UINT UpdateNumberOfFiles( HWND hDlg, UINT64 FileSize, UINT64 SplitSize ) noexcept
{
	/* Prevent division by zero */
	if ( SplitSize != 0 )
	{
		const ULONGLONG ullSplitSize = SplitSize;

		UINT NumFiles = static_cast<UINT>(FileSize / ullSplitSize);

		/* If there's any fraction left over */
		if ( FileSize % ullSplitSize != 0 )
		{
			/* It means another file */
			NumFiles++;
		}

		SetDlgItemInt( hDlg, IDC_NUM_FILES, NumFiles, FALSE );

		return (NumFiles);
	}
	else
	{
		return 0;
	}
}

wstring GetWin32ErrorString( DWORD dwError )
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

	wstring sErrMsg = (LPCTSTR) lpMsgBuf;

	// Free the buffer.
	LocalFree( lpMsgBuf );

	return sErrMsg;
}


//IMPLEMENT_DYNAMIC( CSplitDlg, CommonDlg )

BOOL CSplitDlg::OnInitDialog()
{
	CDialog::OnInitDialog();

	{
		bREGISTERED = IsRegisteredAndValid( m_RegData, ProductCode::Concat );

		/* Unregistered/unsaved default */
		SETTINGS ss{ .Size = PresetItemSizes[0].Size };

		//			if ( bREGISTERED )
		{
			LoadSettings( ss );
		}

		/* Default to an unknown size */
		int SizeButton = IDC_REM_RB;
		
		/* See if it matches one of the pre-sets, and if it does, it's a preset size :) */
		for ( const auto & it : PresetItemSizes )
		{
			if ( ss.Size == it.Size )
			{
				SizeButton = IDC_COMMON_RB;
				break;
			}
		}

		/* Select the appropriate size radio button */
		CheckRadioButton( IDC_COMMON_RB, IDC_REM_RB, SizeButton );
		/* Fill the size combo box now we've got the radio button option */
		PopulateSizeList( GetDlgItem( IDC_SIZE_CB )->m_hWnd, SizeButton == IDC_COMMON_RB ?
																	CommonDriveSizes :
																	LocalRemovableDriveSizes );

		m_SplitSize = ss.Size;

		/* Set the default batch file name */
		{
			fs::path pat{ m_sSrcFileName };
			pat.replace_filename( "concat.bat" );

			m_sBatchName = pat;

			SetDlgItemText( IDC_BATCH_NAME, m_sBatchName.c_str() );
		}

		/* Disable the batch facility */
		static_cast<CButton*>(GetDlgItem( IDC_CREATE_COPY_FILE ))->SetCheck( BST_UNCHECKED );

		//if ( !bREGISTERED )
		//{
		//	/* Unregistered version doesn't have this facility */
		//	EnableWindow( GetDlgItem( hDlg, IDC_CREATE_COPY_FILE ), FALSE );
		//	EnableWindow( GetDlgItem( hDlg, IDC_BATCH_NAME ), FALSE );
		//	EnableWindow( GetDlgItem( hDlg, IDC_BATCH_NAME_CHANGE ), FALSE );
		//}

		{
			DWORD dwErr;
			{
				/* Open the file for reading */
				CFileHandle hFile( CreateFile( m_sSrcFileName.c_str(), GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL ) );

				if ( hFile.IsValid() )
				{
					LARGE_INTEGER Size;
					if ( GetFileSizeEx( hFile, &Size ) )
					{
						m_FSize = Size.QuadPart;

						/* Calculate the number of files it will take to split
						 * the source file into the designated size chunks
						 */
						m_NumFiles = UpdateNumberOfFiles( this->m_hWnd, m_FSize, m_SplitSize );

						dwErr = ERROR_SUCCESS;
					}
					else
					{
						// Never expect this to happen
						_ASSERT( false );
						dwErr = GetLastError();
					}
				}
				else
				{
					dwErr = GetLastError();
				}
			}

			if ( dwErr != ERROR_SUCCESS )
			{
				const wstring sError = GetWin32ErrorString( dwErr );
				const wstring sMsg = std::format( L"Unable to access: {}. {}", m_sSrcFileName, sError );
				::MessageBox( this->m_hWnd, sMsg.c_str(), szSplitAppName, MB_OK | MB_ICONSTOP );
				//					psdd->FSize = 0;
									// No point in continuing
				EndDialog( IDCANCEL );
				return TRUE;
			}

			// Take a copy of the original file name
			m_sToFileName = m_sSrcFileName;

			/* Get the "to" range of names populated.
			 * Needs to be done here to cater for the initial situation where no removable drives are available and the size
			 * is not a preset one - this condition doesn't give rise to an event that eventually calls this method elsewhere.
			 */
			DisplayDestnFileNameRange( this->m_hWnd, m_sToFileName.c_str(), m_NumFiles );

			/* Fill in the original file size */
			{
				static NUMBERFMT nf;

				/* Only do this once! It's rather long winded */
				if ( nf.lpThousandSep == NULL )
				{
					TCHAR szBuffer[5];
					static TCHAR szDecSep[5];
					static TCHAR szThousandsSep[5];

					GetLocaleInfo( LOCALE_USER_DEFAULT, LOCALE_SDECIMAL, &szDecSep[0], _countof( szDecSep ) );
					GetLocaleInfo( LOCALE_USER_DEFAULT, LOCALE_STHOUSAND, &szThousandsSep[0], _countof( szThousandsSep ) );

					/* I want no fractions */
					nf.NumDigits = 0;

					/* But all the system defaults for the others */
					GetLocaleInfo( LOCALE_USER_DEFAULT, LOCALE_ILZERO, &szBuffer[0], _countof( szBuffer ) );
					nf.LeadingZero = _ttoi( szBuffer );

					/* The grouping string is a curious format "x[;y][;0]" that needs converting
					to a value of xy */
					GetLocaleInfo( LOCALE_USER_DEFAULT, LOCALE_SGROUPING, &szBuffer[0], _countof( szBuffer ) );
					{
						for ( const auto ch : szBuffer )
						{
							if ( ch == _T( '0' ) )
							{
								break;
							}
							else if ( isdigit( ch ) )
							{
								nf.Grouping = nf.Grouping * 10 + (ch & 0x0f);
							}
						}
					}

					nf.lpDecimalSep = szDecSep;
					nf.lpThousandSep = szThousandsSep;

					GetLocaleInfo( LOCALE_USER_DEFAULT, LOCALE_INEGNUMBER, &szBuffer[0], _countof( szBuffer ) );

					nf.NegativeOrder = _ttoi( szBuffer );
				}


				{
					TCHAR szFmtValue[30];
					{
						TCHAR szValue[30];
						_ui64tot_s( m_FSize, szValue, _countof( szValue ), 10 );

						GetNumberFormat( LOCALE_USER_DEFAULT, 0, szValue, &nf, szFmtValue, _countof( szFmtValue ) );
					}

					{
						CString sFmt;
						/* Get the %s format string from the dialog control */
						GetDlgItemText( IDC_ORIG_SIZE, sFmt );

						/* Format the value string */
						CString sDispText;
						sDispText.Format( sFmt, (LPCTSTR) szFmtValue );

						/* Update the dialog control */
						SetDlgItemText( IDC_ORIG_SIZE, sDispText );
					}
				}
			}
		}
	}

	/* Prevent entering anything except numbers in the combo's edit field */
	{
		HWND hComboEdit = GetDlgItem( IDC_SIZE_CB )->GetDlgItem( 0x03e9 )->m_hWnd;
		DWORD EdStyle = GetWindowLong( hComboEdit, GWL_STYLE );
		EdStyle |= ES_NUMBER;
		SetWindowLong( hComboEdit, GWL_STYLE, EdStyle );

		/* Limit the number of characters to a max uint64 length */
		Edit_LimitText( hComboEdit, sizeof( "18446744073709551615" ) - 1 );
	}

	/* Match the current size to something in the combo box to display something in the edit field */
	MatchSizeToCBEntry( this->m_hWnd, m_SplitSize );

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
BEGIN_MESSAGE_MAP( CSplitDlg, CommonDlg )
	ON_BN_CLICKED( IDC_BATCH_NAME_CHANGE, &CSplitDlg::OnClickedBatchNameChange )
	ON_BN_CLICKED( IDC_CHANGE_DESTN, &CSplitDlg::OnClickedChangeDestn )
	ON_MESSAGE( UWM_WORKER_FINISHED, OnWorkerFinished )
	ON_WM_SYSCOMMAND()
	ON_CONTROL_RANGE( BN_CLICKED, IDC_COMMON_RB, IDC_REM_RB, OnRadioButtonsClicked )
	ON_MESSAGE( UWM_UPDATE_PROGRESS, OnUpdateProgress )

	ON_CBN_SELCHANGE( IDC_SIZE_CB, &CSplitDlg::OnSelchangeSizeCombo )
	ON_CBN_EDITCHANGE( IDC_SIZE_CB, &CSplitDlg::OnEditchangeSizeCombo )
END_MESSAGE_MAP()


void CSplitDlg::OnClickedBatchNameChange()
{
//			if ( bREGISTERED )
	{
		/* Bring up a standard File Save As dialog */
		OPENFILENAME ofn;

		ZeroMemory( &ofn, sizeof( ofn ) );

		TCHAR szBatchName[_MAX_PATH];
		lstrcpy( szBatchName, m_sBatchName.c_str() );

		ofn.lStructSize = sizeof( ofn );
		ofn.hwndOwner = this->m_hWnd;
		ofn.lpstrFile = szBatchName;
		ofn.nMaxFile = _countof( szBatchName );
		ofn.Flags = OFN_EXPLORER | OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;

		if ( GetSaveFileName( &ofn ) )
		{
			m_sBatchName = szBatchName;
			SetDlgItemText( IDC_BATCH_NAME, szBatchName );
		}
	}
}


void CSplitDlg::OnClickedChangeDestn()
{
	/* Bring up a standard File Save As dialog */
	OPENFILENAME ofn;

	ZeroMemory( &ofn, sizeof( ofn ) );

	TCHAR szToName[_MAX_PATH];
	lstrcpy( szToName, m_sToFileName.c_str() );

	ofn.lStructSize = sizeof( ofn );
	ofn.hwndOwner = this->m_hWnd;
	ofn.lpstrFile = szToName;
	ofn.nMaxFile = _countof( szToName );
	ofn.Flags = OFN_EXPLORER | OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;

	if ( GetSaveFileName( &ofn ) )
	{
		m_sToFileName = szToName;
		DisplayDestnFileNameRange( this->m_hWnd, szToName, m_NumFiles );
	}
}

static const int CtrlsToEnableWhileOperating[]{ IDC_SPLITTING, IDC_FNUM, IDC_CURRFILE };

void CSplitDlg::OnOK()
{
	/* Was the shift key down (to start timing)? */
	const bool bShiftKeyDown = GetKeyState( VK_SHIFT ) & 0x8000 ? true : false;

	const bool bCreateBatchFile = static_cast<CButton*>(GetDlgItem( IDC_CREATE_COPY_FILE ))->GetCheck() == BST_CHECKED;

	// If registered, save the current spit size value so the UI defaults to it when next invoked.
//			if ( bREGISTERED )
	{
		SETTINGS ss{ .Size = m_SplitSize };

		SaveSettings( ss );
	}

	for ( auto id : CtrlsToEnableWhileOperating )
	{
		GetDlgItem( id )->EnableWindow( true );
	}
	//			EnableWindow( GetDlgItem( hDlg, IDC_PROGRESS ), true );

	SplitEm( this->m_hWnd, GetDlgItem(IDC_PROGRESS)->m_hWnd, bCreateBatchFile, bShiftKeyDown);
}


LRESULT CSplitDlg::OnWorkerFinished( WPARAM, LPARAM )
{
	CommonDlg::OnWorkerFinished( CtrlsToEnableWhileOperating, szSplitAppName );
	return 0;
}

void CSplitDlg::OnRadioButtonsClicked( UINT nID )
{
	PopulateSizeList( GetDlgItem( IDC_SIZE_CB )->m_hWnd, nID == IDC_COMMON_RB ? CommonDriveSizes : LocalRemovableDriveSizes );
	/* Match the current size to something in the combo box to display something in the edit field */
	MatchSizeToCBEntry( this->m_hWnd, m_SplitSize );

}

void CSplitDlg::OnSelchangeSizeCombo()
{
	/* A different item in the drop list has been chosen */
	CComboBox* pcb = static_cast<CComboBox*>( GetDlgItem( IDC_SIZE_CB ) );
	const UINT SelItem = pcb->GetCurSel();
	if ( SelItem != CB_ERR )
	{
		const UINT64 Size = g_vItemSizes[SelItem];
		if ( Size != 0 )
		{
			m_SplitSize = Size;
		}

		// Calculate the number of files it will take to split
		// the source file into the designated size chunks
		m_NumFiles = UpdateNumberOfFiles( this->m_hWnd, m_FSize, m_SplitSize );
		DisplayDestnFileNameRange( this->m_hWnd, m_sToFileName.c_str(), m_NumFiles );
	}
}

void CSplitDlg::OnEditchangeSizeCombo()
{
	/* Get the size from the edit control */
	TCHAR szValue[sizeof( "18446744073709551615" )];

	GetDlgItemText( IDC_SIZE_CB, szValue, _countof( szValue ) );

	const UINT64 Size = _tcstoui64( szValue, NULL, 10 );

	if ( Size != 0 )
	{
		m_SplitSize = Size;
	}

	// Calculate the number of files it will take to split
	// the source file into the designated size chunks
	m_NumFiles = UpdateNumberOfFiles( this->m_hWnd, m_FSize, m_SplitSize );
	DisplayDestnFileNameRange( this->m_hWnd, m_sToFileName.c_str(), m_NumFiles );
}

LRESULT CSplitDlg::OnUpdateProgress( WPARAM wParam, LPARAM )
{
	/* wParam is the index into the files array */
	const int indx = (int) wParam;

	/* Construct the numerical file we're doing now */
	const auto NumChars = NumberOfCharactersToDisplayValue( m_NumFiles );

	const wstring sPath = CreateNumericalName( m_sToFileName.c_str(), indx + 1, NumChars );

	CommonDlg::OnUpdateProgress( indx, sPath.c_str() );
	return 0;
}

void CSplitDlg::DisplayOperationCompleteMessage()
{
	CString sMsg( MAKEINTRESOURCE( IDS_SPLIT_OK ) );
	MessageBox( sMsg, szSplitAppName, MB_OK | MB_ICONEXCLAMATION );
}
