// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

#include "common.h"
#include "CommonTypes.h"
#include "CommonMacros.h"
#include "daccess.h"
#include "rhassert.h"
#include "slist.h"
#include "gcrhinterface.h"
#include "ObjectLayout.h"
#include "shash.h"
#include "RWLock.h"
#include "TypeManager.h"
#include "TargetPtrs.h"
#include "eetype.h"
#include "varint.h"
#include "PalRedhawkCommon.h"
#include "PalRedhawk.h"
#include "holder.h"
#include "Crst.h"
#include "RuntimeInstance.h"
#include "event.h"
#include "regdisplay.h"
#include "StackFrameIterator.h"
#include "thread.h"
#include "threadstore.h"

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

struct DebugTypeEntry
{
    DebugTypeEntry *Next;
    const char *TypeName;
    const char *FieldName;
    const uint32_t FieldOffset;
};

struct GlobalValueEntry
{
    GlobalValueEntry *Next;
    const char *Name;
    const void *Address;
};

// This structure is part of a in-memory serialization format that is used by diagnostic tools to
// reason about the runtime. As a contract with our diagnostic tools it must be kept up-to-date
// by changing the MajorVersion when breaking changes occur. If you are changing the runtime then
// you are responsible for understanding what changes are breaking changes. You can do this by
// reading the specification (Documentation\design-docs\diagnostics\ProcessMemoryFormatSpec.md) 
// to understand what promises the runtime makes to diagnostic tools. Any change that would make that
// document become inaccurate is a breaking change.
//
// If you do want to make a breaking change please coordinate with diagnostics team as breaking changes
// require debugger side components to be updated, and then the new versions will need to be distributed
// to customers. Ideally you will check in updates to the runtime components, the debugger parser
// components, and the format specification at the same time.
// 
// Although not guaranteed to be exhaustive, at a glance these are some potential breaking changes:
//   - Removing a field from this structure
//   - Reordering fields in the structure
//   - Changing the data type of a field in this structure
//   - Changing the data type of a field in another structure that is being refered to here with
//       the offsetof() operator
//   - Changing the data type of a global whose address is recorded in this structure
//   - Changing the meaning of a field or global refered to in this structure so that it can no longer
//     be used in the manner the format specification describes.
struct NativeAOTRuntimeDebugHeader
{
    // The cookie serves as a sanity check against process corruption or being requested
    // to treat some other non-.Net module as though it did contain the coreRT runtime.
    // It can also be changed if we want to make a breaking change so drastic that
    // earlier debuggers should treat the module as if it had no .Net runtime at all.
    // If the cookie is valid a debugger is safe to assume the Major/Minor version fields
    // will follow, but any contents beyond that depends on the version values.
    // The cookie value is currently set to 0x6e, 0x66, 0x31, 0x36 (NF16 in ascii)
    const uint8_t Cookie[4] = { 0x6e, 0x66, 0x31, 0x36 };
    
    // This counter can be incremented to indicate breaking changes
    // This field must be encoded little endian, regardless of the typical endianess of
    // the machine
    const uint16_t MajorVersion = 1;

    // This counter can be incremented to indicate back-compatible changes
    // This field must be encoded little endian, regardless of the typical endianess of
    // the machine
    const uint16_t MinorVersion = 0;

    // These flags must be encoded little endian, regardless of the typical endianess of
    // the machine. Ie Bit 0 is the least significant bit of the first byte.
    // Bit 0 - Set if the pointer size is 8 bytes, otherwise pointer size is 4 bytes
    // Bit 1 - Set if the machine is big endian
    // The high 30 bits are reserved. Changes to these bits will be considered a
    // back-compatible change.
    const uint32_t Flags = sizeof(void*) == 8 ? 0x1 : 0x0;

    // Reserved - Currently it only serves as alignment padding for the pointers which 
    // follow but future usage will be considered a back-compatible change.
    const uint32_t ReservedPadding = 0;

    // Header pointers below here are encoded using the defined pointer size and endianess
    // specified in the Flags field. The data within the contracts they point to also uses
    // the same pointer size and endianess encoding unless otherwise specified.

    DebugTypeEntry *DebugTypesList = nullptr;

    GlobalValueEntry *GlobalsList = nullptr;
};

extern "C" NativeAOTRuntimeDebugHeader g_NativeAOTRuntimeDebugHeader = {};

#define MAKE_DEBUG_ENTRY(TypeName, FieldName)                                                                               \
    do                                                                                                                      \
    {                                                                                                                       \
        currentType = new (nothrow) DebugTypeEntry{ previousType, #TypeName, #FieldName, offsetof(TypeName, FieldName) };   \
        previousType = currentType;                                                                                         \
    } while(0)                                                                                                              \

#define MAKE_GLOBAL_ENTRY(Name)                                                                                             \
    do                                                                                                                      \
    {                                                                                                                       \
        currentGlobal = new (nothrow) GlobalValueEntry{ previousGlobal, #Name, Name };                                                     \
        previousGlobal = currentGlobal;                                                                                       \
    } while(0)                                                                                                              \

extern "C" void PopulateDebugHeaders()
{
    DebugTypeEntry *previousType = nullptr;
    DebugTypeEntry *currentType = nullptr;

    MAKE_DEBUG_ENTRY(ThreadStore, m_ThreadList);

    MAKE_DEBUG_ENTRY(ThreadBuffer, m_pNext);
    MAKE_DEBUG_ENTRY(ThreadBuffer, m_rgbAllocContextBuffer);
    MAKE_DEBUG_ENTRY(ThreadBuffer, m_threadId);
    MAKE_DEBUG_ENTRY(ThreadBuffer, m_pThreadStressLog);

    MAKE_DEBUG_ENTRY(EEType, m_uBaseSize);
    MAKE_DEBUG_ENTRY(EEType, m_usComponentSize);

    MAKE_DEBUG_ENTRY(Object, m_pEEType);
    MAKE_DEBUG_ENTRY(Array, m_Length);

    MAKE_DEBUG_ENTRY(RuntimeInstance, m_pThreadStore);

    GlobalValueEntry *previousGlobal = nullptr;
    GlobalValueEntry *currentGlobal = nullptr;

    RuntimeInstance *g_pTheRuntimeInstance = GetRuntimeInstance();
    MAKE_GLOBAL_ENTRY(g_pTheRuntimeInstance);

    g_NativeAOTRuntimeDebugHeader.DebugTypesList = currentType;
    g_NativeAOTRuntimeDebugHeader.GlobalsList = currentGlobal;
}
