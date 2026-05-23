/*==============================================================================
 * ProcessMonitor.cpp  -  Professional Windows Process Monitor
 *
 * ARCHITECTURE (for the next developer)
 * ======================================
 *  Data Layer:
 *    ProcessInfo    - Per-PID snapshot: CPU %, RAM, Peak RAM, Private Mem
 *    ProcessGroup   - All PIDs sharing one exe name, with aggregated totals
 *    g_prevSnapshot - std::unordered_map<DWORD,ProcessInfo> kept between
 *                     refreshes to compute CPU deltas
 *
 *  UI Layer:
 *    g_hList   - SysListView32 (LVS_REPORT + LVS_OWNERDATA-free + groups)
 *    g_hStatus - MSCTLS_STATUSBAR32 showing totals
 *    g_hSearch - Edit control for live name filtering
 *    Custom WM_PAINT on main window draws the dark header banner
 *
 *  Refresh Loop:
 *    WM_TIMER (ID_TIMER) fires every REFRESH_MS
 *    -> CollectSnapshots()  builds g_groups from a Toolhelp32 snapshot
 *    -> RebuildListView()   clears and repopulates the ListView with groups
 *    -> UpdateStatus()      pushes totals to the status bar
 *
 *  CPU Calculation:
 *    cpuPct = ((deltaKernel + deltaUser) * 100ns) / (wallSeconds * cores) * 100
 *    First sample always returns 0 % - correct on second tick.
 *
 *  Memory Display:
 *    FormatBytes() auto-scales to B / KB / MB / GB.
 *
 *  Extending:
 *    - Add a field to ProcessInfo, populate in CollectSnapshots()
 *    - Add an entry to COLUMNS[], render the value in RebuildListView()
 *    - For new right-click actions, add cases to the WM_CONTEXTMENU handler
 *==============================================================================
 */

#define UNICODE
#define _UNICODE
#ifndef WINVER
#define WINVER 0x0601
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include <algorithm>
#include <commctrl.h>
#include <cwctype>
#include <deque>
#include <dwmapi.h>
#include <iomanip>
#include <map>
#include <psapi.h>
#include <shellapi.h>
#include <sstream>
#include <string>
#include <tlhelp32.h>
#include <unordered_map>
#include <unordered_set>
#include <uxtheme.h>
#include <vector>
#include <windows.h>
#include <winternl.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "uxtheme.lib")

// ── IDs ──────────────────────────────────────────────────────────
static const int ID_TIMER = 1;
static const int IDC_LIST = 101;
static const int IDC_STATUS = 102;
static const int IDC_SEARCH = 103;
static const int IDC_REFRESH = 104;
static const int IDC_UNIT = 105;
static const int IDC_SPEED = 106;
static const int IDC_TYPE_TOGGLE = 107;
static int g_refreshMs = 500;
static const int HDR_H = 72;    // header banner height (px)
static const int TOOL_H = 48;   // toolbar strip height (px)
static const int STATUS_H = 30; // custom status bar height

// ── Palette: Obsidian ─────────────────────────────────────────────
namespace C {
COLORREF BG = RGB(13, 13, 20);         // Deep Obsidian
COLORREF PANEL = RGB(22, 22, 34);      // Elevated Surface
COLORREF CARD = RGB(28, 30, 44);       // Card Background
COLORREF ACCENT = RGB(80, 170, 255);   // Cool Azure
COLORREF ACCENT2 = RGB(170, 120, 255); // Soft Violet
COLORREF TEXT = RGB(220, 224, 232);    // Clean White
COLORREF DIMTXT = RGB(100, 108, 128);  // Neutral Gray
COLORREF ROWALT = RGB(17, 17, 26);     // Subtle Stripe
COLORREF SEL = RGB(35, 55, 100);       // Azure Selection
COLORREF GRPBG = RGB(24, 26, 40);      // Group Header
COLORREF BORDER = RGB(42, 44, 62);     // Subtle Border
COLORREF DANGER = RGB(255, 85, 85);    // Critical
COLORREF WARN = RGB(255, 185, 50);     // Warning
COLORREF SUCCESS = RGB(80, 220, 130);  // Good
} // namespace C

// ── Data structures ───────────────────────────────────────────────

// One process's complete measured snapshot.
// Add new metrics here; populate them in CollectSnapshots().
struct ProcessInfo {
  DWORD pid = 0;
  std::wstring name; // e.g. L"chrome.exe"
  bool isSystem = false;
  bool isExtension = false;
  std::wstring subName = L"";
  double cpuPct = 0;       // 0 – 100 %
  SIZE_T ramBytes = 0;     // Working Set
  SIZE_T peakRamBytes = 0; // Peak Working Set
  SIZE_T privBytes = 0;    // Private Bytes
  // Internal: previous sample times for CPU delta
  ULONGLONG prevKernel = 0;
  ULONGLONG prevUser = 0;
  LONGLONG prevTick = 0; // QueryPerformanceCounter tick
};

// All PIDs that share the same executable name.
struct ProcessGroup {
  std::wstring name;
  bool isSystem = false;
  bool hasExtensions = false;
  std::vector<ProcessInfo> members;
  double totalCpu = 0;
  SIZE_T totalRam = 0;
  SIZE_T totalPeakRam = 0;
  SIZE_T totalPriv = 0;
};

// ── Globals ───────────────────────────────────────────────────────
static HWND g_hWnd = nullptr;
static HWND g_hList = nullptr;
static HWND g_hSearch = nullptr;
static HWND g_hRefresh = nullptr;
static HFONT g_hFont = nullptr;
static HFONT g_hFontBd = nullptr;
static HFONT g_hFontTitle = nullptr;
static HFONT g_hFontSm = nullptr;
static int g_cores = 1;
static std::unordered_map<DWORD, ProcessInfo> g_prev; // CPU delta store
static std::vector<ProcessGroup> g_groups;

struct ProcessRow {
  bool isGroup = false;
  std::wstring groupName;
  std::wstring col[6];
  int iconIdx = -1;
  double rawCpu = 0.0;
  SIZE_T rawRam = 0;
};
static std::vector<ProcessRow> g_flatRows;

static std::wstring g_filter; // live search filter

static std::unordered_set<std::wstring> g_expandedGroups;
enum class MemUnit { Auto, MB, GB };
static MemUnit g_unit = MemUnit::Auto;
static HWND g_hUnit = nullptr;
static HWND g_hSpeed = nullptr;

enum class ProcessTypeFilter { All, OS, ThirdParty };
static ProcessTypeFilter g_typeFilter = ProcessTypeFilter::All;
static HWND g_hTypeToggle = nullptr;

static HIMAGELIST g_hIml = nullptr;
static std::unordered_map<std::wstring, int> g_iconCache;
static std::unordered_map<std::wstring, bool> g_isSystemCache;
static SIZE_T g_totalSysRam = 1;

static std::deque<double> g_cpuHistory;
static std::deque<double> g_ramHistory;
static const int MAX_HISTORY = 120;
static const int SIDE_W = 320; // Side panel width

static ULONGLONG g_lastSysIdle = 0, g_lastSysKern = 0, g_lastSysUser = 0;
static DWORD g_totalThreads = 0;
static std::wstring g_statusText;

