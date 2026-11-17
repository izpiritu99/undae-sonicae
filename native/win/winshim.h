// Declarations of the Windows and C runtime functions vsthost.c uses, written out by hand so the Windows host can be
// built with the ordinary Linux gcc and ld (no mingw, no Windows SDK), for 64-bit and for 32-bit Windows.
//   64-bit: compile with -mabi=ms, so every function and function pointer uses the Windows x64 convention.
//   32-bit: compile with -m32 -fleading-underscore; Windows API functions are stdcall (WINAPI), the C runtime cdecl.
// `long` is 8 bytes in the 64-bit build, unlike on Windows, so fixed-width types are used everywhere. There is no
// libgcc in the 32-bit build: avoid 64-bit division and double <-> 64-bit integer conversions.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

#if defined(__x86_64__)
#define WINAPI
typedef __builtin_ms_va_list va_list;
#define va_start(a, l) __builtin_ms_va_start(a, l)
#define va_end(a) __builtin_ms_va_end(a)
#else
#define WINAPI __attribute__((stdcall))
typedef __builtin_va_list va_list;
#define va_start(a, l) __builtin_va_start(a, l)
#define va_end(a) __builtin_va_end(a)
#endif

// msvcrt.dll
void *malloc(size_t); void *calloc(size_t, size_t); void *realloc(void *, size_t); void free(void *);
void *memcpy(void *, const void *, size_t); void *memset(void *, int, size_t); int memcmp(const void *, const void *, size_t); void *memmove(void *, const void *, size_t);
size_t strlen(const char *); int strcmp(const char *, const char *); int strncmp(const char *, const char *, size_t);
char *strchr(const char *, int); char *strrchr(const char *, int); char *strstr(const char *, const char *); char *_strdup(const char *);
double strtod(const char *, char **); int sscanf(const char *, const char *, ...); int printf(const char *, ...); int fflush(void *);
int _vsnprintf(char *, size_t, const char *, va_list); int _snprintf(char *, size_t, const char *, ...); char *getenv(const char *);
void *fopen(const char *, const char *); size_t fread(void *, size_t, size_t, void *); int fseek(void *, int32_t, int); int fclose(void *);
#define vsnprintf _vsnprintf
#define strdup _strdup

// kernel32.dll
typedef void *HANDLE;
typedef struct { uint64_t opaque[8]; } CRITICAL_SECTION;                 // 40 bytes on 64-bit Windows, 24 on 32-bit; padded
typedef struct { uint32_t attr; uint64_t t[3]; uint32_t size_hi, size_lo, r0, r1; char name[260]; char alt[14]; char pad[10]; } __attribute__((packed)) FIND_DATA;
WINAPI void *LoadLibraryA(const char *); WINAPI int FreeLibrary(void *); WINAPI void *GetProcAddress(void *, const char *);
WINAPI HANDLE CreateThread(void *, size_t, uint32_t (WINAPI *)(void *), void *, uint32_t, uint32_t *); WINAPI uint32_t GetCurrentThreadId(void);
WINAPI HANDLE GetCurrentThread(void); WINAPI int SetThreadPriority(HANDLE, int);
WINAPI void Sleep(uint32_t); WINAPI void ExitProcess(uint32_t); WINAPI uint32_t SetErrorMode(uint32_t); WINAPI char *GetCommandLineA(void);
WINAPI HANDLE CreateEventA(void *, int, int, const char *); WINAPI int SetEvent(HANDLE); WINAPI uint32_t WaitForSingleObject(HANDLE, uint32_t); WINAPI int CloseHandle(HANDLE);
WINAPI void InitializeCriticalSection(CRITICAL_SECTION *); WINAPI void EnterCriticalSection(CRITICAL_SECTION *); WINAPI void LeaveCriticalSection(CRITICAL_SECTION *);
typedef struct { void *p; } SRWLOCK;                                     // zero-initialised is unlocked
WINAPI void AcquireSRWLockShared(SRWLOCK *); WINAPI void ReleaseSRWLockShared(SRWLOCK *); WINAPI void AcquireSRWLockExclusive(SRWLOCK *); WINAPI void ReleaseSRWLockExclusive(SRWLOCK *);
WINAPI HANDLE GetStdHandle(uint32_t); WINAPI int ReadFile(HANDLE, void *, uint32_t, uint32_t *, void *);
WINAPI HANDLE FindFirstFileA(const char *, FIND_DATA *); WINAPI int FindNextFileA(HANDLE, FIND_DATA *); WINAPI int FindClose(HANDLE);
WINAPI uint8_t SystemFunction036(void *, uint32_t);                      // advapi32.dll: random bytes

