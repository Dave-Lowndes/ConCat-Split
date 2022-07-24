#include <afxmt.h>
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
#include "CommonDlgData.h"
#include "CSelPlusReg.h"
#include "chandle.h"
#include "Globals.h"
#include "resource.h"
#include "SplitThreadData.h"
#include "CheckForUpdate.h"

using std::wstring;
using std::string_view;
using std::vector;
using std::span;
namespace fs = std::filesystem;

class SplitDlgData : public CommonDlgData
{
public:
	// Prevent inadvertent copying
	SplitDlgData() = delete;
	SplitDlgData( const SplitDlgData& ) = delete;
	SplitDlgData operator=( const SplitDlgData& ) = delete;

	SplitDlgData( const CSelPlusReg& spr ) : CommonDlgData( spr.m_RegData ), sSrcFileName{ spr.m_Files.at( 0 ) }
	{
	}

	// Used in the dialog and the worker thread
	UINT NumFiles;				// The number of files that will be created
	UINT64 FSize;				// Original file's size
	UINT64 SplitSize;			// Size of the files to create
	wstring sBatchName;
	wstring sToFileName;			// Initially this is a copy of the source file name, but the user can alter it
	const wstring& sSrcFileName;	// R-O ref to the source file path+name
};

static void CreateNumericalName( LPCTSTR pOrigName, const UINT FileNum, span<wchar_t> pFName, const size_t NumCharsInFileNum ) noexcept
{
	const auto OrigLen = wcslen( pOrigName );

	/* Copy the original string into the destn buffer */
	wcsncpy_s( pFName.data(), pFName.size(), pOrigName, OrigLen );

	/* If we can, construct a string showing the range of file names we will create */
	TCHAR szNumber[_countof( _T( "9999999999" ) )];

	/* Convert the number to a string of a predetermined width e.g. "001" */
	const auto hRes = StringCbPrintf( szNumber, sizeof( szNumber ), L"%0*u", static_cast<unsigned int>(NumCharsInFileNum), FileNum );
	_ASSERT( SUCCEEDED( hRes ) );

	/* Append the number range string onto the destination buffer such that it won't overflow */
	const auto MaxPosForDigits = pFName.size() - NumCharsInFileNum - 1;
	if ( OrigLen > MaxPosForDigits )
	{
		/* Buffer isn't large enough, we need to truncate the string */
		lstrcpy( &pFName[MaxPosForDigits], szNumber );
	}
	else
	{
		/* Just append the string */
		lstrcpy( &pFName[OrigLen], szNumber );
	}
}

