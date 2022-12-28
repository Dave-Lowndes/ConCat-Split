#pragma once
#include "CommonThreadData.h"
#include "CSelPlusReg.h"

class ConcatThreadData final : public CommonThreadData
{
public:
	// Prevent inadvertent copying
	ConcatThreadData() = delete;
	ConcatThreadData( const ConcatThreadData& ) = delete;
	ConcatThreadData operator=( const ConcatThreadData& ) = delete;

	ConcatThreadData( const VEC_FILENAMES& files, LPCTSTR pDestnName, HWND hProg, HWND hParent ) : Files{ files }, sToName{ pDestnName }, CommonThreadData( hProg, hParent )
	{
	}

	const VEC_FILENAMES& Files;	// Reference to the (re-arranged) collection of file names that are being joined
	const std::wstring sToName;	// The final copy destination file name
};

extern bool FileExistsAndWritable( LPCTSTR pFileName ) noexcept;

