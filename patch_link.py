import re

# Fix 1: usb_transport.cpp guard _WIN32_WCE -> defined(__MINGW32CE__) || defined(_WIN32_WCE)
p = r'C:\tang-carplay\client\cpp\protocol\usb_transport.cpp'
s = open(p, encoding='utf-8').read()
n0 = s.count('#ifdef _WIN32_WCE')
s = s.replace('#ifdef _WIN32_WCE',
              '#if defined(__MINGW32CE__) || defined(_WIN32_WCE)')
open(p, 'w', encoding='utf-8').write(s)
print('usb_transport.cpp: fixed', n0, 'guard(s)')

# Fix 2: ce_main.cpp explicit W-suffix APIs (WinCE coredll exports only *W)
p2 = r'C:\tang-carplay\client\cpp\ce_main.cpp'
s2 = open(p2, encoding='utf-8').read()
for api in ['DefWindowProc', 'DispatchMessage', 'GetMessage', 'TranslateMessage',
            'PostQuitMessage', 'InvalidateRect', 'BeginPaint', 'EndPaint',
            'GetClientRect', 'FillRect', 'DeleteObject', 'CreateSolidBrush',
            'SetBkMode', 'SetTextColor', 'ExtTextOutW', 'MultiByteToWideChar',
            'CreateWindowW', 'ShowWindow', 'UpdateWindow', 'RegisterClassW',
            'GetSystemMetrics', 'InitializeCriticalSection', 'EnterCriticalSection',
            'LeaveCriticalSection', 'CreateThread', 'CloseHandle', 'WaitForSingleObject',
            'InterlockedExchange', 'InterlockedCompareExchange', 'Sleep', 'wcslen']:
    # Only replace bare calls not already W-suffixed
    s2 = re.sub(r'\b' + api + r'\(', api + 'W(', s2) if not api.endswith('W') else s2
# But InterlockedExchange/CompareExchange/Sleep/InitializeCriticalSection etc have no W variants - revert those
for api in ['InterlockedExchangeW', 'InterlockedCompareExchangeW', 'SleepW',
            'InitializeCriticalSectionW', 'EnterCriticalSectionW', 'LeaveCriticalSectionW',
            'CreateThreadW', 'CloseHandleW', 'WaitForSingleObjectW',
            'MultiByteToWideCharW', 'GetSystemMetricsW', 'GetClientRectW',
            'FillRectW', 'DeleteObjectW', 'CreateSolidBrushW', 'SetBkModeW',
            'SetTextColorW', 'InvalidateRectW', 'PostQuitMessageW',
            'TranslateMessageW', 'BeginPaintW', 'EndPaintW', 'wcslenW']:
    correct = api[:-1]  # strip the erroneous W
    s2 = s2.replace(api + '(', correct + '(')
# Fix the ones that legitimately take W: message APIs + window APIs
# DefWindowProcW, DispatchMessageW, GetMessageW are correct as-is now.
# CreateWindowW/RegisterClassW/ShowWindow/UpdateWindow already had W or no suffix issue.
# ShowWindow/UpdateWindow have no W variant on CE headers? They do: ShowWindow exists plain.
s2 = s2.replace('ShowWindowW(', 'ShowWindow(')
s2 = s2.replace('UpdateWindowW(', 'UpdateWindow(')
open(p2, 'w', encoding='utf-8').write(s2)
print('ce_main.cpp: W-suffix APIs fixed')

# Verify remaining bad refs
for bad in ['DefWindowProc(', 'DispatchMessage(', 'GetMessage(']:
    if bad in s2:
        print('WARNING: still bare:', bad)
print('DONE')