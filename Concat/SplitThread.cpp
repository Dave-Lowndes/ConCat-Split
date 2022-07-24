#include <afxmt.h>
#include <string>
#include <vector>
#include <process.h>
#include <filesystem>
#include "CommonThreadData.h"
#include "CommonThread.h"
#include "SplitThreadData.h"
#include "Globals.h"
#include "resource.h"
#include "CommonDlgData.h"

using std::wstring;
using std::vector;
namespace fs = std::filesystem;

unsigned __stdcall SplitControlThread_Reader( void* pParams )
{
	size_t CurrentBuffer = 0;

	InitializeTransferBuffers();

	/* Start the split writer thread */
	UINT tid;
	::CHandle hThread( reinterpret_cast<HANDLE>(_beginthreadex( NULL, 0, CommonWriterThread, NULL, 0, &tid )) );

	// Get passed a reference (non-null pointer) to the thread data
	SplitThreadData& std = *(static_cast<SplitThreadData*>(pParams));

	DWORD dwError = ERROR_SUCCESS;

	try
	{
		size_t indx = 0;
		for ( vector<HandlePlusSize>::const_iterator it = std.vSplitFiles.begin();
			(it != std.vSplitFiles.end()) && !g_bCancel && (dwError == ERROR_SUCCESS);
			++it, ++indx )
		{
			_ASSERTE( it->m_fh.IsValid() );

			PostMessage( std.hParentWnd, UWM_UPDATE_PROGRESS, indx, 0 );

			{
				ULARGE_INTEGER SizeRemaining;
				SizeRemaining.QuadPart = it->m_SizeToCopy;

				while ( (SizeRemaining.QuadPart > 0) && (dwError == ERROR_SUCCESS) && !g_bCancel )
				{
					/* Update the progress control */
					{
						//TODO Why * 0x8000 David?
						const int ProgPos = static_cast<int>(((std.SrcFileSize - std.SrcRemaining) * 0x8000) / std.SrcFileSize);

						PostMessage( std.hProgress, PBM_SETPOS, ProgPos, 0 );
					}

					/* Handle the next chunk from the source file */

					CTransferBuffer& rtb = g_TransBuffers[CurrentBuffer];

					/* Wait for the buffer to be empty */
					rtb.WaitForEmpty();

					/* Was there any error from the last write? */
					if ( rtb.WriteErrorValue == ERROR_SUCCESS )
					{
						/* Read/Write in small (hopefully optimal sized) chunks */
						DWORD ThisBlockSize;
						const DWORD BufferSize = rtb.GetBufferSize();

						if ( SizeRemaining.QuadPart > BufferSize )
						{
							ThisBlockSize = BufferSize;
						}
						else
						{
							/* Note: the size remaining at this point must be < a DWORD */
							ThisBlockSize = SizeRemaining.LowPart;
						}
#ifdef _DEBUG
						TRACE( "ReadFile %d\n", CurrentBuffer );
						LARGE_INTEGER startCount;
						QueryPerformanceCounter( &startCount );
#endif

						DWORD dwBytesRead;
						if ( ReadFile( std.hSrcFile, rtb.GetBuffer(), ThisBlockSize, &dwBytesRead, NULL ) )
						{
#ifdef _DEBUG
							LARGE_INTEGER endCount;
							QueryPerformanceCounter( &endCount );
							TRACE( "ReadFile %d done in %d\n", CurrentBuffer, endCount.QuadPart - startCount.QuadPart );
#endif

							/* Save the file handle & data length with the buffer */
							rtb.fh = it->m_fh;
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

						SizeRemaining.QuadPart -= ThisBlockSize;
						std.SrcRemaining -= ThisBlockSize;
					}
					else
					{
						dwError = rtb.WriteErrorValue;
					}
				}

				/* Do the source file name for the batch command line */
#ifdef ANSIBATCHOUTPUT
				if ( std.hBatchFile.IsValid() )
#else
				if ( ptd.fBatch != nullptr )
#endif
				{
					/* For the batch file, we only want the filename (no path) */
					const fs::path pat( it->szFName );
					const wstring sFileName = pat.filename();
					CT2CA pA( sFileName.c_str() );

					/* Quote the name to cater for long file names with spaces */
					// Note, prefix the format string to L to get Unicode!
					auto quotedName = std::format( R"("{}")", pA );

					// If there are more to do, add a '+', otherwise a space
					// (the results and knowledge of the copy command syntax
					// will make this clear)
					quotedName += it != std.vSplitFiles.end() - 1 ? '+' : ' ';

#ifdef ANSIBATCHOUTPUT
					DWORD dwBytesWritten;
					WriteFile( std.hBatchFile, quotedName.c_str(), static_cast<DWORD>(quotedName.size() * sizeof( quotedName[0] )), &dwBytesWritten, NULL );
#else
					_fputts( quotedName.c_str(), ptd.fBatch );
#endif
				}
			}
		}

#if 0	// Not necessary now, done automatically when container object destroyed
		/* Close the original file */
		ptd->hSrcFile.Close();

		/* Close any batch file we've created */
		ptd->hBatchFile.Close();
#endif

		/* Stop the writer thread */
		g_StopCommonWriterThread.SetEvent();

		/* Wait for the thread to finish */
		WaitForSingleObject( hThread, 60000 );

		if ( !g_bCancel && (dwError == ERROR_SUCCESS) )
		{
			/* Write the target file name to the batch file */
#ifdef ANSIBATCHOUTPUT
			if ( std.hBatchFile.IsValid() )
#else
			if ( ptd.fBatch != nullptr )
#endif
			{
				/* For the batch file usage, we only want the filename (no path) */
				const fs::path pat( std.sSrcFileName );
				const wstring sFileName = pat.filename();
				CT2CA pA( sFileName.c_str() );

				/* Quote the name to cater for long file names with spaces */
				// Note, prefix the format string with L to get Unicode.
				const auto quotedName = std::format( R"("{}")", pA );

#ifdef ANSIBATCHOUTPUT
				DWORD dwBytesWritten;
				WriteFile( std.hBatchFile, quotedName.c_str(), static_cast<DWORD>(quotedName.size() * sizeof( quotedName[0] )), &dwBytesWritten, NULL );
#else
				_fputts( quotedName.c_str(), ptd.fBatch );
#endif
			}

			/* Mark all the created split files to be retained */
			for ( auto& it : std.vSplitFiles )
			{
				it.m_DeleteOnDestroy = false;
			}

			MessageBeep( MB_OK );
			//			ResMessageBox( hWnd, IDS_SPLIT_OK, szAltName, MB_OK | MB_ICONEXCLAMATION );
		}
		else
		{
			if ( (dwError != -1) && (dwError != ERROR_SUCCESS) )
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
				TCHAR szFmt[256];

				LoadString( g_hResInst, IDS_SPLIT_FAILED, szFmt, _countof( szFmt ) );

				wsprintf( szMsg, szFmt, (LPCTSTR) lpMsgBuf );

				ThreadMessageBox( std.hParentWnd, szMsg, szAltName, MB_OK | MB_ICONERROR );

				// Free the buffer.
				LocalFree( lpMsgBuf );

				/* Set the cancel flag so that we don't close the dialog */
				InterlockedExchange( &g_bCancel, TRUE );
			}
		}
	}
	catch ( std::bad_alloc& )
	{
		dwError = (DWORD) E_OUTOFMEMORY;
	}

	/* Tell the main UI thread that we've done (and to reset the UI) */
	PostMessage( std.hParentWnd, UWM_WORKER_FINISHED, 0, reinterpret_cast<LPARAM>(&std) );

	// No longer need this thread data
	delete& std;

	return dwError;
}

