#include <Windows.h>
#include <Psapi.h>
#include <PathCch.h>
#include <tchar.h>

#include <map>
#include <string>

#include "detours/detours.h"

using MologieDetours::Detour;

using std::map;
using std::string;
using std::wstring;

static unsigned long imageBase;
static unsigned long imageSize;

map<wstring, HMODULE> moduleTable;

WCHAR moduleDir[MAX_PATH];

bool sigTest(const char *data, const char *pattern, const char *mask)
{
	for (; *mask; ++data, ++pattern, ++mask)
	{
		if (*mask == 'x' && *data != *pattern)
			return false;
	}

	return *mask == NULL;
}

void *sigFind(const char *pattern, const char *mask)
{
	unsigned long i = imageBase;
	unsigned long end = i + imageSize - strlen(mask);

	for (; i < end; i++)
	{
		if (sigTest((char *)i, pattern, mask))
			return (void *)i;
	}

	return 0;
}

typedef bool(*BoolCallback)(void *obj, int argc, const char* argv[]);

void *StringTable;

typedef void(*Sim__init_t)(void);
Sim__init_t Sim__init;
typedef void(*Con__printf_t)(const char *format, ...);
Con__printf_t Con__printf;

typedef void *(*LookupNamespace_t)(const char *ns);
LookupNamespace_t LookupNamespace;

typedef const char *(__thiscall *StringTableInsert_t)(void *this_, const char *str, const bool caseSensitive);
StringTableInsert_t StringTableInsert;

typedef void (__thiscall *AddBoolCommand_t)(void *ns, const char* name, BoolCallback cb, const char *usage, int minArgs, int maxArgs);
AddBoolCommand_t AddBoolCommand;

void ConsoleFunction(const char* nameSpace, const char* name, BoolCallback callBack, const char* usage, int minArgs, int maxArgs)
{
	AddBoolCommand(LookupNamespace(nameSpace), StringTableInsert(StringTable, name, false), callBack, usage, minArgs, maxArgs);
}

void printError(char *format)
{
	DWORD dw = GetLastError();
	LPVOID lpMsgBuf;
	FormatMessageA(
		FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
		NULL, dw, MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL), (LPSTR)&lpMsgBuf, 0, NULL);
	Con__printf(format, lpMsgBuf);
	LocalFree(lpMsgBuf);
}

bool doDetachModule(wstring name)
{
	HMODULE module = moduleTable[name];
	if (module == NULL)
		return false;

	char convert[MAX_PATH];
	if (WideCharToMultiByte(CP_ACP, 0, name.c_str(), -1, convert, MAX_PATH, "?", NULL) == 0)
	{
		Con__printf("Detaching module (failed to convert string)");
		printError("\x3%s");
	}
	else
		Con__printf("Detaching module '%s'", convert);

	if (FreeLibrary(module))
	{
		moduleTable[name] = NULL;
		return true;
	}

	printError("   \x03" "Failed to detach: %s");
	return false;
}

HMODULE doAttachModule(wstring name)
{
	doDetachModule(name);
	char convert[MAX_PATH];
	if (WideCharToMultiByte(CP_ACP, 0, name.c_str(), -1, convert, MAX_PATH, "?", NULL) == 0)
	{
		Con__printf("Attaching module (failed to convert string)");
		printError("\x3%s");
	}
	else
		Con__printf("Attaching module '%s'", convert);

	WCHAR filename[MAX_PATH];
	wcscpy_s(filename, moduleDir);
	PathCchAppend(filename, MAX_PATH, name.c_str());

	HMODULE module = LoadLibraryW(filename);
	if (module == NULL)
		printError("   \x03" "Failed to attach: %s");
	else
		moduleTable[name] = module;

	return module;
}

bool tsAttachModule(void *obj, int argc, const char *argv[])
{
	WCHAR convert[MAX_PATH];
	if (MultiByteToWideChar(CP_ACP, 0, argv[1], -1, convert, MAX_PATH) == 0)
	{
		printError("\x03" "Failed to convert TS string to UTF-16: %s");
		return false;
	}
	return doAttachModule(convert) != NULL;
}

bool tsDetachModule(void *obj, int argc, const char *argv[])
{
	WCHAR convert[MAX_PATH];
	if (MultiByteToWideChar(CP_ACP, 0, argv[1], -1, convert, MAX_PATH) == 0)
	{
		printError("\x03" "Failed to convert TS string to UTF-16: %s");
		return false;
	}
	return doDetachModule(convert);
}

