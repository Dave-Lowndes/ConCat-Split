#pragma once
#include <Windows.h>

// Should be 32-bit aligned.
extern __declspec(align(4))
// Signals cancellation of the concat/split operation.
LONG volatile g_bCancel;

// Window message to call the message box in the main UI thread
//static const UINT g_MsgNum = RegisterWindowMessage( _T("7B9ECF2C-15F1-4f90-A351-46D87701C12C") );
#define UWM_TMB (WM_APP+1)

// This message updates the UI for the operation
#define UWM_UPDATE_PROGRESS (WM_APP+2)

#define UWM_WORKER_FINISHED (WM_APP+3)

constexpr inline TCHAR szConcatAppName[]{ _T( "Concat" ) };
constexpr inline TCHAR szSplitAppName[]{ _T( "Split" ) };

inline BOOL bREGISTERED{FALSE};
constexpr inline TCHAR szRegistryKey[]{ _T( "Software\\JD Design\\ConCat" ) };
