//
// CONCAT is a Windows 95 shell extension that takes a multiple selection in an Explorer Window & modifies the
// context menus displayed to provide access to it's concatenation facility.
//

//#define SIDEBYSIDE_COMMONCONTROLS 1 
#include <afxmt.h>
#include <ShellAPI.h>
#include <initguid.h>
#include <shlobj.h>

#include "resource.h"
#include "RegEnc.h"
#include "RegKeyRegistryFuncs.h"
#include "CommonDlg.h"
#include "concat.h"
#include "SplitDlg.h"
#include "ConcatDlg.h"

//
// Global variables
//
static LONG	volatile g_cRefThisDll = 0;          // Reference count for this DLL
TCHAR szConcatAppName[] = _T("Concat");
/*const */TCHAR szSplitAppName[] = _T("Split");

// Should be 32-bit aligned.
__declspec(align(4))
// Signals cancellation of the concat/split operation.
LONG volatile g_bCancel;

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
	auto hInst = AfxGetResourceHandle();
	m_hSplitBitmap = LoadBitmap( hInst, MAKEINTRESOURCE( IDB_SPLIT_MENU_BMP ) );
	m_hConcatBitmap = LoadBitmap( hInst, MAKEINTRESOURCE( IDB_CONCAT_MENU_BMP ) );
	InterlockedIncrement( &g_cRefThisDll );

	/* Try to load the language resource DLL */
	TCHAR szPath[_MAX_PATH];
	DWORD Len = GetModuleFileName( AfxGetInstanceHandle(), szPath, _countof(szPath));

	/* Truncate the ConCat.dll part, and append the language DLL name instead */
	Len -= sizeof("ConCat.dll") - 1;
	lstrcpy( &szPath[ Len ], _T("ConCat.lang") );

	HMODULE hMod = LoadLibrary( szPath );
	if ( hMod != NULL )
	{
		AfxSetResourceHandle( hMod );
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
//		g_hResInst = g_hInstance;
	}
}

CShellExtension::~CShellExtension ()
{
	InterlockedDecrement( &g_cRefThisDll );

	/* Unload the language DLL */
	auto hResInst = AfxGetResourceHandle();
	auto hInstance = AfxGetInstanceHandle();

	if ( hResInst != hInstance )
	{
		FreeLibrary( hResInst );
		/* Reset to original "no separate resource DLL" */
		AfxSetResourceHandle( hInstance );
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

		const size_t NumSel = GetNumSelectedItems();
		/* Single file selected == split, multiple == concat */
		const auto [hBmp, StringID ] = ( NumSel == 1 ) ?
									std::make_tuple( m_hSplitBitmap, IDS_SPLIT_MENU ) :
									std::make_tuple( m_hConcatBitmap, IDS_CONCAT_MENU );

		CString sMenuText( MAKEINTRESOURCE( StringID ) );

		::InsertMenu( hMenu, indexMenu, MF_STRING | MF_BYPOSITION, idCmdFirst + IDOFFSET_CONCAT, sMenuText );

		/* Some people take a purist view to menus having bitmaps, so it's now optional */
		bool bDisplayBitmap = true;
		{
			CRegKey hKey;
			LONG res = hKey.Open( HKEY_CURRENT_USER, szRegistryKey, KEY_READ );

			if ( res == ERROR_SUCCESS )
			{
				DWORD Val;

				/* Read the value */
				res = hKey.QueryDWORDValue( _T( "Options" ), Val );
				
				if ( ERROR_SUCCESS == res )
				{
					/* Copy the settings back to the caller */
					bDisplayBitmap = Val & 0x01;
				}
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

void CShellExtension::SplitFiles( HWND /*hWnd*/ ) noexcept
{
	CSelPlusReg spr{ m_SelItems, m_RegData };

	// The parent window passed down to here isn't the top level Explorer
	// window - which is what gets used if I don't pass this as the parent.
	// Although that seems OK, I think it's best to do the right thing.
	//CWnd Parent;
	//Parent.Attach( hWnd );

	CSplitDlg dlg( spr, /*&Parent*/NULL );
	dlg.DoModal();

	// Prevent destructor destroying the parent window
	//Parent.Detach();
}

void CShellExtension::ConCatenateFiles( HWND /*hWnd*/ ) noexcept
{
	CSelPlusReg spr{ m_SelItems, m_RegData };

	// See the same construct above^
	//CWnd Parent;
	//Parent.Attach( hWnd );

	/* Do the dialog box that presents the informations */
	CConcatDlg dlg( spr, /*&Parent*/NULL );
	dlg.DoModal();

	// Prevent destructor destroying the parent window
	//Parent.Detach();
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
	AFX_MANAGE_STATE( AfxGetStaticModuleState() );

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
					::LoadStringW( AfxGetResourceHandle(), IdStr, (LPWSTR) pszName, cchMax );
				}
				else
				{
					// ANSI
					::LoadStringA( AfxGetResourceHandle(), IdStr, pszName, cchMax );
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

CApp theApp;

BEGIN_MESSAGE_MAP( CApp, CWinApp )
	//{{AFX_MSG_MAP(CApp)
		// NOTE - the ClassWizard will add and remove mapping macros here.
		//    DO NOT EDIT what you see in these blocks of generated code!
	//}}AFX_MSG_MAP
END_MESSAGE_MAP()

BOOL CApp::InitInstance()
{
	CWinApp::InitInstance();

	/* Create the ThreadMessageBox event */
	g_hTMBEvent = CreateEvent( NULL, false, false, NULL );

	return TRUE;
}

int CApp::ExitInstance()
{
	CloseHandle( g_hTMBEvent );
	g_hTMBEvent = NULL;

	return CWinApp::ExitInstance();
}


