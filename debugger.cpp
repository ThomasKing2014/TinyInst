/*
Copyright 2020 Google LLC

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

https ://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#define  _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <stdbool.h>
#include <inttypes.h>

#include "windows.h"
#include "psapi.h"
#include "dbghelp.h"

#include "common.h"
#include "debugger.h"


#define CALLCONV_MICROSOFT_X64 0
#define CALLCONV_THISCALL 1
#define CALLCONV_FASTCALL 2
#define CALLCONV_CDECL 3
#define CALLCONV_DEFAULT 4

#define BREAKPOINT_UNKNOWN 0
#define BREAKPOINT_ENTRYPOINT 1
#define BREAKPOINT_TARGET 2

#define PERSIST_END_EXCEPTION 0x0F22

// cleans up all breakpoint structures
// does not actually remove breakpoints in target process 
void Debugger::DeleteBreakpoints() {
  for (auto iter = breakpoints.begin(); iter != breakpoints.end(); iter++) {
    delete *iter;
  }
  breakpoints.clear();
}

// returns an array of handles for all modules loaded in the target process
DWORD Debugger::GetLoadedModules(HMODULE **modules) {
  DWORD module_handle_storage_size = 1024 * sizeof(HMODULE);
  HMODULE *module_handles = (HMODULE *)malloc(module_handle_storage_size);
  DWORD hmodules_size;
  while (true) {
    if (!EnumProcessModulesEx(child_handle,
                              module_handles,
                              module_handle_storage_size,
                              &hmodules_size,
                              LIST_MODULES_ALL))
    {
      FATAL("EnumProcessModules failed, %x\n", GetLastError());
    }
    if (hmodules_size <= module_handle_storage_size) break;
    module_handle_storage_size *= 2;
    module_handles = (HMODULE *)realloc(module_handles, module_handle_storage_size);
  }
  *modules = module_handles;
  return hmodules_size / sizeof(HMODULE);
}

// parses PE headers and gets the module entypoint
void *Debugger::GetModuleEntrypoint(void *base_address) {
  unsigned char headers[4096];
  SIZE_T num_read = 0;
  if (!ReadProcessMemory(child_handle, base_address, headers, 4096, &num_read) ||
     (num_read != 4096))
  {
    FATAL("Error reading target memory\n");
  }
  DWORD pe_offset;
  pe_offset = *((DWORD *)(headers + 0x3C));
  unsigned char *pe = headers + pe_offset;
  DWORD signature = *((DWORD *)pe);
  if (signature != 0x00004550) {
    FATAL("PE signature error\n");
  }
  pe = pe + 0x18;
  WORD magic = *((WORD *)pe);
  if ((magic != 0x10b) && (magic != 0x20b)) {
    FATAL("Unknown PE magic value\n");
  }
  DWORD entrypoint_offset = *((DWORD *)(pe + 16));
  if (entrypoint_offset == 0) return NULL;
  return (char *)base_address + entrypoint_offset;
}

// parses PE headers and gets the image size
DWORD Debugger::GetImageSize(void *base_address) {
  unsigned char headers[4096];
  SIZE_T num_read = 0;
  if (!ReadProcessMemory(child_handle, base_address, headers, 4096, &num_read) ||
     (num_read != 4096))
  {
    FATAL("Error reading target memory\n");
  }
  DWORD pe_offset;
  pe_offset = *((DWORD *)(headers + 0x3C));
  unsigned char *pe = headers + pe_offset;
  DWORD signature = *((DWORD *)pe);
  if (signature != 0x00004550) {
    FATAL("PE signature error\n");
  }
  pe = pe + 0x18;
  WORD magic = *((WORD *)pe);
  if ((magic != 0x10b) && (magic != 0x20b)) {
    FATAL("Unknown PE magic value\n");
  }
  DWORD SizeOfImage = *((DWORD *)(pe + 56));
  return SizeOfImage;
}

// adds a one-time breakpoint at a specified address
// type, is an arbitrary int
// that can be accessed later when the breakpoint gets hit
void Debugger::AddBreakpoint(void *address, int type) {
  Breakpoint *new_breakpoint = new Breakpoint;
  SIZE_T rwsize = 0;
  if (!ReadProcessMemory(child_handle, address, &(new_breakpoint->original_opcode), 1, &rwsize) ||
     (rwsize != 1)) {
    FATAL("Error reading target memory\n");
  }
  rwsize = 0;
  unsigned char cc = 0xCC;
  if (!WriteProcessMemory(child_handle, address, &cc, 1, &rwsize) || (rwsize != 1)) {
    FATAL("Error writing target memory\n");
  }
  FlushInstructionCache(child_handle, address, 1);
  new_breakpoint->address = address;
  new_breakpoint->type = type;
  breakpoints.push_back(new_breakpoint);
}

// damn it Windows, why don't you have a GetProcAddress
// that works on another process
DWORD Debugger::GetProcOffset(char *data, char *name) {
  DWORD pe_offset;
  pe_offset = *((DWORD *)(data + 0x3C));
  char *pe = data + pe_offset;
  DWORD signature = *((DWORD *)pe);
  if (signature != 0x00004550) {
    return 0;
  }
  pe = pe + 0x18;
  WORD magic = *((WORD *)pe);
  DWORD exporttableoffset;
  if (magic == 0x10b) {
    exporttableoffset = *(DWORD *)(pe + 96);
  } else if (magic == 0x20b) {
    exporttableoffset = *(DWORD *)(pe + 112);
  } else {
    return 0;
  }

  if (!exporttableoffset) return 0;
  char *exporttable = data + exporttableoffset;

  DWORD numentries = *(DWORD *)(exporttable + 24);
  DWORD addresstableoffset = *(DWORD *)(exporttable + 28);
  DWORD nameptrtableoffset = *(DWORD *)(exporttable + 32);
  DWORD ordinaltableoffset = *(DWORD *)(exporttable + 36);
  DWORD *nameptrtable = (DWORD *)(data + nameptrtableoffset);
  WORD *ordinaltable = (WORD *)(data + ordinaltableoffset);
  DWORD *addresstable = (DWORD *)(data + addresstableoffset);

  DWORD i;
  for (i = 0; i < numentries; i++) {
    char *nameptr = data + nameptrtable[i];
    if (strcmp(name, nameptr) == 0) break;
  }

  if (i == numentries) return 0;

  WORD oridnal = ordinaltable[i];
  DWORD offset = addresstable[oridnal];

  return offset;
}

// attempt to obtain the address of target function
// in various ways
char *Debugger::GetTargetAddress(HMODULE module) {
  char* base_of_dll = (char *)module;
  DWORD size_of_image = GetImageSize(base_of_dll);

  // if persist_offset is defined, use that
  if (target_offset) {
    return base_of_dll + target_offset;
  }

  // try the exported symbols next
  BYTE *modulebuf = (BYTE *)malloc(size_of_image);
  SIZE_T num_read;
  if (!ReadProcessMemory(child_handle, base_of_dll, modulebuf, size_of_image, &num_read) ||
     (num_read != size_of_image))
  {
    FATAL("Error reading target memory\n");
  }
  DWORD offset = GetProcOffset((char *)modulebuf, target_method);
  free(modulebuf);
  if (offset) {
    return (char *)module + offset;
  }

  // finally, try the debug symbols
  char *method_address = NULL;
  char base_name[MAX_PATH];
  GetModuleBaseNameA(child_handle,
                     module,
                     (LPSTR)(&base_name),
                     sizeof(base_name));

  char module_path[MAX_PATH];
  if (!GetModuleFileNameExA(child_handle,
                            module,
                            module_path,
                            sizeof(module_path)))
    return NULL;

  ULONG64 buffer[(sizeof(SYMBOL_INFO) +
    MAX_SYM_NAME * sizeof(TCHAR) +
    sizeof(ULONG64) - 1) /
    sizeof(ULONG64)];
  PSYMBOL_INFO pSymbol = (PSYMBOL_INFO)buffer;
  pSymbol->SizeOfStruct = sizeof(SYMBOL_INFO);
  pSymbol->MaxNameLen = MAX_SYM_NAME;
  SymInitialize(child_handle, NULL, false);
  DWORD64 sym_base_address = SymLoadModuleEx(child_handle,
                                             NULL,
                                             module_path,
                                             NULL,
                                             0,
                                             0,
                                             NULL,
                                             0);
  if (SymFromName(child_handle, target_method, pSymbol)) {
    target_offset = (unsigned long)(pSymbol->Address - sym_base_address);
    method_address = base_of_dll + target_offset;
  }
  SymCleanup(child_handle);

  return method_address;
}

// called when a module gets loaded
void Debugger::OnModuleLoaded(HMODULE module, char *module_name) {
  // printf("In on_module_loaded, name: %s, base: %p\n", module_name, module_info.lpBaseOfDll);

  if (target_function_defined && _stricmp(module_name, target_module) == 0) {
    target_address = GetTargetAddress(module);
    if (!target_address) {
      FATAL("Error determining target method address\n");
    }

    AddBreakpoint(target_address, BREAKPOINT_TARGET);
  }
}

// called when a module gets unloaded
void Debugger::OnModuleUnloaded(HMODULE module) { }

// reads numitems entries from stack in remote process
// from stack_addr
// into buffer
void Debugger::ReadStack(void *stack_addr, void **buffer, size_t numitems) {
  SIZE_T numrw = 0;
#ifdef _WIN64
  if (wow64_target) {
    uint32_t *buf32 = (uint32_t *)malloc(numitems * child_ptr_size);
    ReadProcessMemory(child_handle, stack_addr, buf32, numitems * child_ptr_size, &numrw);
    for (size_t i = 0; i < numitems; i++) {
      buffer[i] = (void *)((size_t)buf32[i]);
    }
    free(buf32);
    return;
  }
#endif
  ReadProcessMemory(child_handle, stack_addr, buffer, numitems * child_ptr_size, &numrw);
}

// writes numitems entries from stack in remote process
// from stack_addr
// into buffer
void Debugger::WriteStack(void *stack_addr, void **buffer, size_t numitems) {
  SIZE_T numrw = 0;
#ifdef _WIN64
  if (wow64_target) {
    uint32_t *buf32 = (uint32_t *)malloc(numitems * child_ptr_size);
    for (size_t i = 0; i < numitems; i++) {
      buf32[i] = (uint32_t)((size_t)buffer[i]);
    }
    WriteProcessMemory(child_handle, stack_addr, buf32, numitems * child_ptr_size, &numrw);
    free(buf32);
    return;
  }
#endif
  WriteProcessMemory(child_handle, stack_addr, buffer, numitems * child_ptr_size, &numrw);
}

// called when the target method is reached
void Debugger::HandleTargetReachedInternal(DWORD thread_id) {
  // printf("in OnTargetMethod\n");

  SIZE_T numrw = 0;

  CONTEXT lcContext;
  lcContext.ContextFlags = CONTEXT_ALL;
  HANDLE thread_handle = OpenThread(THREAD_ALL_ACCESS, FALSE, thread_id);
  GetThreadContext(thread_handle, &lcContext);

  // read out and save the params
#ifdef _WIN64
  saved_sp = (void *)lcContext.Rsp;
#else
  saved_sp = (void *)lcContext.Esp;
#endif

  saved_return_address = 0;
  ReadProcessMemory(child_handle, saved_sp, &saved_return_address, child_ptr_size, &numrw);

  if (loop_mode) {
    switch (calling_convention) {
#ifdef _WIN64
    case CALLCONV_DEFAULT:
    case CALLCONV_MICROSOFT_X64:
      if (target_num_args > 0) saved_args[0] = (void *)lcContext.Rcx;
      if (target_num_args > 1) saved_args[1] = (void *)lcContext.Rdx;
      if (target_num_args > 2) saved_args[2] = (void *)lcContext.R8;
      if (target_num_args > 3) saved_args[3] = (void *)lcContext.R9;
      if (target_num_args > 4) {
        ReadStack((void *)(lcContext.Rsp + 5 * child_ptr_size), saved_args + 4, target_num_args - 4);
      }
      break;
    case CALLCONV_CDECL:
      if (target_num_args > 0) {
        ReadStack((void *)(lcContext.Rsp + child_ptr_size), saved_args, target_num_args);
      }
      break;
    case CALLCONV_FASTCALL:
      if (target_num_args > 0) saved_args[0] = (void *)lcContext.Rcx;
      if (target_num_args > 1) saved_args[1] = (void *)lcContext.Rdx;
      if (target_num_args > 3) {
        ReadStack((void *)(lcContext.Rsp + child_ptr_size), saved_args + 2, target_num_args - 2);
      }
      break;
    case CALLCONV_THISCALL:
      if (target_num_args > 0) saved_args[0] = (void *)lcContext.Rcx;
      if (target_num_args > 3) {
        ReadStack((void *)(lcContext.Rsp + child_ptr_size), saved_args + 1, target_num_args - 1);
      }
      break;
#else
    case CALLCONV_MICROSOFT_X64:
      FATAL("X64 callong convention not supported for 32-bit targets");
      break;
    case CALLCONV_DEFAULT:
    case CALLCONV_CDECL:
      if (target_num_args > 0) {
        ReadStack((void *)(lcContext.Esp + child_ptr_size), saved_args, target_num_args);
      }
      break;
    case CALLCONV_FASTCALL:
      if (target_num_args > 0) saved_args[0] = (void *)lcContext.Ecx;
      if (target_num_args > 1) saved_args[1] = (void *)lcContext.Edx;
      if (target_num_args > 3) {
        ReadStack((void *)(lcContext.Esp + child_ptr_size), saved_args + 2, target_num_args - 2);
      }
      break;
    case CALLCONV_THISCALL:
      if (target_num_args > 0) saved_args[0] = (void *)lcContext.Ecx;
      if (target_num_args > 3) {
        ReadStack((void *)(lcContext.Esp + child_ptr_size), saved_args + 1, target_num_args - 1);
      }
      break;
#endif
    default:
      break;
    }

    // todo store any target-specific additional context here
  }

  // modify the return address on the stack so that an exception is triggered
  // when the target function finishes executing
  // another option would be to allocate a block of executable memory
  // and point return address over there, but this is quicker
  size_t return_address = PERSIST_END_EXCEPTION;
  WriteProcessMemory(child_handle, saved_sp, &return_address, child_ptr_size, &numrw);

  CloseHandle(thread_handle);

  if (!target_reached) {
    target_reached = true;
    OnTargetMethodReached(thread_id);
  }
}

// called every time the target method returns
void Debugger::HandleTargetEnded(DWORD thread_id) {
  // printf("in OnTargetMethodEnded\n");

  CONTEXT lcContext;
  lcContext.ContextFlags = CONTEXT_ALL;
  HANDLE thread_handle = OpenThread(THREAD_ALL_ACCESS, FALSE, thread_id);
  GetThreadContext(thread_handle, &lcContext);

  if (loop_mode) {
    // restore params
#ifdef _WIN64
    lcContext.Rip = (size_t)target_address;
    lcContext.Rsp = (size_t)saved_sp;
#else
    lcContext.Eip = (size_t)target_address;
    lcContext.Esp = (size_t)saved_sp;
#endif

    // restore return address as it might have been overwritten by instrumentation
    SIZE_T numrw = 0;
    size_t return_address = PERSIST_END_EXCEPTION;
    WriteProcessMemory(child_handle, saved_sp, &return_address, child_ptr_size, &numrw);

    switch (calling_convention) {
#ifdef _WIN64
    case CALLCONV_DEFAULT:
    case CALLCONV_MICROSOFT_X64:
      if (target_num_args > 0) lcContext.Rcx = (size_t)saved_args[0];
      if (target_num_args > 1) lcContext.Rdx = (size_t)saved_args[1];
      if (target_num_args > 2) lcContext.R8 = (size_t)saved_args[2];
      if (target_num_args > 3) lcContext.R9 = (size_t)saved_args[3];
      if (target_num_args > 4) {
        WriteStack((void *)(lcContext.Rsp + 5 * child_ptr_size), saved_args + 4, target_num_args - 4);
      }
      break;
    case CALLCONV_CDECL:
      if (target_num_args > 0) {
        WriteStack((void *)(lcContext.Rsp + child_ptr_size), saved_args, target_num_args);
      }
      break;
    case CALLCONV_FASTCALL:
      if (target_num_args > 0) lcContext.Rcx = (size_t)saved_args[0];
      if (target_num_args > 1) lcContext.Rdx = (size_t)saved_args[1];
      if (target_num_args > 3) {
        WriteStack((void *)(lcContext.Rsp + child_ptr_size), saved_args + 2, target_num_args - 2);
      }
      break;
    case CALLCONV_THISCALL:
      if (target_num_args > 0) lcContext.Rcx = (size_t)saved_args[0];
      if (target_num_args > 3) {
        WriteStack((void *)(lcContext.Rsp + child_ptr_size), saved_args + 1, target_num_args - 1);
      }
      break;
#else
    case CALLCONV_MICROSOFT_X64:
      FATAL("X64 callong convention not supported for 32-bit targets");
      break;
    case CALLCONV_DEFAULT:
    case CALLCONV_CDECL:
      if (target_num_args > 0) {
        WriteStack((void *)(lcContext.Esp + child_ptr_size), saved_args, target_num_args);
      }
      break;
    case CALLCONV_FASTCALL:
      if (target_num_args > 0) lcContext.Ecx = (size_t)saved_args[0];
      if (target_num_args > 1) lcContext.Edx = (size_t)saved_args[1];
      if (target_num_args > 3) {
        WriteStack((void *)(lcContext.Esp + child_ptr_size), saved_args + 2, target_num_args - 2);
      }
      break;
    case CALLCONV_THISCALL:
      if (target_num_args > 0) lcContext.Ecx = (size_t)saved_args[0];
      if (target_num_args > 3) {
        WriteStack((void *)(lcContext.Esp + child_ptr_size), saved_args + 1, target_num_args - 1);
      }
      break;
#endif
    default:
      break;
    }

    // todo restore any target-specific additional context here

  } else { /*  loop_mode == false */

