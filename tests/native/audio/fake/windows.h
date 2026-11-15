/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/* Fake <windows.h> for tests/native/audio/test_cushion.c: only what src/platform/host_audio.c uses of the waveOut API, with a device
 * simulation behind it (defined in the test). Never used for a real build. */
#ifndef FAKE_WINDOWS_H
#define FAKE_WINDOWS_H
#include <stddef.h>
typedef unsigned long DWORD;
typedef char *LPSTR;
typedef unsigned int MMRESULT;
typedef void *HWAVEOUT;
#define MMSYSERR_NOERROR 0
#define MAXERRORLENGTH 256
#define WHDR_DONE 0x1
#define WAVE_FORMAT_PCM 1
#define WAVE_MAPPER ((unsigned)-1)
#define CALLBACK_NULL 0
typedef struct { unsigned short wFormatTag, nChannels; DWORD nSamplesPerSec, nAvgBytesPerSec; unsigned short nBlockAlign, wBitsPerSample, cbSize; } WAVEFORMATEX;
typedef struct { LPSTR lpData; DWORD dwBufferLength, dwFlags; } WAVEHDR;
typedef void *HANDLE;
typedef void *LPVOID;
typedef int BOOL;
#define WINAPI
#define TRUE 1
#define FALSE 0
#define INFINITE 0xFFFFFFFFu
DWORD GetTickCount(void);
HANDLE CreateEventA(void *attr, BOOL manual, BOOL initial, const char *name);
HANDLE CreateThread(void *attr, size_t stack, DWORD (WINAPI *fn)(LPVOID), LPVOID arg, DWORD flags, DWORD *id);
DWORD WaitForSingleObject(HANDLE h, DWORD ms);
BOOL SetEvent(HANDLE h);
BOOL CloseHandle(HANDLE h);
MMRESULT waveOutOpen(HWAVEOUT *h, unsigned id, const WAVEFORMATEX *f, DWORD a, DWORD b, DWORD c);
MMRESULT waveOutPrepareHeader(HWAVEOUT h, WAVEHDR *w, unsigned size);
MMRESULT waveOutUnprepareHeader(HWAVEOUT h, WAVEHDR *w, unsigned size);
MMRESULT waveOutWrite(HWAVEOUT h, WAVEHDR *w, unsigned size);
MMRESULT waveOutGetErrorTextA(MMRESULT r, char *text, unsigned size);
#endif