// ── Helpers ───────────────────────────────────────────────────────

std::wstring FormatBytes(SIZE_T b) {
  wchar_t buf[64];
  if (g_unit == MemUnit::GB ||
      (g_unit == MemUnit::Auto && b >= (SIZE_T)1 << 30))
    swprintf_s(buf, L"%.2f GB", b / 1073741824.0);
  else if (g_unit == MemUnit::MB ||
           (g_unit == MemUnit::Auto && b >= (SIZE_T)1 << 20))
    swprintf_s(buf, L"%.1f MB", b / 1048576.0);
  else if (b >= (SIZE_T)1 << 10)
    swprintf_s(buf, L"%.0f KB", b / 1024.0);
  else
    swprintf_s(buf, L"%zu B", b);
  return buf;
}

COLORREF CpuColor(double p) {
  if (p < 5)
    return C::TEXT;
  if (p < 25)
    return C::WARN;
  if (p < 60)
    return RGB(255, 140, 50);
  return C::DANGER;
}

inline ULONGLONG FT2ULL(const FILETIME &f) {
  return ((ULONGLONG)f.dwHighDateTime << 32) | f.dwLowDateTime;
}

// ── CPU delta ─────────────────────────────────────────────────────
double CpuPercent(const ProcessInfo &cur, const ProcessInfo &prev) {
  if (prev.prevTick == 0)
    return 0.0;
  static LARGE_INTEGER qpf{};
  if (!qpf.QuadPart)
    QueryPerformanceFrequency(&qpf);
  double wall = (double)(cur.prevTick - prev.prevTick) / qpf.QuadPart;
  if (wall <= 0)
    return 0.0;
  ULONGLONG dt =
      (cur.prevKernel - prev.prevKernel) + (cur.prevUser - prev.prevUser);
  return std::min(100.0, (dt * 1e-7) / (wall * g_cores) * 100.0);
}

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

  if (params.CommandLine.Length == 0 || params.CommandLine.Buffer == nullptr)
    return L"";

  std::wstring cmdLine(params.CommandLine.Length / sizeof(wchar_t), L'\0');
  if (!ReadProcessMemory(hProcess, params.CommandLine.Buffer, &cmdLine[0],
                         params.CommandLine.Length, nullptr))
    return L"";

  return cmdLine;
}

// ── Data collection ───────────────────────────────────────────────
void CollectSnapshots() {
  g_totalThreads = 0;
  HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (hSnap == INVALID_HANDLE_VALUE)
    return;

  LARGE_INTEGER now{};
  QueryPerformanceCounter(&now);
  std::unordered_map<DWORD, ProcessInfo> newSnap;
  std::map<std::wstring, ProcessGroup> groupMap;

  PROCESSENTRY32W pe{};
  pe.dwSize = sizeof(pe);
  if (Process32FirstW(hSnap, &pe)) {
    do {
      ProcessInfo info;
      info.pid = pe.th32ProcessID;
      info.name = pe.szExeFile;
      info.isSystem = false;
      g_totalThreads += pe.cntThreads; // Accurate system thread count

      std::wstring lowerName = info.name;
      for (auto &c : lowerName)
        c = towlower(c);

      HANDLE hp = OpenProcess(
          PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, info.pid);
      if (hp) {
        // Memory
        PROCESS_MEMORY_COUNTERS_EX pmc{};
        pmc.cb = sizeof(pmc);
        if (GetProcessMemoryInfo(hp, (PROCESS_MEMORY_COUNTERS *)&pmc,
                                 sizeof(pmc))) {
          info.ramBytes = pmc.WorkingSetSize;
          info.peakRamBytes = pmc.PeakWorkingSetSize;
          info.privBytes = pmc.PrivateUsage;
        }
        // Extension & CmdLine Check
        auto it = g_prev.find(info.pid);
        if (it != g_prev.end()) {
          info.isExtension = it->second.isExtension;
          info.subName = it->second.subName;
        } else {
          std::wstring cmdLine = GetCommandLineArgs(hp);
          if (cmdLine.find(L"--type=extension") != std::wstring::npos) {
            info.isExtension = true;
            info.subName = L"[Ext]";
          } else if (cmdLine.find(L"--type=utility") != std::wstring::npos ||
                     cmdLine.find(L"--type=plugin") != std::wstring::npos) {
            info.isExtension = true;
            info.subName = L"[Utility]";
          }
        }

        // CPU
        FILETIME cr, ex, ker, usr;
        if (GetProcessTimes(hp, &cr, &ex, &ker, &usr)) {
          info.prevKernel = FT2ULL(ker);
          info.prevUser = FT2ULL(usr);
          info.prevTick = now.QuadPart;
          if (it != g_prev.end())
            info.cpuPct = CpuPercent(info, it->second);
        }

        // Icon Cache & System Cache
        if (g_iconCache.find(info.name) == g_iconCache.end() ||
            g_isSystemCache.find(lowerName) == g_isSystemCache.end()) {
          wchar_t pathBuf[MAX_PATH];
          DWORD pathSize = MAX_PATH;
          bool hasPath = QueryFullProcessImageNameW(hp, 0, pathBuf, &pathSize);

          if (g_iconCache.find(info.name) == g_iconCache.end()) {
            if (hasPath) {
              HICON hIcon[1] = {0};
              ExtractIconExW(pathBuf, 0, nullptr, hIcon, 1);
              if (hIcon[0]) {
                g_iconCache[info.name] = ImageList_AddIcon(g_hIml, hIcon[0]);
                DestroyIcon(hIcon[0]);
              } else
                g_iconCache[info.name] = -1;
            } else
              g_iconCache[info.name] = -1;
          }

          if (g_isSystemCache.find(lowerName) == g_isSystemCache.end()) {
            bool isSys = false;
            if (hasPath) {
              std::wstring path = pathBuf;
              for (auto &c : path)
                c = towlower(c);
              if (path.find(L"c:\\windows\\") == 0)
                isSys = true;
            }
            if (!isSys) {
              if (lowerName == L"system" ||
                  lowerName == L"system idle process" ||
                  lowerName == L"registry" || lowerName == L"smss.exe" ||
                  lowerName == L"csrss.exe" || lowerName == L"wininit.exe" ||
                  lowerName == L"services.exe" || lowerName == L"lsass.exe" ||
                  lowerName == L"svchost.exe" || lowerName == L"winlogon.exe" ||
                  lowerName == L"fontdrvhost.exe" || lowerName == L"dwm.exe" ||
                  lowerName == L"spoolsv.exe" ||
                  lowerName == L"taskhostw.exe" || lowerName == L"sihost.exe" ||
                  lowerName == L"conhost.exe" || lowerName == L"explorer.exe" ||
                  lowerName == L"searchindexer.exe" ||
                  lowerName == L"audiodg.exe" || lowerName == L"taskmgr.exe" ||
                  lowerName == L"ctfmon.exe" || lowerName == L"winrshost.exe" ||
                  lowerName == L"wsl.exe" || lowerName == L"wslhost.exe") {
                isSys = true;
              } else if (info.pid == 0 || info.pid == 4) {
                isSys = true;
              }
            }
            g_isSystemCache[lowerName] = isSys;
          }
        }
        CloseHandle(hp);
      } else {
        if (g_isSystemCache.find(lowerName) == g_isSystemCache.end()) {
          bool isSys = false;
          if (lowerName == L"system" || lowerName == L"system idle process" ||
              lowerName == L"registry" || lowerName == L"smss.exe" ||
              lowerName == L"csrss.exe" || lowerName == L"wininit.exe" ||
              lowerName == L"services.exe" || lowerName == L"lsass.exe" ||
              lowerName == L"svchost.exe" || lowerName == L"winlogon.exe" ||
              lowerName == L"fontdrvhost.exe" || lowerName == L"dwm.exe" ||
              lowerName == L"spoolsv.exe" || lowerName == L"taskhostw.exe" ||
              lowerName == L"sihost.exe" || lowerName == L"conhost.exe" ||
              lowerName == L"explorer.exe" ||
              lowerName == L"searchindexer.exe" ||
              lowerName == L"audiodg.exe" || lowerName == L"taskmgr.exe" ||
              lowerName == L"ctfmon.exe" || lowerName == L"winrshost.exe" ||
              lowerName == L"wsl.exe" || lowerName == L"wslhost.exe") {
            isSys = true;
          } else if (info.pid == 0 || info.pid == 4) {
            isSys = true;
          }
          g_isSystemCache[lowerName] = isSys;
        }
      }

      info.isSystem = g_isSystemCache[lowerName];
      newSnap[info.pid] = info;

      // Group key = lowercase exe name
      std::wstring key = info.name;
      for (auto &ch : key)
        ch = towlower(ch);
      groupMap[key].name = info.name;
      groupMap[key].members.push_back(info);
    } while (Process32NextW(hSnap, &pe));
  }
  CloseHandle(hSnap);

  // Aggregate
  std::vector<ProcessGroup> groups;
  for (auto &[k, g] : groupMap) {
    g.totalCpu = 0;
    g.totalRam = 0;
    g.totalPeakRam = 0;
    g.totalPriv = 0;
    bool anySystem = false;
    bool anyExtension = false;
    for (auto &p : g.members) {
      g.totalCpu += p.cpuPct;
      g.totalRam += p.ramBytes;
      g.totalPeakRam = std::max(g.totalPeakRam, p.peakRamBytes);
      g.totalPriv += p.privBytes;
      if (p.isSystem)
        anySystem = true;
      if (p.isExtension)
        anyExtension = true;
    }
    g.isSystem = anySystem;
    g.hasExtensions = anyExtension;
    // Sort sub-items by RAM descending
    std::sort(g.members.begin(), g.members.end(),
              [](const ProcessInfo &a, const ProcessInfo &b) {
                return a.ramBytes > b.ramBytes;
              });
    groups.push_back(std::move(g));
  }
  // Default sort: RAM descending
  std::sort(groups.begin(), groups.end(),
            [](const ProcessGroup &a, const ProcessGroup &b) {
              return a.totalRam > b.totalRam;
            });

  g_groups = std::move(groups);
  g_prev = std::move(newSnap);

  // High-Accuracy System CPU for Dashboard
  FILETIME idle, kern, usr;
  if (GetSystemTimes(&idle, &kern, &usr)) {
    ULONGLONG i = FT2ULL(idle), k = FT2ULL(kern), u = FT2ULL(usr);
    if (g_lastSysIdle > 0) {
      ULONGLONG dIdle = i - g_lastSysIdle;
      ULONGLONG dKern = k - g_lastSysKern;
      ULONGLONG dUsr = u - g_lastSysUser;
      ULONGLONG total = dKern + dUsr;
      if (total > 0) {
        double sysCpu = (double)(total - dIdle) / total * 100.0;
        g_cpuHistory.push_back(std::max(0.0, std::min(100.0, sysCpu)));
      }
    }
    g_lastSysIdle = i;
    g_lastSysKern = k;
    g_lastSysUser = u;
  }

  // System RAM History
  MEMORYSTATUSEX mem{};
  mem.dwLength = sizeof(mem);
  if (GlobalMemoryStatusEx(&mem)) {
    g_ramHistory.push_back((double)mem.dwMemoryLoad);
  }

  if (g_cpuHistory.size() > MAX_HISTORY)
    g_cpuHistory.pop_front();
  if (g_ramHistory.size() > MAX_HISTORY)
    g_ramHistory.pop_front();
}

