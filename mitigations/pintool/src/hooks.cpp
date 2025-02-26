#include <iostream>

#include "config.h"
#include "hooks.h"
#include "memory.h"

/**********************************************************

This function parses the ntdll library in order to create 
an array for syscalls, taking syscall number as index of 
the array and name of syscall as cell content.

***********************************************************/

VOID HOOKS_EnumSyscalls() {

	unsigned char *image = (unsigned char *)W::GetModuleHandle("ntdll");
	W::IMAGE_DOS_HEADER *dos_header = (W::IMAGE_DOS_HEADER *) image;
	W::IMAGE_NT_HEADERS *nt_headers = (W::IMAGE_NT_HEADERS *)(image + dos_header->e_lfanew);
	W::IMAGE_DATA_DIRECTORY *data_directory = &nt_headers->
		OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
	W::IMAGE_EXPORT_DIRECTORY *export_directory = (W::IMAGE_EXPORT_DIRECTORY *)(image + data_directory->VirtualAddress);
	// RVAs from image base
	W::DWORD *address_of_names = (W::DWORD*)(image + export_directory->AddressOfNames);
	W::DWORD *address_of_functions = (W::DWORD*)(image + export_directory->AddressOfFunctions);
	UINT16 *address_of_name_ordinals = (W::UINT16*)(image + export_directory->AddressOfNameOrdinals);
	// NumberOfNames can be 0: in that case the module will export by ordinal only 
	W::DWORD number_of_names = MIN(export_directory->NumberOfFunctions, export_directory->NumberOfNames);
	size_t ntcalls = 0, zwcalls = 0;

	for (W::DWORD i = 0; i < number_of_names; i++) {
		// AddressOfNameOrdinals contains the ordinals associated with the function names in AddressOfNames
		const char *name = (const char *)(image + address_of_names[i]);
		// AddressOfFunctions points to an array of RVAs of the functions/symbols in the module
		unsigned char *addr = image + address_of_functions[address_of_name_ordinals[i]];
		if (!memcmp(name, "Zw", 2) || !memcmp(name, "Nt", 2)) {
			// does the signature match one of these cases?
			// 1:   mov eax, syscall_number ; mov ecx, some_value
			// 2:   mov eax, syscall_number ; xor ecx, ecx
			// 3:   mov eax, syscall_number ; mov edx, 0x7ffe0300
			// DCD remember to add one more case if we go for Windows 8+
			if (addr[0] == 0xb8 && (addr[5] == 0xb9 || addr[5] == 0x33 || addr[5] == 0xba)) {
				ADDRINT syscall_number = *(UINT32*)(addr + 1);
				// DCD by using a map for every Zw/Nt pair we will skip duplicates
				ASSERT(syscall_number < MAXSYSCALLS, "Syscall ID from NTDLL too high - please increase MAXSYSCALLS");
				if (!syscallIDs[syscall_number] || !memcmp(name, "Nt", 2)) {
					syscallIDs[syscall_number] = strdup(name);
				}
			}
		}
	}

}

// internal method to look up syscall index by name
INT lookupIndex(CONST CHAR* syscallName) {
	for (int i = 0; i < MAXSYSCALLS; ++i) {
		if (!strcmp(syscallIDs[i], syscallName)) return i;
	}

	ASSERT(false, "Unknown system call number");
	return 0;
}

/**********************************************************

This function extracts 'count' system call arguments using 
PIN_GetSyscallArgument 

***********************************************************/

VOID HOOKS_SyscallGetArguments(CONTEXT *ctx, SYSCALL_STANDARD std, int count, ...) {
	va_list args;
	va_start(args, count);
	for (int i = 0; i < count; i++) {
		int index = va_arg(args, int);
		ADDRINT *ptr = va_arg(args, ADDRINT *);
		*ptr = PIN_GetSyscallArgument(ctx, std, index);
	}
	va_end(args);
}

/**********************************************************

This function initializes TLS data for a thread. Fine as
TLS is used for hooks only, otherwise should be moved.

***********************************************************/