// ws2_32.dll
typedef uintptr_t SOCKET;
struct sockaddr_in { int16_t family; uint16_t port; uint32_t addr; char zero[8]; };
WINAPI int WSAStartup(uint16_t, void *); WINAPI SOCKET socket(int, int, int); WINAPI int bind(SOCKET, const void *, int); WINAPI int listen(SOCKET, int);
WINAPI SOCKET accept(SOCKET, void *, int *); WINAPI int getsockname(SOCKET, void *, int *); WINAPI int send(SOCKET, const char *, int, int); WINAPI int recv(SOCKET, char *, int, int);
WINAPI int closesocket(SOCKET); WINAPI int shutdown(SOCKET, int); WINAPI int setsockopt(SOCKET, int, int, const char *, int);

// user32.dll: just enough to give a plug-in a window and run its messages
typedef struct { uint32_t size, style; intptr_t (WINAPI *proc)(void *, uint32_t, uintptr_t, intptr_t); int32_t cls_extra, wnd_extra; void *instance, *icon, *cursor, *brush; const char *menu, *name; void *icon_sm; } WNDCLASSEXA;
typedef struct { uintptr_t opaque[8]; } MSG;                             // only passed through, never read
typedef struct { int32_t left, top, right, bottom; } RECT;
WINAPI uint16_t RegisterClassExA(const WNDCLASSEXA *); WINAPI void *GetModuleHandleA(const char *);
WINAPI void *CreateWindowExA(uint32_t exstyle, const char *cls, const char *title, uint32_t style, int32_t x, int32_t y, int32_t w, int32_t h, void *parent, void *menu, void *instance, void *param);
WINAPI int DestroyWindow(void *); WINAPI int ShowWindow(void *, int32_t); WINAPI int SetWindowPos(void *, void *, int32_t, int32_t, int32_t, int32_t, uint32_t); WINAPI int AdjustWindowRectEx(RECT *, uint32_t style, int32_t menu, uint32_t exstyle);
WINAPI intptr_t DefWindowProcA(void *, uint32_t, uintptr_t, intptr_t);
#if defined(__x86_64__)
WINAPI intptr_t SetWindowLongPtrA(void *, int32_t, intptr_t); WINAPI intptr_t GetWindowLongPtrA(void *, int32_t);
#else
WINAPI intptr_t SetWindowLongA(void *, int32_t, intptr_t); WINAPI intptr_t GetWindowLongA(void *, int32_t);   // 32-bit user32 has no ...Ptr versions
#define SetWindowLongPtrA SetWindowLongA
#define GetWindowLongPtrA GetWindowLongA
#endif
WINAPI int PeekMessageA(MSG *, void *, uint32_t, uint32_t, uint32_t); WINAPI int TranslateMessage(const MSG *); WINAPI intptr_t DispatchMessageA(const MSG *);
WINAPI uint32_t MsgWaitForMultipleObjects(uint32_t count, const HANDLE *handles, int32_t all, uint32_t ms, uint32_t mask);
WINAPI uintptr_t SetTimer(void *, uintptr_t id, uint32_t ms, void *proc); WINAPI int KillTimer(void *, uintptr_t id); WINAPI void *LoadCursorA(void *, const char *);
