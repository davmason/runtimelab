// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

#include "common.h"
#include "gcenv.h"
#include "gcheaputilities.h"
#include "gcinterface.dac.h"
#include "rhassert.h"
#include "TargetPtrs.h"
#include "varint.h"
#include "PalRedhawkCommon.h"
#include "PalRedhawk.h"
#include "holder.h"
#include "RuntimeInstance.h"
#include "regdisplay.h"
#include "StackFrameIterator.h"
#include "thread.h"
#include "threadstore.h"

GPTR_DECL(EEType, g_pFreeObjectEEType);

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
    // The cookie value is currently set to 0x4E 0x41 0x44 0x48 (NADH in ascii)
    const uint8_t Cookie[4] = { 0x4E, 0x41, 0x44, 0x48 };
    
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

#define MAKE_DEBUG_ENTRY_HARDCODED(TypeName, FieldName, Offset)                                                             \
    do                                                                                                                      \
    {                                                                                                                       \
        currentType = new (nothrow) DebugTypeEntry{ previousType, #TypeName, #FieldName, Offset };                          \
        previousType = currentType;                                                                                         \
    } while(0)                                                                                                              \

#define MAKE_SIZE_ENTRY(TypeName)                                                                                           \
    do                                                                                                                      \
    {                                                                                                                       \
        currentType = new (nothrow) DebugTypeEntry{ previousType, #TypeName, "SIZEOF", sizeof(TypeName) };                  \
        previousType = currentType;                                                                                         \
    } while(0)                                                                                                              \

#define MAKE_SIZE_ENTRY_HARDCODED(TypeName, Size)                                                                           \
    do                                                                                                                      \
    {                                                                                                                       \
        currentType = new (nothrow) DebugTypeEntry{ previousType, #TypeName, "SIZEOF", Size };                              \
        previousType = currentType;                                                                                         \
    } while(0)                                                                                                              \

#define MAKE_GLOBAL_ENTRY(Name)                                                                                             \
    do                                                                                                                      \
    {                                                                                                                       \
        currentGlobal = new (nothrow) GlobalValueEntry{ previousGlobal, #Name, Name };                                      \
        previousGlobal = currentGlobal;                                                                                     \
    } while(0)                                                                                                              \

extern "C" void PopulateDebugHeaders()
{
    DebugTypeEntry *previousType = nullptr;
    DebugTypeEntry *currentType = nullptr;

    MAKE_SIZE_ENTRY(GcDacVars);
    MAKE_DEBUG_ENTRY(GcDacVars, major_version_number);
    MAKE_DEBUG_ENTRY(GcDacVars, minor_version_number);
    MAKE_DEBUG_ENTRY(GcDacVars, generation_size);
    MAKE_DEBUG_ENTRY(GcDacVars, total_generation_count);
    MAKE_DEBUG_ENTRY(GcDacVars, built_with_svr);
    MAKE_DEBUG_ENTRY(GcDacVars, finalize_queue);

    MAKE_SIZE_ENTRY(ThreadStore);
    MAKE_DEBUG_ENTRY(ThreadStore, m_ThreadList);

    MAKE_SIZE_ENTRY(ThreadBuffer);
    MAKE_DEBUG_ENTRY(ThreadBuffer, m_pNext);
    MAKE_DEBUG_ENTRY(ThreadBuffer, m_rgbAllocContextBuffer);
    MAKE_DEBUG_ENTRY(ThreadBuffer, m_threadId);
    MAKE_DEBUG_ENTRY(ThreadBuffer, m_pThreadStressLog);

    // EEThreadID is forward declared and not available
    MAKE_SIZE_ENTRY_HARDCODED(EEThreadID, sizeof(void*));
    MAKE_DEBUG_ENTRY_HARDCODED(EEThreadID, m_FiberPtrId, 0);

    MAKE_SIZE_ENTRY(EEType);
    MAKE_DEBUG_ENTRY(EEType, m_uBaseSize);
    MAKE_DEBUG_ENTRY(EEType, m_usComponentSize);

    MAKE_SIZE_ENTRY(Object);
    MAKE_DEBUG_ENTRY(Object, m_pEEType);

    MAKE_SIZE_ENTRY(Array);
    MAKE_DEBUG_ENTRY(Array, m_Length);

    MAKE_SIZE_ENTRY(RuntimeInstance);
    MAKE_DEBUG_ENTRY(RuntimeInstance, m_pThreadStore);

    GlobalValueEntry *previousGlobal = nullptr;
    GlobalValueEntry *currentGlobal = nullptr;

    RuntimeInstance *g_pTheRuntimeInstance = GetRuntimeInstance();
    MAKE_GLOBAL_ENTRY(g_pTheRuntimeInstance);

    MAKE_GLOBAL_ENTRY(g_gcDacGlobals);
    MAKE_GLOBAL_ENTRY(g_pFreeObjectEEType);

    g_NativeAOTRuntimeDebugHeader.DebugTypesList = currentType;
    g_NativeAOTRuntimeDebugHeader.GlobalsList = currentGlobal;
}
