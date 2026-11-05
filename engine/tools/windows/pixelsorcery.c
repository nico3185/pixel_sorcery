/* PixelSorcery.exe - PixelSorcery on Windows as one file (built by make_zip.sh, no game data).
 *
 * Standalone (the release's PixelSorcery-VERSION-windows.exe): the engine's tree is appended to
 * this program as a zip, followed by its size (8 bytes, little endian) and "PSPAYLD1". The first
 * run unpacks it into %LOCALAPPDATA%\PixelSorcery, installs the toolchain (install.ps1: MSYS2 with
 * MinGW64, in its own window), adds a Start menu shortcut, then opens the window.
 * Inside an unpacked tree (PixelSorcery.exe beside engine\): opens PixelSorcery's window
 * (engine\tools\gui\pixelsorcery_gui.py: find your dumps, build, play, the shelf).
 *   PixelSorcery.exe play CODE    plays a built title instead (play.sh CODE). */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MSYS "C:\\msys64"
static const char MAGIC[8] = {'P', 'S', 'P', 'A', 'Y', 'L', 'D', '1'};

static int exists(const char *p) { return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES; }

static void fail(const char *msg) { MessageBoxA(NULL, msg, "PixelSorcery", MB_ICONERROR); exit(1); }

static int run(const char *cmd, int wait, int console) {   /* a process; its exit code when waited */
    STARTUPINFOA si = {sizeof si}; PROCESS_INFORMATION pi; DWORD rc = 0;
    char *c = _strdup(cmd);
    if (!CreateProcessA(NULL, c, NULL, NULL, FALSE, console ? CREATE_NEW_CONSOLE : CREATE_NO_WINDOW,
                        NULL, NULL, &si, &pi)) { free(c); return -1; }
    if (wait) { WaitForSingleObject(pi.hProcess, INFINITE); GetExitCodeProcess(pi.hProcess, &rc); }
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread); free(c);
    return (int)rc;
}

/* the appended payload: written to a temp zip, unpacked under root; returns the tree's folder */
static int unpack(const char *self, const char *root, char *tree, size_t tsz) {
    FILE *f = fopen(self, "rb"); if (!f) return 0;
    char tail[16]; unsigned long long n = 0;
    if (_fseeki64(f, -16, SEEK_END) || fread(tail, 1, 16, f) != 16 || memcmp(tail + 8, MAGIC, 8)) { fclose(f); return 0; }
    memcpy(&n, tail, 8);
    char zip[MAX_PATH]; GetTempPathA(sizeof zip, zip); strcat(zip, "PixelSorcery-payload.zip");
    FILE *o = fopen(zip, "wb"); if (!o) { fclose(f); return 0; }
    _fseeki64(f, -16 - (long long)n, SEEK_END);
    char buf[1 << 16]; unsigned long long left = n;
    while (left) { size_t k = fread(buf, 1, left < sizeof buf ? (size_t)left : sizeof buf, f); if (!k) break; fwrite(buf, 1, k, o); left -= k; }
    fclose(o); fclose(f);
    CreateDirectoryA(root, NULL);
    char cmd[3 * MAX_PATH + 200];
    snprintf(cmd, sizeof cmd, "powershell -NoProfile -Command \"Expand-Archive -Force -LiteralPath '%s' -DestinationPath '%s'\"", zip, root);
    int rc = run(cmd, 1, 0); DeleteFileA(zip);
    if (rc) return 0;
    WIN32_FIND_DATAA fd; char pat[MAX_PATH + 32]; snprintf(pat, sizeof pat, "%s\\PixelSorcery-*-windows", root);
    HANDLE h = FindFirstFileA(pat, &fd); if (h == INVALID_HANDLE_VALUE) return 0;
    snprintf(tree, tsz, "%s\\%s", root, fd.cFileName);   /* (the newest version wins on a reinstall) */
    while (FindNextFileA(h, &fd)) if (strcmp(fd.cFileName, strrchr(tree, '\\') + 1) > 0) snprintf(tree, tsz, "%s\\%s", root, fd.cFileName);
    FindClose(h);
    return 1;
}

int main(int argc, char **argv) {
    char self[MAX_PATH], tree[MAX_PATH], cmd[4096], p[MAX_PATH + 64];
    GetModuleFileNameA(NULL, self, sizeof self);
    snprintf(tree, sizeof tree, "%s", self); *strrchr(tree, '\\') = 0;
    snprintf(p, sizeof p, "%s\\engine\\tools\\gui\\pixelsorcery_gui.py", tree);
    if (!exists(p)) {                                          /* standalone: unpack first */
        char root[MAX_PATH]; GetEnvironmentVariableA("LOCALAPPDATA", root, sizeof root); strcat(root, "\\PixelSorcery");
        if (!unpack(self, root, tree, sizeof tree)) fail("This PixelSorcery.exe holds no engine (download the release's PixelSorcery-VERSION-windows.exe).");
        snprintf(cmd, sizeof cmd, "%s\\PixelSorcery.exe", tree);
        if (!exists(cmd)) CopyFileA(self, cmd, FALSE);         /* the shortcut's target (the zip has a plain one) */
        char lnk[MAX_PATH]; GetEnvironmentVariableA("APPDATA", lnk, sizeof lnk);
        strcat(lnk, "\\Microsoft\\Windows\\Start Menu\\Programs\\PixelSorcery.lnk");
        char ps[4 * MAX_PATH + 300];
        snprintf(ps, sizeof ps, "powershell -NoProfile -Command \"$s=(New-Object -ComObject WScript.Shell).CreateShortcut('%s');"
                 "$s.TargetPath='%s\\PixelSorcery.exe';$s.WorkingDirectory='%s';$s.Save()\"", lnk, tree, tree);
        run(ps, 1, 0);
    }
    SetCurrentDirectoryA(tree);
    snprintf(p, sizeof p, "%s\\engine\\.venv\\bin\\python", tree);
    if (!exists(MSYS "\\msys2_shell.cmd") || !exists(p)) {    /* the toolchain, once (its own window) */
        if (run("powershell -NoProfile -ExecutionPolicy Bypass -File engine\\tools\\windows\\install.ps1", 1, 1) != 0
            || !exists(MSYS "\\msys2_shell.cmd"))
            fail("The toolchain installation did not finish. Run engine\\tools\\windows\\install.ps1 from the PixelSorcery folder.");
    }
    /* MSYS2's MinGW64 on PATH: the engine runs zsh scripts, cmake, ninja and gcc */
    char path[32768]; GetEnvironmentVariableA("PATH", path, sizeof path);
    char np[sizeof path + 128]; snprintf(np, sizeof np, MSYS "\\mingw64\\bin;" MSYS "\\usr\\bin;%s", path);
    SetEnvironmentVariableA("PATH", np); SetEnvironmentVariableA("MSYSTEM", "MINGW64");
    SetEnvironmentVariableA("PYTHONUTF8", "1");                /* UTF-8 I/O: game titles, paths */
    if (argc >= 3 && !strcmp(argv[1], "play"))
        snprintf(cmd, sizeof cmd, MSYS "\\usr\\bin\\zsh.exe ./play.sh %s", argv[2]);
    else
        snprintf(cmd, sizeof cmd, MSYS "\\mingw64\\bin\\pythonw.exe engine\\tools\\gui\\pixelsorcery_gui.py");
    return run(cmd, 0, 0) < 0;
}
