#pragma once
#include <vector>
#include <string>
#include <optional>

#include <RegDataV3.h>

// Rename (or eliminate) this to something like STRINGVEC - code may be more understandable?
typedef std::vector<std::wstring> SELITEMS;

// Encapsulates the data that is passed down to the dialog functions
class CSelPlusReg
{
public:
	// Prevent inadvertent copying
	CSelPlusReg() = delete;
	CSelPlusReg( const CSelPlusReg& ) = delete;
	CSelPlusReg operator=( const CSelPlusReg& ) = delete;

	CSelPlusReg( const SELITEMS& Files, const std::optional<CMyRegData>& RegData ) noexcept : m_Files{ Files }, m_RegData{ RegData }
	{
	}

	//private:
	const SELITEMS& m_Files;			// Reference to the collection of file names that were selected in Explorer (entry 0 in split is the original file)
	const std::optional<CMyRegData>& m_RegData;	// Reference to the registration data
};

