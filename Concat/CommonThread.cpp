#include <afxmt.h>
#include "CommonThreadData.h"

#if 0
/* Using the optimal disk size causes much greater processor usage - probably
   as there are more loops, but it also gave rise to faster transfers (and a
   loss of some display updates!) - so is there a more optimal choice?
 */
static DWORD GetOptimalSize()
{
	DWORD SectorsPerCluster;
	DWORD BytesPerSector;

	if ( GetDiskFreeSpace( _T( "C:\\" ), &SectorsPerCluster, &BytesPerSector, NULL, NULL ) )
	{
		return 4 * SectorsPerCluster * BytesPerSector;
	}
	else
	{
		return 4096;
	}
}
const DWORD Granularity = GetOptimalSize();
#else
static DWORD GetSysGran() noexcept
{
	SYSTEM_INFO si;
	GetSystemInfo( &si );
	return si.dwAllocationGranularity;
}

/*const */DWORD Granularity = GetSysGran();
#endif

/* Allocate 2 buffers of the maximal block size - so that we only do heap
 * allocation once for the whole split operation.
 * Having 2 buffers allows the reader thread to keep going at the same time
 * as the writer thread.
 */
 // I think it's pointless having more than 2 buffers, no more throughput will occur
CTransferBuffer g_TransBuffers[2];

void InitializeTransferBuffers()
{
	for ( auto& tb : g_TransBuffers )
	{
		tb.SetBufferEmptied();
		tb.InitBuffer( Granularity );
	}
}

unsigned __stdcall CommonWriterThread( void* /*pParams*/ )
{
	size_t CurBuf = 0;
#if _DEBUG
	size_t NumWrites = 0;
#endif

	do
	{
		/* Use the current transfer buffer */
		CTransferBuffer& rtb = g_TransBuffers[CurBuf];

		// Wait for the signal to do something.
		const DWORD eventID = rtb.WaitForFilledOrStop();

		if ( eventID == WAIT_OBJECT_0 )
		{
			/* Write the chunk out to the destn file */
			DWORD dwBytesWritten;
#ifdef _DEBUG
			TRACE( "WriteFile %d\n", CurBuf );
			LARGE_INTEGER startCount;
			QueryPerformanceCounter( &startCount );
#endif

			if ( WriteFile( rtb.fh, rtb.GetBuffer(), rtb.SizeOfData, &dwBytesWritten, NULL ) )
			{
#ifdef _DEBUG
				LARGE_INTEGER endCount;
				QueryPerformanceCounter( &endCount );
				TRACE( "WriteFile %d done in %d\n", CurBuf, endCount.QuadPart - startCount.QuadPart );
#endif
				/* All ok this chunk */
				_ASSERTE( rtb.SizeOfData == dwBytesWritten );
#if _DEBUG
				NumWrites++;
#endif
			}
			else
			{
				// Save the error code in the transfer buffer so the controlling thread can examine it the next time it accesses this buffer
				rtb.WriteErrorValue = GetLastError();
			}

			/* Signal that this buffer is now empty */
			rtb.SetBufferEmptied();

			/* Next buffer */
			CurBuf = (CurBuf + 1) % _countof( g_TransBuffers );
		}
		else
		{
			/* It must be the signal to quit */
			break;
		}
	} while ( true );

	return 0;
}

