#pragma once
#include <string>
#include <vector>
#include "CommonThreadData.h"
#include "chandle.h"

class HandlePlusSize
{
public:
	CFileHandle m_fh;			// The file handle
	UINT64 m_SizeToCopy;	// The number of bytes to copy to the file
	TCHAR szFName[_MAX_PATH];	// The file name
	bool m_DeleteOnDestroy;

	HandlePlusSize() noexcept
	{
		m_DeleteOnDestroy = true;	// assume something is going to go wrong and the file should be tidied up on destruction
		szFName[0] = _T( '\0' );
		m_SizeToCopy = 0;
	}

	~HandlePlusSize()
	{
		if ( m_fh.IsValid() )
		{
			if ( m_DeleteOnDestroy )
			{
				/* Closing allocated large files on slow (USB flash drives is very slow.
				 * This allows it to be done much faster.
				 */
				bool bDeletedOnClose;
				{
					FILE_DISPOSITION_INFO fdi;
					fdi.DeleteFile = true;
					bDeletedOnClose = SetFileInformationByHandle( m_fh, FileDispositionInfo, &fdi, sizeof( fdi ) ) ? true : false;
					if ( !bDeletedOnClose )
					{
						DWORD dwe = GetLastError();
						dwe = dwe;
					}
				}

				m_fh.Close();

				/* If the close has deleted it, there's no need to delete it this way as well */
				if ( !bDeletedOnClose )
				{
					DeleteFile( szFName );
				}
			}
		}
	}

	// Needed	HandlePlusSize( const HandlePlusSize& ) = delete;
	// Needed!	HandlePlusSize( HandlePlusSize&& ) = delete;
	//	HandlePlusSize& operator=( const HandlePlusSize& ) = delete;
	//	HandlePlusSize& operator=( HandlePlusSize&& ) = delete;
};

class SplitThreadData : public CommonThreadData
{
public:
	// Prevent inadvertent copying
	SplitThreadData() = delete;
	SplitThreadData( const SplitThreadData& ) = delete;
	SplitThreadData operator=( const SplitThreadData& ) = delete;

	SplitThreadData( HWND hProg, HWND hParent, const std::wstring& srcFName, UINT64 srcFSize, size_t numNumerics, HANDLE hsrc ) :
		sSrcFileName{ srcFName },
		SrcFileSize{ srcFSize },
		NumNumericChars{ numNumerics },
		hSrcFile{ hsrc },
		CommonThreadData( hProg, hParent )
	{
	}

	/* Split members used to pass values to the worker thread */

	const size_t NumNumericChars;	// The number of characters that are needed to create numeric file names of a fixed width
	UINT64 SrcRemaining;
	const UINT64 SrcFileSize;
	const CFileHandle hSrcFile;

	// Currently a UTF-8 batch file will only work from an existing command
	// prompt window that's got the UTF-8 code page set using chcp 65001, or
	// from Windows Explorer when the (beta) setting
	// "Use Unicode UTF-8 for worldwide language support" is set.
	// So, for now, stick with ANSI output
#ifdef ANSIBATCHOUTPUT
	CFileHandle hBatchFile;
#else
	FILE* fBatch = nullptr;
#endif

	std::vector<HandlePlusSize> vSplitFiles;

	const std::wstring& sSrcFileName;	// R/O ref to the name of the file that's being split - used to create the batch file contents.

#ifndef ANSIBATCHOUTPUT
	~SplitThreadData()
	{
		if ( fBatch != nullptr )
		{
			fclose( fBatch );
		}
	}
#endif
};

