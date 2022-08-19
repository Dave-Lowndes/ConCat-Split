#pragma once
#include <optional>
#include <vector>
#include <afxwin.h>
#include <span>

#include "RegDataV3.h"

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

class CommonDlg : public CDialog
{
public:
	// Prevent inadvertent copying
	CommonDlg() = delete;
	CommonDlg( const CommonDlg& ) = delete;
	CommonDlg operator=( const CommonDlg& ) = delete;

	CommonDlg( UINT DlgId, const std::optional<CMyRegData>& RegData, CWnd * pParentWnd ) noexcept : m_RegData{ RegData }, CDialog( DlgId, pParentWnd )
	{
	}
protected:
	void UIDisable() noexcept;
	afx_msg void OnSysCommand( UINT nID, LPARAM lParam );
	void OnWorkerFinished( std::span<const int> CtrlsToDisable, LPCTSTR szMsgBoxCaption ) noexcept;
	LRESULT OnUpdateProgress( int FileNum, LPCTSTR szPath );
	virtual void DisplayOperationCompleteMessage() = 0;
private:
	std::vector<HWND> vhDisabledCtrls;	// The dialog controls that need re-enabling at the end of the operation
	void UIEnable() noexcept;
	afx_msg LRESULT OnThreadMessageBox( WPARAM, LPARAM );
	afx_msg void OnClickedAbout();
	virtual void OnCancel();
	DECLARE_MESSAGE_MAP()

	const std::optional<CMyRegData>& m_RegData;	// Reference to the registration data
	CPerfTimer m_tim;	// Used to time the operations
};

extern bool bUpdateChecked;
extern HANDLE g_hTMBEvent;
extern void TMBHandler( HWND hDlg, LPARAM lParam ) noexcept;

extern constexpr size_t NumberOfCharactersToDisplayValue( size_t Value ) noexcept;
extern void AboutHandler( HWND hDlg, const std::optional<CMyRegData>& RegData ) noexcept;
extern int ResMessageBox( HWND hWnd, int ResId, LPCTSTR pCaption, const int Flags ) noexcept;
extern int ThreadMessageBox( HWND hParent, LPCTSTR lpText, LPCTSTR lpCaption, UINT Type ) noexcept;