// ── Columns ───────────────────────────────────────────────────────
struct ColDef {
  const wchar_t *title;
  int cx;
  int fmt;
};
static const ColDef COLS[] = {
    {L"Process Name", 210, LVCFMT_LEFT}, {L"PID", 65, LVCFMT_RIGHT},
    {L"CPU %", 80, LVCFMT_RIGHT},        {L"Memory", 105, LVCFMT_RIGHT},
    {L"Peak Memory", 115, LVCFMT_RIGHT}, {L"Private Mem", 115, LVCFMT_RIGHT},
};
static const int NCOLS = (int)(sizeof(COLS) / sizeof(COLS[0]));

void SetupColumns() {
  for (int i = 0; i < NCOLS; ++i) {
    LVCOLUMNW c{};
    c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
    c.pszText = (LPWSTR)COLS[i].title;
    c.cx = COLS[i].cx;
    c.fmt = COLS[i].fmt;
    ListView_InsertColumn(g_hList, i, &c);
  }
}

// ── List rebuild ──────────────────────────────────────────────────
void RebuildListView() {
  SIZE_T totRam = 0;
  double totCpu = 0;

  std::vector<ProcessRow> newRows;

  for (auto &grp : g_groups) {
    if (g_typeFilter == ProcessTypeFilter::OS && !grp.isSystem)
      continue;
    if (g_typeFilter == ProcessTypeFilter::ThirdParty && grp.isSystem)
      continue;

    // Apply search filter
    std::wstring nameLo = grp.name;
    for (auto &ch : nameLo)
      ch = towlower(ch);
    if (!g_filter.empty() && nameLo.find(g_filter) == std::wstring::npos)
      continue;

    totRam += grp.totalRam;
    totCpu += grp.totalCpu;

    bool expanded = g_expandedGroups.count(grp.name) > 0;

    wchar_t hdr[300], cpu[32], ram[32], peak[32], priv[32];
    if (grp.hasExtensions) {
      swprintf_s(hdr, L"%s  %s  [\U0001F9E9]  (%zu)",
                 expanded ? L"\u25BE" : L"\u25B8", grp.name.c_str(),
                 grp.members.size());
    } else {
      swprintf_s(hdr, L"%s  %s  (%zu)", expanded ? L"\u25BE" : L"\u25B8",
                 grp.name.c_str(), grp.members.size());
    }
    swprintf_s(cpu, L"%.1f%%", grp.totalCpu);
    wcscpy_s(ram, FormatBytes(grp.totalRam).c_str());
    wcscpy_s(peak, FormatBytes(grp.totalPeakRam).c_str());
    wcscpy_s(priv, FormatBytes(grp.totalPriv).c_str());

    ProcessRow p;
    p.isGroup = true;
    p.groupName = grp.name;
    p.col[0] = hdr;
    p.col[1] = L"";
    p.col[2] = cpu;
    p.col[3] = ram;
    p.col[4] = peak;
    p.col[5] = priv;
    if (g_iconCache.count(grp.name) > 0)
      p.iconIdx = g_iconCache[grp.name];
    p.rawCpu = grp.totalCpu;
    p.rawRam = grp.totalRam;
    newRows.push_back(p);

    if (expanded) {
      for (auto &mem : grp.members) {
        wchar_t pcpu[32], pram[32], ppeak[32], ppriv[32], ppid[16], pname[300];
        if (!mem.subName.empty()) {
          swprintf_s(pname, L"    %s %s", mem.subName.c_str(),
                     mem.name.c_str());
        } else {
          swprintf_s(pname, L"    %s", mem.name.c_str());
        }
        swprintf_s(pcpu, L"%.1f%%", mem.cpuPct);
        swprintf_s(ppid, L"%lu", mem.pid);
        wcscpy_s(pram, FormatBytes(mem.ramBytes).c_str());
        wcscpy_s(ppeak, FormatBytes(mem.peakRamBytes).c_str());
        wcscpy_s(ppriv, FormatBytes(mem.privBytes).c_str());

        ProcessRow c;
        c.isGroup = false;
        c.groupName = grp.name;
        c.col[0] = pname;
        c.col[1] = ppid;
        c.col[2] = pcpu;
        c.col[3] = pram;
        c.col[4] = ppeak;
        c.col[5] = ppriv;
        c.rawCpu = mem.cpuPct;
        c.rawRam = mem.ramBytes;
        newRows.push_back(c);
      }
    }
  }

  g_flatRows = std::move(newRows);
  ListView_SetItemCountEx(g_hList, g_flatRows.size(),
                          LVSICF_NOSCROLL | LVSICF_NOINVALIDATEALL);

  // Synchronous redraw to prevent flicker
  RedrawWindow(g_hList, nullptr, nullptr,
               RDW_ERASE | RDW_FRAME | RDW_INVALIDATE | RDW_ALLCHILDREN);

  // Store status text for custom drawing
  wchar_t sb[200];
  swprintf_s(
      sb,
      L"  %zu processes   \u2502   RAM: %s   \u2502   CPU: %.1f%%   \u2502  "
      L" Refresh: %dms",
      g_flatRows.size(), FormatBytes(totRam).c_str(), totCpu, g_refreshMs);
  g_statusText = sb;
  // Invalidate status bar area
  RECT rc;
  GetClientRect(g_hWnd, &rc);
  RECT sr = {0, rc.bottom - STATUS_H, rc.right, rc.bottom};
  InvalidateRect(g_hWnd, &sr, FALSE);
}

