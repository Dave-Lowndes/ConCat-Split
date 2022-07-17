//
// CONCAT is a Windows 95 shell extension that takes a multiple selection in an Explorer Window & modifies the
// context menus displayed to provide access to it's concatenation facility.
//

//#define INC_OLE2
//#define OEMRESOURCE

//#define SIDEBYSIDE_COMMONCONTROLS 1 
#include <afxmt.h>
//#include <windows.h>
#include <Commdlg.h>
#include <ShellAPI.h>
#include <windowsx.h>
#include <initguid.h>
#include <shlobj.h>

#include "resource.h"
#include "RegEnc.h"
#include "concat.h"

#include <algorithm>

#include "chandle.h"

#include "AboutDlg.h"
#include "AboutDlgResourceIds.h"
#include "CheckForUpdate.h"
#include "RegKeyRegistryFuncs.h"
#include <span>
#include <strsafe.h>


/*
Don't acknowledge successful entry of the registration details - have the user close and reopen to force the proper (complex) registration code to run.

In the initialisation
Records the OS version number in the global time difference variable (see below). A fooling tactic.
Rearranges the product GUID.
Start the worker thread.
Does some real work.
Sleeps.
Gets the registration details from the registry.
Does a bit more real work.
Signals the worker thread.
Do a bit more work.
Sleep & determine if the registration is valid.


Have a worker thread that:
Records the current time.
Waits for a signal - but only for a short time (1 sec timeout).
Saves the time difference in a global variable - adjusting the value to look like an OS version number.
Restores the mangled registration GUID.
*/
//
// Global variables
//
static LONG	volatile g_cRefThisDll = 0;          // Reference count for this DLL
HINSTANCE   g_hInstance;                // Instance handle for this DLL
HMODULE		g_hResInst;					// Instance handle for the resource DLL
TCHAR szAppName[] = _T("Concat");
const static TCHAR szAltName[] = _T("Split");
BOOL bREGISTERED;
DW_UNION g_GetVersionResult;	// Used to foil hacked versions - accessed by different parts.

#if 0
/* Using the optimal disk size causes much greater processor usage - probably
   as there are more loops, but it also gave rise to faster transfers (and a
   loss of some display updates!) - so is there a more optimal choice?
 */
