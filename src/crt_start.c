/* crt_start.c — CRT-free process entry, used when the build defines TK_NOCRT.
 *
 * Why: MinGW's static CRT start-up objects drag in locale initialisation,
 * atexit bookkeeping, secure-cookie setup and the stdio flush machinery. None of
 * that is used here (no malloc, no printf, no streams, no C++ statics), and it
 * costs about a third of the binary. Dropping it is the difference between
 * ~107 KB and well under the 80 KB target.
 *
 * What we lose and why it does not matter:
 *   - no C++ global ctors/dtors        (this is C)
 *   - no atexit()/_onexit()            (nothing registers one)
 *   - no stdio flush of FILE streams   (we write with WriteFile only)
 *   - no locale/mbstowcs tables        (our own UTF-16 conversion via kernel32)
 *   - no CRT heap                      (the whole program has zero mallocs)
 * What we must still do ourselves:
 *   - pass the right nCmdShow to WinMain, and hand WinMain's return value to
 *     ExitProcess so the scheduled task sees the 0/1/2 contract.
 *
 * The symbol name is the one the linker looks for by default for
 * /SUBSYSTEM:WINDOWS on both architectures (x86 decorates it with a leading
 * underscore, x64 does not; plain C names do exactly the right thing).
 *
 * SPDX-License-Identifier: MIT
 */
#ifdef TK_NOCRT

#include <windows.h>

/* WinMain is implemented in main.c and already prototyped by windows.h; no
 * duplicate declaration here, which -Wredundant-decls rightly notices. */

static char cmdline_buf[4096];

/* cdecl, deliberately: on x86 the linker's default entry for
 * /SUBSYSTEM:WINDOWS is the *undecorated* _WinMainCRTStartup, and a WINAPI
 * (stdcall) definition would emit _WinMainCRTStartup@0 and rely on ld's
 * stdcall fixup. WinMain itself keeps its WINAPI decoration, which the
 * prototype from windows.h already guarantees. */
int WinMainCRTStartup(void);
int WinMainCRTStartup(void)
{
    HINSTANCE inst = GetModuleHandleW(NULL);
    int       show = SW_SHOWNORMAL;
    LPSTR     cmd  = NULL;
    int       rc;

    /* ShowWindow state comes from the STARTUPINFO the parent supplied; reading
     * it directly is what the CRT does before calling WinMain. */
    {
        STARTUPINFOW si;
        GetStartupInfoW(&si);
        if (si.dwFlags & STARTF_USESHOWWINDOW) show = (int)si.wShowWindow;
    }

    /* WinMain's lpCmdLine is the ANSI remainder of the command line. TimeKeeper
     * parses GetCommandLineW() itself, so NULL is honest and saves a conversion. */
    (void)cmdline_buf;

    rc = WinMain(inst, NULL, cmd, show);
    ExitProcess((UINT)rc);
    return rc;      /* unreachable: ExitProcess does not return */
}

#endif /* TK_NOCRT */
