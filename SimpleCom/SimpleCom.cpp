#include "stdafx.h"

#define _CRT_SECURE_NO_WARNINGS

#include <iostream>
#include <vector>

#include "SerialSetup.h"
#include "WinAPIException.h"

static HANDLE stdoutRedirectorThread;

static HANDLE hSerial;
static OVERLAPPED serialReadOverlapped = { 0, 0, 0, 0, nullptr };
static volatile bool terminated;


/*
 * Entry point for stdin redirector.
 * stdin redirects stdin to serial (write op).
 */
static void StdInRedirector(HWND parent_hwnd) {
	HANDLE hStdIn = GetStdHandle(STD_INPUT_HANDLE);
	OVERLAPPED overlapped = { 0 };
	TCHAR console_data[4];
	char send_data[4];
	DWORD data_len;

	overlapped.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
	if (overlapped.hEvent == NULL) {
		WinAPIException ex(GetLastError(), _T("SimpleCom"));
		MessageBox(parent_hwnd, ex.GetErrorText(), ex.GetErrorCaption(), MB_OK | MB_ICONERROR);
		return;
	}

	while (!terminated) {
		ReadConsole(hStdIn, console_data, 4, &data_len, nullptr);
		if ((data_len == 3) && (console_data[0] == 0x1b) && (console_data[1] == 0x4f) && (console_data[2] == 0x50)) { // F1 key
			if (MessageBox(parent_hwnd, _T("Do you want to leave from this serial session?"), _T("SimpleCom"), MB_YESNO | MB_ICONQUESTION) == IDYES) {
				terminated = true;
				break;
			}
			else {
				continue;
			}
		}
		else {
			// ReadConsole() is called as ANS mode (pInputControl is NULL)
			for (int i = 0; i < data_len; i++) {
				send_data[i] = console_data[i] & 0xff;
			}
		}

		ResetEvent(overlapped.hEvent);
		DWORD nBytesWritten;
		if (!WriteFile(hSerial, send_data, data_len, &nBytesWritten, &overlapped)) {
			if (GetLastError() == ERROR_IO_PENDING) {
				if (!GetOverlappedResult(hSerial, &overlapped, &nBytesWritten, TRUE)) {
					break;
				}
			}
		}

	}

	CloseHandle(overlapped.hEvent);
}

#define READ_BUF_SIZE 4096

/*
 * Entry point for stdout redirector.
 * stdout redirects serial (read op) to stdout.
 */
DWORD WINAPI StdOutRedirector(_In_ LPVOID lpParameter) {
	HANDLE hStdOut = GetStdHandle(STD_OUTPUT_HANDLE);
	std::vector<char> buf(READ_BUF_SIZE, 0x00);
	DWORD nBytesRead;

	while (!terminated) {
		if (!ResetEvent(serialReadOverlapped.hEvent))
		{
			OutputDebugString(_T("ResetEvent failed.\r\n"));
		}

		if (!ReadFile(hSerial, buf.data(), buf.size(), &nBytesRead, &serialReadOverlapped)) {
			if (GetLastError() == ERROR_IO_PENDING) {
				if (!GetOverlappedResult(hSerial, &serialReadOverlapped, &nBytesRead, TRUE)) {
					OutputDebugString(_T("StdOutRedirector: GetOverlappedResult failed.\r\n"));
					break;
				}
			}
			else
			{
				OutputDebugString(_T("StdOutRedirector: Some error reading the serial port.\r\n"));
			}
		}

		if (nBytesRead > 0) {
			DWORD nBytesWritten;
			WriteFile(hStdOut, buf.data(), nBytesRead, &nBytesWritten, NULL);
		}

	}

	return 0;
}

static HWND GetParentWindow() {
	HWND current = GetConsoleWindow();

	while (true) {
		HWND parent = GetParent(current);

		if (parent == NULL) {
			TCHAR window_text[MAX_PATH] = { 0 };
			GetWindowText(current, window_text, MAX_PATH);

			// If window_text is empty, SimpleCom might be run on Windows Terminal (Could not get valid owner window text)
			return (window_text[0] == 0) ? NULL : current;
		}
		else {
			current = parent;
		}

	}

}