#ifdef _WIN64
    lcContext.Rip = (size_t)saved_return_address;
#else
    lcContext.Eip = (size_t)saved_return_address;
#endif

    // restore target entry breakpoint
    // note that this time, the breakpoint address might be
    // in instrumented code
    // so we need to use translated address
    AddBreakpoint((void *)GetTranslatedAddress((size_t)target_address),
                  BREAKPOINT_TARGET);
  }

  SetThreadContext(thread_handle, &lcContext);
  CloseHandle(thread_handle);
}

// called when process entrypoint gets reached
void Debugger::OnEntrypoint() {
  // printf("Entrypoint\n");

  HMODULE *module_handles = NULL;
  DWORD num_modules = GetLoadedModules(&module_handles);
  for (DWORD i = 0; i < num_modules; i++) {
    char base_name[MAX_PATH];
    GetModuleBaseNameA(child_handle, module_handles[i], (LPSTR)(&base_name), sizeof(base_name));
    if(trace_debug_events)
      printf("Debugger: Loaded module %s at %p\n", base_name, (void *)module_handles[i]);
    OnModuleLoaded(module_handles[i], base_name);
  }
  if (module_handles) free(module_handles);

  child_entrypoint_reached = true;

  if (trace_debug_events) printf("Debugger: Process entrypoint reached\n");
}

