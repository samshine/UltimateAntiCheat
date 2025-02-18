//By AlSch092 @github
#include "AntiDebugger.hpp"

/*
	StartAntiDebugThread - creates a new thread on `CheckForDebugger`
*/
void Debugger::AntiDebug::StartAntiDebugThread()
{
	auto settings = this->GetSettings().lock();

	if (settings)
	{
		if (!settings->bUseAntiDebugging)
		{
			Logger::logf(Info, "Anti-Debugger was disabled in settings, debugging will be allowed");
			return;
		}
	}
	else
	{
		Logger::logf(Warning, "Settings were NULL or failed to lock weak_ptr<Settings> @ Debugger::AntiDebug::StartAntiDebugThread");
	}

	this->DetectionThread = make_unique<Thread>((LPTHREAD_START_ROUTINE)Debugger::AntiDebug::CheckForDebugger, (LPVOID)this, true, false);

	Logger::logf(Info, "Created Debugger detection thread with Id: %d", this->DetectionThread->GetId());
}

/*
	CheckForDebugger - Thread function which loops and checks for the presense of debuggers
*/
void Debugger::AntiDebug::CheckForDebugger(LPVOID AD)
{
	if (AD == nullptr)
	{
		Logger::logf(Err, "AntiDbg class was NULL @ CheckForDebugger");
		return;
	}

	Debugger::AntiDebug* AntiDbg = reinterpret_cast<Debugger::AntiDebug*>(AD);

	Logger::logf(Info, "STARTED Debugger detection thread");

	bool MonitoringDebugger = true;

	const int MonitorLoopDelayMS = 1000;

	while (MonitoringDebugger)
	{
		if (AntiDbg == NULL)
		{
			Logger::logf(Err, "AntiDbg class was NULL @ CheckForDebugger");
			return;
		}

		if (AntiDbg->DetectionThread->IsShutdownSignalled())
		{
			Logger::logf(Info, "Shutting down Debugger detection thread with Id: %d", AntiDbg->DetectionThread->GetId());
			return; //exit thread
		}

		HANDLE CheckHardwareRegistersThread = CreateThread(0, 0, (LPTHREAD_START_ROUTINE)_IsHardwareDebuggerPresent, (LPVOID)AntiDbg, 0, 0);

		if (CheckHardwareRegistersThread == INVALID_HANDLE_VALUE || CheckHardwareRegistersThread == NULL)
		{
			Logger::logf(Warning, "Failed to create new thread to call _IsHardwareDebuggerPresent: %d", GetLastError());
		}
		else
		{
			WaitForSingleObject(CheckHardwareRegistersThread, 2000); //Shouldn't take more than 2000ms to call _IsHardwareDebuggerPresent
		}

		if (AntiDbg->RunDetectionFunctions())
		{
			Logger::logf(Info, "Atleast one debugger detection function caught a debugger!"); //optionally, iterate over DetectedMethods list
		}

		Sleep(MonitorLoopDelayMS);
	}
}


/*
	_IsHardwareDebuggerPresent - suspends threads + Checks debug registers for Dr0-3,6,7 being > 0
*/
void Debugger::AntiDebug::_IsHardwareDebuggerPresent(LPVOID AD)
{
	if (AD == nullptr)
	{
		Logger::logf(Err, "AntiDbg class was NULL @ _IsHardwareDebuggerPresent");
		return;
	}

	Debugger::AntiDebug* AntiDbg = reinterpret_cast<Debugger::AntiDebug*>(AD);

	THREADENTRY32 te32;
	te32.dwSize = sizeof(THREADENTRY32);

	HANDLE hThreadSnap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
	if (hThreadSnap == INVALID_HANDLE_VALUE)
	{
		Logger::logf(Err, "Error: unable to create toolhelp snapshot: %d\n", GetLastError());
		return;
	}

	DWORD currentProcessID = GetCurrentProcessId();

	if (Thread32First(hThreadSnap, &te32))
	{
		do
		{
			if (te32.th32OwnerProcessID == currentProcessID && te32.th32ThreadID != GetCurrentThreadId())
			{
				HANDLE hThread = OpenThread(THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME | THREAD_QUERY_INFORMATION, FALSE, te32.th32ThreadID);

				if (hThread == NULL)
				{
					Logger::logf(Warning, "Error: unable to OpenThread on thread with id %d\n", te32.th32ThreadID);
					continue;
				}

				SuspendThread(hThread);

				CONTEXT context;
				context.ContextFlags = CONTEXT_DEBUG_REGISTERS;

				if (GetThreadContext(hThread, &context))
				{
					if (context.Dr0 || context.Dr1 || context.Dr2 || context.Dr3 || context.Dr6 || context.Dr7)
					{
						Logger::logf(Detection, "Found at least one debug register enabled (hardware debugging)");
						ResumeThread(hThread);

						if (!AntiDbg->Flag(Detections::HARDWARE_REGISTERS))
						{ //optionally take further action, `Flag` will already log a warning
						}

						CloseHandle(hThreadSnap);
						CloseHandle(hThread);
						return;
					}
				}
				else
				{
					Logger::logf(Err, "GetThreadContext failed with: %d", GetLastError());
					ResumeThread(hThread);
					CloseHandle(hThread);
					continue;
				}

				ResumeThread(hThread);
				CloseHandle(hThread);
			}
		} while (Thread32Next(hThreadSnap, &te32));
	}
	else
	{
		Logger::logf(Err, "Thread32First Failed: %d\n", GetLastError());
		return;
	}

	CloseHandle(hThreadSnap);
	return;
}