static DWORD GetOptimalSize()
{
	DWORD SectorsPerCluster;
	DWORD BytesPerSector;

	if ( GetDiskFreeSpace( _T("C:\\"), &SectorsPerCluster, &BytesPerSector, NULL, NULL ) )
	{
		return 4*SectorsPerCluster * BytesPerSector;
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

const DWORD Granularity = GetSysGran();
#endif

// Should be 32-bit aligned.
__declspec(align(4))
// Signals cancellation of the concat/split operation.
static LONG volatile g_bCancel;

//#undef LoadLibraryA

static void CenterDialog( HWND hWnd ) noexcept
{
	HWND hWndParent = GetParent( hWnd );
	POINT   Point;
	RECT    DialogRect;
	RECT    ParentRect;
	int      nWidth;
	int      nHeight;

	// Get the size of the dialog box.
	GetWindowRect( hWnd, &DialogRect );

	// And the parent
	GetClientRect( hWndParent, &ParentRect);

	// Calculate the height and width for MoveWindow().
	nWidth = DialogRect.right - DialogRect.left;
	nHeight = DialogRect.bottom - DialogRect.top;

	// Find the center point and convert to screen coordinates.
	Point.x = (ParentRect.right/* - ParentRect.left*/) / 2;
	Point.y = (ParentRect.bottom/* - ParentRect.top*/ ) / 2;

	ClientToScreen( hWndParent, &Point );

	// Calculate the new X, Y starting point.
	Point.x -= nWidth / 2;
	Point.y -= nHeight / 2;

	// Move the window.
	MoveWindow( hWnd, Point.x, Point.y, nWidth, nHeight, FALSE );
}
 
// The signalling event for the ThreadMessageBox facility
static HANDLE g_hTMBEvent;

//
// DllMain is the DLL's entry point.
//
// Input parameters:
//   hInstance  = Instance handle
//   dwReason   = Code specifying the reason DllMain was called
//   lpReserved = Reserved (do not use)
//
// Returns:
//   TRUE if successful, FALSE if not
//

extern "C" int APIENTRY DllMain( HINSTANCE hInstance, DWORD dwReason, LPVOID /* lpReserved */ )
{
	//
	// If dwReason is DLL_PROCESS_ATTACH, save the instance handle so it
	// can be used again later.
	//
	if ( dwReason == DLL_PROCESS_ATTACH )
	{
		g_hInstance = hInstance;
		g_hResInst = g_hInstance;	// Default to english resources in the main DLL itself

		/* Create the ThreadMessageBox event */
        g_hTMBEvent = CreateEvent( NULL, false, false, NULL );
	}
	else
	if ( dwReason == DLL_PROCESS_DETACH )
	{
		CloseHandle( g_hTMBEvent );
	}
	return TRUE;
}

/////////////////////////////////////////////////////////////////////////////
// Other functions

const TCHAR szRegistryKey[] = _T("Software\\JD Design\\ConCat");

/////////////////////////////////////////////////////////////////////////////
// In-process server functions

//
// DllGetClassObject is called by the shell to create a class factory object.
//
// Input parameters:
//   rclsid = Reference to class ID specifier
//   riid   = Reference to interface ID specifier
//   ppv    = Pointer to location to receive interface pointer
//
// Returns:
//   HRESULT code signifying success or failure
//
_Check_return_
STDAPI DllGetClassObject( _In_ REFCLSID rclsid, _In_ REFIID riid, _Outptr_ LPVOID *ppv )
{
	*ppv = NULL;

	// Make sure the class ID is CLSID_ShellExtension. Otherwise, the class
	// factory doesn't support the object type specified by rclsid.
	if ( !IsEqualCLSID( rclsid, CLSID_ShellExtension ) )
	{
		return ResultFromScode (CLASS_E_CLASSNOTAVAILABLE);
	}

	// Instantiate a class factory object.
	CClassFactory * pClassFactory{ new (std::nothrow) CClassFactory() };
	if ( pClassFactory == NULL )
	{
		// Can't conceivably happen!
		return ResultFromScode (E_OUTOFMEMORY);
	}
	else
	{
		// Get the interface pointer from QueryInterface and copy it to *ppv.
		const auto hr = pClassFactory->QueryInterface( riid, ppv );
		pClassFactory->Release();
		return hr;
	}
}

//
// DllCanUnloadNow is called by the shell to find out if the DLL can be
// unloaded. The answer is yes if (and only if) the module reference count
// stored in g_cRefThisDll is 0.
//
// Input parameters:
//   None
//
// Returns:
//   HRESULT code equal to S_OK if the DLL can be unloaded, S_FALSE if not
//
__control_entrypoint( DllExport )
STDAPI DllCanUnloadNow (void)
{
	return ResultFromScode ((g_cRefThisDll == 0) ? S_OK : S_FALSE);
}

/////////////////////////////////////////////////////////////////////////////
// CClassFactory member functions

CClassFactory::CClassFactory () noexcept
{
	m_cRef = 1;
    InterlockedIncrement( &g_cRefThisDll );
}

CClassFactory::~CClassFactory ()
{
	InterlockedDecrement( &g_cRefThisDll );
}

STDMETHODIMP CClassFactory::QueryInterface (REFIID riid, LPVOID FAR *ppv) noexcept
{
	if (IsEqualIID (riid, IID_IUnknown))
	{
		*ppv = (LPUNKNOWN) (LPCLASSFACTORY) this;
		m_cRef++;
		return NOERROR;
	}

	else if (IsEqualIID (riid, IID_IClassFactory))
	{
		*ppv = (LPCLASSFACTORY) this;
		m_cRef++;
		return NOERROR;
	}

	else
	{
		*ppv = NULL;
		return ResultFromScode (E_NOINTERFACE);
	}
}

STDMETHODIMP_(ULONG) CClassFactory::AddRef () noexcept
{
	return ++m_cRef;
}

STDMETHODIMP_(ULONG) CClassFactory::Release () noexcept
{
	if ( --m_cRef == 0 )
	{
		delete this;
		return 0;
	}
	return m_cRef;
}

//
// CreateInstance is called by the shell to create a shell extension object.
//
// Input parameters:
//   pUnkOuter = Pointer to controlling unknown
//   riid      = Reference to interface ID specifier
//   ppvObj    = Pointer to location to receive interface pointer
//
// Returns:
//   HRESULT code signifying success or failure
//

STDMETHODIMP CClassFactory::CreateInstance( LPUNKNOWN pUnkOuter, REFIID riid, LPVOID FAR *ppvObj ) noexcept
{
	*ppvObj = NULL;

	// Return an error code if pUnkOuter is not NULL, because we don't
	// support aggregation.
	if ( pUnkOuter != NULL )
	{
		return ResultFromScode (CLASS_E_NOAGGREGATION);
	}

	// Instantiate a shell extension object.
	CShellExtension * pShellExtension{ new (std::nothrow) CShellExtension() };
	if ( pShellExtension == NULL )
	{
		// Can't conceivably happen
		return ResultFromScode (E_OUTOFMEMORY);
	}
	else
	{
		// Get the interface pointer from QueryInterface and copy it to *ppvObj.
		const auto hr = pShellExtension->QueryInterface (riid, ppvObj);
		pShellExtension->Release();
		return hr;
	}
}

//
// LockServer increments or decrements the DLL's lock count.
//

STDMETHODIMP CClassFactory::LockServer (BOOL /* fLock */) noexcept
{
	return ResultFromScode (E_NOTIMPL);
}

/////////////////////////////////////////////////////////////////////////////
// CShellExtension member functions

CShellExtension::CShellExtension () noexcept
{
	m_cRef = 1;
	m_hSplitBitmap = LoadBitmap( g_hInstance, MAKEINTRESOURCE( IDB_SPLIT_MENU_BMP ) );
	m_hConcatBitmap = LoadBitmap( g_hInstance, MAKEINTRESOURCE( IDB_CONCAT_MENU_BMP ) );
	InterlockedIncrement( &g_cRefThisDll );

	/* Try to load the language resource DLL */
	TCHAR szPath[_MAX_PATH];
	DWORD Len = GetModuleFileName( g_hInstance, szPath, _countof( szPath ) );

	/* Truncate the ConCat.dll part, and append the language DLL name instead */
	Len -= sizeof("ConCat.dll") - 1;
	lstrcpy( &szPath[ Len ], _T("ConCat.lang") );

	HMODULE hMod = LoadLibrary( szPath );
	if ( hMod != NULL )
	{
		g_hResInst = hMod;
#if 0
		{
			TCHAR szMsg[100];
			wsprintf( szMsg, _T("Loaded lang dll for thread %x, handle %x\n" ), GetCurrentThreadId(), hMod );
			OutputDebugString( szMsg );
		}
#endif
	}
	else
	{
		/* Use the main DLL itself as the resource DLL */
		g_hResInst = g_hInstance;
	}
}

CShellExtension::~CShellExtension ()
{
	InterlockedDecrement( &g_cRefThisDll );

	/* Unload the language DLL */
	if ( g_hResInst != g_hInstance )
	{
		FreeLibrary( g_hResInst );
		/* Reset to original "no separate resource DLL" */
		g_hResInst = g_hInstance;
#if 0
		{
			TCHAR szMsg[100];
			wsprintf( szMsg, _T("Freed lang dll for thread %x, handle %x\n" ), GetCurrentThreadId(), g_hResInst );
			OutputDebugString( szMsg );
		}
#endif
	}

	DeleteObject( m_hSplitBitmap );
	m_hSplitBitmap = NULL;
	DeleteObject( m_hConcatBitmap );
	m_hConcatBitmap = NULL;
}

STDMETHODIMP CShellExtension::QueryInterface (REFIID riid, LPVOID FAR *ppv) noexcept
{
	if (IsEqualIID (riid, IID_IUnknown))
	{
		*ppv = (LPUNKNOWN) (LPCONTEXTMENU) this;
		m_cRef++;
		return NOERROR;
	}

	else if (IsEqualIID (riid, IID_IContextMenu))
	{
		*ppv = (LPCONTEXTMENU) this;
		m_cRef++;
		return NOERROR;
	}

	else if (IsEqualIID (riid, IID_IShellExtInit))
	{
		*ppv = (LPSHELLEXTINIT) this;
		m_cRef++;
		return NOERROR;
	}

	else
	{
		*ppv = NULL;
		return ResultFromScode (E_NOINTERFACE);
	}
}

STDMETHODIMP_(ULONG) CShellExtension::AddRef () noexcept
{
	return ++m_cRef;
}

STDMETHODIMP_(ULONG) CShellExtension::Release () noexcept
{
	if ( --m_cRef == 0 )
	{
		delete this;
		return 0;
	}
	return m_cRef;
}

static bool FileExistsAndWritable( LPCTSTR pFileName ) noexcept
{
	CFileHandle hFile( CreateFile( pFileName, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL ) );

	/* If we can open the file for writing, it must exist */
	return hFile.IsValid();
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

	return( bExists );
}

//
// QueryContextMenu is called before a context menu is displayed so the
// extension handler can add items to the menu.
//
// Input parameters:
//   hMenu      = Context menu handle
//   indexMenu  = Index for first new menu item
//   idCmdFirst = Item ID for first new menu item
//   idCmdLast  = Maximum menu item ID that can be used
//   uFlags     = Flags
//
// Returns:
//   HRESULT code signifying success or failure. If successful, the 'code'
//   field specifies the number of items added to the context menu.
//

STDMETHODIMP CShellExtension::QueryContextMenu( HMENU hMenu, UINT indexMenu, UINT idCmdFirst, UINT /* idCmdLast */, UINT uFlags  ) noexcept
{
	//
	// Add new menu items to the context menu.
	//
	bool bInsert = true;

	if (uFlags & CMF_VERBSONLY) 
	{
		/* Nothing for shortcuts */
		bInsert = false;
	}

	if ( uFlags & CMF_DEFAULTONLY )
	{
		bInsert = false;
	}

	if ( bInsert )
	{
		/* If we have only 1 file selected, we are doing a split, if multiple a concatenation */
		TCHAR szMenuText[ 80 ];
		HBITMAP hBmp;

		const size_t NumSel = GetNumSelectedItems();
		/* Single file selected == split, multiple == concat */
		if ( NumSel == 1 )
		{
			::LoadString( g_hResInst, IDS_SPLIT_MENU, szMenuText, _countof(szMenuText) );
			hBmp = m_hSplitBitmap;
		}
		else
		{
			::LoadString( g_hResInst, IDS_CONCAT_MENU, szMenuText, _countof(szMenuText) );
			hBmp = m_hConcatBitmap;
		}

		::InsertMenu( hMenu, indexMenu, MF_STRING | MF_BYPOSITION, idCmdFirst + IDOFFSET_CONCAT, szMenuText );

		/* Some people take a purist view to menus having bitmaps, so it's now optional */
		bool bDisplayBitmap = true;
		{
			LONG res;
			HKEY hKey;

			res = RegOpenKeyEx( HKEY_CURRENT_USER, szRegistryKey, 0, KEY_READ, &hKey );

			if ( res == ERROR_SUCCESS )
			{
				DWORD Val;
				DWORD dwSize = sizeof( Val );

				/* Read the value */
				res = RegQueryValueEx( hKey, _T("Options"), NULL, NULL, (LPBYTE) &Val, &dwSize );
				
				if ( ERROR_SUCCESS == res )
				{
					/* Copy the settings back to the caller */
					bDisplayBitmap = Val & 0x01;
				}

				RegCloseKey( hKey );
			}
		}

		if ( bDisplayBitmap )
		{
			SetMenuItemBitmaps( hMenu, indexMenu, MF_BYPOSITION, hBmp, NULL );
		}

		indexMenu++;

		return ResultFromScode( MAKE_SCODE ( SEVERITY_SUCCESS, 0, USHORT ( IDOFFSET_CONCAT + 1 ) ) );
	}
	else
	{
		return NOERROR;
	}
}

// Encapsulates the data that is passed down to the dialog functions
class CSelPlusReg
{
public:
	// Prevent inadvertent copying
	CSelPlusReg() = delete;
	CSelPlusReg( const CSelPlusReg& ) = delete;
	CSelPlusReg operator=( const CSelPlusReg& ) = delete;

	CSelPlusReg( const SELITEMS& Files, const std::optional<CMyRegData>& RegData ) noexcept : m_Files{ Files }, m_RegData{RegData}
	{
	}

//private:
	const SELITEMS & m_Files;			// Reference to the collection of file names that were selected in Explorer (entry 0 in split is the original file)
	const std::optional<CMyRegData> & m_RegData;	// Reference to the registration data
};

static int ResMessageBox( HWND hWnd, int ResId, LPCTSTR pCaption, const int Flags ) noexcept
{
	TCHAR szMsg[256];

	::LoadString( g_hResInst, ResId, szMsg, _countof(szMsg) );

	return( MessageBox( hWnd, szMsg, pCaption, Flags ) );
}

static int ModifyPathForControl( HDC hDC, HFONT hFont, RECT * rect, LPCTSTR pName ) noexcept
{
	/* Create a memory DC with the same attributes as the control */
	HDC hMemDC;
	hMemDC = CreateCompatibleDC( hDC );
	HFONT hOldFont = (HFONT) SelectObject( hMemDC, hFont );

	/* DrawText the string into the in-memory DC */
	DrawText( hMemDC, pName, -1, rect, DT_PATH_ELLIPSIS /*| DT_WORD_ELLIPSIS*/ | DT_MODIFYSTRING );

	/* Because DT_PATH_ELLIPSIS doesn't necessarily make the text fit, check it now
	 * and if it doesn't, make it right aligned so we can see the extension
     * (which is the most important part here).
     */
	SIZE size;

    GetTextExtentPoint32( hMemDC, pName, lstrlen( pName ), &size );

	int diff = size.cx - ( rect->right - rect->left );
	if ( diff < 0 )
	{
		diff = 0;
	}

	SelectObject( hMemDC, hOldFont );

	DeleteDC( hMemDC );

	return diff;
}

/*
Concurrent split thread behaviour.
There are 2 threads that perform the split operation:

1. SplitControlThread_Reader
----------------------------
This is the most significant. It instigates #2 and controls the overall split
operation.
It creates the resultant split files and pre-sizes them (so that we can't run
out of disk space) and reads blocks from the file to be split into buffers
(just 2) for thread #2 to write to the split files.

2. CommonWriterThread
--------------------
On a signal from thread #1 this thread writes a buffer to a split file.

More Details on the core signaling & data shared by the 2 threads:

There are 2 buffers used so that one thread can be reading into one buffer
while the other thread is writing from the other one.
Each buffer has 2 events - a buffer filled event and a buffer emptied one. They
are set not unsurprisingly when a buffer is filled, and when it is emptied.

Thread #1
---------
Starts with buffer 0.
1 . Waits for buffer 0's empty signal (both buffers are initially signaled as empty
FWIW).
2. Reads from the source file into the buffer.
3. Sets the buffer's full event.
Switches to the other buffer and loops to 1.

When the final part of the file has been read (into a buffer) it
4. Sets the signal to finish thread #2.

So, initially both buffers are empty, and if reading is faster than writing,
buffer 1 will be filled before the writer has finished writing buffer 0 - so
thread #1 waits (at 1) for thread #2 to signal buffer 0 as being empty.
Eventually the writer thread signals the buffer is empty and the thread
continues. Generally one thread can read while the other is writing at all
times (depending on the speed of the devices for reading & writing).

Thread #2
---------
Starts with buffer 0.
1. Waits for the buffer's full signal & the finish signal from thread #1.
If it's the buffer full signal...
2. Writes the buffer out to the split file.
3. Sets the buffer's empty signal.
Switches to the other buffer and loops to 1.

If it gets the finish signal... the thread exits.

Note that at 1 - the buffer full signal is the first event in an array because
it must take priority over the finish signal. The WaitForMultipleObjects API
(aka CMultiLock) defines this behaviour.

The Buffers
-----------
CTransferBuffer defines the buffer & associated parameters (the split file
handle, and length of data in the buffer).
The buffer itself is a fixed size, allocated at the start of the operation in
order to minimise heap allocations (and reduce CPU time and contention in the
heap) during the copying.
*/

static CEvent g_StopCommonWriterThread;

class CTransferBuffer
{
public:
	// Prevent inadvertent copying
	//CTransferBuffer() = default;
	//CTransferBuffer( const CTransferBuffer& ) = delete;
	CTransferBuffer operator=( const CTransferBuffer& ) = delete;

	BYTE * GetBuffer() noexcept
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
		return static_cast<DWORD>( vBuffer.size() );
	}
	HANDLE fh;	// The file handle
	DWORD SizeOfData;	// The size of the data in the buffer. Because of Win32 API limitations, the buffer size is always < 32-bits (4GB)
	DWORD WriteErrorValue;	// GetLastError value if write fails
private:
	vector<BYTE> vBuffer;
	CEvent evtBufferFilled;		// A signal from the reader to the writer that the buffer is ready
	CEvent evtBufferEmptied;	// A signal from the writer to the reader that the buffer is free

public:
	void SetBufferEmptied()
	{
		evtBufferEmptied.SetEvent();
	}
	void SetBufferFilled()
	{
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

/* Allocate 2 buffers of the maximal block size - so that we only do heap
 * allocation once for the whole split operation.
 * Having 2 buffers allows the reader thread to keep going at the same time
 * as the writer thread.
 */
// I think it's pointless having more than 2 buffers, no more throughput will occur
static CTransferBuffer g_TransBuffers[2];

static DWORD ConcatenateFile( HANDLE hDestnFile, LPCTSTR pFileName, size_t & CurrentBuffer, HWND hProgress )
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
			while ( ( Remaining.QuadPart > 0 ) && ( dwError == ERROR_SUCCESS ) && !g_bCancel )
			{
				{
				const int ProgPos = static_cast<int>( ( ( FileSize.QuadPart- Remaining.QuadPart ) * 0x8000)/FileSize.QuadPart );

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
					TRACE("ReadFile %d\n", CurrentBuffer);
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

	return( dwError );
}

class CThreadMessageBoxParams
{
public:
	LPCTSTR pText;
	LPCTSTR pCaption;
	UINT Type;
	int RetVal;
};

// Window message to call the message box in the main UI thread
//static const UINT g_MsgNum = RegisterWindowMessage( _T("7B9ECF2C-15F1-4f90-A351-46D87701C12C") );
#define UWM_TMB (WM_APP+1)

// This message updates the UI for the operation
#define UWM_UPDATE_PROGRESS (WM_APP+2)

#define UWM_WORKER_FINISHED (WM_APP+3)

static int ThreadMessageBox( HWND hParent, LPCTSTR lpText, LPCTSTR lpCaption, UINT Type ) noexcept
{
	CThreadMessageBoxParams mbp;

	mbp.pText = lpText;
	mbp.pCaption = lpCaption;
	mbp.Type = Type;
	mbp.RetVal = 0;

	PostMessage( hParent, UWM_TMB/*g_MsgNum*/, 0, reinterpret_cast<LPARAM>( &mbp ) );
	WaitForSingleObject( g_hTMBEvent, INFINITE );

	return mbp.RetVal;
}

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

/* Common members used for split and concat threads */
class CommonThreadData
{
public:
	// Prevent inadvertent copying
	CommonThreadData() = default;
	CommonThreadData( const CommonThreadData& ) = delete;
	CommonThreadData operator=( const CommonThreadData& ) = delete;

	HWND hProgress;	// Handle to the progress control
	HWND hParentWnd;	// Dialog (parent) window handle
};

class ConcatThreadData : public CommonThreadData
{
public:
	// Prevent inadvertent copying
	ConcatThreadData() = default;
	ConcatThreadData( const ConcatThreadData& ) = delete;
	ConcatThreadData operator=( const ConcatThreadData& ) = delete;

	SELITEMS Files;		// The collection of file names that are being joined

	wstring sTempName;	// The temporary working file name while concatenating
	wstring sToName;	// The final copy destination file name
};

class SplitThreadData : public CommonThreadData
{
public:
	// Prevent inadvertent copying
	SplitThreadData() = default;
	SplitThreadData( const SplitThreadData& ) = delete;
	SplitThreadData operator=( const SplitThreadData& ) = delete;

	/* Split members used to pass values to the worker thread */

	size_t MaxNumCharsForNumericName;	// The number of characters that are needed to create numeric file names of a fixed width
	UINT64 SrcRemaining;
	UINT64 SrcFileSize;
	CFileHandle hSrcFile;
	CFileHandle hBatchFile;

	vector<HandlePlusSize> vSplitFiles;

	wstring sSrcFileName;	// The name of the file that's being split - used to create the batch file contents.

};

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

static unsigned __stdcall CommonWriterThread( void * /*pParams*/ )
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
	}
	while (true);

	return 0;
}


static unsigned __stdcall ConcatControlThread_Reader( void * pParams )
{
	// Get passed a reference (non-null pointer) to the thread data
	ConcatThreadData & ptd = *(static_cast<ConcatThreadData *>(pParams));

	bool bErrorAlreadyReported = false;

	/* Find the total file size required for the target file */
	auto [dwError, TargetSize] = GetTargetSize(ptd.Files);

	if ( dwError == ERROR_SUCCESS )
	{
		/* Open the file for writing */
		CFileHandle hTempFile( CreateFile( ptd.sTempName.c_str(), GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL));

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
						for ( auto & tb : g_TransBuffers )
						{
							tb.SetBufferEmptied();
							tb.InitBuffer( Granularity );
						}

						/* Start the concatenate writer thread */
						UINT tid;
						::CHandle hThread( reinterpret_cast<HANDLE>( _beginthreadex( NULL, 0, CommonWriterThread, NULL, 0, &tid ) ) );

						size_t CurrentBuffer = 0;

						/* Loop for each file we're concatenating */
						SELITEMS::const_iterator itName;
						UINT indx;

						for ( indx = 0, itName = ptd.Files.begin();
							( itName != ptd.Files.end() ) && ( dwError == ERROR_SUCCESS) && !g_bCancel;
							++indx, ++itName )
						{
							/* Update the progress control with the item number and filename of the current item */
							PostMessage( ptd.hParentWnd, UWM_UPDATE_PROGRESS, indx, 0 );

							/* Read the contents of the file */
							/* Append them to the new temporary file */
							dwError = ConcatenateFile( hTempFile, itName->c_str(), CurrentBuffer, ptd.hProgress );
							if ( dwError != ERROR_SUCCESS )
							{
								LPVOID lpMsgBuf;

								FormatMessage( 
									FORMAT_MESSAGE_ALLOCATE_BUFFER | 
									FORMAT_MESSAGE_FROM_SYSTEM | 
									FORMAT_MESSAGE_IGNORE_INSERTS,
									NULL,
									dwError,
									MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
									(LPTSTR) &lpMsgBuf,
									0,
									NULL );

								TCHAR szMsg[1024];
								TCHAR szFmt[100];
								LoadString( g_hResInst, IDS_FAIL_JOIN, szFmt, _countof( szFmt ) );
								wsprintf( szMsg, szFmt/*_T("Failed while joining file '%s'\n\n%s")*/, (LPCTSTR) itName->c_str(), (LPCTSTR) lpMsgBuf );

//								LoadString( g_hResInst, IDS_FAIL_JOIN_CAPTION, szFmt, _countof( szFmt ) );
								ThreadMessageBox( ptd.hParentWnd, szMsg, szAppName, MB_OK | MB_ICONERROR );

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
					catch( std::bad_alloc & )
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
	if ( ( dwError == ERROR_SUCCESS ) && !g_bCancel )
	{
		/* If we're copying onto the first file we selected, first of all delete the original */
		if ( FileExistsAndWritable( ptd.sToName.c_str() ) )
		{
			/* Delete the original file */
			DeleteFile( ptd.sToName.c_str() );
		}

		/* Then rename the temporary file to have the desired name */
		if ( !MoveFile( ptd.sTempName.c_str(), ptd.sToName.c_str() ) )
		{
			/* Failed to rename the file - tell the user */
			TCHAR szMsg[300];
			LoadString( g_hResInst, IDS_FAIL_REN_TEMP, szMsg, _countof( szMsg ) );

			ThreadMessageBox( ptd.hParentWnd, szMsg, ptd.sTempName.c_str(), MB_OK | MB_ICONERROR);
		}

		MessageBeep( MB_OK );
	}
	else
	{
		/* Delete the temporary file */
		DeleteFile( ptd.sTempName.c_str() );
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
				MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
				(LPTSTR) &lpMsgBuf,
				0,
				NULL );

			TCHAR szMsg[1024];
			//TCHAR szFmt[100];
			//LoadString( g_hResInst, IDS_FAIL_JOIN, szFmt, _countof( szFmt ) );
			wsprintf( szMsg, _T( "Failed to join temporary file '%s'.\n\n%s" ), ptd.sTempName.c_str(), (LPCTSTR) lpMsgBuf);

			//		LoadString( g_hResInst, IDS_FAIL_JOIN_CAPTION, szFmt, _countof( szFmt ) );
			ThreadMessageBox( ptd.hParentWnd, szMsg, szAppName, MB_OK | MB_ICONERROR );

			// Free the buffer.
			LocalFree( lpMsgBuf );
		}

		/* Set the cancel flag so that we don't close the dialog */
		InterlockedExchange( &g_bCancel, TRUE );
	}

	/* Tell the main UI thread that we've done (and to reset the UI) */
	PostMessage( ptd.hParentWnd, UWM_WORKER_FINISHED, 0, reinterpret_cast<LPARAM>( &ptd ) );

	return 0;
}

static void TMBHandler( HWND hDlg, LPARAM lParam ) noexcept
{
	CThreadMessageBoxParams * pmbp = reinterpret_cast<CThreadMessageBoxParams *>( lParam );

	pmbp->RetVal = MessageBox( hDlg, pmbp->pText, pmbp->pCaption, pmbp->Type );
	SetEvent( g_hTMBEvent );
}

#if 1
class CPerfTimer
{
public:
	/* Init */
	CPerfTimer() noexcept
	{
		m_Start.QuadPart = 0;	// Only done for the use of InUse()

		if ( m_Freq.QuadPart == 0 )
		{
			QueryPerformanceFrequency( &m_Freq );
		}
	}
	bool InUse() const noexcept
	{
		return m_Start.QuadPart != 0;
	}
	void SetNotInUse() noexcept
	{
		m_Start.QuadPart = 0;
	}
	void SetStartTimeNow() noexcept
	{
		QueryPerformanceCounter( &m_Start );
	}
	__int64 GetTimeToNow() const noexcept
	{
		LARGE_INTEGER now;
		QueryPerformanceCounter( &now );

		return now.QuadPart - m_Start.QuadPart;
	}
	static unsigned __int64 AsMilliSeconds( __int64 period ) noexcept
	{
		//#ifdef _DEBUG
		//		return period / (m_Freq.QuadPart / 100000);
		//#else
		return period / (m_Freq.QuadPart / 1000);
		//#endif
	}

private:
	LARGE_INTEGER m_Start;
	static LARGE_INTEGER m_Freq; // Counts per second
};

LARGE_INTEGER CPerfTimer::m_Freq;
#endif

static void AboutHandler( HWND hDlg, const optional<CMyRegData> & RegData ) noexcept
{
	RegCheckData rcd( RegData, g_hInstance, ProductCode::Concat );

	DialogBoxParam( g_hResInst, MAKEINTRESOURCE( IDD_ABOUT_DLG ), hDlg, (DLGPROC) AboutDlg, reinterpret_cast<LPARAM>(&rcd) );

	if ( !bREGISTERED )
	{
		/* No point saving if there's nothing there! */
		if ( !rcd.sReturnedRegKey.empty() )
		{
			/* Store the entry to the registry ready for the next time */
			SaveMyRegistrationToTheRegistry( szRegistryKey, rcd.sReturnedRegKey );

			/* Message: If you've entered your registration details, press Close or Cancel to close the TouchPro dialog to have the changes take effect" */
			ResMessageBox( hDlg, IDS_CLOSE_FOR_REG, szAppName, MB_OK );
		}
	}
}

static bool bUpdateChecked = false;

class DlgDataCommon
{
public:
	// Prevent inadvertent copying
	//DlgDataCommon() = default;
	//DlgDataCommon( const DlgDataCommon& ) = delete;
	//DlgDataCommon operator=( const DlgDataCommon& ) = delete;

	DlgDataCommon( const std::optional<CMyRegData>& RegData ) noexcept : m_RegData{RegData}
	{
	}
	const std::optional<CMyRegData>& m_RegData;	// Reference to the registration data
	std::vector<HWND> vhDisabledCtrls;	// The dialog controls that need re-enabling at the end of the operation
private:
};

static void UIEnable( const DlgDataCommon& td, HWND hParentWnd ) noexcept
{
	/* Re-enable the control windows */
	for ( const auto it : td.vhDisabledCtrls )
	{
		EnableWindow( it, true );
	}

	/* Change the caption of the Cancel button back */
	{
		TCHAR szCancel[30];

		LoadString( g_hResInst, IDS_CLOSE, szCancel, _countof( szCancel ) );
		SetDlgItemText( hParentWnd, IDCANCEL, szCancel );
	}

	/* Re-enable closing the dialog box */
	EnableMenuItem( ::GetSystemMenu( hParentWnd, FALSE ), SC_CLOSE, MF_BYCOMMAND | MF_ENABLED );
}

static BOOL CALLBACK EnumChildProcToDisableSomeControls( HWND hWnd, LPARAM lParam )
{
	if ( IsWindowEnabled( hWnd ) )
	{
		// Don't disable the Cancel/Close button (that's used to cancel the operation prematurely)
		if ( GetWindowLongPtr( hWnd, GWLP_ID ) != IDCANCEL )
		{
			// See https://docs.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-createwindoww for the predefined class names
			TCHAR szClass[20];
			const int NumChars = GetClassName( hWnd, szClass, _countof( szClass ) );

			const bool bIsButtonCtrl = (NumChars == sizeof( "Button" ) - 1) && (_wcsicmp( szClass, _T( "Button" ) ) == 0);

			const bool bDisableThisCtrl = bIsButtonCtrl ?
				// Don't disable any group boxes (buttons) - it causes a visual quirk with the overlaid check box
				(GetWindowLong( hWnd, GWL_STYLE ) & BS_GROUPBOX) != BS_GROUPBOX :
				// Don't disable static (label) controls
				!((NumChars == sizeof( "STATIC" ) - 1) && (_wcsicmp( szClass, _T( "STATIC" ) ) == 0));

			if ( bDisableThisCtrl )
			{
				EnableWindow( hWnd, false );

				/* Add the window handle to the vector that I use to re-enable them */
				auto pvControls = reinterpret_cast< vector<HWND> * >(lParam);

				pvControls->push_back( hWnd );
			}
		}
	}

	return TRUE;
}

static auto UIDisable( HWND hParentWnd ) noexcept
{
	vector<HWND> vDisabledControls;

	/* Move the keyboard focus to the cancel button before we disable anything */
	SendMessage( hParentWnd, WM_NEXTDLGCTL, (WPARAM) GetDlgItem( hParentWnd, IDCANCEL ), TRUE );

	/* Disable the currently enabled child windows to prevent interaction */
	EnumChildWindows( hParentWnd, EnumChildProcToDisableSomeControls, (LPARAM) &vDisabledControls );

	/* Change the caption of the Close button to Cancel - it now functions to cancel the operation */
	{
		TCHAR szCancel[30];

		LoadString( g_hResInst, IDS_CANCEL, szCancel, _countof( szCancel ) );
		SetDlgItemText( hParentWnd, IDCANCEL, szCancel );
	}

	/* Need to disable closing the dialog box */
	EnableMenuItem( ::GetSystemMenu( hParentWnd, FALSE ), SC_CLOSE, MF_BYCOMMAND | MF_DISABLED | MF_GRAYED );

	return vDisabledControls;
}

static void ConcatEm( HWND hWnd, ConcatThreadData& td, HWND hProgress )
{
	/* Create a temporary file for the result in the same directory as the final target file */
	TCHAR szPath[MAX_PATH];
	LPTSTR pFileNamePart;

	if ( 0 != GetFullPathName( td.sToName.c_str(), MAX_PATH, szPath, &pFileNamePart ) )
	{
		/* Remove the file name from the path, to establish just the path to put the temporary file in! */
		*pFileNamePart = '\0';
	}
	else
	{
		/* Something is wrong, but let's put the file in the temporary directory */
		GetTempPath( MAX_PATH, szPath );
	}

	TCHAR szTempName[MAX_PATH];

	/* Create the temporary output file in the place where the user wants it */
	if ( 0 != GetTempFileName( szPath, _T( "JDD" ), 0, szTempName ) )
	{
		td.hProgress = hProgress;
		td.hParentWnd = hWnd;
		td.sTempName = szTempName;

		/* Disable all the UI components while the thread runs */
		auto disabledCtrls = UIDisable( hWnd );

		/* Clear the cancel flag */
		InterlockedExchange( &g_bCancel, FALSE );

		UINT tid;
		::CHandle hThread( reinterpret_cast<HANDLE>(_beginthreadex( NULL, 0, ConcatControlThread_Reader, &td, 0, &tid )) );
	}
	else
	{
		ResMessageBox( hWnd, IDS_FAIL_CREATE_TEMP /*
				_T("Failed to create temporary file.\n\n")
				_T("The directory path may be invalid, ")
				_T("you may not have access to that directory, or the disk may be full.")*/,
			szAppName, MB_OK | MB_ICONERROR );
	}
}

class ConcatDlgData : public DlgDataCommon
{
public:
	// Prevent inadvertent copying
	//ConcatDlgData() = default;
	//ConcatDlgData( const ConcatDlgData& ) = delete;
	//ConcatDlgData operator=( const ConcatDlgData& ) = delete;

	ConcatDlgData( const CSelPlusReg & spr ) : DlgDataCommon(spr.m_RegData)
	{
		// Take a copy of the file names
		m_td.Files = spr.m_Files;
	}
	ConcatThreadData m_td;
};

class SplitDlgData : public DlgDataCommon
{
public:
	// Prevent inadvertent copying
	//SplitDlgData() = default;
	//SplitDlgData( const SplitDlgData& ) = delete;
	//SplitDlgData operator=( const SplitDlgData& ) = delete;

	SplitDlgData( const CSelPlusReg& spr ) : DlgDataCommon( spr.m_RegData ), sSrcFileName{ spr.m_Files.at( 0 ) }
	{
	}

	// Used in the dialog and the worker thread
	UINT NumFiles;				// The number of files that will be created
	UINT64 FSize;				// Original file's size
	UINT64 SplitSize;			// Size of the files to create
	wstring sBatchName;
	wstring sToFileName;			// Initially this is a copy of the source file name, but the user can alter it
	const wstring & sSrcFileName;	// Ref to the source file path+name
};

INT_PTR CALLBACK ConcatDlg( HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam )
{
	static CPerfTimer tim;

	/* Get this dialog's data class pointer - note this is not valid until first initialised in WM_INITDIALOG processing */
	ConcatDlgData * pcdd = reinterpret_cast<ConcatDlgData*>(::GetWindowLongPtr( hDlg, DWLP_USER ));

	switch ( message )
	{
	/* inter-thread MessageBox */
	case UWM_TMB:
		TMBHandler( hDlg, lParam );
		break;

	case UWM_WORKER_FINISHED:
		/* Calculate the time it's taken to do the split */
		{
			UINT64 PeriodInMS = tim.GetTimeToNow();
			PeriodInMS = tim.AsMilliSeconds( PeriodInMS );

			/* Do the common operations to re-enable the UI aspects that were disabled during this operation */
			{
				{
					const ConcatThreadData * ptd = reinterpret_cast<ConcatThreadData*>(lParam);
					UIEnable( *pcdd, hDlg );
					delete ptd;
				}

				/* Disable this to match the original state */
				EnableWindow( GetDlgItem( hDlg, IDC_CONCATING ), false );
				EnableWindow( GetDlgItem( hDlg, IDC_CURRFILE ), false );
				EnableWindow( GetDlgItem( hDlg, IDC_FNUM ), false );
				PostMessage( GetDlgItem( hDlg, IDC_PROGRESS ), PBM_SETPOS, 0, 0 );
//				EnableWindow( GetDlgItem( hDlg, IDC_PROGRESS ), false );
			}

			if ( tim.InUse() )
			{
				CString sMsg;
				sMsg.Format( _T("The operation took: %I64u.%I64u seconds"), PeriodInMS / 1000,  PeriodInMS % 1000 );
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

			SetDlgItemInt( hDlg, IDC_FNUM, indx+1, false );

			{
				TCHAR szPath[_MAX_PATH];

				lstrcpyn( szPath, pcdd->m_td.Files.at(indx).c_str(), _countof(szPath) );
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
				std::ranges::sort( pcdd->m_td.Files );
			}

			const HWND hList = GetDlgItem( hDlg, IDC_COPY_LIST );

			/* Displaying the path items using ellipsis notation */
			/* Add all the files to the list box */
			for ( const auto & itName : pcdd->m_td.Files )
			{
				ListBox_AddString( hList, itName.c_str() );
			}

			/* Copy the first name to be the destination name */
			TCHAR szToName[_MAX_PATH];
			lstrcpy( szToName, pcdd->m_td.Files.at(0).c_str() );

			if ( bREGISTERED )
			{
				/* If the file has an extension > 3 characters long, and numerical,
				 * it's likely to be one of the split files created by Split.
				 * Therefore, attempt to "guess" the correct start name
				 */
				TCHAR szDrive[_MAX_DRIVE];
				TCHAR szDir[_MAX_DIR];
				TCHAR szFName[_MAX_FNAME];
				TCHAR szExt[_MAX_EXT];

				_tsplitpath_s( szToName, szDrive, szDir, szFName, szExt );

				/* Any numerical entries at the end of the extension are
				 * likely to have been created by Split, so remove them.
				 * The number of digits at the end are likely to be related
				 * to the number of selected files, so just remove those many.
				 */
				size_t NumDigits = 0;
				{
					size_t NumFiles = pcdd->m_td.Files.size();
					do
					{
						NumFiles /= 10;
						NumDigits++;
					}
					while ( NumFiles > 0 );
				}

				auto ExtLen = wcslen( szExt );
				LPTSTR pNameExt;

				/* Do we have an extension? */
				if ( ExtLen != 0 )
				{
					/* Yes - so use it */
					pNameExt = szExt;
				}
				else
				{
					/* No - so use the filename instead */
					pNameExt = szFName;
					ExtLen = wcslen( pNameExt );
				}

				// At this point, expect ExtLen to be > NumDigits so that the following loop conditions are safe
				if ( ExtLen > NumDigits )
				{
					for ( size_t indx = ExtLen - 1; indx >= ExtLen - NumDigits; --indx )
					{
						/* If the extension character is a digit */
						if ( isdigit( pNameExt[indx] ) )
						{
							/* Remove it */
							pNameExt[indx] = '\0';
						}
						else
						{
							/* Stop at the first non-numerical character */
							break;
						}
					}
				}

				// Rebuild the name with the numerics removed from either the extension or filename.
				_tmakepath_s( szToName, szDrive, szDir, szFName, szExt );
			}

			SetDlgItemText( hDlg, IDC_TO, szToName );
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

				const int Excess = ModifyPathForControl( lpdis->hDC, GetWindowFont(GetDlgItem( hDlg, IDC_COPY_LIST )),
							&txtrc, szBuffer );

				TEXTMETRIC tm;
				GetTextMetrics( lpdis->hDC, &tm );

				const int y = (lpdis->rcItem.bottom + lpdis->rcItem.top - tm.tmHeight) / 2;

				ExtTextOut( lpdis->hDC, lpdis->rcItem.left-Excess, y, ETO_CLIPPED | ETO_OPAQUE, &lpdis->rcItem, szBuffer, (UINT) wcslen(szBuffer), NULL );

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
		if ( ( wParam & 0x0FFF0 ) == SC_CLOSE )
		{
			/* If the default OK button is disabled, we must be doing the Split operation */
			if ( !IsWindowEnabled( GetDlgItem( hDlg, IDOK ) ) )
			{
                return TRUE;
			}
		}
		return FALSE;

	case WM_COMMAND:
		switch( LOWORD( wParam ) )
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
					const BOOL bDownEnabled = indx != (int) ( pcdd->m_td.Files.size() - 1 );

					{
						/* If we're disabling a control that currently has the focus, we must move the focus */
						if ( !bUpEnabled && ( GetFocus() == hUp ) )
						{
							/* Next control */
							FORWARD_WM_NEXTDLGCTL( hDlg, 0, FALSE, SendMessage );
						}

						EnableWindow( hUp, bUpEnabled );

						if ( !bDownEnabled && ( GetFocus() == hDown ) )
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
					auto & FileNameAtPos = pcdd->m_td.Files.at( pos );
					auto & FileNameAtCurItem = pcdd->m_td.Files.at( CurItem );

					std::swap( FileNameAtPos, FileNameAtCurItem );

					/* Re-display the item in the list box */
					ListBox_InsertString( hList, pos, FileNameAtPos.c_str() );
					ListBox_DeleteString( hList, pos+1 );

					ListBox_InsertString( hList, CurItem, FileNameAtCurItem.c_str() );
					ListBox_DeleteString( hList, CurItem+1 );

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
				{
					TCHAR szToNameTmp[_MAX_PATH];
					GetDlgItemText( hDlg, IDC_TO, szToNameTmp, _countof( szToNameTmp ) );
					pcdd->m_td.sToName = szToNameTmp;
				}

				/* Check that the string is a valid file name */
				// *** TO BE DONE???
				{
					BOOL bOk = FALSE;

					/* If the file already exists, ask the user if they want to delete it */
					if ( FileExistsAndWritable( pcdd->m_td.sToName.c_str() ) )
					{
						/* File already exists message */
						TCHAR szFmtMsg[256];

						::LoadString( g_hResInst, IDS_ALREADY_EXISTSPROMPT, szFmtMsg, _countof(szFmtMsg) );

						TCHAR szMsg[_MAX_PATH + _countof(szFmtMsg)];
						wsprintf( szMsg, szFmtMsg, pcdd->m_td.sToName.c_str() );

						if ( IDYES == MessageBox( hDlg, szMsg, szAppName, MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2 ) )
						{
							bOk = TRUE;
						}
					}
					else
					{
						/* Get the current error code, because checking if the file exists will change it */
						const DWORD dwError = GetLastError();

						if ( FileExists( pcdd->m_td.sToName.c_str() ) )
						{
							LPVOID lpMsgBuf;

							FormatMessage( 
								FORMAT_MESSAGE_ALLOCATE_BUFFER | 
								FORMAT_MESSAGE_FROM_SYSTEM | 
								FORMAT_MESSAGE_IGNORE_INSERTS,
								NULL,
								dwError,
								MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
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
							tim.SetStartTimeNow();
						}
						else
						{
							tim.SetNotInUse();
						}

						ConcatEm( hDlg, pcdd->m_td, GetDlgItem( hDlg, IDC_PROGRESS ) );
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
			return( FALSE );
		}
		break;

	default:
		/* We've not processed the message */
		return( FALSE );
	}

	/* We've processed the message */
	return( TRUE );
}
#if 0
/* Converts an integer value (+ve) to a string with leading zeros
 * Eliminates having sprintf!
 */
constexpr static void SpecialIntToString( UINT Value, span<wchar_t> pBuffer, const size_t LastIndex ) noexcept
{
	pBuffer[ LastIndex ] = '\0';

	size_t indx;

	for ( indx = LastIndex-1; ( static_cast<long long>(indx) >= 0 ) && ( Value != 0 ); indx-- )
	{
		pBuffer[indx] = (TCHAR) ( '0' + Value % 10 );
		Value /= 10;
	}

	/* Fill the remainder of the buffer with 0's */
	for ( ; static_cast<long long>(indx) >= 0; indx-- )
	{
		pBuffer[indx] = _T('0' );
	}
}
#endif

static void CreateNumericalName( LPCTSTR pOrigName, const UINT FileNum, std::span<wchar_t> pFName, const size_t NumCharsInFileNum ) noexcept
{
	const auto OrigLen = wcslen( pOrigName );

	/* Copy the original string into the destn buffer */
	wcsncpy_s( pFName.data(), pFName.size(), pOrigName, OrigLen );

	/* If we can, construct a string showing the range of file names we will create */
	TCHAR szNumber[ _countof( _T("9999999999") )];

	/* Convert the number to a string of a predetermined width e.g. "001" */
	const auto hRes = StringCbPrintf( szNumber, sizeof( szNumber ), L"%0*u", static_cast<unsigned int>(NumCharsInFileNum), FileNum);
	_ASSERT( SUCCEEDED( hRes ) );

	/* Append the number range string onto the destination buffer such that it won't overflow */
	const auto MaxPosForDigits = pFName.size() - NumCharsInFileNum - 1;
	if ( OrigLen > MaxPosForDigits )
	{
		/* Buffer isn't large enough, we need to truncate the string */
		lstrcpy( &pFName[ MaxPosForDigits ], szNumber);
	}
	else
	{
		/* Just append the string */
		lstrcpy( &pFName[ OrigLen ], szNumber );
	}
}

static UINT UpdateNumberOfFiles( HWND hDlg, UINT64 FileSize, UINT64 SplitSize ) noexcept
{
	/* Prevent division by zero */
	if ( SplitSize != 0 )
	{
		const ULONGLONG ullSplitSize = SplitSize;

		UINT NumFiles = static_cast<UINT>( FileSize / ullSplitSize );

		/* If there's any fraction left over */
		if ( FileSize % ullSplitSize != 0 )
		{
			/* It means another file */
			NumFiles++;
		}

		SetDlgItemInt( hDlg, IDC_NUM_FILES, NumFiles, FALSE );

		return ( NumFiles );
	}
	else
	{
		return 0;
	}
}

constexpr static size_t NumberOfCharactersToDisplayValue( UINT Value ) noexcept
{
	size_t DigitWidth = 0;

	do
	{
		Value /= 10;
		DigitWidth++;
	}
	while ( Value != 0 );

	return( DigitWidth );
}

static void DisplayDestnFileNameRange( HWND hDlg, LPCTSTR pFName, const UINT NumFiles ) noexcept
{
	const auto NumChars = NumberOfCharactersToDisplayValue( NumFiles );

	TCHAR szFullName[_MAX_PATH];

	/* Create the first file name */
	CreateNumericalName( pFName, 1, szFullName, NumChars );

	TCHAR fname[_MAX_FNAME];
	TCHAR fext[_MAX_EXT];
	TCHAR szDrive[_MAX_DRIVE];
	TCHAR szDir[_MAX_DIR];

	_tsplitpath_s( szFullName, szDrive, szDir, fname, fext );

	/* Display the destn path */
	TCHAR szBuffer[2 * (_MAX_FNAME + _MAX_EXT + 1) + 3];
	_tmakepath_s( szBuffer, szDrive, szDir, NULL, NULL );
	SetDlgItemText( hDlg, IDC_DEST_PATH, szBuffer );

	/* Compose the first file name */
	lstrcpy( szBuffer, fname );
	lstrcat( szBuffer, fext );

	/* There's no point showing a "to" filename if we've only got a single one! */
	if ( NumFiles > 1 )
	{
		lstrcat( szBuffer, _T("…") );

		CreateNumericalName( pFName, NumFiles, szFullName, NumChars );
		_tsplitpath_s( szFullName, NULL, 0, NULL, 0, fname, _countof(fname), fext, _countof(fext) );
		lstrcat( szBuffer, fname );
		lstrcat( szBuffer, fext );
	}

	SetDlgItemText( hDlg, IDC_DEST_NAME, szBuffer );
}

struct SETTINGS
{
	UINT64 Size;
};

static bool LoadSettings( SETTINGS & pSettings ) noexcept
{
	HKEY hk;
	LONG Result = RegOpenKeyEx( HKEY_CURRENT_USER, _T("Software\\JD Design\\ConCat"), 0, KEY_READ, &hk );

	if ( ERROR_SUCCESS == Result )
	{
		DWORD dwSize;

		dwSize = sizeof( SETTINGS );

		/* Now read the settings entry */
		Result = RegQueryValueEx( hk, _T("Settings"), 0, NULL, (LPBYTE) &pSettings, &dwSize );

		/* Close the registry key */
		RegCloseKey( hk );
	}

	return Result == ERROR_SUCCESS;
}


static bool SaveSettings( SETTINGS Settings ) noexcept
{
	HKEY hk;
	LONG Result = RegOpenKeyEx( HKEY_CURRENT_USER, _T("Software\\JD Design\\ConCat"), 0, KEY_WRITE, &hk );

	if ( ERROR_SUCCESS == Result )
	{
		DWORD dwSize;

		dwSize = sizeof( SETTINGS );

		/* Now save the settings entry */
		Result = RegSetValueEx( hk, _T("Settings"), 0, REG_BINARY, (LPBYTE) &Settings, dwSize );

		/* Close the registry key */
		RegCloseKey( hk );
	}

	return Result == ERROR_SUCCESS;
}

static DWORD CreateAndSizeDestinationFiles( const SplitDlgData& td, vector<HandlePlusSize>& vFiles, size_t MaxNumCharsForNumericName, HWND hParentWnd, LONGLONG SrcFileSize ) noexcept
{
	DWORD dwError = ERROR_SUCCESS;

	// Set the collection size - that we're going to fill in
	vFiles.resize( td.NumFiles );

	for ( UINT indx = 0;
		(indx < vFiles.size()) && (dwError == ERROR_SUCCESS) && !g_bCancel;
		++indx )
	{
		auto& CurFile = vFiles[indx];

		/* Create the next file name */
		CreateNumericalName( td.sToFileName.c_str(), indx + 1, CurFile.szFName, MaxNumCharsForNumericName );

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
			CurFile.m_SizeToCopy = indx < td.NumFiles - 1 ?
				td.SplitSize :
				SrcFileSize - (td.NumFiles - 1) * td.SplitSize;

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

static unsigned __stdcall SplitControlThread_Reader( void * pParams )
{
	size_t CurrentBuffer = 0;

	/* Initially the buffers are signalled as empty */
	for ( auto &tb : g_TransBuffers )
	{
		tb.SetBufferEmptied();
		tb.InitBuffer( Granularity );
	}

	/* Start the split writer thread */
	UINT tid;
	::CHandle hThread( reinterpret_cast<HANDLE>( _beginthreadex( NULL, 0, CommonWriterThread, NULL, 0, &tid ) ) );

	// Get passed a reference (non-null pointer) to the thread data
	SplitThreadData& ptd = *(static_cast<SplitThreadData*>(pParams));

	DWORD dwError = ERROR_SUCCESS;

    try
    {

	if ( dwError == ERROR_SUCCESS )
	{
		size_t indx = 0;
		for ( vector<HandlePlusSize>::const_iterator it = ptd.vSplitFiles.begin();
				( it != ptd.vSplitFiles.end() ) && !g_bCancel && ( dwError == ERROR_SUCCESS );
				++it, ++indx )
		{
			_ASSERTE( it->m_fh.IsValid() );

			PostMessage( ptd.hParentWnd, UWM_UPDATE_PROGRESS, indx, 0 );

			{
				ULARGE_INTEGER SizeRemaining;
				SizeRemaining.QuadPart = it->m_SizeToCopy;

				while ( ( SizeRemaining.QuadPart > 0 ) && ( dwError == ERROR_SUCCESS ) && !g_bCancel )
				{
					/* Update the progress control */
					{
						//TODO Why * 0x8000 David?
					const int ProgPos = static_cast<int>( ( ( ptd.SrcFileSize - ptd.SrcRemaining ) * 0x8000)/ptd.SrcFileSize );

					PostMessage( ptd.hProgress, PBM_SETPOS, ProgPos, 0 );
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
						if ( ReadFile( ptd.hSrcFile, rtb.GetBuffer(), ThisBlockSize, &dwBytesRead, NULL ) )
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
						ptd.SrcRemaining -= ThisBlockSize;
					}
					else
					{
						dwError = rtb.WriteErrorValue;
					}
				}

				/* Do the source file name for the batch command line */
				if ( ptd.hBatchFile.IsValid() )
				{
					/* For the batch file, we only want the filename (no path) */
					TCHAR szName[_MAX_FNAME];
					TCHAR szExt[_MAX_EXT];
					TCHAR szFileName[_MAX_PATH];

					_tsplitpath_s( it->szFName, NULL, 0, NULL, 0, szName, _countof(szName), szExt, _countof(szExt) );
					_tmakepath_s( szFileName, NULL, NULL, szName, szExt );

					/* The filename needs quoting to cater for names with spaces */
					DWORD dwBytesWritten;
					WriteFile( ptd.hBatchFile, "\"", 1, &dwBytesWritten, NULL );

					CT2CA szAFN( szFileName );
					WriteFile( ptd.hBatchFile, szAFN, (DWORD) wcslen( szFileName ), &dwBytesWritten, NULL );

					WriteFile( ptd.hBatchFile, "\"", 1, &dwBytesWritten, NULL );

					if ( it != ptd.vSplitFiles.end()-1 )
					{
						WriteFile( ptd.hBatchFile, "+", 1, &dwBytesWritten, NULL );
					}
				}
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

	if ( !g_bCancel && ( dwError == ERROR_SUCCESS ) )
	{
		/* Write the target file name to the batch file */
		if ( ptd.hBatchFile.IsValid() )
		{
			DWORD dwBytesWritten;

			WriteFile( ptd.hBatchFile, " ", 1, &dwBytesWritten, NULL );

			/* For the batch file, we only want the filename (no path) */
			TCHAR szName[_MAX_FNAME];
			TCHAR szExt[_MAX_EXT];
			TCHAR szFileName[_MAX_PATH];

			// TODO: Modify to use C++ path facilities
			_tsplitpath_s( ptd.sSrcFileName.c_str(), NULL, 0, NULL, 0, szName, _countof(szName), szExt, _countof(szExt) );
			_tmakepath_s( szFileName, NULL, NULL, szName, szExt );

			/* Quote the name to cater for long file names with spaces */
			WriteFile( ptd.hBatchFile, "\"", 1, &dwBytesWritten, NULL );

			CT2CA szAFN( szFileName );
			WriteFile( ptd.hBatchFile, szAFN, (DWORD) wcslen( szFileName ), &dwBytesWritten, NULL );

			WriteFile( ptd.hBatchFile, "\"", 1, &dwBytesWritten, NULL );
		}

		/* Mark all the created split files to be retained */
		for ( auto & it : ptd.vSplitFiles )
		{
			it.m_DeleteOnDestroy = false;
		}

		MessageBeep( MB_OK );
//			ResMessageBox( hWnd, IDS_SPLIT_OK, szAltName, MB_OK | MB_ICONEXCLAMATION );
	}
	else
	{
		if ( ( dwError != -1 ) && ( dwError != ERROR_SUCCESS ) )
		{
			LPVOID lpMsgBuf;

			FormatMessage( 
				FORMAT_MESSAGE_ALLOCATE_BUFFER | 
				FORMAT_MESSAGE_FROM_SYSTEM | 
				FORMAT_MESSAGE_IGNORE_INSERTS,
				NULL,
				dwError,
				MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
				(LPTSTR) &lpMsgBuf,
				0,
				NULL );

			TCHAR szMsg[1024];
			TCHAR szFmt[256];

			LoadString( g_hResInst, IDS_SPLIT_FAILED, szFmt, _countof( szFmt ) );

			wsprintf( szMsg, szFmt, (LPCTSTR) lpMsgBuf );

			ThreadMessageBox( ptd.hParentWnd, szMsg, szAltName, MB_OK | MB_ICONERROR );

			// Free the buffer.
			LocalFree( lpMsgBuf );

			/* Set the cancel flag so that we don't close the dialog */
			InterlockedExchange( &g_bCancel, TRUE );
		}
	}
	}
	catch( std::bad_alloc & )
	{
		dwError = (DWORD) E_OUTOFMEMORY;
	}

	/* Tell the main UI thread that we've done (and to reset the UI) */
	PostMessage( ptd.hParentWnd, UWM_WORKER_FINISHED, 0, reinterpret_cast<LPARAM>( &ptd ) );

	return dwError;
}

static void SplitEm( HWND hWnd, SplitDlgData & osf, HWND hProgress, bool bCreateBatchFile, LPCTSTR pSrcFileName )
{
	/* On exit from the dialog the following are set:
		* pFiles[0] is the original file name
		* szToName is the destn file name
		* NumFiles is the number of files we SHOULD create
		* SplitSize is the size of each file
		*/

	/* Create the DOS batch file */
	CFileHandle hBatchFile;
	DWORD dwBytesWritten;

	if ( bCreateBatchFile )
	{
		/* Create the file */
		hBatchFile.Attach( CreateFile( osf.sBatchName.c_str(), GENERIC_WRITE, 0, NULL, CREATE_NEW,
						FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL ) );

		if ( !hBatchFile.IsValid() )
		{
			const DWORD dwErr = GetLastError();

			if ( dwErr == ERROR_FILE_EXISTS )
			{
				TCHAR szMsg[_MAX_PATH+200];
				TCHAR szFmt[100];
				LoadString( g_hResInst, IDS_OVERWRITE_BAT, szFmt, _countof( szFmt ) );
				wsprintf( szMsg, szFmt/*_T("The batch file \"%s\" already exists.\n\nDo you want to overwrite it?")*/,
							osf.sBatchName.c_str() );

				if ( IDYES == MessageBox( hWnd, szMsg, szAltName, MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2 ) )
				{
					hBatchFile.Attach( CreateFile( osf.sBatchName.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
									FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL ) );
				}
			}

			if ( !hBatchFile.IsValid() )
			{
				ResMessageBox( hWnd, IDS_FAIL_CREATE_BAT/*_T("Failed to create DOS batch file")*/, szAltName, MB_OK | MB_ICONINFORMATION );
			}
		}
	}


	/* Open the original file for reading */
	CFileHandle hSrcFile( CreateFile( pSrcFileName, GENERIC_READ, 0, NULL, OPEN_EXISTING,
					FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL ) );

	if ( hSrcFile.IsValid() )
	{
		const auto NumChars = NumberOfCharactersToDisplayValue( osf.NumFiles );

		/* Form the batch command */
		if ( hBatchFile.IsValid() )
		{
			WriteFile( hBatchFile, "copy /b ", sizeof("copy /b ")-1, &dwBytesWritten, NULL );
		}

		LARGE_INTEGER SrcFileSize;
		if ( GetFileSizeEx( hSrcFile, &SrcFileSize ) )
		{
			UINT64 SrcRemaining;
			SrcRemaining = SrcFileSize.QuadPart;

			/* Initialise the progress control range */
			SendMessage( hProgress, PBM_SETRANGE32, 0, 0x8000 );

			/* Fill in the data to pass to the thread */
			auto ptd = new SplitThreadData();
			ptd->sSrcFileName = osf.sSrcFileName;
			ptd->MaxNumCharsForNumericName = NumChars;
			ptd->hBatchFile.Attach( hBatchFile.Detach() );
			ptd->hParentWnd = hWnd;
			ptd->hProgress = hProgress;
			ptd->hSrcFile.Attach( hSrcFile.Detach() );
			ptd->SrcFileSize = SrcFileSize.QuadPart;
			ptd->SrcRemaining = SrcRemaining;

			auto dwError = CreateAndSizeDestinationFiles( osf, ptd->vSplitFiles, ptd->MaxNumCharsForNumericName, ptd->hParentWnd, ptd->SrcFileSize );


			/* Disable all the UI components while the thread runs */
			osf.vhDisabledCtrls = UIDisable( hWnd );

			/* Clear the cancel flag */
			InterlockedExchange( &g_bCancel, FALSE );

			/* Start the split worker thread */
			UINT tid;
			::CHandle hThread( reinterpret_cast<HANDLE>( _beginthreadex( NULL, 0, SplitControlThread_Reader, ptd, 0, &tid ) ) );
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
			MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
			(LPTSTR) &lpMsgBuf,
			0,
			NULL );

		TCHAR szMsg[1024];
		TCHAR szFmt[100];
		LoadString( g_hResInst, IDS_FAIL_OPEN, szFmt, _countof( szFmt ) );
		wsprintf( szMsg, szFmt, pSrcFileName, (LPCTSTR) lpMsgBuf );

		MessageBox( hWnd, szMsg, szAltName, MB_OK | MB_ICONERROR );

		// Free the buffer.
		LocalFree( lpMsgBuf );
	}
}

enum eDrives { CommonDriveSizes, LocalRemovableDriveSizes };

/* Array of sizes for the items in the split combo list - because the combo per-item data is limited to 32-bit) */
static vector<UINT64> g_vItemSizes;

static const struct PresetSizes
{
	const TCHAR * Description;
	unsigned __int64 Size;
}  PresetItemSizes[] = 
{
	//				_T("360 KB"), (354*1024)
	//				,_T("720 KB"), (713*1024)
	//				,_T("1.2 MB"), (1185*1024)
	//				,
	_T("1.4 MB"), (1423*1024)

	,_T("32 MB"), (32 * 1024*1024)
	,_T("64 MB"), (64 * 1024*1024)
	,_T("120 MB"), (120 * 1024*1024)
	,_T("128 MB"), (128 * 1024*1024)
	,_T("256 MB"), (256 * 1024*1024)
	,_T("512 MB"), (512 * 1024*1024)

	,_T("650 MB 74 min CD"), (333000 * 2048)
	,_T("700 MB 80 min CD"), (360000 * 2048)
	,_T("790 MB 90 min CD"), (405000 * 2048)
	,_T("870 MB 99 min CD"), (445500 * 2048)

	,_T("Max. FAT32 file (4 GB -1)"), 4294967295// (2^32-1) bytes

	,_T("4.7 GB DVD+R/RW"), (2295104i64 * 2048)
	,_T("4.7 GB DVD-R/RW"), (2298496i64 * 2048)

	,_T("25 GB Blu-ray single"), 25025314816
	,_T("50 GB Blu-ray dual"), 50050629632
};

/* CD sizes taken from http://www.faqs.org/faqs/cdrom/cd-recordable/part4/

Folks interested in "doing the math" should note that only 2048 bytes of
each 2352-byte sector is used for data on typical (Mode 1) discs.  The rest
is used for error correction and miscellaneous fields.  This is why you can
fit 747MB of audio WAV files onto a disc that holds 650MB of data.

It should also be noted that hard drive manufacturers don't measure
megabytes in the same way that CD-R and RAM manufacturers do.  The "MB" for
CD-Rs and RAM means 1024x1024, but for hard drives it means 1000x1000.
Keep this in mind when purchasing a hard drive that needs to hold an entire
CD.  A data CD that can hold 650 "RAM" MB of data holds about 682 "disk" MB
of data, which is why many CD-Rs are mislabeled as having a 680MB capacity.
(The notion of "unformatted capacity" is a nonsensical myth.)

  21 minutes ==  94,500 sectors == 184.6MB CD-ROM == 212.0MB CD-DA
  63 minutes == 283,500 sectors == 553.7MB CD-ROM == 635.9MB CD-DA
  74 minutes == 333,000 sectors == 650.3MB CD-ROM == 746.9MB CD-DA
  80 minutes == 360,000 sectors == 703.1MB CD-ROM == 807.4MB CD-DA
  90 minutes == 405,000 sectors == 791.0MB CD-ROM == 908.4MB CD-DA
  99 minutes == 445,500 sectors == 870.1MB CD-ROM == 999.3MB CD-DA
*/

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
			/* Populate the combo with the standard floppy sizes */
			for ( int item = 0; item < _countof( PresetItemSizes ); item++ )
			{
                /*const int indx = */ComboBox_AddString( hCB, PresetItemSizes[item].Description );
				g_vItemSizes.push_back( PresetItemSizes[item].Size );
			}
		}
		break;

	case LocalRemovableDriveSizes:
		/* Populate the combo box with the sizes of the removable devices on the system */
		/* This may take a noticeable time, so show the hourglass */
		{
			HCURSOR hOldCur = SetCursor( LoadCursor( NULL, IDC_WAIT ) );

			/* Now enumerate the removable drives on the OS */
			TCHAR szDrive[ sizeof("X:\\")*26 + 1 ];
			GetLogicalDriveStrings( _countof( szDrive ), szDrive );

			for ( LPCTSTR pDrive = szDrive; pDrive[0] != _T('\0'); pDrive += sizeof("X:\\") )
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
						szSize[3] = _T(' ');

						StrFormatByteSizeW( FreeSpace.QuadPart, &szSize[4], _countof( szSize )-4 );
						/*const int indx = */ComboBox_AddString( hCB, szSize );
						g_vItemSizes.push_back( FreeSpace.QuadPart );
//						ComboBox_SetItemData( hCB, indx, FreeSpace.LowPart );
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
		const UINT64 Size = g_vItemSizes[NumItems-1];

		if ( (Size ) == SizeValue )
		{
			/* A match - select this item */
			ComboBox_SetCurSel( hCB, NumItems-1 );
			break;
		}
	}

	/* If we haven't found a match */
	if ( NumItems <= 0 )
	{
		/* Set the size in the edit field of the combo box */

		/* Convert the size to text */
		TCHAR szValue[sizeof("18446744073709551615")];
		_ui64tot_s( SizeValue, szValue, _countof(szValue), 10 );
		/* Stick the text in the edit field */
		SetWindowText( hCB, szValue );
	}

	/* Enable the KB static text indicator for custom sizes */
	ShowWindow( GetDlgItem( hDlg, IDC_KB_IND ), NumItems < 0 ? SW_SHOWNA : SW_HIDE );
}

INT_PTR CALLBACK SplitDlg( HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam )
{
	/* Get this dialog's data class pointer - note this is not valid until first initialised in WM_INITDIALOG processing */
	SplitDlgData* psdd = reinterpret_cast<SplitDlgData*>(::GetWindowLongPtr( hDlg, DWLP_USER ));

	static CPerfTimer tim;

	switch ( message )
	{
	/* Is this the special message to do an inter-thread MessageBox? */
	case UWM_TMB:
		TMBHandler( hDlg, lParam );
		break;

	case UWM_WORKER_FINISHED:
		/* Calculate the time it's taken to do the split */
		{
			UINT64 PeriodInMS = tim.GetTimeToNow();
			PeriodInMS = tim.AsMilliSeconds( PeriodInMS );

			/* Do the common operations to re-enable the UI aspects that were disabled during this operation */
			{
				{
					const SplitThreadData * ptd = reinterpret_cast<SplitThreadData*>(lParam);
					UIEnable( *psdd, hDlg );
					delete ptd;
				}

				/* Match the corresponding enable of this item so things look matched if the operation is cancelled */
				EnableWindow( GetDlgItem( hDlg, IDC_SPLITTING ), false );
				EnableWindow( GetDlgItem( hDlg, IDC_FNUM ), false );
				EnableWindow( GetDlgItem( hDlg, IDC_CURRFILE ), false );
				PostMessage( GetDlgItem( hDlg, IDC_PROGRESS ), PBM_SETPOS, 0, 0 );
//				EnableWindow( GetDlgItem( hDlg, IDC_PROGRESS ), false );
			}

			if ( tim.InUse() )
			{
				CString sMsg;
				sMsg.Format( _T("The operation took: %I64u.%I64u seconds"), PeriodInMS / 1000,  PeriodInMS % 1000 );
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
				TCHAR szDrive[_MAX_DRIVE];
				TCHAR szDir[_MAX_DIR];

				_tsplitpath_s( psdd->sSrcFileName.c_str(), szDrive, _countof(szDrive), szDir, _countof(szDir), NULL, 0, NULL, 0);

				TCHAR szBatchName[_MAX_PATH];
				_tmakepath_s( szBatchName, szDrive, szDir, _T("concat"), _T("bat"));
				psdd->sBatchName = szBatchName;

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
				{
					/* Open the file for reading */
					CFileHandle hFile( CreateFile( psdd->sSrcFileName.c_str(), GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL));

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
						}
						else
						{
							// Never expect this to happen
							_ASSERT( false );
						}
					}
					else
					{
						ResMessageBox( hDlg, IDS_DONT_CONTINUE, szAltName, MB_OK | MB_ICONSTOP );
						psdd->FSize = 0;
					}
				}

				// Take a copy of the orginal file name
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
						GetLocaleInfo( LOCALE_USER_DEFAULT, LOCALE_STHOUSAND, &szThousandsSep[0], _countof( szThousandsSep ));

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
								if ( ch == _T('0') )
								{
									break;
								}
								else if ( isdigit( szBuffer[i] ) )
								{
									nf.Grouping = nf.Grouping*10 + (szBuffer[i] & 0x0f);
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
							_ui64tot_s( psdd->FSize, szValue, _countof(szValue), 10 );

							GetNumberFormat( LOCALE_USER_DEFAULT, 0, szValue, &nf, szFmtValue, _countof(szFmtValue) );
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
			Edit_LimitText( hComboEdit, sizeof("18446744073709551615")-1 );
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
		if ( ( wParam & 0x0FFF0 ) == SC_CLOSE )
		{
			/* If the default OK button is disabled, we must be doing the Split operation */
			if ( !IsWindowEnabled( GetDlgItem( hDlg, IDOK ) ) )
			{
                return TRUE;
			}
		}
		return FALSE;

	case WM_COMMAND:
		switch( LOWORD( wParam ) )
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

				//TODO: The timer should ideally be started after any UI prompts to overwrite existing files have been done
				/* Was the shift key down (to start timing)? */
				if ( bShiftKeyDown )
				{
					/* Save the time at the start of the operation */
					tim.SetStartTimeNow();
				}
				else
				{
					tim.SetNotInUse();
				}

				SplitEm( hDlg, *psdd, GetDlgItem( hDlg, IDC_PROGRESS ), bCreateBatchFile, psdd->sSrcFileName.c_str() );
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
//                    UINT64 Size = static_cast<UINT>( ComboBox_GetItemData( (HWND) lParam, SelItem ) );
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

				TCHAR szValue[sizeof("18446744073709551615")];

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
			return( FALSE );
		}
		break;

	/* Progress Message */
	case UWM_UPDATE_PROGRESS:
		{
			/* wParam is the index into the files array */
			const int indx = (int) wParam;

			SetDlgItemInt( hDlg, IDC_FNUM, indx+1, false );

			{
				TCHAR szPath[_MAX_PATH];

				/* Construct the numerical file we're doing now */
				const auto NumChars = NumberOfCharactersToDisplayValue( psdd->NumFiles );

				CreateNumericalName( psdd->sToFileName.c_str(), indx+1, szPath, NumChars );

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
		return( FALSE );
	}

	/* We've processed the message */
	return( TRUE );
}

void CShellExtension::SplitFiles( HWND hWnd ) noexcept
{
	CSelPlusReg spr{ m_SelItems, m_RegData };

	if ( IDOK == DialogBoxParam( g_hResInst, MAKEINTRESOURCE( IDD_SPLIT ), hWnd, SplitDlg, (LPARAM) (LPSTR) &spr ) )
	{
	}
}

void CShellExtension::ConCatenateFiles( HWND hWnd ) noexcept
{
	CSelPlusReg spr{ m_SelItems, m_RegData };

	/* Do the dialog box that presents the informations */
	if ( IDOK == DialogBoxParam( g_hResInst, MAKEINTRESOURCE( IDD_CONCAT ), hWnd, ConcatDlg, (LPARAM) (LPSTR) &spr ) )
	{
	}
}

//
// InvokeCommand is called when a menu item added by the extension handler
// is selected from a ZIP file's context menu.
//
// Input parameters:
//   lpcmi = Pointer to CMINVOKECOMMAND structure
//
// Returns:
//   HRESULT code signifying success or failure
//

STDMETHODIMP CShellExtension::InvokeCommand( LPCMINVOKECOMMANDINFO lpcmi ) noexcept
{
	//
	// Return an error code if we've been called programmatically or
	// lpcmi->lpVerb contains an invalid offset.
	//
	if ( HIWORD( lpcmi->lpVerb ) )
	{
		return ResultFromScode (E_FAIL);
	}

	const auto cmdId = LOWORD( lpcmi->lpVerb );

	if ( cmdId > IDOFFSET_CONCAT )
	{
		return ResultFromScode( E_INVALIDARG );
	}

	/* Must get the selected item data populated */
	const HRESULT hr = GetSelectedData();

	if ( hr == NOERROR )
	{
		// Execute the command Id
		switch ( cmdId )
		{
		case IDOFFSET_CONCAT:
			/* If we have 1 file selected, we're splitting, otherwise concatenating */
			if ( m_SelItems.size() == 1 )
			{
				SplitFiles( lpcmi->hwnd );
			}
			else
			{
				ConCatenateFiles( lpcmi->hwnd );
			}
			break;

		default:
			// Should be impossible!
			_ASSERT( false );
			break;
		}
	}

	return NOERROR;
}

//
// GetCommandString is called to retrieve a string of help text or a
// language-independent command string for an item added to the context
// menu.
//
// Input parameters:
//   idCmd    = 0-based offset of menu item identifier
//   uFlags   = Requested information type
//   reserved = Pointer to reserved value (do not use)
//   pszName  = Pointer to buffer to receive the string
//   cchMax   = Buffer size
//
// Returns:
//   HRESULT code signifying success or failure
//

STDMETHODIMP CShellExtension::GetCommandString( UINT_PTR idCmd, UINT uFlags, UINT FAR * /* reserved */, LPSTR pszName, UINT cchMax ) noexcept
{
	//
	// Return an error code if idCmd contains an invalid offset.
	//
	if ( idCmd > IDOFFSET_CONCAT )
	{
		return ResultFromScode( E_INVALIDARG );
	}

	//
	// Copy the requested string to the caller's buffer.
	//
	switch (idCmd)
	{
	case IDOFFSET_CONCAT:
		switch ( uFlags )
		{
		case GCS_VERBA:
		case GCS_VERBW:
			/* Shell wants us to return the language independent command name string that we can respond to */
			break;

		case GCS_VALIDATE:
			/* Shell wants us to validate the command??? */
			MessageBeep( MB_OK );
			break;

		// Explorer never seems to request this now, but 3'rd party shells may do
		case GCS_HELPTEXTW:
		case GCS_HELPTEXTA:
			{
				const UINT IdStr = m_SelItems.size() == 1 ? IDS_SPLIT_DESCRIPTION : IDS_CONCAT_DESCRIPTION;

				// Unicode or ANSI?
				if ( uFlags == GCS_HELPTEXTW )
				{
					// Unicode
					::LoadStringW( g_hResInst, IdStr, (LPWSTR) pszName, cchMax );
				}
				else
				{
					// ANSI
					::LoadStringA( g_hResInst, IdStr, pszName, cchMax );
				}
			}
			break;

		default:
			// What is it then?
			uFlags = uFlags;
			break;
		}
		break;

	default:
		// Should be impossible
		_ASSERT( false );
		break;
	}
	return NOERROR;
}

//
// Initialize is called by the shell to initialize a shell extension.
//
// Input parameters:
//   pidlFolder = Pointer to ID list identifying parent folder
//   lpdobj     = Pointer to IDataObject interface for selected object(s)
//   hKeyProgId = Registry key handle
//
// Returns:
//   HRESULT code signifying success or failure
//
static const FORMATETC g_fe = { CF_HDROP, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };

STDMETHODIMP CShellExtension::Initialize( LPCITEMIDLIST /* pidlFolder */, LPDATAOBJECT lpdobj, HKEY /* hKeyProgID */ ) noexcept
{
	HRESULT hr = E_FAIL;

	//
	// Fail the call if lpdobj is NULL.
	//
	if ( lpdobj != NULL )
	{
		// Get the user registration data.
		this->m_RegData = GetMyRegistrationFromTheRegistry( szRegistryKey );

		STGMEDIUM medium;

		//
		// Render the data referenced by the IDataObject pointer to an HGLOBAL
		// storage medium in CF_HDROP format.
		//
		hr = lpdobj->GetData( const_cast<FORMATETC *>( &g_fe ), &medium );

		if ( SUCCEEDED(hr) )
		{
			// Keep a copy (reference counted) until we're finished with it.
			m_SelData = lpdobj;

			ReleaseStgMedium (&medium);
		}

		}

	return hr;
}

HRESULT CShellExtension::GetSelectedData() noexcept
{
	HRESULT hr;

	{
//		static const FORMATETC fe = { CF_HDROP, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
		STGMEDIUM medium;

		// Render the data referenced by the IDataObject pointer to an HGLOBAL
		// storage medium in CF_HDROP format.
		hr = m_SelData->GetData( const_cast<FORMATETC *>( &g_fe ), &medium );

		if ( !FAILED( hr ) )
		{
			const UINT NumFiles = DragQueryFile( (HDROP) medium.hGlobal, 0xFFFFFFFF, NULL, 0 );

			/* Delete any prior saved items storage */
			m_SelItems.clear();

			try
			{
				/* Pre-allocate the storage for the items */
				m_SelItems.reserve( NumFiles );	

				/* Loop for each file name we have been passed */
				UINT fno;
				for ( fno = 0; fno < NumFiles; fno++ )
				{
					TCHAR szFName[_MAX_PATH];

					/* How long a string is it? */
					const int NoChars = DragQueryFile( (HDROP) medium.hGlobal, fno, szFName, _MAX_PATH );

					if ( 0 != NoChars )
					{
						/* Add the item to the vector */
						m_SelItems.push_back( szFName );
					}
				}

				/* Have we successfully finished all the files? */
				if ( ( fno == NumFiles ) && ( m_SelItems.size() == NumFiles ) )
				{
					/* Great - we can proceed normally */
					hr = NOERROR;
				}
				else
				{
					/* Problem! If we haven't got all the files then it's
					 * pointless continuing to do those we have got as this
					 * would confuse the user.
					 * We now need to free off any memory we have allocated!
					 * First do the local allocations, then the global array
					 */
// No point doing this now?					TidyUp();

					hr = E_OUTOFMEMORY;
				}
			}
			catch( ... )
			{
				// Assume the cause is insufficient memory
				hr = E_OUTOFMEMORY;
			}

			ReleaseStgMedium (&medium);
		}
	}

	return hr;
}

size_t CShellExtension::GetNumSelectedItems() noexcept
{
//	static const FORMATETC fe = { CF_HDROP, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
	STGMEDIUM medium;

	// Render the data referenced by the IDataObject pointer to an HGLOBAL
	// storage medium in CF_HDROP format.
	HRESULT hr;
	hr = m_SelData->GetData( const_cast<FORMATETC *>( &g_fe ), &medium );

	size_t NumItems;

	if ( hr == S_OK )
	{
		NumItems = DragQueryFile( (HDROP) medium.hGlobal, 0xFFFFFFFF, NULL, 0 );

		ReleaseStgMedium (&medium);
	}
	else
	{
		NumItems = 0;
	}

	return NumItems;
}