VOID HOOKS_SetTLSKey(THREADID tid) {
	
	pintool_tls* tdata = new pintool_tls; // POD is zero-initialized

	if (PIN_SetThreadData(tls_key, tdata, tid) == FALSE) {
		LOG_AR("PIN_SetThreadData failed");
		PIN_ExitProcess(1);
	}

}

/**********************************************************

This function is a callback called when the entrypoint of a
syscall is reached. It checks whether there is a hook on
entry and/or exit registered for it, and saves all necessary
arguments. We store arguments in one place for both hooks.

***********************************************************/

VOID HOOKS_SyscallEntry(THREADID thread_id, CONTEXT *ctx, SYSCALL_STANDARD std, void *v) {

	// get the syscall number
	ADDRINT syscall_number = PIN_GetSyscallNumber(ctx, std);

	if (syscall_number == 0) {
		LOG_AR("==> WARNING: PIN returned 0 as system call number, possible int 2e case?");
		return;
	}

	if (!(_rwKnob || _nxKnob)) return; // TODO check as we add more hooks

	if (syscall_number < MAXSYSCALLS) {
		syscall_hook hookEntry = sysEntryHooks[syscall_number];
		syscall_hook hookExit = sysExitHooks[syscall_number];

		if (hookEntry || hookExit) { // fill sc (we may have a hook on exit only)
			pintool_tls *tdata = static_cast<pintool_tls*>(PIN_GetThreadData(tls_key, thread_id));
			syscall_t *sc = &tdata->sc;
			sc->syscall_number = syscall_number;
									 
			// 0 .. 7 -> &sc->arg0 .. &sc->arg7 to fill sc using a variadic function
			// TODO selectively fill by number of arguments (Pin fetches garbage now)
			HOOKS_SyscallGetArguments(ctx, std, SYSCALL_NUM_ARG,
				0, &sc->arg0, 1, &sc->arg1, 2, &sc->arg2, 3, &sc->arg3,
				4, &sc->arg4, 5, &sc->arg5, 6, &sc->arg6, 7, &sc->arg7,
				8, &sc->arg8, 9, &sc->arg9, 10, &sc->arg10, 11, &sc->arg11);

			// call on-entry hook
			if (hookEntry) hookEntry(sc, ctx, std);
		}
	}
}

/**********************************************************

This function is a callback for sycall exit events.

***********************************************************/

VOID HOOKS_SyscallExit(THREADID thread_id, CONTEXT *ctx, SYSCALL_STANDARD std, void *v) {

	pintool_tls *tdata = static_cast<pintool_tls*>(PIN_GetThreadData(tls_key, thread_id));
	syscall_t *sc = &tdata->sc;

	if (!(_rwKnob || _nxKnob)) return; // TODO check as we add more hooks

	if (sc->syscall_number < MAXSYSCALLS) {
		syscall_hook hook = sysExitHooks[sc->syscall_number];
		if (hook) hook(sc, ctx, std);
	} else {
		// win32k system calls have higher ordinals (e.g. 0x2007)
		// see https://j00ru.vexillium.org/syscalls/win32k/32/
		// we use GenericScan to apx detect memory allocated by them
		// GDI stuff on Win7 SP1 from our experiments
		// - 0x10fa NtGdiPolyTextOutW
		// - 0x12db iNtGdiDrawStream
		if (sc->syscall_number == 0x10fa || sc->syscall_number == 0x12db)
			HOOKS_GenericScan_exit(sc, ctx, std);
	}

}

VOID HOOKS_GenericScan_exit(syscall_t *sc, CONTEXT *ctx, SYSCALL_STANDARD std) {
	if (MEMORY_AddMappedMemoryStar(NULL, 0x7fff0000, true)) {
		//LOG_AR("SYSCALL %08x", sc->syscall_number);
	}
}


