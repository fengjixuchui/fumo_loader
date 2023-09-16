#include "fumo_loader.h"
#include <winternl.h>

using fnLdrLoadDll = NTSTATUS(NTAPI*)(PWCHAR PathToFile, ULONG Flags, PUNICODE_STRING ModuleFileName, HMODULE* ModuleHandle);
using fnLdrGetProcedureAddress = NTSTATUS(NTAPI*)(HMODULE ModuleHandle, PANSI_STRING FunctionName, WORD Oridinal, PVOID* FunctionAddress);
using fnRtlAnsiStringToUnicodeString = decltype(&RtlAnsiStringToUnicodeString);
using fnDllMain = BOOL(WINAPI*)(HMODULE hModule, DWORD dwReason, LPVOID lpReserved);

typedef struct _MANUAL_MAPPING_DATA {
	PVOID ImageBase;
	fnLdrLoadDll LdrLoadDll;
	fnLdrGetProcedureAddress LdrGetProcedureAddress;
	fnRtlAnsiStringToUnicodeString RtlAnsiStringToUnicodeString;
} MANUAL_MAPPING_DATA, *PMANUAL_MAPPING_DATA;

MANUAL_MAPPING_DATA GetManualMappingData(PVOID pImageBase) {
	HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll");

	MANUAL_MAPPING_DATA data = {0};
	data.ImageBase = pImageBase;
	data.LdrLoadDll = (fnLdrLoadDll)GetProcAddress(hNtdll, "LdrLoadDll");
	data.LdrGetProcedureAddress = (fnLdrGetProcedureAddress)GetProcAddress(hNtdll, "LdrGetProcedureAddress");
	data.RtlAnsiStringToUnicodeString = (fnRtlAnsiStringToUnicodeString)GetProcAddress(hNtdll, "RtlAnsiStringToUnicodeString");
	return data;
}

DWORD Shellcode(PMANUAL_MAPPING_DATA pMmData) {
	// resolve imports
	auto nt_headers = (PIMAGE_NT_HEADERS)((ULONG_PTR)pMmData->ImageBase + ((PIMAGE_DOS_HEADER)pMmData->ImageBase)->e_lfanew);
	auto import_descriptor = (PIMAGE_IMPORT_DESCRIPTOR)((ULONG_PTR)pMmData->ImageBase + nt_headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);

	while (import_descriptor->Name != 0) {
		auto module_name = (PCHAR)((ULONG_PTR)pMmData->ImageBase + import_descriptor->Name);

		USHORT module_name_length = 0;
		while (module_name[module_name_length] != 0)
			module_name_length++;

		ANSI_STRING ansi_module_name = {0};
		ansi_module_name.Buffer = module_name;
		ansi_module_name.Length = module_name_length;
		ansi_module_name.MaximumLength = module_name_length + 1;

		UNICODE_STRING unicode_module_name = {0};
		pMmData->RtlAnsiStringToUnicodeString(&unicode_module_name, &ansi_module_name, TRUE);

		HMODULE module_handle = nullptr;
		pMmData->LdrLoadDll(nullptr, 0, &unicode_module_name, &module_handle);

		auto original_first_thunk = (PIMAGE_THUNK_DATA)((ULONG_PTR)pMmData->ImageBase + import_descriptor->OriginalFirstThunk);
		auto first_thunk = (PIMAGE_THUNK_DATA)((ULONG_PTR)pMmData->ImageBase + import_descriptor->FirstThunk);

		while (original_first_thunk->u1.AddressOfData != 0) {
			auto import_by_name = (PIMAGE_IMPORT_BY_NAME)((ULONG_PTR)pMmData->ImageBase + original_first_thunk->u1.AddressOfData);
			auto function_name = (PCHAR)import_by_name->Name;

			USHORT function_name_length = 0;
			while (function_name[function_name_length] != 0)
				function_name_length++;

			ANSI_STRING ansi_function_name = {0};
			ansi_function_name.Buffer = function_name;
			ansi_function_name.Length = function_name_length;
			ansi_function_name.MaximumLength = function_name_length + 1;

			PVOID function_address = nullptr;
			pMmData->LdrGetProcedureAddress(module_handle, &ansi_function_name, 0, &function_address);

			first_thunk->u1.Function = (ULONG_PTR)function_address;

			original_first_thunk++;
			first_thunk++;
		}

		import_descriptor++;
	}

	// call entry point
	auto entry_point = (fnDllMain)((ULONG_PTR)pMmData->ImageBase + nt_headers->OptionalHeader.AddressOfEntryPoint);
	entry_point((HMODULE)pMmData->ImageBase, DLL_PROCESS_ATTACH, nullptr);

	return ERROR_SUCCESS;
}