static DWORD CreateAndSizeDestinationFiles( const SplitDlgData& sdd, vector<HandlePlusSize>& vFiles, size_t NumNumericChars, HWND hParentWnd, LONGLONG SrcFileSize ) noexcept
{
	DWORD dwError = ERROR_SUCCESS;

	// Set the collection size - that we're going to fill in
	vFiles.resize( sdd.NumFiles );

	for ( UINT indx = 0;
		(indx < vFiles.size()) && (dwError == ERROR_SUCCESS) && !g_bCancel;
		++indx )
	{
		auto& CurFile = vFiles[indx];

		/* Create the next file name */
		CreateNumericalName( sdd.sToFileName.c_str(), indx + 1, CurFile.szFName, NumNumericChars );

		CurFile.m_fh.Attach( CreateFile( CurFile.szFName, GENERIC_WRITE | DELETE, 0, NULL,
			CREATE_NEW,
			FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL ) );

		/* Does the file already exist? */
		/* If it does, ask if we can overwrite it */
		if ( !CurFile.m_fh.IsValid() )
		{
			const DWORD dwErr = GetLastError();

			if ( dwErr == ERROR_FILE_EXISTS )
			{
				TCHAR szMsg[_MAX_PATH + 200];
				TCHAR szFmt[100];
				LoadString( g_hResInst, IDS_OVERWRITE_PROMPT, szFmt, _countof( szFmt ) );

				wsprintf( szMsg, szFmt/*_T("The file \"%s\" already exists.\n\nDo you want to overwrite it?")*/, static_cast<LPCTSTR>(CurFile.szFName) );

				if ( IDYES == MessageBox( hParentWnd, szMsg, szAltName, MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2 ) )
				{
					CurFile.m_fh.Attach( CreateFile( CurFile.szFName, GENERIC_WRITE, 0, NULL,
						CREATE_ALWAYS,
						FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL ) );
				}
				else
				{
					/* User's said "No" - there's no point in carrying on now */
					dwError = (DWORD) -1;	// Special value so we don't display another message box.
				}
			}
		}

		if ( CurFile.m_fh.IsValid() )
		{
			/* We can determine the sizes of the split files easily. All but the last
			 * one are the split size, and the last one is the remainder.
			 */
			CurFile.m_SizeToCopy = indx < sdd.NumFiles - 1 ?
				sdd.SplitSize :
				SrcFileSize - (sdd.NumFiles - 1) * sdd.SplitSize;

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

extern unsigned __stdcall SplitControlThread_Reader( void* pParams );

static void SplitEm( HWND hWnd, SplitDlgData& sdd, HWND hProgress, bool bCreateBatchFile, bool bStartTiming )
{
	/* On exit from the dialog the following are set:
		* pFiles[0] is the original file name
		* szToName is the destn file name
		* NumFiles is the number of files we SHOULD create
		* SplitSize is the size of each file
		*/

		/* Create the DOS batch file */
#ifdef ANSIBATCHOUTPUT
	CFileHandle hBatchFile;
#else
	FILE* fBatch = nullptr;
#endif

	if ( bCreateBatchFile )
	{
#ifdef ANSIBATCHOUTPUT
		hBatchFile.Attach( CreateFile( sdd.sBatchName.c_str(), GENERIC_WRITE, 0, NULL, CREATE_NEW,
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
				TCHAR szMsg[_MAX_PATH + 200];
				TCHAR szFmt[100];
				LoadString( g_hResInst, IDS_OVERWRITE_BAT, szFmt, _countof( szFmt ) );
				wsprintf( szMsg, szFmt/*_T("The batch file \"%s\" already exists.\n\nDo you want to overwrite it?")*/,
					sdd.sBatchName.c_str() );

				if ( IDYES == MessageBox( hWnd, szMsg, szAltName, MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2 ) )
				{
#ifdef ANSIBATCHOUTPUT
					hBatchFile.Attach( CreateFile( sdd.sBatchName.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
						FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL ) );
#else
					_wfopen_s( &fBatch, osf.sBatchName.c_str(), L"wt, ccs=UTF-8" );
#endif
				}
			}

#ifdef ANSIBATCHOUTPUT
			if ( !hBatchFile.IsValid() )
#else
			if ( fBatch == nullptr )
#endif
			{
				ResMessageBox( hWnd, IDS_FAIL_CREATE_BAT/*_T("Failed to create DOS batch file")*/, szAltName, MB_OK | MB_ICONINFORMATION );
			}
		}
	}


	/* Open the original file for reading */
	CFileHandle hSrcFile( CreateFile( sdd.sSrcFileName.c_str(), GENERIC_READ, 0, NULL, OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL ) );

	if ( hSrcFile.IsValid() )
	{
		const auto NumChars = NumberOfCharactersToDisplayValue( sdd.NumFiles );

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
			SendMessage( hProgress, PBM_SETRANGE32, 0, 0x8000 );

			/* Fill in the data to pass to the thread */
			auto pstd = new SplitThreadData( hProgress,
				hWnd,
				sdd.sSrcFileName,
				SrcFileSize.QuadPart,
				NumChars,
				hSrcFile.Detach() );
#ifdef ANSIBATCHOUTPUT
			pstd->hBatchFile.Attach( hBatchFile.Detach() );
#else
			ptd->fBatch = fBatch;
#endif
			pstd->SrcRemaining = SrcRemaining;

			auto dwError = CreateAndSizeDestinationFiles( sdd,
				pstd->vSplitFiles,
				pstd->NumNumericChars,
				pstd->hParentWnd,
				pstd->SrcFileSize );

			if ( dwError == ERROR_SUCCESS )
			{
				/* Disable all the UI components while the thread runs */
				sdd.UIDisable( hWnd );

				/* Was the shift key down (to start timing)? */
				if ( bStartTiming )
				{
					/* Save the time at the start of the operation */
					sdd.m_tim.SetStartTimeNow();
				}
				else
				{
					sdd.m_tim.SetNotInUse();
				}

				/* Clear the cancel flag */
				InterlockedExchange( &g_bCancel, FALSE );

				/* Start the split worker thread */
				UINT tid;
				::CHandle hThread( reinterpret_cast<HANDLE>(_beginthreadex( NULL, 0, SplitControlThread_Reader, pstd, 0, &tid )) );
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

		TCHAR szMsg[1024];
		TCHAR szFmt[100];
		LoadString( g_hResInst, IDS_FAIL_OPEN, szFmt, _countof( szFmt ) );
		wsprintf( szMsg, szFmt, sdd.sSrcFileName.c_str(), (LPCTSTR) lpMsgBuf );

		MessageBox( hWnd, szMsg, szAltName, MB_OK | MB_ICONERROR );

		// Free the buffer.
		LocalFree( lpMsgBuf );
	}
}

struct SETTINGS
{
	UINT64 Size;
};

static bool LoadSettings( SETTINGS& pSettings ) noexcept
{
	HKEY hk;
	LONG Result = RegOpenKeyEx( HKEY_CURRENT_USER, _T( "Software\\JD Design\\ConCat" ), 0, KEY_READ, &hk );

	if ( ERROR_SUCCESS == Result )
	{
		DWORD dwSize;

		dwSize = sizeof( SETTINGS );

		/* Now read the settings entry */
		Result = RegQueryValueEx( hk, _T( "Settings" ), 0, NULL, (LPBYTE) &pSettings, &dwSize );

		/* Close the registry key */
		RegCloseKey( hk );
	}

	return Result == ERROR_SUCCESS;
}


static bool SaveSettings( SETTINGS Settings ) noexcept
{
	HKEY hk;
	LONG Result = RegOpenKeyEx( HKEY_CURRENT_USER, _T( "Software\\JD Design\\ConCat" ), 0, KEY_WRITE, &hk );

	if ( ERROR_SUCCESS == Result )
	{
		DWORD dwSize;

		dwSize = sizeof( SETTINGS );

		/* Now save the settings entry */
		Result = RegSetValueEx( hk, _T( "Settings" ), 0, REG_BINARY, (LPBYTE) &Settings, dwSize );

		/* Close the registry key */
		RegCloseKey( hk );
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

/* Array of sizes for the items in the split combo list - because the combo per-item data is limited to 32-bit) */
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

		if ( (Size) == SizeValue )
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
		TCHAR szValue[sizeof( "18446744073709551615" )];
		_ui64tot_s( SizeValue, szValue, _countof( szValue ), 10 );
		/* Stick the text in the edit field */
		SetWindowText( hCB, szValue );
	}

	/* Enable the KB static text indicator for custom sizes */
	ShowWindow( GetDlgItem( hDlg, IDC_KB_IND ), NumItems < 0 ? SW_SHOWNA : SW_HIDE );
}

// Constructs a string to show in the UI like this: "filename.ext001...filename.ext199"
static void DisplayDestnFileNameRange( HWND hDlg, LPCTSTR pFName, const UINT NumFiles ) noexcept
{
	const auto NumChars = NumberOfCharactersToDisplayValue( NumFiles );

	TCHAR szFullName[_MAX_PATH];

	/* Create the first file name */
	CreateNumericalName( pFName, 1, szFullName, NumChars );

	fs::path pat{ szFullName };

	/* Display the destn path */
	SetDlgItemText( hDlg, IDC_DEST_PATH, pat.parent_path().c_str() );

	/* Compose the first file name */
	wstring strFNameRange = pat.filename();

	/* There's no point showing a "to" filename if we've only got a single one! */
	if ( NumFiles > 1 )
	{
		strFNameRange += _T( '…' );

		CreateNumericalName( pFName, NumFiles, szFullName, NumChars );

		fs::path pat2{ szFullName };

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

INT_PTR CALLBACK SplitDlg( HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam )
{
	/* Get this dialog's data class pointer - note this is not valid until first initialised in WM_INITDIALOG processing */
	SplitDlgData* psdd = reinterpret_cast<SplitDlgData*>(::GetWindowLongPtr( hDlg, DWLP_USER ));

	switch ( message )
	{
		/* Is this the special message to do an inter-thread MessageBox? */
	case UWM_TMB:
		TMBHandler( hDlg, lParam );
		break;

	case UWM_WORKER_FINISHED:
		/* Calculate the time it's taken to do the split */
	{
		UINT64 PeriodInMS = psdd->m_tim.GetTimeToNow();
		PeriodInMS = psdd->m_tim.AsMilliSeconds( PeriodInMS );

		/* Do the common operations to re-enable the UI aspects that were disabled during this operation */
		{
			psdd->UIEnable( hDlg );

			/* Match the corresponding enable of this item so things look matched if the operation is cancelled */
			EnableWindow( GetDlgItem( hDlg, IDC_SPLITTING ), false );
			EnableWindow( GetDlgItem( hDlg, IDC_FNUM ), false );
			EnableWindow( GetDlgItem( hDlg, IDC_CURRFILE ), false );
			PostMessage( GetDlgItem( hDlg, IDC_PROGRESS ), PBM_SETPOS, 0, 0 );
			//				EnableWindow( GetDlgItem( hDlg, IDC_PROGRESS ), false );
		}

		if ( psdd->m_tim.InUse() )
		{
			CString sMsg;
			sMsg.Format( _T( "The operation took: %I64u.%I64u seconds" ), PeriodInMS / 1000, PeriodInMS % 1000 );
			MessageBox( hDlg, sMsg, szAltName, MB_OK );
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

	case WM_INITDIALOG:
		// lParam is a pointer to the minimal data passed from the caller - the registration data and selected file names
		// Copy those parts to this dialog's class data
		psdd = new SplitDlgData( *((const CSelPlusReg*) lParam) );

		// Store the pointer to the data so that it's retrievable
		::SetWindowLongPtr( hDlg, DWLP_USER, reinterpret_cast<LONG_PTR>(psdd) );

		{
			bREGISTERED = IsRegisteredAndValid( *(psdd->m_RegData), ProductCode::Concat );

			SETTINGS ss;

			/* Unregistered/unsaved default */
			ss.Size = PresetItemSizes[0].Size;

			if ( bREGISTERED )
			{
				LoadSettings( ss );
			}

			/* Default to an unknown size */
			int SizeButton = IDC_REM_RB;

			/* See if it matches one of the pre-sets, and if it does, it's a preset size :) */
			for ( size_t indx = 0; indx < _countof( PresetItemSizes ); ++indx )
			{
				if ( ss.Size == PresetItemSizes[indx].Size )
				{
					SizeButton = IDC_COMMON_RB;
					break;
				}
			}

			/* Select the appropriate size radio button */
			CheckRadioButton( hDlg, IDC_COMMON_RB, IDC_REM_RB, SizeButton );
			/* Fill the size combo box now we've got the radio button option */
			PopulateSizeList( GetDlgItem( hDlg, IDC_SIZE_CB ), SizeButton == IDC_COMMON_RB ? CommonDriveSizes : LocalRemovableDriveSizes );

			psdd->SplitSize = ss.Size;

			/* Set the default batch file name */
			{
				fs::path pat{ psdd->sSrcFileName };
				pat.replace_filename( "concat.bat" );

				psdd->sBatchName = pat;

				SetDlgItemText( hDlg, IDC_BATCH_NAME, psdd->sBatchName.c_str() );
			}

			/* Disable the batch facility */
			Button_SetCheck( GetDlgItem( hDlg, IDC_CREATE_COPY_FILE ), BST_UNCHECKED );

			if ( !bREGISTERED )
			{
				/* Shareware version doesn't have this facility */
				EnableWindow( GetDlgItem( hDlg, IDC_CREATE_COPY_FILE ), FALSE );
				EnableWindow( GetDlgItem( hDlg, IDC_BATCH_NAME ), FALSE );
				EnableWindow( GetDlgItem( hDlg, IDC_BATCH_NAME_CHANGE ), FALSE );
			}

			{
				DWORD dwErr;
				{
					/* Open the file for reading */
					CFileHandle hFile( CreateFile( psdd->sSrcFileName.c_str(), GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL ) );

					if ( hFile.IsValid() )
					{
						LARGE_INTEGER Size;
						if ( GetFileSizeEx( hFile, &Size ) )
						{
							psdd->FSize = Size.QuadPart;

							/* Calculate the number of files it will take to split
							 * the source file into the designated size chunks
							 */
							psdd->NumFiles = UpdateNumberOfFiles( hDlg, psdd->FSize, psdd->SplitSize );

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
					const wstring sMsg = std::format( L"Unable to access: {}. {}", psdd->sSrcFileName, sError );
					MessageBox( hDlg, sMsg.c_str(), szAltName, MB_OK | MB_ICONSTOP );
//					psdd->FSize = 0;
					// No point in continuing
					EndDialog( hDlg, IDCANCEL );
					return TRUE;
				}

				// Take a copy of the original file name
				psdd->sToFileName = psdd->sSrcFileName;

				/* Get the "to" range of names populated.
				 * Needs to be done here to cater for the initial situation where no removable drives are available and the size
				 * is not a preset one - this condition doesn't give rise to an event that eventually calls this method elsewhere.
				 */
				DisplayDestnFileNameRange( hDlg, psdd->sToFileName.c_str(), psdd->NumFiles );

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
							for ( int i = 0; i < _countof( szBuffer ); i++ )
							{
								const TCHAR ch = szBuffer[i];
								if ( ch == _T( '0' ) )
								{
									break;
								}
								else if ( isdigit( szBuffer[i] ) )
								{
									nf.Grouping = nf.Grouping * 10 + (szBuffer[i] & 0x0f);
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
							_ui64tot_s( psdd->FSize, szValue, _countof( szValue ), 10 );

							GetNumberFormat( LOCALE_USER_DEFAULT, 0, szValue, &nf, szFmtValue, _countof( szFmtValue ) );
						}

						{
							TCHAR szDispText[50];
							TCHAR szFmt[50];
							/* Get the %s format string from the dialog control */
							GetDlgItemText( hDlg, IDC_ORIG_SIZE, szFmt, _countof( szFmt ) );

							/* Format the value string */
							wsprintf( szDispText, szFmt, (LPCTSTR) szFmtValue );

							/* Update the dialog control */
							SetDlgItemText( hDlg, IDC_ORIG_SIZE, szDispText );
						}
					}
				}
			}
		}

		/* Prevent entering anything except numbers in the combo's edit field */
		{
			HWND hComboEdit = GetDlgItem( GetDlgItem( hDlg, IDC_SIZE_CB ), 0x03e9 );
			DWORD EdStyle = GetWindowLong( hComboEdit, GWL_STYLE );
			EdStyle |= ES_NUMBER;
			SetWindowLong( hComboEdit, GWL_STYLE, EdStyle );

			/* Limit the number of characters to a max uint64 length */
			Edit_LimitText( hComboEdit, sizeof( "18446744073709551615" ) - 1 );
		}

		/* Match the current size to something in the combo box to display something in the edit field */
		MatchSizeToCBEntry( hDlg, psdd->SplitSize );

		CenterDialog( hDlg );

		/* Silent check for updates - once per instantiation */
		{
			if ( !bUpdateChecked )
			{
				bUpdateChecked = true;

				RegCheckData rcd( *(psdd->m_RegData), g_hInstance, ProductCode::Concat );
				CheckForUpdate( hDlg, true, rcd );
			}
		}
		break;

	case WM_DESTROY:
		delete psdd;
		psdd = nullptr;
		::SetWindowLongPtr( hDlg, DWLP_USER, reinterpret_cast<LONG_PTR>(psdd) );
		break;

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
		case IDC_BATCH_NAME_CHANGE:
			if ( bREGISTERED )
			{
				/* Bring up a standard File Save As dialog */
				OPENFILENAME ofn;

				ZeroMemory( &ofn, sizeof( ofn ) );

				TCHAR szBatchName[_MAX_PATH];
				lstrcpy( szBatchName, psdd->sBatchName.c_str() );

				ofn.lStructSize = sizeof( ofn );
				ofn.hwndOwner = hDlg;
				ofn.lpstrFile = szBatchName;
				ofn.nMaxFile = _countof( szBatchName );
				ofn.Flags = OFN_EXPLORER | OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;

				if ( GetSaveFileName( &ofn ) )
				{
					psdd->sBatchName = szBatchName;
					SetDlgItemText( hDlg, IDC_BATCH_NAME, szBatchName );
				}
			}
			break;

		case IDC_CHANGE_DESTN:
		{
			/* Bring up a standard File Save As dialog */
			OPENFILENAME ofn;

			ZeroMemory( &ofn, sizeof( ofn ) );

			TCHAR szToName[_MAX_PATH];
			lstrcpy( szToName, psdd->sToFileName.c_str() );

			ofn.lStructSize = sizeof( ofn );
			ofn.hwndOwner = hDlg;
			ofn.lpstrFile = szToName;
			ofn.nMaxFile = _countof( szToName );
			ofn.Flags = OFN_EXPLORER | OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;

			if ( GetSaveFileName( &ofn ) )
			{
				psdd->sToFileName = szToName;
				DisplayDestnFileNameRange( hDlg, szToName, psdd->NumFiles );
			}
		}
		break;

		case IDOK:
		{
			/* Was the shift key down (to start timing)? */
			const bool bShiftKeyDown = GetKeyState( VK_SHIFT ) & 0x8000 ? true : false;

			const bool bCreateBatchFile = Button_GetCheck( GetDlgItem( hDlg, IDC_CREATE_COPY_FILE ) ) == BST_CHECKED;

			// If registered, save the current spit size value so the UI defaults to it when next invoked.
			if ( bREGISTERED )
			{
				SETTINGS ss;
				ss.Size = psdd->SplitSize;

				SaveSettings( ss );
			}

			EnableWindow( GetDlgItem( hDlg, IDC_SPLITTING ), true );
			EnableWindow( GetDlgItem( hDlg, IDC_FNUM ), true );
			EnableWindow( GetDlgItem( hDlg, IDC_CURRFILE ), true );
			//			EnableWindow( GetDlgItem( hDlg, IDC_PROGRESS ), true );

			SplitEm( hDlg, *psdd, GetDlgItem( hDlg, IDC_PROGRESS ), bCreateBatchFile, bShiftKeyDown );
		}
		break;

		case IDCANCEL:
			/* If we're in the middle of the split operation, this is used to cancel the operation */
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
			AboutHandler( hDlg, *psdd->m_RegData );
			break;

			/* Handle the 2 radio button click events to toggle the entries in the combo box */
		case IDC_COMMON_RB:
		case IDC_REM_RB:
			if ( HIWORD( wParam ) == BN_CLICKED )
			{
				PopulateSizeList( GetDlgItem( hDlg, IDC_SIZE_CB ), LOWORD( wParam ) == IDC_COMMON_RB ? CommonDriveSizes : LocalRemovableDriveSizes );
				/* Match the current size to something in the combo box to display something in the edit field */
				MatchSizeToCBEntry( hDlg, psdd->SplitSize );
			}
			break;

		case IDC_SIZE_CB:
			/* Has the selected item in the combo's list changed? */
			if ( HIWORD( wParam ) == CBN_SELCHANGE )
			{
				/* A different item in the drop list has been chosen */
				const UINT SelItem = ComboBox_GetCurSel( (HWND) lParam );
				if ( SelItem != CB_ERR )
				{
					const UINT64 Size = g_vItemSizes[SelItem];
					if ( Size != 0 )
					{
						psdd->SplitSize = Size;
					}

					/* Hide the KB static text indicator */
					ShowWindow( GetDlgItem( hDlg, IDC_KB_IND ), SW_HIDE );
				}
			}
			/* Has the user typed in the combo's edit field? */
			else if ( HIWORD( wParam ) == CBN_EDITCHANGE )
			{
				/* Get the KB size from the edit control */
//				UINT Size = GetDlgItemInt( hDlg, LOWORD( wParam ), NULL, false );

				TCHAR szValue[sizeof( "18446744073709551615" )];

				GetDlgItemText( hDlg, LOWORD( wParam ), szValue, _countof( szValue ) );

				const UINT64 Size = _tcstoui64( szValue, NULL, 10 );

				if ( Size != 0 )
				{
					psdd->SplitSize = Size;
				}

				/* Show the KB static text indicator for custom sizes */
				ShowWindow( GetDlgItem( hDlg, IDC_KB_IND ), SW_SHOWNA );
			}
			else
			{
				break;
			}

			/* Calculate the number of files it will take to split
			 * the source file into the designated size chunks
			 */
			psdd->NumFiles = UpdateNumberOfFiles( hDlg, psdd->FSize, psdd->SplitSize );
			DisplayDestnFileNameRange( hDlg, psdd->sToFileName.c_str(), psdd->NumFiles );
			break;

		default:
			/* We've not processed the message */
			return(FALSE);
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

			/* Construct the numerical file we're doing now */
			const auto NumChars = NumberOfCharactersToDisplayValue( psdd->NumFiles );

			CreateNumericalName( psdd->sToFileName.c_str(), indx + 1, szPath, NumChars );

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
					for ( auto Len = wcslen( pPath ); pPath < &szPath[_MAX_PATH]; pPath++, Len-- )
					{
						SIZE size;
						GetTextExtentPoint32( hMemDC, pPath, (int) Len, &size );
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

	default:
		/* We've not processed the message */
		return(FALSE);
	}

	/* We've processed the message */
	return(TRUE);
}

