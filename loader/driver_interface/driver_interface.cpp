#include "driver_interface.h"

std::optional<std::reference_wrapper<fumo::DriverInterface>> fumo::DriverInterface::Open(LPCWSTR lpFileName) {
    HANDLE hDevice = CreateFileW(lpFileName, GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (hDevice == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }
    return std::optional<std::reference_wrapper<fumo::DriverInterface>>(*new DriverInterface(hDevice));
}

ULONG fumo::DriverInterface::GetVersion() {
    IO_VERSION_RESPONSE_DATA version_response = {0};
    if (!DeviceIoControl(hDevice, IO_VERSION_REQUEST, 
        nullptr, 0, 
        &version_response, sizeof(version_response), 
        nullptr, nullptr)) {
        return 0;
    }
    return version_response.Version;
}

VOID fumo::DriverInterface::Unload() {
    DeviceIoControl(hDevice, IO_UNLOAD_REQUEST, 
        nullptr, 0, 
        nullptr, 0, 
        nullptr, nullptr);
}

PVOID fumo::DriverInterface::AllocateKernelMemory(ULONG size) {
    IO_ALLOC_REQUEST_DATA alloc_request = {0};
    alloc_request.Size = size;

    IO_ALLOC_RESPONSE_DATA alloc_response = {0};
    if (!DeviceIoControl(hDevice, IO_ALLOC_REQUEST, 
        &alloc_request, sizeof(alloc_request), 
        &alloc_response, sizeof(alloc_response), 
        nullptr, nullptr)) {
        return nullptr;
    }

    return alloc_response.Address;
}

BOOL fumo::DriverInterface::ExposeKernelMemory(ULONG pid, PVOID address, ULONG size) {
    IO_MAP_MEMORY_REQUEST_DATA map_data = {0};
    map_data.Pid = pid;
    map_data.Address = address;
    map_data.Size = size;

    return DeviceIoControl(hDevice, IO_MAP_MEMORY_REQUEST, 
        &map_data, sizeof(map_data), 
        nullptr, 0, 
        nullptr, nullptr);
}

BOOL fumo::DriverInterface::ExecuteCode(ULONG pid, PVOID address, PVOID argument) {
    IO_EXECUTE_REQUEST_DATA execute_request = {0};
    execute_request.Pid = pid;
    execute_request.Address = address;
    execute_request.Argument = argument;

    return DeviceIoControl(hDevice, IO_EXECUTE_REQUEST, 
        &execute_request, sizeof(execute_request), 
        nullptr, 0, 
        nullptr, nullptr);
}

fumo::DriverInterface::~DriverInterface() {
    std::cout << "Closing handle" << std::endl;
    if (hDevice != INVALID_HANDLE_VALUE) {
        CloseHandle(hDevice);
    }
}