// called when the debugger hits a breakpoint
int Debugger::HandleDebuggerBreakpoint(void *address, DWORD thread_id) {
  int ret = BREAKPOINT_UNKNOWN;
  SIZE_T rwsize = 0;

  Breakpoint *breakpoint = NULL, *tmp_breakpoint;
  for (auto iter = breakpoints.begin(); iter != breakpoints.end(); iter++) {
    tmp_breakpoint = *iter;
    if (tmp_breakpoint->address == address) {
      breakpoint = tmp_breakpoint;
      breakpoints.erase(iter);
      break;
    }
  }

  if (!breakpoint) return ret;

  // restore address
  if (!WriteProcessMemory(child_handle, address, &breakpoint->original_opcode, 1, &rwsize) ||
     (rwsize != 1))
  {
    FATAL("Error writing child memory\n");
  }
  FlushInstructionCache(child_handle, address, 1);
  // restore context
  CONTEXT lcContext;
  lcContext.ContextFlags = CONTEXT_ALL;
  HANDLE thread_handle = OpenThread(THREAD_ALL_ACCESS, FALSE, thread_id);
  GetThreadContext(thread_handle, &lcContext);
#ifdef _WIN64
  lcContext.Rip--;
#else
  lcContext.Eip--;
#endif
  SetThreadContext(thread_handle, &lcContext);
  CloseHandle(thread_handle);
  // handle breakpoint
  switch (breakpoint->type) {
  case BREAKPOINT_ENTRYPOINT:
    OnEntrypoint();
    break;
  case BREAKPOINT_TARGET:
    if (trace_debug_events) printf("Target method reached\n");
    HandleTargetReachedInternal(thread_id);
    break;
  default:
    break;
  }

  // return the brekpoint type
  ret = breakpoint->type;

  // delete the breakpoint object
  free(breakpoint);

  return ret;
}

