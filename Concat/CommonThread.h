#pragma once
#include <afxmt.h>

extern unsigned __stdcall CommonWriterThread( void* /*pParams*/ );
extern /*const */DWORD Granularity;
extern void InitializeTransferBuffers();