// ── Custom draw (dark theme) ──────────────────────────────────────
LRESULT HandleCustomDraw(LPARAM lp) {
  auto *cd = (LPNMLVCUSTOMDRAW)lp;
  switch (cd->nmcd.dwDrawStage) {
  case CDDS_PREPAINT:
    return CDRF_NOTIFYITEMDRAW;

  case CDDS_ITEMPREPAINT: {
    int row = (int)cd->nmcd.dwItemSpec;
    if (row < 0 || row >= (int)g_flatRows.size())
      return CDRF_DODEFAULT;
    const auto &r = g_flatRows[row];

    bool isGroup = r.isGroup;
    bool isHot = (ListView_GetHotItem(g_hList) == row);

    COLORREF bg = isGroup ? C::GRPBG : ((row % 2 == 0) ? C::BG : C::ROWALT);

    UINT state = ListView_GetItemState(g_hList, row, LVIS_SELECTED);
    if (state & LVIS_SELECTED)
      bg = C::SEL;
    else if (isHot && !isGroup)
      bg = RGB(25, 28, 42);

    cd->clrTextBk = bg;
    cd->clrText = isGroup ? RGB(240, 242, 250) : C::TEXT;

    SelectObject(cd->nmcd.hdc, isGroup ? g_hFontBd : g_hFont);

    return CDRF_NOTIFYSUBITEMDRAW | CDRF_NEWFONT;
  }
  case CDDS_ITEMPREPAINT | CDDS_SUBITEM: {
    int row = (int)cd->nmcd.dwItemSpec;
    if (row < 0 || row >= (int)g_flatRows.size())
      return CDRF_DODEFAULT;
    const auto &r = g_flatRows[row];

    bool isGroup = r.isGroup;
    bool isHot = (ListView_GetHotItem(g_hList) == row);

    UINT state = ListView_GetItemState(g_hList, row, LVIS_SELECTED);
    COLORREF bg = isGroup ? C::GRPBG : ((row % 2 == 0) ? C::BG : C::ROWALT);
    if (state & LVIS_SELECTED)
      bg = C::SEL;
    else if (isHot && !isGroup)
      bg = RGB(25, 28, 42);

    cd->clrTextBk = bg;

    // Draw accent stripe on group header rows (first subitem only)
    if (isGroup && cd->iSubItem == 0) {
      RECT rc;
      ListView_GetSubItemRect(g_hList, row, 0, LVIR_BOUNDS, &rc);
      RECT stripe = {rc.left, rc.top + 2, rc.left + 3, rc.bottom - 2};
      HBRUSH ab = CreateSolidBrush(C::ACCENT);
      FillRect(cd->nmcd.hdc, &stripe, ab);
      DeleteObject(ab);
    }

    if (cd->iSubItem == 2 || cd->iSubItem == 3) {
      RECT rc;
      ListView_GetSubItemRect(g_hList, row, cd->iSubItem, LVIR_BOUNDS, &rc);

      HBRUSH bgBr = CreateSolidBrush(bg);
      FillRect(cd->nmcd.hdc, &rc, bgBr);
      DeleteObject(bgBr);

      const wchar_t *buf = r.col[cd->iSubItem].c_str();

      double ratio = 0.0;
      COLORREF barCol = 0;
      if (cd->iSubItem == 2) { // CPU bar
        double v = r.rawCpu;
        ratio = std::min(1.0, v / 100.0);
        // Gradient: green -> amber -> red based on load
        if (v < 15)
          barCol = RGB(40, 140, 100);
        else if (v < 40)
          barCol = RGB(180, 150, 40);
        else if (v < 70)
          barCol = RGB(220, 120, 30);
        else
          barCol = RGB(220, 60, 60);
      } else { // Memory bar
        double val = (double)r.rawRam;
        ratio = std::min(1.0, val / (double)std::max((SIZE_T)1, g_totalSysRam));
        barCol = RGB(70, 100, 200); // Azure bar
      }

      // Draw bar background track
      RECT trackRc = rc;
      trackRc.left += 4;
      trackRc.right -= 4;
      trackRc.top += 4;
      trackRc.bottom -= 4;
      HBRUSH trackBr = CreateSolidBrush(RGB(20, 20, 32));
      FillRect(cd->nmcd.hdc, &trackRc, trackBr);
      DeleteObject(trackBr);

      // Draw filled bar
      RECT barRc = trackRc;
      barRc.right = barRc.left + (int)((barRc.right - barRc.left) * ratio);
      if (barRc.right > barRc.left) {
        HBRUSH br = CreateSolidBrush(barCol);
        FillRect(cd->nmcd.hdc, &barRc, br);
        DeleteObject(br);
      }

      // Draw text on top
      SetBkMode(cd->nmcd.hdc, TRANSPARENT);
      SetTextColor(cd->nmcd.hdc, RGB(230, 232, 240));
      SelectObject(cd->nmcd.hdc, isGroup ? g_hFontBd : g_hFont);
      rc.right -= 8;
      DrawTextW(cd->nmcd.hdc, buf, -1, &rc,
                DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

      return CDRF_SKIPDEFAULT;
    }

    if (isGroup) {
      cd->clrText = RGB(240, 242, 250);
    } else {
      cd->clrText = C::TEXT;
    }

    SelectObject(cd->nmcd.hdc, isGroup ? g_hFontBd : g_hFont);
    return CDRF_NEWFONT;
  }
  }
  return CDRF_DODEFAULT;
}

// ── Header banner paint ───────────────────────────────────────────
void PaintHeader(HWND hWnd, HDC dc) {
  RECT rc{};
  GetClientRect(hWnd, &rc);
  rc.bottom = HDR_H;
  // Gradient: deep navy → indigo → subtle purple
  for (int x = rc.left; x < rc.right; ++x) {
    double t = (double)x / std::max(1L, rc.right - rc.left);
    BYTE r = (BYTE)(12 + t * 18);
    BYTE g = (BYTE)(12 + t * 8);
    BYTE b = (BYTE)(22 + t * 30);
    HPEN p = CreatePen(PS_SOLID, 1, RGB(r, g, b));
    SelectObject(dc, p);
    MoveToEx(dc, x, rc.top, nullptr);
    LineTo(dc, x, rc.bottom);
    DeleteObject(p);
  }
  // Gradient accent line: azure → violet
  for (int x = rc.left; x < rc.right; ++x) {
    double t = (double)x / std::max(1L, rc.right - rc.left);
    BYTE r = (BYTE)(80 + t * 90);
    BYTE g = (BYTE)(170 - t * 50);
    BYTE b = (BYTE)(255);
    HPEN p = CreatePen(PS_SOLID, 2, RGB(r, g, b));
    SelectObject(dc, p);
    MoveToEx(dc, x, HDR_H - 1, nullptr);
    LineTo(dc, x, HDR_H + 1);
    DeleteObject(p);
  }

  // Title text
  SetBkMode(dc, TRANSPARENT);
  SelectObject(dc, g_hFontTitle);
  SetTextColor(dc, C::ACCENT);
  RECT tr = {20, 10, 500, HDR_H - 16};
  DrawTextW(dc, L"HALE", -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

  // Subtitle
  SelectObject(dc, g_hFontSm);
  SetTextColor(dc, C::DIMTXT);
  RECT sr = {22, 46, 600, HDR_H - 4};
  DrawTextW(dc, L"System Vitality Monitor", -1, &sr, DT_LEFT | DT_TOP);
}

void DrawDashboard(HWND hWnd, HDC dc) {
  RECT rc;
  GetClientRect(hWnd, &rc);

  int dashX = rc.right - SIDE_W;
  if (dashX < 0)
    return;

  RECT dr = {dashX, HDR_H + TOOL_H, rc.right, rc.bottom - STATUS_H};
  HBRUSH dashBr = CreateSolidBrush(C::PANEL);
  FillRect(dc, &dr, dashBr);
  DeleteObject(dashBr);

  // Separator line
  HPEN sep = CreatePen(PS_SOLID, 1, C::BORDER);
  SelectObject(dc, sep);
  MoveToEx(dc, dashX, dr.top, nullptr);
  LineTo(dc, dashX, dr.bottom);
  DeleteObject(sep);

  int pad = 16;
  int cardX = dashX + pad;
  int cardW = SIDE_W - pad * 2;

  // Helper: draw a card background
  auto DrawCard = [&](int y, int h) {
    RECT cardRc = {cardX - 4, y, cardX + cardW + 4, y + h};
    HBRUSH cb = CreateSolidBrush(C::CARD);
    FillRect(dc, &cardRc, cb);
    DeleteObject(cb);
    HPEN bp = CreatePen(PS_SOLID, 1, C::BORDER);
    HGDIOBJ old = SelectObject(dc, bp);
    SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, cardRc.left, cardRc.top, cardRc.right, cardRc.bottom);
    SelectObject(dc, old);
    DeleteObject(bp);
  };

  SetBkMode(dc, TRANSPARENT);

  // === System Overview Card ===
  int sysY = dr.top + 10;
  DrawCard(sysY, 130);

  SelectObject(dc, g_hFontBd);
  SetTextColor(dc, C::ACCENT);
  RECT hdr1 = {cardX + 10, sysY + 8, cardX + cardW, sysY + 30};
  DrawTextW(dc, L"System Overview", -1, &hdr1, DT_LEFT | DT_TOP);

  SelectObject(dc, g_hFont);
  ULONGLONG uptimeMs = GetTickCount64();
  int hrs = (int)(uptimeMs / 3600000);
  int mins = (int)((uptimeMs % 3600000) / 60000);

  struct {
    const wchar_t *label;
    wchar_t value[64];
  } stats[] = {{L"CPU Cores", {}}, {L"Threads", {}}, {L"Uptime", {}}};
  swprintf_s(stats[0].value, L"%d", g_cores);
  swprintf_s(stats[1].value, L"%lu", g_totalThreads);
  swprintf_s(stats[2].value, L"%dh %dm", hrs, mins);

  for (int i = 0; i < 3; ++i) {
    int iy = sysY + 40 + i * 28;
    SetTextColor(dc, C::DIMTXT);
    RECT lr = {cardX + 14, iy, cardX + 120, iy + 22};
    DrawTextW(dc, stats[i].label, -1, &lr,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SetTextColor(dc, C::TEXT);
    RECT vr = {cardX + 120, iy, cardX + cardW - 10, iy + 22};
    DrawTextW(dc, stats[i].value, -1, &vr,
              DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
  }

  // === Graph helper ===
  auto DrawGraph = [&](const std::deque<double> &data, int y,
                       const wchar_t *title, COLORREF lineCol,
                       COLORREF fillCol) {
    DrawCard(y, 160);

    // Title + current value
    SelectObject(dc, g_hFontBd);
    SetTextColor(dc, C::ACCENT);
    RECT tr = {cardX + 10, y + 8, cardX + cardW - 10, y + 28};
    DrawTextW(dc, title, -1, &tr, DT_LEFT | DT_TOP);

    if (!data.empty()) {
      wchar_t val[32];
      swprintf_s(val, L"%.1f%%", data.back());
      SetTextColor(dc, C::TEXT);
      SelectObject(dc, g_hFontBd);
      DrawTextW(dc, val, -1, &tr, DT_RIGHT | DT_TOP);
    }

    // Graph area
    RECT gr = {cardX + 10, y + 36, cardX + cardW - 10, y + 148};
    HBRUSH grBr = CreateSolidBrush(RGB(16, 16, 26));
    FillRect(dc, &gr, grBr);
    DeleteObject(grBr);

    // Grid lines at 25%, 50%, 75%
    int gh = gr.bottom - gr.top;
    for (int i = 1; i <= 3; ++i) {
      int gy = gr.bottom - (gh * i / 4);
      HPEN gridP = CreatePen(PS_DOT, 1, RGB(35, 38, 55));
      SelectObject(dc, gridP);
      MoveToEx(dc, gr.left, gy, nullptr);
      LineTo(dc, gr.right, gy);
      DeleteObject(gridP);
    }

    // Y-axis labels
    SelectObject(dc, g_hFontSm);
    SetTextColor(dc, RGB(60, 65, 80));
    RECT l0 = {gr.left - 1, gr.bottom - 14, gr.left + 22, gr.bottom};
    DrawTextW(dc, L"0", -1, &l0, DT_LEFT | DT_BOTTOM | DT_SINGLELINE);
    int midY = gr.top + gh / 2;
    RECT l50 = {gr.left - 1, midY - 6, gr.left + 26, midY + 8};
    DrawTextW(dc, L"50", -1, &l50, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    if (data.size() < 2)
      return;

    double step = (double)(gr.right - gr.left) / (MAX_HISTORY - 1);
    int startIdx = MAX_HISTORY - (int)data.size();

    // Area fill
    HBRUSH fb = CreateSolidBrush(fillCol);
    for (size_t i = 0; i < data.size(); ++i) {
      int x = gr.left + (int)((startIdx + i) * step);
      int py = gr.bottom - 1 - (int)(data[i] * (gh - 2) / 100.0);
      int w = std::max(1, (int)step + 1);
      RECT vr = {x, py, x + w, gr.bottom - 1};
      FillRect(dc, &vr, fb);
    }
    DeleteObject(fb);

    // Line on top
    HPEN lp2 = CreatePen(PS_SOLID, 2, lineCol);
    SelectObject(dc, lp2);
    for (size_t i = 0; i < data.size(); ++i) {
      int x = gr.left + (int)((startIdx + i) * step);
      int py = gr.bottom - 1 - (int)(data[i] * (gh - 2) / 100.0);
      if (i == 0)
        MoveToEx(dc, x, py, nullptr);
      else
        LineTo(dc, x, py);
    }
    DeleteObject(lp2);
  };

  DrawGraph(g_cpuHistory, dr.top + 150, L"CPU Usage", RGB(80, 200, 255),
            RGB(20, 50, 70));
  DrawGraph(g_ramHistory, dr.top + 320, L"RAM Usage", RGB(170, 120, 255),
            RGB(40, 25, 65));

  // === Top Eaters Card ===
  int ty = dr.top + 492;
  DrawCard(ty, 170);

  SelectObject(dc, g_hFontBd);
  SetTextColor(dc, C::ACCENT);
  RECT trr = {cardX + 10, ty + 8, cardX + cardW - 10, ty + 28};
  DrawTextW(dc, L"Top Memory Usage", -1, &trr, DT_LEFT | DT_TOP);

  std::vector<ProcessGroup *> sorted;
  for (auto &g : g_groups)
    sorted.push_back(&g);
  std::sort(sorted.begin(), sorted.end(), [](ProcessGroup *a, ProcessGroup *b) {
    return a->totalRam > b->totalRam;
  });

  SIZE_T maxRam = sorted.empty() ? 1 : sorted[0]->totalRam;
  for (int i = 0; i < std::min(5, (int)sorted.size()); ++i) {
    int iy = ty + 36 + i * 26;
    // Mini bar
    double ratio = (double)sorted[i]->totalRam / std::max((SIZE_T)1, maxRam);
    RECT barBg = {cardX + 14, iy + 16, cardX + cardW - 14, iy + 22};
    HBRUSH bbr = CreateSolidBrush(RGB(20, 20, 32));
    FillRect(dc, &barBg, bbr);
    DeleteObject(bbr);
    RECT barFill = barBg;
    barFill.right =
        barFill.left + (int)((barFill.right - barFill.left) * ratio);
    HBRUSH fbr = CreateSolidBrush(RGB(60, 90, 180));
    FillRect(dc, &barFill, fbr);
    DeleteObject(fbr);

    // Label
    SelectObject(dc, g_hFontSm);
    SetTextColor(dc, C::TEXT);
    wchar_t name[128];
    swprintf_s(name, L"%s", sorted[i]->name.c_str());
    RECT nr = {cardX + 14, iy, cardX + cardW / 2, iy + 18};
    DrawTextW(dc, name, -1, &nr, DT_LEFT | DT_TOP | DT_END_ELLIPSIS);
    // Value
    SetTextColor(dc, C::DIMTXT);
    wchar_t val[32];
    wcscpy_s(val, FormatBytes(sorted[i]->totalRam).c_str());
    RECT vr2 = {cardX + cardW / 2, iy, cardX + cardW - 14, iy + 18};
    DrawTextW(dc, val, -1, &vr2, DT_RIGHT | DT_TOP);
  }
}

// ── Window procedure ──────────────────────────────────────────────
LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
  case WM_CREATE: {
    g_hWnd = hWnd;
    // Initialize System Stats
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    g_cores = si.dwNumberOfProcessors;

    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof(ms);
    GlobalMemoryStatusEx(&ms);
    g_totalSysRam = (SIZE_T)ms.ullTotalPhys;

    // Font hierarchy
    g_hFont = CreateFontW(14, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                          OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                          CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    g_hFontBd =
        CreateFontW(16, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET,
                    OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                    DEFAULT_PITCH, L"Segoe UI Semibold");
    g_hFontTitle = CreateFontW(26, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    g_hFontSm = CreateFontW(12, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");

    RECT rc{};
    GetClientRect(hWnd, &rc);
    int W = rc.right, H = rc.bottom;

    // Search box (taller, better positioned)
    g_hSearch = CreateWindowExW(
        0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 20, HDR_H + 10,
        280, 28, hWnd, (HMENU)IDC_SEARCH, nullptr, nullptr);
    SendMessage(g_hSearch, WM_SETFONT, (WPARAM)g_hFont, TRUE);
    SendMessage(g_hSearch, 0x1501 /*EM_SETCUEBANNER*/, FALSE,
                (LPARAM)L"  \U0001F50D  Search processes...");

    // Refresh button (pill style)
    g_hRefresh = CreateWindowExW(0, L"BUTTON", L"\u21BB  Refresh",
                                 WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, W - 165,
                                 HDR_H + 10, 140, 28, hWnd, (HMENU)IDC_REFRESH,
                                 nullptr, nullptr);
    SendMessage(g_hRefresh, WM_SETFONT, (WPARAM)g_hFont, TRUE);

    // Unit button (pill style)
    g_hUnit = CreateWindowExW(
        0, L"BUTTON", L"Unit: Auto", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
        W - 315, HDR_H + 10, 140, 28, hWnd, (HMENU)IDC_UNIT, nullptr, nullptr);
    SendMessage(g_hUnit, WM_SETFONT, (WPARAM)g_hFont, TRUE);

    // Speed button (pill style)
    g_hSpeed = CreateWindowExW(
        0, L"BUTTON", L"Speed: 0.5s", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
        W - 465, HDR_H + 10, 140, 28, hWnd, (HMENU)IDC_SPEED, nullptr, nullptr);
    SendMessage(g_hSpeed, WM_SETFONT, (WPARAM)g_hFont, TRUE);

    // Type Toggle button (pill style)
    g_hTypeToggle = CreateWindowExW(0, L"BUTTON", L"View: All",
                                    WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                    W - 655, HDR_H + 10, 180, 28, hWnd,
                                    (HMENU)IDC_TYPE_TOGGLE, nullptr, nullptr);
    SendMessage(g_hTypeToggle, WM_SETFONT, (WPARAM)g_hFont, TRUE);

    // ListView
    int listTop = HDR_H + TOOL_H;
    g_hList = CreateWindowExW(0, WC_LISTVIEWW, L"",
                              WS_CHILD | WS_VISIBLE | LVS_REPORT |
                                  LVS_SHOWSELALWAYS | LVS_OWNERDATA,
                              0, listTop, W, H - listTop - STATUS_H, hWnd,
                              (HMENU)IDC_LIST, nullptr, nullptr);
    SetWindowTheme(g_hList, L"Explorer", nullptr);
    SendMessage(g_hList, WM_SETFONT, (WPARAM)g_hFont, TRUE);
    ListView_SetExtendedListViewStyle(g_hList, LVS_EX_FULLROWSELECT |
                                                   LVS_EX_DOUBLEBUFFER);
    ListView_SetBkColor(g_hList, C::BG);
    ListView_SetTextBkColor(g_hList, C::BG);
    ListView_SetTextColor(g_hList, C::TEXT);
    g_hIml = ImageList_Create(24, 24, ILC_COLOR32 | ILC_MASK, 1, 1);
    ListView_SetImageList(g_hList, g_hIml, LVSIL_SMALL);
    SetupColumns();

    // Initial collect + timer
    CollectSnapshots();
    RebuildListView();
    SetTimer(hWnd, ID_TIMER, g_refreshMs, nullptr);
    return 0;
  }
  case WM_TIMER: {
    CollectSnapshots();
    RebuildListView();

    // Flicker-free dashboard update via synchronous redraw
    RECT rc;
    GetClientRect(hWnd, &rc);
    RECT dashRc = {rc.right - SIDE_W, HDR_H + TOOL_H, rc.right, rc.bottom};
    RedrawWindow(hWnd, &dashRc, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
    return 0;
  }

  case WM_SIZE: {
    int W = LOWORD(lp), H = HIWORD(lp);
    int listW = (W > SIDE_W + 200) ? W - SIDE_W : W;

    if (g_hList) {
      SetWindowPos(g_hList, nullptr, 0, HDR_H + TOOL_H, listW,
                   H - HDR_H - TOOL_H - STATUS_H, SWP_NOZORDER);

      // Auto-stretch the first column (Process Name)
      int totalColW = 0;
      for (int i = 1; i < 6; ++i)
        totalColW += ListView_GetColumnWidth(g_hList, i);
      ListView_SetColumnWidth(g_hList, 0,
                              std::max(200, listW - totalColW - 20));
    }
    if (g_hRefresh)
      SetWindowPos(g_hRefresh, nullptr, W - 165, HDR_H + 10, 140, 28,
                   SWP_NOZORDER);
    if (g_hUnit)
      SetWindowPos(g_hUnit, nullptr, W - 315, HDR_H + 10, 140, 28,
                   SWP_NOZORDER);
    if (g_hSpeed)
      SetWindowPos(g_hSpeed, nullptr, W - 465, HDR_H + 10, 140, 28,
                   SWP_NOZORDER);
    if (g_hTypeToggle)
      SetWindowPos(g_hTypeToggle, nullptr, W - 655, HDR_H + 10, 180, 28,
                   SWP_NOZORDER);
    InvalidateRect(hWnd, nullptr, FALSE);
    return 0;
  }
  case WM_PAINT: {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hWnd, &ps);
    RECT rc;
    GetClientRect(hWnd, &rc);

    // Double buffering for entire window
    HDC memDC = CreateCompatibleDC(hdc);
    HBITMAP memBM = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
    HGDIOBJ oldBM = SelectObject(memDC, memBM);

    // Fill background
    HBRUSH bgBr = CreateSolidBrush(C::BG);
    FillRect(memDC, &rc, bgBr);
    DeleteObject(bgBr);

    PaintHeader(hWnd, memDC);
    DrawDashboard(hWnd, memDC);

    // Fill toolbar strip area
    RECT tr = rc;
    tr.top = HDR_H;
    tr.bottom = HDR_H + TOOL_H;
    HBRUSH tb = CreateSolidBrush(C::PANEL);
    FillRect(memDC, &tr, tb);
    DeleteObject(tb);

    // Toolbar bottom border
    HPEN tbBorder = CreatePen(PS_SOLID, 1, C::BORDER);
    SelectObject(memDC, tbBorder);
    MoveToEx(memDC, 0, HDR_H + TOOL_H - 1, nullptr);
    LineTo(memDC, rc.right, HDR_H + TOOL_H - 1);
    DeleteObject(tbBorder);

    // Custom status bar at bottom
    RECT sbRc = {0, rc.bottom - STATUS_H, rc.right, rc.bottom};
    HBRUSH sbBr = CreateSolidBrush(C::PANEL);
    FillRect(memDC, &sbRc, sbBr);
    DeleteObject(sbBr);
    // Accent top border on status bar
    HPEN sbLine = CreatePen(PS_SOLID, 1, C::BORDER);
    SelectObject(memDC, sbLine);
    MoveToEx(memDC, 0, rc.bottom - STATUS_H, nullptr);
    LineTo(memDC, rc.right, rc.bottom - STATUS_H);
    DeleteObject(sbLine);
    // Status text
    SetBkMode(memDC, TRANSPARENT);
    SetTextColor(memDC, C::DIMTXT);
    SelectObject(memDC, g_hFontSm);
    RECT stRc = {12, rc.bottom - STATUS_H + 2, rc.right - 12, rc.bottom - 2};
    DrawTextW(memDC, g_statusText.c_str(), -1, &stRc,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    BitBlt(hdc, 0, 0, rc.right, rc.bottom, memDC, 0, 0, SRCCOPY);

    SelectObject(memDC, oldBM);
    DeleteObject(memBM);
    DeleteDC(memDC);
    EndPaint(hWnd, &ps);
    return 0;
  }
  case WM_ERASEBKGND:
    return 1; // Prevent flickering by not erasing background

  case WM_DRAWITEM: {
    auto *dis = (LPDRAWITEMSTRUCT)lp;
    if (dis->CtlID == IDC_REFRESH || dis->CtlID == IDC_UNIT ||
        dis->CtlID == IDC_SPEED || dis->CtlID == IDC_TYPE_TOGGLE) {
      bool selected = (dis->itemState & ODS_SELECTED);
      bool disabled = (dis->itemState & ODS_DISABLED);

      COLORREF bg = C::CARD;
      if (selected) {
        bg = RGB(55, 45, 120);
      } else if (!disabled) {
        if (dis->CtlID == IDC_TYPE_TOGGLE) {
          bg = RGB(50, 90, 180); // Distinct bright blue/indigo to make it pop
        } else {
          bg = RGB(35, 38, 58);
        }
      }

      // Pill-shaped button with RoundRect
      HBRUSH br = CreateSolidBrush(bg);
      HPEN border = CreatePen(
          PS_SOLID, (dis->CtlID == IDC_TYPE_TOGGLE) ? 2 : 1,
          (dis->CtlID == IDC_TYPE_TOGGLE) ? RGB(100, 150, 255) : C::BORDER);
      HGDIOBJ oldBr2 = SelectObject(dis->hDC, br);
      HGDIOBJ oldPen = SelectObject(dis->hDC, border);
      RoundRect(dis->hDC, dis->rcItem.left, dis->rcItem.top, dis->rcItem.right,
                dis->rcItem.bottom, 14, 14);
      SelectObject(dis->hDC, oldBr2);
      SelectObject(dis->hDC, oldPen);
      DeleteObject(br);
      DeleteObject(border);

      wchar_t text[64];
      GetWindowTextW(dis->hwndItem, text, 64);

      SetBkMode(dis->hDC, TRANSPARENT);
      SetTextColor(dis->hDC, disabled ? RGB(70, 72, 90) : C::TEXT);
      SelectObject(dis->hDC,
                   (dis->CtlID == IDC_TYPE_TOGGLE) ? g_hFontBd : g_hFontSm);
      DrawTextW(dis->hDC, text, -1, &dis->rcItem,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
      return TRUE;
    }
    return 0;
  }

  case WM_COMMAND:
    if (LOWORD(wp) == IDC_REFRESH) {
      CollectSnapshots();
      RebuildListView();
    }
    if (LOWORD(wp) == IDC_UNIT) {
      if (g_unit == MemUnit::Auto) {
        g_unit = MemUnit::MB;
        SetWindowTextW(g_hUnit, L"Unit: MB");
      } else if (g_unit == MemUnit::MB) {
        g_unit = MemUnit::GB;
        SetWindowTextW(g_hUnit, L"Unit: GB");
      } else {
        g_unit = MemUnit::Auto;
        SetWindowTextW(g_hUnit, L"Unit: Auto");
      }
      RebuildListView();
    }
    if (LOWORD(wp) == IDC_SPEED) {
      if (g_refreshMs == 500)
        g_refreshMs = 1000;
      else if (g_refreshMs == 1000)
        g_refreshMs = 2000;
      else if (g_refreshMs == 2000)
        g_refreshMs = 5000;
      else
        g_refreshMs = 500;

      wchar_t buf[32];
      if (g_refreshMs == 500)
        wcscpy_s(buf, L"Speed: 0.5s");
      else
        swprintf_s(buf, L"Speed: %ds", g_refreshMs / 1000);
      SetWindowTextW(g_hSpeed, buf);

      KillTimer(hWnd, ID_TIMER);
      SetTimer(hWnd, ID_TIMER, g_refreshMs, nullptr);
    }

    if (LOWORD(wp) == IDC_TYPE_TOGGLE) {
      if (g_typeFilter == ProcessTypeFilter::All) {
        g_typeFilter = ProcessTypeFilter::OS;
        SetWindowTextW(g_hTypeToggle, L"View: Windows Programmes");
      } else if (g_typeFilter == ProcessTypeFilter::OS) {
        g_typeFilter = ProcessTypeFilter::ThirdParty;
        SetWindowTextW(g_hTypeToggle, L"View: 3rd Party");
      } else {
        g_typeFilter = ProcessTypeFilter::All;
        SetWindowTextW(g_hTypeToggle, L"View: All");
      }
      RebuildListView();
    }

    if (HIWORD(wp) == EN_CHANGE && LOWORD(wp) == IDC_SEARCH) {
      wchar_t buf[128];
      GetWindowTextW(g_hSearch, buf, 128);
      g_filter = buf;
      for (auto &ch : g_filter)
        ch = towlower(ch);
      RebuildListView();
    }
    return 0;

  case WM_NOTIFY: {
    auto *hdr = (LPNMHDR)lp;
    if (hdr->idFrom == IDC_LIST && hdr->code == LVN_GETDISPINFOW) {
      auto *pdi = (NMLVDISPINFOW *)lp;
      int idx = pdi->item.iItem;
      if (idx >= 0 && idx < (int)g_flatRows.size()) {
        const auto &r = g_flatRows[idx];
        if (pdi->item.mask & LVIF_TEXT) {
          if (pdi->item.iSubItem >= 0 && pdi->item.iSubItem < 6) {
            wcsncpy_s(pdi->item.pszText, pdi->item.cchTextMax,
                      r.col[pdi->item.iSubItem].c_str(), _TRUNCATE);
          }
        }
        if (pdi->item.mask & LVIF_IMAGE) {
          pdi->item.iImage = r.iconIdx;
        }
      }
      return 0;
    }
    if (hdr->idFrom == IDC_LIST && hdr->code == NM_CUSTOMDRAW)
      return HandleCustomDraw(lp);
    if (hdr->idFrom == IDC_LIST && hdr->code == NM_DBLCLK) {
      auto *nmv = (LPNMITEMACTIVATE)lp;
      if (nmv->iItem >= 0 && nmv->iItem < (int)g_flatRows.size()) {
        const auto &r = g_flatRows[nmv->iItem];
        if (r.isGroup) {
          if (g_expandedGroups.count(r.groupName))
            g_expandedGroups.erase(r.groupName);
          else
            g_expandedGroups.insert(r.groupName);
          RebuildListView();
        }
      }
    }
    break;
  }
  case WM_CTLCOLOREDIT: {
    HDC dc = (HDC)wp;
    SetBkColor(dc, C::PANEL);
    SetTextColor(dc, C::TEXT);
    static HBRUSH eb = CreateSolidBrush(C::PANEL);
    return (LRESULT)eb;
  }
  case WM_DESTROY:
    KillTimer(hWnd, ID_TIMER);
    DeleteObject(g_hFont);
    DeleteObject(g_hFontBd);
    DeleteObject(g_hFontTitle);
    DeleteObject(g_hFontSm);
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcW(hWnd, msg, wp, lp);
}

// ── Entry point ───────────────────────────────────────────────────
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int nShow) {
  INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES};
  InitCommonControlsEx(&icc);

  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = WndProc;
  wc.hInstance = hInst;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.hbrBackground = (HBRUSH)GetStockObject(NULL_BRUSH);
  wc.lpszClassName = L"ProcessMonitorClass";
  wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
  RegisterClassExW(&wc);

  HWND hWnd = CreateWindowExW(
      0, L"ProcessMonitorClass", L"HALE \u2014 System Vitality Monitor",
      WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, CW_USEDEFAULT,
      CW_USEDEFAULT, 1100, 750, nullptr, nullptr, hInst, nullptr);

  ShowWindow(hWnd, nShow);
  UpdateWindow(hWnd);

  MSG msg{};
  while (GetMessageW(&msg, nullptr, 0, 0)) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  return (int)msg.wParam;
}