VOID HOOKS_AddSyscalls() {
	// for functions whose semantics is to allocate memory
	// we wrote custom hooks that reason on their arguments,
	// while for other functions we used for now the generic
	// handler to detect memory possibly allocated by them

	//sysEntryHooks[lookupIndex("NtUnmapViewOfSection")] = &HOOKS_NtUnmapViewOfSection_entry;
	
	if (!_rwKnob && !_nxKnob) goto END;

	sysExitHooks[lookupIndex("NtProtectVirtualMemory")] = &HOOKS_NtProtectVirtualMemory_exit; //&HOOKS_GenericScan_exit;
	sysExitHooks[lookupIndex("NtUnmapViewOfSection")] = &HOOKS_GenericScan_exit; // &HOOKS_NtUnmapViewOfSection_exit;
	sysExitHooks[lookupIndex("NtFreeVirtualMemory")] = &HOOKS_NtFreeVirtualMemory_exit;
	sysExitHooks[lookupIndex("NtFreeUserPhysicalPages")] = &HOOKS_NtFreeUserPhysicalPages_exit;
	sysExitHooks[lookupIndex("NtQueryVirtualMemory")] = &HOOKS_NtQueryVirtualMemory_exit;
	sysExitHooks[lookupIndex("NtAllocateVirtualMemory")] = &HOOKS_NtAllocateVirtualMemory_exit;
	sysExitHooks[lookupIndex("NtAllocateUserPhysicalPages")] = &HOOKS_GenericScan_exit; // &HOOKS_NtAllocateUserPhysicalPages_exit;
	sysExitHooks[lookupIndex("NtMapViewOfSection")] = &HOOKS_NtMapViewOfSection_exit;

	sysExitHooks[lookupIndex("NtGetMUIRegistryInfo")] = &HOOKS_GenericScan_exit; //&HOOKS_NtGetMUIRegistryInfo_exit;
	sysExitHooks[lookupIndex("NtQueryValueKey")] = &HOOKS_GenericScan_exit;
	sysExitHooks[lookupIndex("NtRequestWaitReplyPort")] = &HOOKS_GenericScan_exit;
	sysExitHooks[lookupIndex("NtClose")] = &HOOKS_GenericScan_exit;
	sysExitHooks[lookupIndex("NtCreateSection")] = &HOOKS_GenericScan_exit;
	sysExitHooks[lookupIndex("NtMapCMFModule")] = &HOOKS_GenericScan_exit;
	sysExitHooks[lookupIndex("NtOpenFile")] = &HOOKS_GenericScan_exit;
	sysExitHooks[lookupIndex("NtOpenKey")] = &HOOKS_GenericScan_exit;
	sysExitHooks[lookupIndex("NtOpenProcessToken")] = &HOOKS_GenericScan_exit;
	sysExitHooks[lookupIndex("NtOpenProcessTokenEx")] = &HOOKS_GenericScan_exit;
	sysExitHooks[lookupIndex("NtOpenSection")] = &HOOKS_GenericScan_exit;
	sysExitHooks[lookupIndex("NtQueryAttributesFile")] = &HOOKS_GenericScan_exit;
	sysExitHooks[lookupIndex("NtQueryInformationProcess")] = &HOOKS_GenericScan_exit;
	sysExitHooks[lookupIndex("NtQueryInformationToken")] = &HOOKS_GenericScan_exit;
	sysExitHooks[lookupIndex("NtQuerySection")] = &HOOKS_GenericScan_exit;
	//sysExitHooks[lookupIndex("")] = &HOOKS_GenericScan_exit;
	

END:
	PIN_AddSyscallEntryFunction(&HOOKS_SyscallEntry, NULL);
	PIN_AddSyscallExitFunction(&HOOKS_SyscallExit, NULL);

}

VOID HOOKS_Init() {
	HOOKS_EnumSyscalls();
	HOOKS_AddSyscalls();
}