Detour<Sim__init_t> *detour_Sim__init;

void hook_Sim__init(void)
{
	Con__printf = (Con__printf_t)sigFind("\x8B\x4C\x24\x04\x8D\x44\x24\x08\x50\x6A\x00\x6A\x00\xE8\x00\x00\x00\x00\x83\xC4\x0C\xC3", "xxxxxxxxxxxxxx????xxxx");

	if (Con__printf == NULL)
		return detour_Sim__init->GetOriginalFunction()();

	Con__printf("BlocklandLoader Init:");
	
	if (GetModuleFileNameW(NULL, moduleDir, MAX_PATH) == 0
	 || PathCchRemoveFileSpec(moduleDir, MAX_PATH) != S_OK
	 || PathCchAppend(moduleDir, MAX_PATH, L"modules") != S_OK)
	{
		printError("   \x03" "Failed to determine directory: %s");
		return detour_Sim__init->GetOriginalFunction()();
	}

	LookupNamespace = (LookupNamespace_t)sigFind("\x8B\x44\x24\x04\x85\xC0\x75\x05", "xxxxxxxx");
	StringTableInsert = (StringTableInsert_t)sigFind("\x53\x8B\x5C\x24\x08\x55\x56\x57\x53", "xxxxxxxxx");
	StringTable = (void *)(*(unsigned int *)(*(unsigned int *)((unsigned int)LookupNamespace + 15)));

	AddBoolCommand = (AddBoolCommand_t)sigFind(
		"\x8B\x44\x24\x04\x56\x50\xE8\x00\x00\x00\x00\x8B\xF0\xA1\x00\x00\x00\x00\x40\xB9\x00\x00\x00\x00\xA3"
		"\x00\x00\x00\x00\xE8\x00\x00\x00\x00\x8B\x4C\x24\x10\x8B\x54\x24\x14\x8B\x44\x24\x18\x89\x4E\x18\x8B"
		"\x4C\x24\x0C\x89\x56\x10\x89\x46\x14\xC7\x46\x0C\x05\x00\x00\x00\x89\x4E\x28\x5E\xC2\x14\x00",
		"xxxxxxx????xxx????xx????x????x????xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx");

	// TODO: Inform the user if the above searches fail

	ConsoleFunction(NULL, "attachModule", tsAttachModule, "attachModule(string filename)", 2, 2);
	ConsoleFunction(NULL, "detachModule", tsDetachModule, "detachModule(string filename)", 2, 2);

	bool foundAny = false;

	WCHAR moduleSearch[MAX_PATH];
	wcscpy_s(moduleSearch, moduleDir);
	PathCchAppend(moduleSearch, MAX_PATH, L"*.dll");

	char convert[MAX_PATH];
	if (WideCharToMultiByte(CP_ACP, 0, moduleSearch, -1, convert, MAX_PATH, "?", NULL) == 0)
	{
		Con__printf("   Search path: (failed to convert string)");
		printError("      \x3%s");
	}
	else
		Con__printf("   Search path: %s", convert);
	Con__printf("");

	WIN32_FIND_DATAW ffd;
	HANDLE hFind = FindFirstFileW(moduleSearch, &ffd);

	if (hFind != INVALID_HANDLE_VALUE) do
	{
		if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			continue;

		foundAny = true;
		doAttachModule(ffd.cFileName);
	} while (FindNextFileW(hFind, &ffd) != 0);

	if (!foundAny)
	{
		Con__printf("BlocklandLoader found no DLLs in the 'modules' directory");
		Con__printf("--------------------------------------------------------");
	}

	Con__printf("");

	return detour_Sim__init->GetOriginalFunction()();
}

bool __stdcall DllMain(void *, unsigned int reason, void *)
{
	if (reason == DLL_PROCESS_ATTACH)
	{
		MODULEINFO info;
		GetModuleInformation(GetCurrentProcess(), GetModuleHandle(NULL), &info, sizeof MODULEINFO);

		imageBase = (unsigned long)info.lpBaseOfDll;
		imageSize = info.SizeOfImage;

		Sim__init = (Sim__init_t)sigFind("\x56\x33\xF6\x57\x89\x35", "xxxxxx");

		if (Sim__init)
			detour_Sim__init = new Detour<Sim__init_t>(Sim__init, hook_Sim__init);
		else
			return false;
	}

	return true;
}

extern "C" void __declspec(dllexport) __cdecl loader() {}