int main()
{
	DCB dcb;
	TString device;
	HWND parent_hwnd = GetParentWindow();

	// Serial port configuration
	try {
		SerialSetup setup;
		if (!setup.ShowConfigureDialog(NULL, parent_hwnd)) {
			return -1;
		}
		device = _T("\\\\.\\") + setup.GetPort();
		setup.SaveToDCB(&dcb);
	}
	catch (WinAPIException e) {
		MessageBox(parent_hwnd, e.GetErrorText(), e.GetErrorCaption(), MB_OK | MB_ICONERROR);
		return -1;
	}
	catch (SerialSetupException e) {
		MessageBox(parent_hwnd, e.GetErrorText(), e.GetErrorCaption(), MB_OK | MB_ICONERROR);
		return -2;
	}

	// Open serial device
	hSerial = CreateFile(device.c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
	if (hSerial == INVALID_HANDLE_VALUE) {
		WinAPIException e(GetLastError(), NULL); // Use WinAPIException to get error string.
		MessageBox(parent_hwnd, e.GetErrorText(), _T("Open serial connection"), MB_OK | MB_ICONERROR);
		return -4;
	}

	TString title = _T("SimpleCom: ") + device;
	SetConsoleTitle(title.c_str());

	if (!SetCommState(hSerial, &dcb))
	{
		DWORD last_error = GetLastError();
		OutputDebugString(_T("ResetEvent failed.\r\n"));
	}
	if (!PurgeComm(hSerial, PURGE_TXABORT | PURGE_RXABORT | PURGE_TXCLEAR | PURGE_RXCLEAR))
	{
		DWORD last_error = GetLastError();
		OutputDebugString(_T("PurgeComm failed.\r\n"));
	}
	if (!SetCommMask(hSerial, EV_RXCHAR))
	{
		DWORD last_error = GetLastError();
		OutputDebugString(_T("SetCommMask failed.\r\n"));
	}
	if (!SetupComm(hSerial, READ_BUF_SIZE, 256))
	{
		DWORD last_error = GetLastError();
		OutputDebugString(_T("SetupComm failed.\r\n"));
	}

	COMMTIMEOUTS comm_timeouts;
	if (!GetCommTimeouts(hSerial, &comm_timeouts))
	{
		DWORD last_error = GetLastError();
		OutputDebugString(_T("GetCommTimeouts failed.\r\n"));
	}

	comm_timeouts.ReadIntervalTimeout = 1;
	comm_timeouts.ReadTotalTimeoutMultiplier = 0;
	comm_timeouts.ReadTotalTimeoutConstant = 10;
	comm_timeouts.WriteTotalTimeoutMultiplier = 0;
	comm_timeouts.WriteTotalTimeoutConstant = 0;
	
	if (!SetCommTimeouts(hSerial, &comm_timeouts))
	{
		DWORD last_error = GetLastError();
		OutputDebugString(_T("SetCommTimeouts failed.\r\n"));
	}

	serialReadOverlapped.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
	serialReadOverlapped.Internal = 0;
	serialReadOverlapped.InternalHigh = 0;
	serialReadOverlapped.Offset = 0;
	serialReadOverlapped.OffsetHigh = 0;
	
	if (serialReadOverlapped.hEvent == NULL) {
		WinAPIException ex(GetLastError(), _T("SimpleCom"));
		MessageBox(parent_hwnd, ex.GetErrorText(), ex.GetErrorCaption(), MB_OK | MB_ICONERROR);
		CloseHandle(hSerial);
		return -1;
	}

	DWORD mode;

	HANDLE hStdIn = GetStdHandle(STD_INPUT_HANDLE);
	GetConsoleMode(hStdIn, &mode);
	mode &= ~ENABLE_PROCESSED_INPUT;
	mode &= ~ENABLE_LINE_INPUT;
	mode |= ENABLE_VIRTUAL_TERMINAL_INPUT;
	SetConsoleMode(hStdIn, mode);

	HANDLE hStdOut = GetStdHandle(STD_OUTPUT_HANDLE);
	GetConsoleMode(hStdOut, &mode);
	mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
	SetConsoleMode(hStdOut, mode);

	terminated = false;

	// Create stdout redirector
	stdoutRedirectorThread = CreateThread(NULL, 0, &StdOutRedirector, NULL, 0, NULL);
	if (stdoutRedirectorThread == NULL) {
		WinAPIException ex(GetLastError(), _T("SimpleCom"));
		MessageBox(parent_hwnd, ex.GetErrorText(), ex.GetErrorCaption(), MB_OK | MB_ICONERROR);
		CloseHandle(hSerial);
		return -2;
	}

	// stdin redirector would perform in current thread
	StdInRedirector(parent_hwnd);

	// stdin redirector should be finished at this point.
	// It means end of serial communication. So we should terminate stdout redirector.
	CancelIoEx(hSerial, &serialReadOverlapped);
	WaitForSingleObject(stdoutRedirectorThread, INFINITE);

	CloseHandle(hSerial);
	CloseHandle(serialReadOverlapped.hEvent);

	return 0;
}