VOID HOOKS_NtProtectVirtualMemory_exit(syscall_t *sc, CONTEXT *ctx, SYSCALL_STANDARD std) {

	W::PVOID baseAddr = *(W::PVOID*)sc->arg1;
	// we were getting erratic results for size, probably needs to be saved on entry
	//W::PULONG size = (W::PULONG)sc->arg2; // on output gets rounded to page size
	//W::ULONG protect = (W::ULONG)sc->arg3;

	ADDRINT _eax = PIN_GetContextReg(ctx, REG_GAX);
	if (_eax != 0) return;

	ADDRINT _startAddr, _size;
	MEM_MASK _mask;
	MEMORY_QueryWindows((ADDRINT)baseAddr, &_startAddr, &_size, &_mask);

	// memory hook
	MEMORY_CheckPermissionsForArea(_startAddr, _size, _mask);
}

VOID HOOKS_NtGetMUIRegistryInfo_exit(syscall_t *sc, CONTEXT *ctx, SYSCALL_STANDARD std) {
	W::PVOID data = (W::PVOID)sc->arg2;

	ADDRINT _eax = PIN_GetContextReg(ctx, REG_GAX);
	if (_eax != 0) return; // leap of faith

	ADDRINT _startAddr, _size;
	MEM_MASK _mask;
	MEMORY_QueryWindows((ADDRINT)data, &_startAddr, &_size, &_mask);

	// memory hook
	MEMORY_RegisterArea(_startAddr, _size, _mask);
}


// TODO: move to TLS field; what about AllocationBase?
W::SIZE_T storage_NtUnmapViewOfSection;
VOID HOOKS_NtUnmapViewOfSection_entry(syscall_t *sc, CONTEXT *ctx, SYSCALL_STANDARD std) {
	W::PVOID baseAddr = (W::PVOID)sc->arg1;

	W::MEMORY_BASIC_INFORMATION memInfo;
	W::VirtualQuery(baseAddr, &memInfo, sizeof(memInfo));

	ASSERT(baseAddr == memInfo.AllocationBase, "Unaligned base for NtUnmapViewOfSection");
	
	// necessary for memory hook
	storage_NtUnmapViewOfSection = memInfo.RegionSize;
}

VOID HOOKS_NtUnmapViewOfSection_exit(syscall_t * sc, CONTEXT * ctx, SYSCALL_STANDARD std) {

	W::PVOID baseAddr = (W::PVOID)sc->arg1;

	ADDRINT _eax = PIN_GetContextReg(ctx, REG_GAX);
	if (_eax != 0) return;

	// memory hook
	//LOG_AR("%08x %08x", (ADDRINT)baseAddr, storage_NtUnmapViewOfSection);
	MEMORY_UnregisterArea((ADDRINT)baseAddr, storage_NtUnmapViewOfSection);
}


VOID HOOKS_NtFreeVirtualMemory_exit(syscall_t * sc, CONTEXT * ctx, SYSCALL_STANDARD std) {

	W::PVOID *baseAddr = (W::PVOID*)sc->arg1;
	W::PSIZE_T size = (W::PSIZE_T)sc->arg2;
	W::ULONG type = (W::ULONG)sc->arg3;

	ADDRINT _eax = PIN_GetContextReg(ctx, REG_GAX);
	if (_eax != 0) return;

	// memory hook
	if (type == MEM_RELEASE) {
		LOG_AR("%08x %08x", (ADDRINT)*baseAddr, *size);
		MEMORY_UnregisterArea((ADDRINT)*baseAddr, *size);
	}

}



VOID HOOKS_NtFreeUserPhysicalPages_exit(syscall_t * sc, CONTEXT * ctx, SYSCALL_STANDARD std) {

	W::PULONG_PTR  userArray = (W::PULONG_PTR)sc->arg2;
	W::PULONG_PTR  numberOfPages = (W::PULONG_PTR)sc->arg1;

	ADDRINT _eax = PIN_GetContextReg(ctx, REG_GAX);
	if (_eax != 0) return;

	// TODO this one seems nasty
	//HOOKS_GenericScan_exit(sc, ctx, std); // perhaps not really needed? at least in our tests

}


