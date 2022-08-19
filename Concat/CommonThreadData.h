#pragma once
//#include <Windows.h>
#include <afxmt.h>
#include <vector>

extern CEvent g_StopCommonWriterThread;/* Common members used for split and concat threads */

class CommonThreadData
{
public:
	// Prevent inadvertent copying
	CommonThreadData() = delete;
	CommonThreadData( const CommonThreadData& ) = delete;
	CommonThreadData operator=( const CommonThreadData& ) = delete;

	CommonThreadData( HWND hProg, HWND hParent ) : hProgress{ hProg }, hParentWnd{ hParent }
	{
	}

	HWND hProgress;	// Handle to the progress control
	HWND hParentWnd;	// Dialog (parent) window handle
};

class CTransferBuffer
{
public:
	// Prevent inadvertent copying
	//CTransferBuffer() = default;
	//CTransferBuffer( const CTransferBuffer& ) = delete;
	CTransferBuffer operator=( const CTransferBuffer& ) = delete;

	BYTE* GetBuffer() noexcept
	{
		return &vBuffer[0];
	}
	void InitBuffer( size_t BufSize )
	{
		vBuffer.resize( BufSize );

		WriteErrorValue = ERROR_SUCCESS;
		SizeOfData = 0;
		fh = INVALID_HANDLE_VALUE;
	}
	DWORD GetBufferSize() const noexcept
	{
		return static_cast<DWORD>(vBuffer.size());
	}
	HANDLE fh = INVALID_HANDLE_VALUE;	// The file handle
	DWORD SizeOfData = 0;	// The size of the data in the buffer. Because of Win32 API limitations, the buffer size is always < 32-bits (4GB)
	DWORD WriteErrorValue = ERROR_SUCCESS;	// GetLastError value if write fails
private:
	std::vector<BYTE> vBuffer;
	CEvent evtBufferFilled;		// A signal from the reader to the writer that the buffer is ready
	CEvent evtBufferEmptied;	// A signal from the writer to the reader that the buffer is free

public:
	void SetBufferEmptied()
	{
		evtBufferEmptied.SetEvent();
	}
	void SetBufferFilled( HANDLE hDestnFile, DWORD NumBytes )
	{
		fh = hDestnFile;
		SizeOfData = NumBytes;

		evtBufferFilled.SetEvent();
	}
	void WaitForEmpty()
	{
		CSingleLock WriterWait( &evtBufferEmptied );
		WriterWait.Lock();
	}
	void WaitForFilled()
	{
		CSingleLock WriterWait( &evtBufferFilled );
		WriterWait.Lock();
	}
	DWORD WaitForFilledOrStop()
	{
		// The buffer event MUST be first here so that it takes precedence over
		// the stop signal, otherwise we may handle the stop while there is a
		// buffer outstanding.
		CSyncObject* evts[2] = { &evtBufferFilled, &g_StopCommonWriterThread };
		CMultiLock Waiter( evts, _countof( evts ) );

		const DWORD eventID = Waiter.Lock( INFINITE, false );
		return eventID;
	}
};

extern CTransferBuffer g_TransBuffers[2];

class CThreadMessageBoxParams
{
public:
	LPCTSTR pText;
	LPCTSTR pCaption;
	UINT Type;
	int RetVal;
};

// Number of steps in the progress control
#define PROGRESS_CTL_MAX 20