// called when a dll gets loaded
void Debugger::HandleDllLoadInternal(LOAD_DLL_DEBUG_INFO *LoadDll) {
  // Don't do anything until the processentrypoint is reached.
  // Before that time we can't do much anyway, a lot of calls are going to fail
  // Modules loaded before entrypoint is reached are going to be enumerated at that time
  if (child_entrypoint_reached) {
    char filename[MAX_PATH];
    GetFinalPathNameByHandleA(LoadDll->hFile, (LPSTR)(&filename), sizeof(filename), 0);
    char *base_name = strrchr(filename, '\\');
    if (base_name) base_name += 1;
    else base_name = filename;
    if (trace_debug_events)
      printf("Debugger: Loaded module %s at %p\n",
        base_name,
        (void *)LoadDll->lpBaseOfDll);
    OnModuleLoaded((HMODULE)LoadDll->lpBaseOfDll, base_name);
  }
}

// called when a process gets created
// or attached to
void Debugger::OnProcessCreated(CREATE_PROCESS_DEBUG_INFO *info) {
  if (attach_mode) {
    // assume entrypoint has been reached already
    child_handle = info->hProcess;
    child_thread_handle = info->hThread;
    child_entrypoint_reached = true;
    GetProcessPlatform();
  } else {
    // add a brekpoint to the process entrypoint
    void *entrypoint = GetModuleEntrypoint(info->lpBaseOfImage);
    AddBreakpoint(entrypoint, BREAKPOINT_ENTRYPOINT);
  }
}