VOID Shellcode_End() {}

DWORD fumo::MapImage(DriverInterface* pDriver, ULONG pid, PVOID pImage) {
	// parse the PE header
	auto dos_header = (PIMAGE_DOS_HEADER)pImage;
	if (dos_header->e_magic != IMAGE_DOS_SIGNATURE)
		return ERROR_BAD_EXE_FORMAT;

	// parse the NT header
	auto nt_headers = (PIMAGE_NT_HEADERS)((ULONG_PTR)pImage + dos_header->e_lfanew);
	if (nt_headers->Signature != IMAGE_NT_SIGNATURE)
		return ERROR_BAD_EXE_FORMAT;

	// make sure the image is 64-bit
	if (nt_headers->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64)
		return ERROR_BAD_EXE_FORMAT;

	ULONG size_of_shellcode = (ULONG)((SIZE_T)Shellcode_End - (SIZE_T)Shellcode);
	ULONG size_of_shellcode_data = sizeof(MANUAL_MAPPING_DATA);
	auto size_of_image = nt_headers->OptionalHeader.SizeOfImage;
	auto size_of_mapping = size_of_image + size_of_shellcode + size_of_shellcode_data;

	auto kernel_image = pDriver->AllocateKernelMemory(size_of_mapping);

	// copy headers
	memcpy(kernel_image, pImage, nt_headers->OptionalHeader.SizeOfHeaders);

	// map sections
	auto section_header = IMAGE_FIRST_SECTION(nt_headers);
	for (auto i = 0; i < nt_headers->FileHeader.NumberOfSections; i++) {
		auto section = &section_header[i];
		if (section->SizeOfRawData == 0)
			continue;
		auto section_data = (PVOID)((ULONG_PTR)pImage + section->PointerToRawData);
		memcpy((PVOID)((ULONG_PTR)kernel_image + section->VirtualAddress), section_data, section->SizeOfRawData);
	}

	// relocate the image
	auto delta = (ULONG_PTR)kernel_image - nt_headers->OptionalHeader.ImageBase;
	if (delta != 0) {
		auto base_relocation = (PIMAGE_BASE_RELOCATION)((ULONG_PTR)kernel_image + nt_headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].VirtualAddress);
		while (base_relocation->VirtualAddress != 0) {
			auto relocation = (PWORD)((ULONG_PTR)base_relocation + sizeof(IMAGE_BASE_RELOCATION));
			auto number_of_relocations = (base_relocation->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(WORD);
			for (auto i = 0; i < number_of_relocations; i++) {
				if (relocation[i] >> 12 == IMAGE_REL_BASED_DIR64) {
					auto address = (PULONG_PTR)((ULONG_PTR)kernel_image + base_relocation->VirtualAddress + (relocation[i] & 0xFFF));
					*address += delta;
				}
			}
			base_relocation = (PIMAGE_BASE_RELOCATION)((ULONG_PTR)base_relocation + base_relocation->SizeOfBlock);
		}
	}

	// write the shellcode
	auto shellcode_addr = (PVOID)((ULONG_PTR)kernel_image + size_of_image);
	memcpy(shellcode_addr, Shellcode, size_of_shellcode);

	// write the manual mapping data
	auto ManualMappingData = GetManualMappingData(kernel_image);
	auto manual_mapping_data_addr = (PMANUAL_MAPPING_DATA)((ULONG_PTR)shellcode_addr + size_of_shellcode);
	memcpy(manual_mapping_data_addr, &ManualMappingData, size_of_shellcode_data);

	if (!pDriver->ExposeKernelMemory(pid, kernel_image, size_of_mapping))
		return GetLastError();

	if (!pDriver->ExecuteCode(pid, shellcode_addr, manual_mapping_data_addr))
		return GetLastError();

	return ERROR_SUCCESS;
}