VOID HOOKS_NtQueryVirtualMemory_exit(syscall_t * sc, CONTEXT * ctx, SYSCALL_STANDARD std) {

	W::PVOID baseAddr = (W::PVOID)sc->arg1;
	MEMORY_BASIC_INFORMATION *mem = (MEMORY_BASIC_INFORMATION*)sc->arg3;

	ADDRINT _eax = PIN_GetContextReg(ctx, REG_GAX);
	if (_eax != 0) return;

	// TODO: use it to refresh our map? as of now nothing to do here
	/*
	cout << "NtQueryVirtualMemory --->" <<
		"Allocation Base: 0x" << hex << mem->AllocationBase << endl <<
		"Base Address: 0x" << hex << mem->BaseAddress << endl <<
		"Size: 0x" << hex << mem->RegionSize << endl <<
		"Protect: 0x" << hex << mem->Protect << endl << endl;
		*/

}


VOID HOOKS_NtAllocateVirtualMemory_exit(syscall_t * sc, CONTEXT * ctx, SYSCALL_STANDARD std) {

	W::PVOID baseAddr = *(W::PVOID*)sc->arg1;
	W::PSIZE_T size = (W::PSIZE_T)sc->arg3;
	W::ULONG allocationType = (W::ULONG)sc->arg4;
	W::ULONG protect = (W::ULONG)sc->arg5;

	ADDRINT _eax = PIN_GetContextReg(ctx, REG_GAX);
	if (_eax != 0) return;

	// memory hook
	MEMORY_RegisterArea((ADDRINT)baseAddr, *size, MEMORY_WinToPinCast(protect));

	/*
	cout << "NtAllocateVirtualMemory -> " << endl
		<< "BaseAddress: 0x" << hex << (UINT32)*baseAddr << endl
		<< "EndAddress: 0x" << hex << (UINT32)*baseAddr + *size << endl
		<< "Size: " << *size << endl
		<< "Allocation Type: " << hex << allocationType << endl
		<< "Protection: " << hex << protect << endl << endl;
	*/
}


// UNUSED: we are relying on the generic scanner for now...
VOID HOOKS_NtAllocateUserPhysicalPages_exit(syscall_t * sc, CONTEXT * ctx, SYSCALL_STANDARD std) {

	W::PULONG_PTR  baseAddr = (W::PULONG_PTR)sc->arg1;
	W::PULONG_PTR  size = (W::PULONG_PTR)sc->arg2;

	ADDRINT _eax = PIN_GetContextReg(ctx, REG_GAX);
	if (_eax != 0) return;
	
	// TODO this one seems nasty
	//HOOKS_GenericScan_exit(sc, ctx, std);
}


VOID HOOKS_NtMapViewOfSection_exit(syscall_t * sc, CONTEXT * ctx, SYSCALL_STANDARD std) {
	#define STATUS_IMAGE_NOT_AT_BASE 0x40000003
	W::PVOID baseAddr = *(W::PVOID*)sc->arg2;
	W::PSIZE_T size = (W::PSIZE_T)sc->arg6;
	W::ULONG protect = (W::ULONG)sc->arg9;

	ADDRINT _eax = PIN_GetContextReg(ctx, REG_GAX);
	if (_eax != 0 && _eax != STATUS_IMAGE_NOT_AT_BASE) return;

	ADDRINT _startAddr, _size;
	MEM_MASK _mask;
	MEMORY_QueryWindows((ADDRINT)baseAddr, &_startAddr, &_size, &_mask);

	// memory hook
	MEMORY_RegisterArea(_startAddr, _size, _mask);
}

// UNUSED
VOID HOOKS_GenericHookDereference_exit(syscall_t *sc, CONTEXT *ctx, UINT32 argNum) {
	W::PHANDLE pHandle = (W::PHANDLE)sc->arg0;

	ADDRINT _eax = PIN_GetContextReg(ctx, REG_GAX);
	if (_eax != 0) return;

	if (pHandle != NULL) {
		LOG_AR(" %08x", (ADDRINT)*pHandle);
		//return; // TODO why?
	}

	ADDRINT _startAddr, _size;
	MEM_MASK _mask;
	MEMORY_QueryWindows((ADDRINT)*pHandle, &_startAddr, &_size, &_mask);

	// memory hook
	MEMORY_CheckPermissionsForArea(_startAddr, _size, _mask);
}

