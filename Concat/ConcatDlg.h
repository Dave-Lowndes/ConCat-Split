#pragma once
#include <afxwin.h>
#include "CommonDlg.h"
#include "resource.h"
#include "CSelPlusReg.h"

class CConcatDlg : public CommonDlg
{
public:
	CConcatDlg( const CSelPlusReg& spr, CWnd * pParentWnd ) :
		CommonDlg( IDD, spr.m_RegData, pParentWnd )
	{
		// Take a copy of the file names. A full copy is taken as the collection can be sorted/re-arranged.
		m_Files = spr.m_Files;
	}

public:
	DECLARE_MESSAGE_MAP()

private:
	enum { IDD = IDD_CONCAT };

	void ConcatEm( HWND hProgress, LPCTSTR szToName );
	VEC_FILENAMES m_Files;		// The collection of file names that are being joined

	virtual BOOL OnInitDialog();
	afx_msg LRESULT OnWorkerFinished( WPARAM, LPARAM );
	afx_msg LRESULT OnUpdateProgress( WPARAM, LPARAM );
	afx_msg void OnUpDown( UINT nID );
	afx_msg void OnDrawItem( int nIDCtl, LPDRAWITEMSTRUCT lpDrawItemStruct );
	afx_msg void OnSelchangeCopyList();
	virtual void OnOK();
	virtual void DisplayOperationCompleteMessage();
};
