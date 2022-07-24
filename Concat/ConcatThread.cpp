#include <afxmt.h>
#include <string>
#include <filesystem>

#include "CommonThreadData.h"
#include "CommonThread.h"
#include "CSelPlusReg.h"
#include "chandle.h"
#include "ConcatThreadData.h"
#include "Globals.h"
#include "resource.h"
#include "CommonDlgData.h"

using std::wstring;
using std::tuple;
namespace fs = std::filesystem;

CEvent g_StopCommonWriterThread;

static tuple<DWORD, LARGE_INTEGER> GetTargetSize( const SELITEMS& Files ) noexcept
{
	DWORD dwError = ERROR_SUCCESS;
	LARGE_INTEGER TargetSize;
	TargetSize.QuadPart = 0;

	/* Loop round each file to be joined and add up the sizes */
	for ( auto& itName : Files )
	{
		WIN32_FIND_DATA fd;

		HANDLE hFind = FindFirstFile( itName.c_str(), &fd );
		if ( hFind != INVALID_HANDLE_VALUE )
		{
			LARGE_INTEGER fsize;

			fsize.HighPart = fd.nFileSizeHigh;
			fsize.LowPart = fd.nFileSizeLow;

			// Accumulate the size
			TargetSize.QuadPart += fsize.QuadPart;

			FindClose( hFind );
		}
		else
		{
			dwError = GetLastError();
			break;
		}
	}

	return { dwError, TargetSize };
}