// called when an exception in the target occurs
DebuggerStatus Debugger::HandleExceptionInternal(EXCEPTION_RECORD *exception_record,
                                                 DWORD thread_id)
{
  // note: instrumentation could have placed breakpoints
  // on the same addresses as debugger
  // handle one-time debugger breakpoints first
  if ((exception_record->ExceptionCode == EXCEPTION_BREAKPOINT) ||
      (exception_record->ExceptionCode == 0x4000001f))
  {
    void *address = exception_record->ExceptionAddress;
    // printf("Breakpoint at address %p\n", address);
    int breakpoint_type = HandleDebuggerBreakpoint(address, thread_id);
    if (breakpoint_type == BREAKPOINT_TARGET) {
      return DEBUGGER_TARGET_START;
    } else if (breakpoint_type != BREAKPOINT_UNKNOWN) {
      return DEBUGGER_CONTINUE;
    }
  }

  // check if cleient can handle it
  if (OnException(exception_record, thread_id)) {
    return DEBUGGER_CONTINUE;
  }

  // don't print exceptions handled by clients
  if (trace_debug_events)
    printf("Debugger: Exception %x at address %p\n",
      exception_record->ExceptionCode,
      exception_record->ExceptionAddress);

  switch (exception_record->ExceptionCode)
  {
  case EXCEPTION_BREAKPOINT:
  case 0x4000001f: //STATUS_WX86_BREAKPOINT
    // not handled above
    dbg_continue_status = DBG_EXCEPTION_NOT_HANDLED;
    return DEBUGGER_CONTINUE;

  case EXCEPTION_ACCESS_VIOLATION: {
    if (target_function_defined && 
       ((size_t)exception_record->ExceptionAddress == PERSIST_END_EXCEPTION))
    {
      if (trace_debug_events) printf("Debugger: Persistence method ended\n");
      HandleTargetEnded(thread_id);
      return DEBUGGER_TARGET_END;
    } else {
      // Debug(&DebugEv->u.Exception.ExceptionRecord);
      dbg_continue_status = DBG_EXCEPTION_NOT_HANDLED;
      return DEBUGGER_CRASHED;
    }
    break;
  }

  case EXCEPTION_ILLEGAL_INSTRUCTION:
  case EXCEPTION_PRIV_INSTRUCTION:
  case EXCEPTION_INT_DIVIDE_BY_ZERO:
  case EXCEPTION_STACK_OVERFLOW:
  case STATUS_HEAP_CORRUPTION:
  case STATUS_STACK_BUFFER_OVERRUN:
  case STATUS_FATAL_APP_EXIT:
    dbg_continue_status = DBG_EXCEPTION_NOT_HANDLED;
    return DEBUGGER_CRASHED;
    break;

  default:
    printf("Unhandled exception %x\n", exception_record->ExceptionCode);
    dbg_continue_status = DBG_EXCEPTION_NOT_HANDLED;
    return DEBUGGER_CONTINUE;
  }
}

