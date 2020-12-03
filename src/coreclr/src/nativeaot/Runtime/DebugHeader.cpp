//
// Copyright (c) Microsoft Corporation.  All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.
//
#include "common.h"
#include "CommonTypes.h"
#include "CommonMacros.h"
#include "daccess.h"
#include "PalRedhawkCommon.h"
#include "PalRedhawk.h"


struct GCDebugContract;
extern const GCDebugContract g_GCDebugContract;
namespace WKS
{
    struct gc_flavor_debug_contract;
    extern const gc_flavor_debug_contract g_gc_flavor_debug_contract;
}
struct RuntimeInstanceDebugContract;
extern const RuntimeInstanceDebugContract g_RuntimeInstanceDebugContract;
struct ThreadStoreDebugContract;
extern const ThreadStoreDebugContract g_ThreadStoreDebugContract;
struct ThreadDebugContract;
extern const ThreadDebugContract g_ThreadDebugContract;
struct ObjectDebugContract;
extern const ObjectDebugContract g_ObjectDebugContract;
struct EETypeDebugContract;
extern const EETypeDebugContract g_EETypeDebugContract;


struct NetNativeRuntimeDebugHeader
{
    // The cookie serves as a sanity check against process corruption or being requested
    // to treat some other non-.Net module as though it did contain the coreRT runtime.
    // It can also be changed if we want to make a breaking change so drastic that
    // earlier debuggers should treat the module as if it had no .Net runtime at all.
    // If the cookie is valid a debugger is safe to assume the Major/Minor version fields
    // will follow, but any contents beyond that depends on the version values.
    // The cookie value is currently set to 0x6e, 0x66, 0x31, 0x36 (NF16 in ascii)
    uint8_t Cookie[4];
    
    // This counter can be incremented to indicate breaking changes
    // This field must be encoded little endian, regardless of the typical endianess of
    // the machine
    uint16_t MajorVersion;

    // This counter can be incremented to indicate back-compatible changes
    // This field must be encoded little endian, regardless of the typical endianess of
    // the machine
    uint16_t MinorVersion;

    // These flags must be encoded little endian, regardless of the typical endianess of
    // the machine. Ie Bit 0 is the least significant bit of the first byte.
    // Bit 0 - Set if the pointer size is 8 bytes, otherwise pointer size is 4 bytes
    // Bit 1 - Set if the machine is big endian
    // The high 30 bits are reserved. Changes to these bits will be considered a
    // back-compatible change.
    uint32_t Flags;

    // Reserved - Currently it only serves as alignment padding for the pointers which 
    // follow but future usage will be considered a back-compatible change.
    uint32_t ReservedPadding;

    // Header pointers below here are encoded using the defined pointer size and endianess
    // specified in the Flags field. The data within the contracts they point to also uses
    // the same pointer size and endianess encoding unless otherwise specified.

    const GCDebugContract* GCContractAddress;
    const WKS::gc_flavor_debug_contract* WksGCContractAddress;
    const RuntimeInstanceDebugContract* RuntimeInstanceContractAddress;
    const ThreadStoreDebugContract* ThreadStoreDebugContractAddress;
    const ThreadDebugContract* ThreadDebugContractAddress;
    const ObjectDebugContract* ObjectDebugContractAddress;
    const EETypeDebugContract* EETypeDebugContractAddress;

};

extern "C"
const NetNativeRuntimeDebugHeader g_NetNativeRuntimeDebugHeader =
{
    { 0x6e, 0x66, 0x31, 0x36 },          //Cookie
    1,                                   //MajorVersion
    0,                                   //MinorVersion
    (sizeof(void*) == 8 ? 0x1 : 0x0) |   //Flags - PointerSize
    0x0,                                 //Flags - Endianess
    0,                                   //ReservedPadding
    &g_GCDebugContract,
    &WKS::g_gc_flavor_debug_contract,
    &g_RuntimeInstanceDebugContract,
    &g_ThreadStoreDebugContract,
    &g_ThreadDebugContract,
    &g_ObjectDebugContract,
    &g_EETypeDebugContract
};