static DWORD ConcatenateFile( HANDLE hDestnFile, LPCTSTR pFileName, size_t& CurrentBuffer, HWND hProgress )
{
	DWORD dwError = ERROR_SUCCESS;

	/* Open the file for reading */
	CFileHandle hSrcFile( CreateFile( pFileName, GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL ) );

	if ( hSrcFile.IsValid() )
	{
		/* Find the file size */
		LARGE_INTEGER FileSize;

		/* Check for errors */
		if ( GetFileSizeEx( hSrcFile, &FileSize ) )
		{
			auto Remaining = FileSize;

			PostMessage( hProgress, PBM_SETRANGE32, 0, 0x8000 );

			/* Do the copy a block at a time */
			while ( (Remaining.QuadPart > 0) && (dwError == ERROR_SUCCESS) && !g_bCancel )
			{
				{
					const int ProgPos = static_cast<int>(((FileSize.QuadPart - Remaining.QuadPart) * 0x8000) / FileSize.QuadPart);

					PostMessage( hProgress, PBM_SETPOS, ProgPos, 0 );
				}

				CTransferBuffer& rtb = g_TransBuffers[CurrentBuffer];

				/* Wait for the buffer to be empty */
				rtb.WaitForEmpty();

				/* Was there any error from the last write? */
				if ( rtb.WriteErrorValue == ERROR_SUCCESS )
				{
					/* Read/Write in small (hopefully optimal sized) chunks */
					const DWORD BufferSize = rtb.GetBufferSize();

					const DWORD ThisBlockSize = Remaining.QuadPart > BufferSize ?
						BufferSize :
						// This is valid to do because we've just checked that it's the remainder
						Remaining.LowPart;

#ifdef _DEBUG
					TRACE( "ReadFile %d\n", CurrentBuffer );
					LARGE_INTEGER startCount;
					QueryPerformanceCounter( &startCount );
#endif

					/* Handle the next chunk from the source file */
					DWORD dwBytesRead;
					if ( ReadFile( hSrcFile, rtb.GetBuffer(), ThisBlockSize, &dwBytesRead, NULL ) )
					{
#ifdef _DEBUG
						LARGE_INTEGER endCount;
						QueryPerformanceCounter( &endCount );
						TRACE( "ReadFile %d done in %d\n", CurrentBuffer, endCount.QuadPart - startCount.QuadPart );
#endif

						/* Save the file handle & data length with the buffer */
						rtb.fh = hDestnFile;
						rtb.SizeOfData = dwBytesRead;

						/* Signal to the writer that there's something waiting for it */
						rtb.SetBufferFilled();

						/* Use the next buffer */
						CurrentBuffer = (CurrentBuffer + 1) % _countof( g_TransBuffers );
					}
					else
					{
						dwError = GetLastError();
					}

					Remaining.QuadPart -= ThisBlockSize;
				}
				else
				{
					dwError = rtb.WriteErrorValue;
				}
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

	return(dwError);
}

unsigned __stdcall ConcatControlThread_Reader( void* pParams )
{
	// Get passed a reference (non-null pointer) to the thread data
	ConcatThreadData& ctd = *(static_cast<ConcatThreadData*>(pParams));

	bool bErrorAlreadyReported = false;

	// Construct a temp file name in the same directory as the destination
	TCHAR szTempName[MAX_PATH];
	{
		fs::path destPath{ ctd.sToName };
		GetTempFileName( destPath.parent_path().c_str(), _T( "JDD" ), 0, szTempName );
	}

	/* Find the total file size required for the target file */
	auto [dwError, TargetSize] = GetTargetSize( ctd.Files );

	if ( dwError == ERROR_SUCCESS )
	{
		/* Open the file for writing */
		CFileHandle hTempFile( CreateFile( szTempName, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL ) );

		if ( hTempFile.IsValid() )
		{
			/* Pre-size the destination file to ensure its going to fit before we bother doing the long winded operation */
			if ( SetFilePointerEx( hTempFile, TargetSize, NULL, FILE_BEGIN ) )
			{
				if ( SetEndOfFile( hTempFile ) )
				{
					/* Back to the start */
					SetFilePointer( hTempFile, 0, NULL, FILE_BEGIN );

					/* We should be OK to write the file */
					try
					{
						/* Initially the buffers are signalled as empty */
						InitializeTransferBuffers();

						/* Start the concatenate writer thread */
						UINT tid;
						::CHandle hThread( reinterpret_cast<HANDLE>(_beginthreadex( NULL, 0, CommonWriterThread, NULL, 0, &tid )) );

						size_t CurrentBuffer = 0;

						/* Loop for each file we're concatenating */
						SELITEMS::const_iterator itName;
						UINT indx;

						for ( indx = 0, itName = ctd.Files.begin();
							(itName != ctd.Files.end()) && (dwError == ERROR_SUCCESS) && !g_bCancel;
							++indx, ++itName )
						{
							/* Update the progress control with the item number and filename of the current item */
							PostMessage( ctd.hParentWnd, UWM_UPDATE_PROGRESS, indx, 0 );

							/* Read the contents of the file */
							/* Append them to the new temporary file */
							dwError = ConcatenateFile( hTempFile, itName->c_str(), CurrentBuffer, ctd.hProgress );
							if ( dwError != ERROR_SUCCESS )
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
								LoadString( g_hResInst, IDS_FAIL_JOIN, szFmt, _countof( szFmt ) );
								wsprintf( szMsg, szFmt/*_T("Failed while joining file '%s'\n\n%s")*/, (LPCTSTR) itName->c_str(), (LPCTSTR) lpMsgBuf );

								//								LoadString( g_hResInst, IDS_FAIL_JOIN_CAPTION, szFmt, _countof( szFmt ) );
								ThreadMessageBox( ctd.hParentWnd, szMsg, szAppName, MB_OK | MB_ICONERROR );

								// Free the buffer.
								LocalFree( lpMsgBuf );

								bErrorAlreadyReported = true;
							}
						}

						/* Stop the writer thread */
						g_StopCommonWriterThread.SetEvent();

						/* Wait for the thread to finish */
						WaitForSingleObject( hThread, 60000 );
					}
					catch ( std::bad_alloc& )
					{
						dwError = (DWORD) E_OUTOFMEMORY;
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
			dwError = GetLastError();
		}
	}

	/* If we've succeeded and not canceled */
	if ( (dwError == ERROR_SUCCESS) && !g_bCancel )
	{
		/* If we're copying onto the first file we selected, first of all delete the original */
		if ( FileExistsAndWritable( ctd.sToName.c_str() ) )
		{
			/* Delete the original file */
			DeleteFile( ctd.sToName.c_str() );
		}

		/* Then rename the temporary file to have the desired name */
		if ( !MoveFile( szTempName, ctd.sToName.c_str() ) )
		{
			/* Failed to rename the file - tell the user */
			TCHAR szMsg[300];
			LoadString( g_hResInst, IDS_FAIL_REN_TEMP, szMsg, _countof( szMsg ) );

			ThreadMessageBox( ctd.hParentWnd, szMsg, szTempName, MB_OK | MB_ICONERROR );
		}

		MessageBeep( MB_OK );
	}
	else
	{
		/* Delete the temporary file */
		DeleteFile( szTempName );
	}

	if ( dwError != ERROR_SUCCESS )
	{
		if ( !bErrorAlreadyReported )
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
			//TCHAR szFmt[100];
			//LoadString( g_hResInst, IDS_FAIL_JOIN, szFmt, _countof( szFmt ) );
			wsprintf( szMsg, _T( "Failed to join temporary file '%s'.\n\n%s" ), szTempName, (LPCTSTR) lpMsgBuf );

			//		LoadString( g_hResInst, IDS_FAIL_JOIN_CAPTION, szFmt, _countof( szFmt ) );
			ThreadMessageBox( ctd.hParentWnd, szMsg, szAppName, MB_OK | MB_ICONERROR );

			// Free the buffer.
			LocalFree( lpMsgBuf );
		}

		/* Set the cancel flag so that we don't close the dialog */
		InterlockedExchange( &g_bCancel, TRUE );
	}

	/* Tell the main UI thread that we've done (and to reset the UI) */
	PostMessage( ctd.hParentWnd, UWM_WORKER_FINISHED, 0, reinterpret_cast<LPARAM>(&ctd) );

	// No longer need this thread data
	delete& ctd;

	return 0;
}

bool FileExistsAndWritable( LPCTSTR pFileName ) noexcept
{
	CFileHandle hFile( CreateFile( pFileName, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL ) );

	/* If we can open the file for writing, it must exist */
	return hFile.IsValid();
}