// standard debugger loop that listens to events in the target process
DebuggerStatus Debugger::DebugLoop()
{
  DebuggerStatus ret;
  bool alive = true;

  if (dbg_continue_needed) {
    ContinueDebugEvent(dbg_debug_event.dwProcessId,
      dbg_debug_event.dwThreadId,
      dbg_continue_status);
  }

  LPDEBUG_EVENT DebugEv = &dbg_debug_event;

  while (alive)
  {

    BOOL wait_ret = WaitForDebugEvent(DebugEv, 100);

    // printf("time: %lld\n", get_cur_time_us());

    if (wait_ret) {
      dbg_continue_needed = true;
    } else {
      dbg_continue_needed = false;
    }

    if (GetCurTime() > dbg_timeout_time) return DEBUGGER_HANGED;

    if (!wait_ret) {
      //printf("WaitForDebugEvent returned 0\n");
      continue;
    }

    dbg_continue_status = DBG_CONTINUE;

    // printf("eventCode: %x\n", DebugEv->dwDebugEventCode);

    switch (DebugEv->dwDebugEventCode)
    {
    case EXCEPTION_DEBUG_EVENT:
      ret = HandleExceptionInternal(&DebugEv->u.Exception.ExceptionRecord, DebugEv->dwThreadId);
      if (ret == DEBUGGER_CRASHED) OnCrashed(&DebugEv->u.Exception.ExceptionRecord);
      if (ret != DEBUGGER_CONTINUE) return ret;
      break;

    case CREATE_THREAD_DEBUG_EVENT:
      break;

    case CREATE_PROCESS_DEBUG_EVENT: {
      if (trace_debug_events) printf("Debugger: Process created or attached\n");
      OnProcessCreated(&DebugEv->u.CreateProcessInfo);
      CloseHandle(DebugEv->u.CreateProcessInfo.hFile);
      break;
    }

    case EXIT_THREAD_DEBUG_EVENT:
      break;

    case EXIT_PROCESS_DEBUG_EVENT:
      if (trace_debug_events) printf("Debugger: Process exit\n");
      OnProcessExit();
      alive = false;
      break;

    case LOAD_DLL_DEBUG_EVENT: {
      HandleDllLoadInternal(&DebugEv->u.LoadDll);
      CloseHandle(DebugEv->u.LoadDll.hFile);
      break;
    }

    case UNLOAD_DLL_DEBUG_EVENT:
      if (trace_debug_events)
        printf("Debugger: Unloaded module from %p\n", DebugEv->u.UnloadDll.lpBaseOfDll);
      OnModuleUnloaded((HMODULE)DebugEv->u.UnloadDll.lpBaseOfDll);
      break;

   default:
      break;
    }

    ContinueDebugEvent(DebugEv->dwProcessId,
      DebugEv->dwThreadId,
      dbg_continue_status);
  }

  return DEBUGGER_PROCESS_EXIT;
}