/*
	AddDetectedFlag - adds `flag` to DebuggerMethodsDetected after checking for duplicate entry
	returns FALSE if `flag` is duplicate entry
*/
inline bool Debugger::AntiDebug::AddDetectedFlag(Detections flag)
{
	bool isDuplicate = false;

	for (Detections f : this->DebuggerMethodsDetected)
	{
		if (f == flag)
		{
			isDuplicate = true;
		}
	}

	if (!isDuplicate)
		this->DebuggerMethodsDetected.push_back(flag);

	return isDuplicate;
}

/*
	Flag - adds `flag` to detected methods list and tells server we've caught a debugger
	returns false on error, true on success
*/
bool Debugger::AntiDebug::Flag(Debugger::Detections flag)
{
	bool wasDuplicate = AddDetectedFlag(flag);

	if (wasDuplicate)
		return true; //function still succeeds even though it was duplicate (no error)

	if (this->GetNetClient() != nullptr)
	{
		if (this->GetNetClient()->FlagCheater(DetectionFlags::DEBUGGER) != Error::OK) //the type of debugger doesn't really matter at the server-side, we can optionally modify the outbound packet to make debugger detections more granular
		{
			Logger::logf(Err, "Failed to notify server of caught debugger status");
			return false;
		}
	}
	else
	{
		Logger::logf(Err, "NetClient was NULL @ AntiDebug::Flag");
		return false;
	}

	return true;
}

/*
	PreventWindowsDebuggers - experimental, patches over some common debugging routines
*/
bool Debugger::AntiDebug::PreventWindowsDebuggers()
{
	HMODULE ntdll = GetModuleHandleA("ntdll.dll");

	if (!ntdll)
	{
		Logger::logf(Err, "Failed to find ntdll.dll @ AntiDebug::PreventWindowsDebuggers");
		return false;
	}

	DWORD dwOldProt = 0;

	UINT64 DbgBreakpoint_Address = (UINT64)GetProcAddress(ntdll, "DbgBreakPoint");
	UINT64 DbgUiRemoteBreakin_Address = (UINT64)GetProcAddress(ntdll, "DbgUiRemoteBreakin");

	if (DbgBreakpoint_Address)
	{
		if (VirtualProtect((LPVOID)DbgBreakpoint_Address, 1, PAGE_EXECUTE_READWRITE, &dwOldProt))
		{
			__try
			{
				*(BYTE*)DbgBreakpoint_Address = 0xC3;
			}
			__except(EXCEPTION_EXECUTE_HANDLER)
			{
				Logger::logf(Err, "Failed to patch over DbgBreakpoint @ AntiDebug::PreventWindowsDebuggers");
				return false;
			}

			VirtualProtect((LPVOID)DbgBreakpoint_Address, 1, dwOldProt, &dwOldProt);
		}
	}

	if (DbgUiRemoteBreakin_Address)
	{
		if (VirtualProtect((LPVOID)DbgUiRemoteBreakin_Address, 1, PAGE_EXECUTE_READWRITE, &dwOldProt))
		{
			__try
			{
				*(BYTE*)DbgUiRemoteBreakin_Address = 0xC3;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				Logger::logf(Err, "Failed to patch over DbgUiRemoteBreakin @ AntiDebug::PreventWindowsDebuggers");
				return false;
			}

			VirtualProtect((LPVOID)DbgUiRemoteBreakin_Address, 1, dwOldProt, &dwOldProt);
		}
	}

	return true;

}

/*
	HideThreadFromDebugger - hides `hThread` from windows debuggers by calling NtSetInformationThread
	returns `true` on success
*/
bool Debugger::AntiDebug::HideThreadFromDebugger(HANDLE hThread)
{
	typedef NTSTATUS(NTAPI* pNtSetInformationThread) (HANDLE, UINT, PVOID, ULONG);
	NTSTATUS Status;

	pNtSetInformationThread NtSetInformationThread = (pNtSetInformationThread)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtSetInformationThread");

	if (NtSetInformationThread == NULL)
		return false;

	if (hThread == NULL)
		Status = NtSetInformationThread(GetCurrentThread(), 0x11, 0, 0);
	else
		Status = NtSetInformationThread(hThread, 0x11, 0, 0);

	return Status != 0 ? false : true;
}