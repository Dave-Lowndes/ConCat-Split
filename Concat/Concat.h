#include <atlbase.h>
#include <vector>
#include <optional>
#include "RegDataV3.h"

#if defined(_M_IA64)
#pragma comment(linker,"/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='ia64' publicKeyToken='6595b64144ccf1df' language='*'\"")
#else
#if defined(_M_X64)
#pragma comment(linker,"/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='amd64' publicKeyToken='6595b64144ccf1df' language='*'\"")
#else
#pragma comment(linker,"/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='x86' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif
#endif

/* Menu item ID's */
#define IDOFFSET_CONCAT	0x00

#define GUID_STR "{B2D4ADE0-0C85-11cf-872F-444553540000}"

DEFINE_GUID(CLSID_ShellExtension, 
0xb2d4ade0, 0xc85, 0x11cf, 0x87, 0x2f, 0x44, 0x45, 0x53, 0x54, 0x0, 0x0);

//
// CClassFactory defines a shell extension class factory object.
//
class CClassFactory : public IClassFactory
{
protected:
    ULONG   m_cRef;         // Object reference count
    
public:
    CClassFactory () noexcept;
    ~CClassFactory ();
        
    // IUnknown methods
    STDMETHODIMP            QueryInterface (REFIID, LPVOID FAR *) noexcept override;
    STDMETHODIMP_(ULONG)    AddRef () noexcept override;
    STDMETHODIMP_(ULONG)    Release () noexcept override;
    
    // IClassFactory methods
    STDMETHODIMP    CreateInstance (LPUNKNOWN, REFIID, LPVOID FAR *) noexcept override;
    STDMETHODIMP    LockServer (BOOL) noexcept override;
};

// Rename (or eliminate) this to something like STRINGVEC - code may be more understandable?
typedef std::vector<std::wstring> SELITEMS;

//
// CShellExtension defines a context menu shell extension object.
//
class CShellExtension : public IContextMenu, IShellExtInit
{
private:
    ULONG m_cRef;	// Object reference count
	SELITEMS m_SelItems;	// The selected files and folders
	HRESULT GetSelectedData() noexcept;	// Converts the clipboard selection format to the usable form
	size_t GetNumSelectedItems() noexcept;	// Returns the number of items selected in Explorer
    // The registration information
    std::optional<CMyRegData> m_RegData;

public:
    CShellExtension () noexcept;
    ~CShellExtension ();
    
    // IUnknown methods
    STDMETHODIMP            QueryInterface (REFIID, LPVOID FAR *) noexcept override;
    STDMETHODIMP_(ULONG)    AddRef () noexcept override;
    STDMETHODIMP_(ULONG)    Release () noexcept override;
    
    // IContextMenu methods
    STDMETHODIMP QueryContextMenu( HMENU hMenu, UINT indexMenu, UINT idCmdFirst, UINT idCmdLast, UINT uFlags ) noexcept override;

    STDMETHODIMP InvokeCommand( LPCMINVOKECOMMANDINFO lpcmi ) noexcept override;

    STDMETHODIMP GetCommandString( UINT_PTR idCmd, UINT uFlags, UINT FAR *reserved, LPSTR pszName, UINT cchMax ) noexcept override;

    // IShellExtInit method
    STDMETHODIMP Initialize (LPCITEMIDLIST pidlFolder, LPDATAOBJECT lpdobj, HKEY hKeyProgID) noexcept override;

private:
	HBITMAP m_hSplitBitmap;
	HBITMAP m_hConcatBitmap;
	void ConCatenateFiles( HWND hWnd ) noexcept;
	void SplitFiles( HWND hWnd ) noexcept;

	CComPtr<IDataObject> m_SelData;
};