// starts the target process
void Debugger::StartProcess(char *cmd) {
  dbg_continue_needed = false;

  STARTUPINFOA si;
  PROCESS_INFORMATION pi;
  HANDLE hJob = NULL;
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION job_limit;

  DeleteBreakpoints();

  if (sinkhole_stds && devnul_handle == INVALID_HANDLE_VALUE) {
    devnul_handle = CreateFile(
      "nul",
      GENERIC_READ | GENERIC_WRITE,
      FILE_SHARE_READ | FILE_SHARE_WRITE,
      NULL,
      OPEN_EXISTING,
      0,
      NULL);

    if (devnul_handle == INVALID_HANDLE_VALUE) {
      FATAL("Unable to open the nul device.");
    }
  }
  BOOL inherit_handles = TRUE;

  ZeroMemory(&si, sizeof(si));
  si.cb = sizeof(si);
  ZeroMemory(&pi, sizeof(pi));

  if (sinkhole_stds) {
    si.hStdOutput = si.hStdError = devnul_handle;
    si.dwFlags |= STARTF_USESTDHANDLES;
  } else {
    inherit_handles = FALSE;
  }

  if (mem_limit || cpu_aff) {
    hJob = CreateJobObject(NULL, NULL);
    if (hJob == NULL) {
      FATAL("CreateJobObject failed, GLE=%d.\n", GetLastError());
    }

    ZeroMemory(&job_limit, sizeof(job_limit));
    if (mem_limit) {
      job_limit.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_PROCESS_MEMORY;
      job_limit.ProcessMemoryLimit = (size_t)(mem_limit * 1024 * 1024);
    }

    if (cpu_aff) {
      job_limit.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_AFFINITY;
      job_limit.BasicLimitInformation.Affinity = (DWORD_PTR)cpu_aff;
    }

    if (!SetInformationJobObject(
      hJob,
      JobObjectExtendedLimitInformation,
      &job_limit,
      sizeof(job_limit)
    )) {
      FATAL("SetInformationJobObject failed, GLE=%d.\n", GetLastError());
    }
  }

  if (!CreateProcessA(NULL,
                      cmd,
                      NULL,
                      NULL,
                      inherit_handles,
                      DEBUG_PROCESS | DEBUG_ONLY_THIS_PROCESS,
                      NULL,
                      NULL,
                      &si,
                      &pi))
  {
    FATAL("CreateProcess failed, GLE=%d.\n", GetLastError());
  }

  child_handle = pi.hProcess;
  child_thread_handle = pi.hThread;
  child_entrypoint_reached = false;
  target_reached = false;

  if (mem_limit || cpu_aff) {
    if (!AssignProcessToJobObject(hJob, child_handle)) {
      FATAL("AssignProcessToJobObject failed, GLE=%d.\n", GetLastError());
    }
  }

  GetProcessPlatform();
}

void Debugger::GetProcessPlatform() {
  BOOL wow64current, wow64remote;
  if (!IsWow64Process(child_handle, &wow64remote)) {
    FATAL("IsWow64Process failed");
  }
  if (wow64remote) {
    wow64_target = 1;
    child_ptr_size = 4;
    if (calling_convention == CALLCONV_DEFAULT) {
      calling_convention = CALLCONV_CDECL;
    }
  }
  if (!IsWow64Process(GetCurrentProcess(), &wow64current)) {
    FATAL("IsWow64Process failed");
  }
  // Will probably fail before we reach this, but oh well
  if (sizeof(void*) < child_ptr_size) {
    FATAL("64-bit build is needed to run 64-bit targets\n");
  }
}

