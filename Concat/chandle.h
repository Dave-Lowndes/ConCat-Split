#pragma once

template<HANDLE t_hNullValue>
class CHandleT
{
public:
	HANDLE m_hHandle;	//-V122

public:
	CHandleT() noexcept : m_hHandle(t_hNullValue)
	{ }

	CHandleT(const CHandleT& Handle) :
	m_hHandle(Handle.Duplicate(::GetCurrentProcess(), 
		DUPLICATE_SAME_ACCESS, FALSE, DUPLICATE_SAME_ACCESS))
	{ }

	explicit CHandleT(HANDLE Handle)  noexcept :
	m_hHandle(Handle)
	{ }

	~CHandleT()
	{
		if (IsValid())
		{
			Close();
		}
	}

	CHandleT& operator=(const CHandleT& Handle)
	{
		if (this != &Handle)
		{
			Close();
			m_hHandle = Handle.Duplicate(::GetCurrentProcess(), 
				DUPLICATE_SAME_ACCESS, FALSE, DUPLICATE_SAME_ACCESS);
		}

		return (*this);
	}

	operator HANDLE() const noexcept
	{
		return m_hHandle;
	}

public:
	bool IsValid() const noexcept
	{
		return (m_hHandle != t_hNullValue);
	}

	void Attach(HANDLE Handle) noexcept
	{
		ATLASSERT( !IsValid() );
		m_hHandle = Handle;
	}

	HANDLE Detach() noexcept
	{
		HANDLE Handle = m_hHandle;
		m_hHandle = t_hNullValue;
		return Handle;
	}

	void Close() noexcept
	{
		if (IsValid())
		{
			ATLVERIFY(::CloseHandle(m_hHandle) != FALSE);
			m_hHandle = t_hNullValue;
		}
	}

#if (_WIN32_WINNT >= 0x0400)
	DWORD GetInformation() const
	{
		ATLASSERT(IsValid());
		DWORD dwHandleInfo = 0;
		ATLVERIFY(::GetHandleInformation(m_hHandle, &dwHandleInfo) != FALSE);
		return dwHandleInfo;
	}

	bool IsFlagInherit() const
	{
		return (GetInformation() & HANDLE_FLAG_INHERIT) != 0;
	}

	bool IsFlagProtectFromClose() const
	{
		return (GetInformation() & HANDLE_FLAG_PROTECT_FROM_CLOSE) != 0;
	}

	void SetInformation(DWORD dwMask, DWORD dwFlags)
	{
		ATLASSERT(IsValid());
		ATLVERIFY(::SetHandleInformation(m_hHandle, dwMask, dwFlags) != FALSE);
	}

	void SetFlagInherit(bool bFlagInherit)
	{
		SetInformation(HANDLE_FLAG_INHERIT, (bFlagInherit) ? HANDLE_FLAG_INHERIT : 0);
	}

	void SetFlagProtectFromClose(bool bFlagProtectFromClose)
	{
		SetInformation(HANDLE_FLAG_PROTECT_FROM_CLOSE, 
			(bFlagProtectFromClose) ? HANDLE_FLAG_PROTECT_FROM_CLOSE : 0);
	}
#endif // (_WIN32_WINNT >= 0x0400)


	HANDLE Duplicate(HANDLE hTargetProcess, DWORD dwDesiredAccess, 
		BOOL bInheritHandle = FALSE, DWORD dwOptions = 0) const
	{
		HANDLE hNewHandle = t_hNullValue;
		if ( IsValid() )
		{
			ATLVERIFY(::DuplicateHandle(::GetCurrentProcess(), m_hHandle, hTargetProcess, 
				&hNewHandle, dwDesiredAccess, bInheritHandle, dwOptions) != FALSE);
		}
		return hNewHandle;
	}
};

typedef CHandleT<nullptr> CHandle;
typedef CHandleT<INVALID_HANDLE_VALUE> CFileHandle;
