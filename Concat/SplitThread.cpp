#include <afxmt.h>
#include <string>
#include <vector>
#include <process.h>
#include <filesystem>
#include <memory>

#include "CommonThreadData.h"
#include "CommonThread.h"
#include "SplitThreadData.h"
#include "Globals.h"
#include "resource.h"
#include "CommonDlg.h"

using std::wstring;
using std::vector;
namespace fs = std::filesystem;
using std::unique_ptr;

unsigned __stdcall SplitControlThread_Reader( unique_ptr<SplitThreadData> ustd )
{
	size_t CurrentBuffer = 0;

	InitializeTransferBuffers();

	/* Start the writer thread */
	std::thread WriterThread( CommonWriterThread );

	// Treat the passed data as a reference
	SplitThreadData& std = *ustd;

	DWORD dwError = ERROR_SUCCESS;

	try
	{
		// Remembers the last set position of the progress control so that I don't tell it to do something unnecessarily
		int PrevProgPos = 0;

		size_t indx = 0;
		for ( vector<HandlePlusSize>::const_iterator it = std.vSplitFiles.begin();
			(it != std.vSplitFiles.end()) && !InterlockedExchangeAdd( &g_bCancel, 0 ) && (dwError == ERROR_SUCCESS);
			++it, ++indx )
		{
			_ASSERTE( it->m_fh.IsValid() );

			PostMessage( std.hParentWnd, UWM_UPDATE_PROGRESS, indx, 0 );

			{
				ULARGE_INTEGER SizeRemaining{ .QuadPart = it->m_SizeToCopy };

				while ( (SizeRemaining.QuadPart > 0) && (dwError == ERROR_SUCCESS) && !InterlockedExchangeAdd( &g_bCancel, 0 ) )
				{
					/* Update the progress control */
					{
						// +1 fiddle to ensure 100%
						const int ProgPos = 1 + static_cast<int>(((std.SrcFileSize - std.SrcRemaining) * PROGRESS_CTL_MAX) / std.SrcFileSize);

						if ( ProgPos != PrevProgPos )
						{
							// The progress bar's position lags what it's set to since MS changed it!
							// Trick to work around it: https://stackoverflow.com/questions/22469876/progressbar-lag-when-setting-position-with-pbm-setpos
							PostMessage( std.hProgress, PBM_SETPOS, static_cast<WPARAM>(ProgPos)+1, 0 );
							PostMessage( std.hProgress, PBM_SETPOS, ProgPos, 0 );
							PrevProgPos = ProgPos;

						}
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
						TRACE( "ReadFile %d\n", CurrentBuffer );	//-V111
						LARGE_INTEGER startCount;
						QueryPerformanceCounter( &startCount );
#endif

						DWORD dwBytesRead;
						if ( ReadFile( std.hSrcFile, rtb.GetBuffer(), ThisBlockSize, &dwBytesRead, NULL ) )
						{
#ifdef _DEBUG
							LARGE_INTEGER endCount;
							QueryPerformanceCounter( &endCount );
							TRACE( "ReadFile %d done in %d\n", CurrentBuffer, endCount.QuadPart - startCount.QuadPart );	//-V111
#endif

							/* Save the file handle & data length with the buffer */
							/* Signal to the writer that there's something waiting for it */
							rtb.SetBufferFilled( it->m_fh, dwBytesRead );

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
					const fs::path pat( it->sFName );
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
		WriterThread.join();

		if ( !InterlockedExchangeAdd( &g_bCancel, 0 ) && (dwError == ERROR_SUCCESS) )
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

			//MessageBeep( MB_OK );
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
					reinterpret_cast<LPTSTR>( &lpMsgBuf ),
					0,
					NULL );

				CString sFmt(MAKEINTRESOURCE( IDS_SPLIT_FAILED ));
				CString sMsg;
				sMsg.Format( sFmt, static_cast<LPCTSTR>( lpMsgBuf ) );

				ThreadMessageBox( std.hParentWnd, sMsg, szSplitAppName, MB_OK | MB_ICONERROR );

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

	return dwError;
}