// kills the target process
// (if not dead already)
DebuggerStatus Debugger::Kill() {
  if (!child_handle) return DEBUGGER_PROCESS_EXIT;

  TerminateProcess(child_handle, 0);
  
  // no timeout for process killing
  dbg_timeout_time = 0xFFFFFFFFFFFFFFFFLL;

  dbg_last_status = DebugLoop();
  if (dbg_last_status != DEBUGGER_PROCESS_EXIT) {
    FATAL("Error killing target process\n");
  }

  CloseHandle(child_handle);
  CloseHandle(child_thread_handle);

  child_handle = NULL;
  child_thread_handle = NULL;

  // delete any breakpoints that weren't hit
  DeleteBreakpoints();

  return dbg_last_status;
}

// attaches to an active process
DebuggerStatus Debugger::Attach(unsigned int pid, uint32_t timeout) {
  attach_mode = true;

  if (!DebugActiveProcess(pid)) {
    FATAL("Could not attach to the process.\n"
          "Make sure the process exists and you have permissions to debug it.\n");
  }

  dbg_last_status = DEBUGGER_ATTACHED;

  return Continue(timeout);
}

// starts the process and waits for the next event
DebuggerStatus Debugger::Run(char *cmd, uint32_t timeout) {
  attach_mode = false;

  StartProcess(cmd);

  return Continue(timeout);
}

// continues after Run() or previous Continue()
// return with a non-terminal status
DebuggerStatus Debugger::Continue(uint32_t timeout) {
  if (!child_handle && (dbg_last_status != DEBUGGER_ATTACHED))
    return DEBUGGER_PROCESS_EXIT;

  if (loop_mode && (dbg_last_status == DEBUGGER_TARGET_END)) {
    // saves us a breakpoint
    dbg_last_status = DEBUGGER_TARGET_START;
    return dbg_last_status;
  }

  dbg_timeout_time = GetCurTime() + timeout;
  dbg_last_status = DebugLoop();

  if (dbg_last_status == DEBUGGER_PROCESS_EXIT) {
    CloseHandle(child_handle);
    CloseHandle(child_thread_handle);
    child_handle = NULL;
    child_thread_handle = NULL;
  }

  return dbg_last_status;
}

// initializes options from command line
void Debugger::Init(int argc, char **argv) {
  sinkhole_stds = false;
  mem_limit = 0;
  cpu_aff = 0;

  attach_mode = false;
  trace_debug_events = false;
  loop_mode = false;
  target_function_defined = false;

  child_handle = NULL;
  child_thread_handle = NULL;

  target_module[0] = 0;
  target_method[0] = 0;
  target_offset = 0;
  saved_args = NULL;
  target_num_args = 0;
  calling_convention = CALLCONV_DEFAULT;

  char *option;

  trace_debug_events = GetBinaryOption("-trace_debug_events",
                                       argc, argv,
                                       trace_debug_events);

  option = GetOption("-target_module", argc, argv);
  if (option) strncpy(target_module, option, MAX_PATH);

  option = GetOption("-target_method", argc, argv);
  if (option) strncpy(target_method, option, MAX_PATH);

  loop_mode = GetBinaryOption("-loop", argc, argv, loop_mode);

  option = GetOption("-nargs", argc, argv);
  if (option) target_num_args = atoi(option);

  option = GetOption("-target_offset", argc, argv);
  if (option) target_offset = strtoul(option, NULL, 0);

  option = GetOption("-callconv", argc, argv);
  if (option) {
    if (strcmp(option, "stdcall") == 0)
      calling_convention = CALLCONV_CDECL;
    else if (strcmp(option, "fastcall") == 0)
      calling_convention = CALLCONV_FASTCALL;
    else if (strcmp(option, "thiscall") == 0)
      calling_convention = CALLCONV_THISCALL;
    else if (strcmp(option, "ms64") == 0)
      calling_convention = CALLCONV_MICROSOFT_X64;
    else
      FATAL("Unknown calling convention");
  }

  // check if we are running in persistence mode
  if (target_module[0] || target_offset || target_method[0]) {
    target_function_defined = true;
    if ((target_module[0] == 0) || ((target_offset == 0) && (target_method[0] == 0))) {
      FATAL("target_module and either target_offset or target_method must be specified together\n");
    }
  }

  if (loop_mode && !target_function_defined) {
    FATAL("Target function needs to be defined to use the loop mode\n");
  }

  if (target_num_args) {
    saved_args = (void **)malloc(target_num_args * sizeof(void *));
  }
}
