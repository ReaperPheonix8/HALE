#include <iostream>
#include <string>
#include <windows.h>
#include <winternl.h>

typedef NTSTATUS(NTAPI *pfnNtQueryInformationProcess)(
    HANDLE ProcessHandle, PROCESSINFOCLASS ProcessInformationClass,
    PVOID ProcessInformation, ULONG ProcessInformationLength,
    PULONG ReturnLength);

std::wstring GetCommandLineArgs(HANDLE hProcess) {
  pfnNtQueryInformationProcess NtQueryInfo =
      (pfnNtQueryInformationProcess)GetProcAddress(
          GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess");
  if (!NtQueryInfo)
    return L"";

  PROCESS_BASIC_INFORMATION pbi;
  ULONG len;
  if (NtQueryInfo(hProcess, ProcessBasicInformation, &pbi, sizeof(pbi), &len) !=
      0)
    return L"";

  PEB peb;
  if (!ReadProcessMemory(hProcess, pbi.PebBaseAddress, &peb, sizeof(peb),
                         nullptr))
    return L"";

  RTL_USER_PROCESS_PARAMETERS params;
  if (!ReadProcessMemory(hProcess, peb.ProcessParameters, &params,
                         sizeof(params), nullptr))
    return L"";

  std::wstring cmdLine(params.CommandLine.Length / sizeof(wchar_t), L'\0');
  if (!ReadProcessMemory(hProcess, params.CommandLine.Buffer, &cmdLine[0],
                         params.CommandLine.Length, nullptr))
    return L"";

  return cmdLine;
}

int main(int argc, char **argv) {
  if (argc < 2)
    return 1;
  DWORD pid = std::stoi(argv[1]);
  HANDLE hp =
      OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
  if (hp) {
    std::wcout << L"CMD: " << GetCommandLineArgs(hp) << std::endl;
    CloseHandle(hp);
  } else {
    std::cout << "Failed to open process" << std::endl;
  }
  return 0;
}
