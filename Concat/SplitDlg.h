#pragma once
#include <afxwin.h>
#include <string>

#include "SplitThreadData.h"
#include "resource.h"

class CSplitDlg final : public CommonDlg
{
	//	DECLARE_DYNAMIC( CSplitDlg )

public:
	CSplitDlg( const CSelPlusReg &spr, CWnd * pParentWnd ) :
					CommonDlg( IDD, spr.m_RegData, pParentWnd ),
					m_sSrcFileName{ spr.m_Files.at( 0 ) }
	{
		//{{AFX_DATA_INIT(CSplitDlg)
		//}}AFX_DATA_INIT
	}

private:
	//{{AFX_DATA(CSplitDlg)
	enum { IDD = IDD_SPLIT };
	//}}AFX_DATA

	afx_msg void OnClickedBatchNameChange();
	afx_msg void OnClickedChangeDestn();
	virtual void OnOK();
	afx_msg LRESULT OnWorkerFinished( WPARAM, LPARAM );
	void OnRadioButtonsClicked( UINT nID );
	afx_msg void OnSelchangeSizeCombo();
	afx_msg void OnEditchangeSizeCombo();
	afx_msg LRESULT OnUpdateProgress( WPARAM, LPARAM );

	//{{AFX_VIRTUAL(CSplitDlg)
	virtual BOOL OnInitDialog();
	//}}AFX_VIRTUAL

	UINT m_NumFiles = 0;				// The number of files that will be created
	UINT64 m_FSize = 0;				// Original file's size
	UINT64 m_SplitSize = 0;			// Size of the files to create
	std::wstring m_sBatchName;
	std::wstring m_sToFileName;			// Initially this is a copy of the source file name, but the user can alter it
	const std::wstring& m_sSrcFileName;	// R-O ref to the source file path+name

	void SplitEm( HWND hWnd, HWND hProgress, bool bCreateBatchFile, bool bStartTiming );
	DWORD CreateAndSizeDestinationFiles( std::vector<HandlePlusSize>& vFiles, size_t NumNumericChars, LONGLONG SrcFileSize ) noexcept;
	virtual void DisplayOperationCompleteMessage();

public:
	//{{AFX_MSG(CSplitDlg)
	//}}AFX_MSG

	DECLARE_MESSAGE_MAP()
};

