/*
 * Copyright (c) 2020, Microsoft Corporation. All rights reserved.
 * Copyright (c) 2022, 2025, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "asm/macroAssembler.hpp"
#include "classfile/vmSymbols.hpp"
#include "code/codeCache.hpp"
#include "code/vtableStubs.hpp"
#include "code/nativeInst.hpp"
#include "interpreter/interpreter.hpp"
#include "jvm.h"
#include "memory/allocation.inline.hpp"
#include "os_windows.hpp"
#include "prims/jniFastGetField.hpp"
#include "prims/jvm_misc.hpp"
#include "runtime/arguments.hpp"
#include "runtime/frame.inline.hpp"
#include "runtime/interfaceSupport.inline.hpp"
#include "runtime/java.hpp"
#include "runtime/javaCalls.hpp"
#include "runtime/javaThread.hpp"
#include "runtime/mutexLocker.hpp"
#include "runtime/osThread.hpp"
#include "runtime/sharedRuntime.hpp"
#include "runtime/stubRoutines.hpp"
#include "runtime/timer.hpp"
#include "symbolengine.hpp"
#include "unwind_windows_aarch64.hpp"
#include "utilities/debug.hpp"
#include "utilities/events.hpp"
#include "utilities/vmError.hpp"
#include "windbghelp.hpp"

// put OS-includes here
# include <sys/types.h>
# include <signal.h>
# include <errno.h>
# include <stdlib.h>
# include <stdio.h>
# include <intrin.h>

#define REG_BCP X22

void os::os_exception_wrapper(java_call_t f, JavaValue* value, const methodHandle& method, JavaCallArguments* args, JavaThread* thread) {
  f(value, method, args, thread);
}

PRAGMA_DISABLE_MSVC_WARNING(4172)
// Returns an estimate of the current stack pointer. Result must be guaranteed
// to point into the calling threads stack, and be no lower than the current
// stack pointer.
address os::current_stack_pointer() {
  int dummy;
  address sp = (address)&dummy;
  return sp;
}

address os::fetch_frame_from_context(const void* ucVoid,
                    intptr_t** ret_sp, intptr_t** ret_fp) {
  address  epc;
  CONTEXT* uc = (CONTEXT*)ucVoid;

  if (uc != nullptr) {
    epc = (address)uc->Pc;
    if (ret_sp) *ret_sp = (intptr_t*)uc->Sp;
    if (ret_fp) *ret_fp = (intptr_t*)uc->Fp;
  } else {
    // construct empty ExtendedPC for return value checking
    epc = nullptr;
    if (ret_sp) *ret_sp = (intptr_t *)nullptr;
    if (ret_fp) *ret_fp = (intptr_t *)nullptr;
  }
  return epc;
}

frame os::fetch_frame_from_context(const void* ucVoid) {
  intptr_t* sp;
  intptr_t* fp;
  address epc = fetch_frame_from_context(ucVoid, &sp, &fp);
  return frame(sp, fp, epc);
}

#ifdef ASSERT
static bool is_interpreter(const CONTEXT* uc) {
  assert(uc != nullptr, "invariant");
  address pc = reinterpret_cast<address>(uc->Pc);
  assert(pc != nullptr, "invariant");
  return Interpreter::contains(pc);
}
#endif

intptr_t* os::fetch_bcp_from_context(const void* ucVoid) {
  assert(ucVoid != nullptr, "invariant");
  CONTEXT* uc = (CONTEXT*)ucVoid;
  assert(is_interpreter(uc), "invariant");
  return reinterpret_cast<intptr_t*>(uc->REG_BCP);
}

void os::win32::context_set_pc(CONTEXT* uc, address pc) {
  uc->Pc = (intptr_t)pc;
}

#ifdef HAVE_PLATFORM_PRINT_NATIVE_STACK
/*
 * Windows/x64 does not use stack frames the way expected by Java: TODO: what about AArch64?
 * [1] in most cases, there is no frame pointer. All locals are addressed via RSP
 * [2] in rare cases, when alloca() is used, a frame pointer is used, but this may
 *     not be RBP.
 * See http://msdn.microsoft.com/en-us/library/ew5tede7.aspx
 *
 * So it's not possible to print the native stack using the
 *     while (...) {...  fr = os::get_sender_for_C_frame(&fr); }
 * loop in vmError.cpp. We need to roll our own loop.
 */
bool os::win32::platform_print_native_stack(outputStream* st, const void* context,
                                            char *buf, int buf_size, address& lastpc)
{
  CONTEXT ctx;
  if (context != nullptr) {
    memcpy(&ctx, context, sizeof(ctx));
  } else {
    RtlCaptureContext(&ctx);
  }

  st->print_cr("Native frames: (J=compiled Java code, j=interpreted, Vv=VM code, C=native code)");

  STACKFRAME stk;
  memset(&stk, 0, sizeof(stk));
  stk.AddrStack.Offset    = ctx.Sp;
  stk.AddrStack.Mode      = AddrModeFlat;
  stk.AddrFrame.Offset    = ctx.Fp;
  stk.AddrFrame.Mode      = AddrModeFlat;
  stk.AddrPC.Offset       = ctx.Pc;
  stk.AddrPC.Mode         = AddrModeFlat;

  // Ensure we consider dynamically loaded dll's
  SymbolEngine::refreshModuleList();

  int count = 0;
  address lastpc_internal = 0;
  while (count++ < StackPrintLimit) {
    intptr_t* sp = (intptr_t*)stk.AddrStack.Offset;
    intptr_t* fp = (intptr_t*)stk.AddrFrame.Offset; // NOT necessarily the same as ctx.Rbp!
    address pc = (address)stk.AddrPC.Offset;

    if (pc != nullptr) {
      if (count == 2 && lastpc_internal == pc) {
        // Skip it -- StackWalk64() may return the same PC
        // (but different SP) on the first try.
      } else {
        // Don't try to create a frame(sp, fp, pc) -- on WinX64, stk.AddrFrame
        // may not contain what Java expects, and may cause the frame() constructor
        // to crash. Let's just print out the symbolic address.
        frame::print_C_frame(st, buf, buf_size, pc);
        // print source file and line, if available
        char buf[128];
        int line_no;
        if (SymbolEngine::get_source_info(pc, buf, sizeof(buf), &line_no)) {
          st->print("  (%s:%d)", buf, line_no);
        } else {
          st->print("  (no source info available)");
        }
        st->cr();
      }
      lastpc_internal = pc;
    }

    PVOID p = WindowsDbgHelp::symFunctionTableAccess64(GetCurrentProcess(), stk.AddrPC.Offset);
    if (!p) {
      // StackWalk64() can't handle this PC. Calling StackWalk64 again may cause crash.
      lastpc = lastpc_internal;
      break;
    }

    BOOL result = WindowsDbgHelp::stackWalk64(
        IMAGE_FILE_MACHINE_ARM64,  // __in      DWORD MachineType,
        GetCurrentProcess(),       // __in      HANDLE hProcess,
        GetCurrentThread(),        // __in      HANDLE hThread,
        &stk,                      // __inout   LP STACKFRAME64 StackFrame,
        &ctx);                     // __inout   PVOID ContextRecord,

    if (!result) {
      break;
    }
  }
  if (count > StackPrintLimit) {
    st->print_cr("...<more frames>...");
  }
  st->cr();

  return true;
}
#endif // HAVE_PLATFORM_PRINT_NATIVE_STACK

bool os::win32::get_frame_at_stack_banging_point(JavaThread* thread,
        struct _EXCEPTION_POINTERS* exceptionInfo, address pc, frame* fr) {
  PEXCEPTION_RECORD exceptionRecord = exceptionInfo->ExceptionRecord;
  address addr = (address) exceptionRecord->ExceptionInformation[1];
  if (Interpreter::contains(pc)) {
    // interpreter performs stack banging after the fixed frame header has
    // been generated while the compilers perform it before. To maintain
    // semantic consistency between interpreted and compiled frames, the
    // method returns the Java sender of the current frame.
    *fr = os::fetch_frame_from_context((void*)exceptionInfo->ContextRecord);
    if (!fr->is_first_java_frame()) {
      assert(fr->safe_for_sender(thread), "Safety check");
      *fr = fr->java_sender();
    }
  } else {
    // more complex code with compiled code
    assert(!Interpreter::contains(pc), "Interpreted methods should have been handled above");
    CodeBlob* cb = CodeCache::find_blob(pc);
    if (cb == nullptr || !cb->is_nmethod() || cb->is_frame_complete_at(pc)) {
      // Not sure where the pc points to, fallback to default
      // stack overflow handling
      return false;
    } else {
      // In compiled code, the stack banging is performed before LR
      // has been saved in the frame.  LR is live, and SP and FP
      // belong to the caller.
      intptr_t* fp = (intptr_t*)exceptionInfo->ContextRecord->Fp;
      intptr_t* sp = (intptr_t*)exceptionInfo->ContextRecord->Sp;
      address pc = (address)(exceptionInfo->ContextRecord->Lr
                         - NativeInstruction::instruction_size);
      *fr = frame(sp, fp, pc);
      if (!fr->is_java_frame()) {
        assert(fr->safe_for_sender(thread), "Safety check");
        assert(!fr->is_first_frame(), "Safety check");
        *fr = fr->java_sender();
      }
    }
  }
  assert(fr->is_java_frame(), "Safety check");
  return true;
}

frame os::get_sender_for_C_frame(frame* fr) {
  ShouldNotReachHere();
  return frame();
}

frame os::current_frame() {
  return frame();  // cannot walk Windows frames this way.  See os::get_native_stack
                   // and os::platform_print_native_stack
}

////////////////////////////////////////////////////////////////////////////////
// thread stack

// Minimum usable stack sizes required to get to user code. Space for
// HotSpot guard pages is added later.

/////////////////////////////////////////////////////////////////////////////
// helper functions for fatal error handler

void os::print_context(outputStream *st, const void *context) {
  if (context == nullptr) return;

  const CONTEXT* uc = (const CONTEXT*)context;

  st->print_cr("Registers:");

  st->print(  "X0 =" INTPTR_FORMAT, uc->X0);
  st->print(", X1 =" INTPTR_FORMAT, uc->X1);
  st->print(", X2 =" INTPTR_FORMAT, uc->X2);
  st->print(", X3 =" INTPTR_FORMAT, uc->X3);
  st->cr();
  st->print(  "X4 =" INTPTR_FORMAT, uc->X4);
  st->print(", X5 =" INTPTR_FORMAT, uc->X5);
  st->print(", X6 =" INTPTR_FORMAT, uc->X6);
  st->print(", X7 =" INTPTR_FORMAT, uc->X7);
  st->cr();
  st->print(  "X8 =" INTPTR_FORMAT, uc->X8);
  st->print(", X9 =" INTPTR_FORMAT, uc->X9);
  st->print(", X10=" INTPTR_FORMAT, uc->X10);
  st->print(", X11=" INTPTR_FORMAT, uc->X11);
  st->cr();
  st->print(  "X12=" INTPTR_FORMAT, uc->X12);
  st->print(", X13=" INTPTR_FORMAT, uc->X13);
  st->print(", X14=" INTPTR_FORMAT, uc->X14);
  st->print(", X15=" INTPTR_FORMAT, uc->X15);
  st->cr();
  st->print(  "X16=" INTPTR_FORMAT, uc->X16);
  st->print(", X17=" INTPTR_FORMAT, uc->X17);
  st->print(", X18=" INTPTR_FORMAT, uc->X18);
  st->print(", X19=" INTPTR_FORMAT, uc->X19);
  st->cr();
  st->print(", X20=" INTPTR_FORMAT, uc->X20);
  st->print(", X21=" INTPTR_FORMAT, uc->X21);
  st->print(", X22=" INTPTR_FORMAT, uc->X22);
  st->print(", X23=" INTPTR_FORMAT, uc->X23);
  st->cr();
  st->print(", X24=" INTPTR_FORMAT, uc->X24);
  st->print(", X25=" INTPTR_FORMAT, uc->X25);
  st->print(", X26=" INTPTR_FORMAT, uc->X26);
  st->print(", X27=" INTPTR_FORMAT, uc->X27);
  st->print(", X28=" INTPTR_FORMAT, uc->X28);
  st->cr();
  st->cr();
}

void os::print_register_info(outputStream *st, const void *context, int& continuation) {
  const int register_count = 29 /* X0-X28 */;
  int n = continuation;
  assert(n >= 0 && n <= register_count, "Invalid continuation value");
  if (context == nullptr || n == register_count) {
    return;
  }

  const CONTEXT* uc = (const CONTEXT*)context;
  while (n < register_count) {
    // Update continuation with next index before printing location
    continuation = n + 1;
# define CASE_PRINT_REG(n, str, id) case n: st->print(str); print_location(st, uc->id);
    switch (n) {
      CASE_PRINT_REG( 0, " X0=", X0); break;
      CASE_PRINT_REG( 1, " X1=", X1); break;
      CASE_PRINT_REG( 2, " X2=", X2); break;
      CASE_PRINT_REG( 3, " X3=", X3); break;
      CASE_PRINT_REG( 4, " X4=", X4); break;
      CASE_PRINT_REG( 5, " X5=", X5); break;
      CASE_PRINT_REG( 6, " X6=", X6); break;
      CASE_PRINT_REG( 7, " X7=", X7); break;
      CASE_PRINT_REG( 8, " X8=", X8); break;
      CASE_PRINT_REG( 9, " X9=", X9); break;
      CASE_PRINT_REG(10, "X10=", X10); break;
      CASE_PRINT_REG(11, "X11=", X11); break;
      CASE_PRINT_REG(12, "X12=", X12); break;
      CASE_PRINT_REG(13, "X13=", X13); break;
      CASE_PRINT_REG(14, "X14=", X14); break;
      CASE_PRINT_REG(15, "X15=", X15); break;
      CASE_PRINT_REG(16, "X16=", X16); break;
      CASE_PRINT_REG(17, "X17=", X17); break;
      CASE_PRINT_REG(18, "X18=", X18); break;
      CASE_PRINT_REG(19, "X19=", X19); break;
      CASE_PRINT_REG(20, "X20=", X20); break;
      CASE_PRINT_REG(21, "X21=", X21); break;
      CASE_PRINT_REG(22, "X22=", X22); break;
      CASE_PRINT_REG(23, "X23=", X23); break;
      CASE_PRINT_REG(24, "X24=", X24); break;
      CASE_PRINT_REG(25, "X25=", X25); break;
      CASE_PRINT_REG(26, "X26=", X26); break;
      CASE_PRINT_REG(27, "X27=", X27); break;
      CASE_PRINT_REG(28, "X28=", X28); break;
    }
# undef CASE_PRINT_REG
    ++n;
  }
}

void os::setup_fpu() {
}

#ifndef PRODUCT
void os::verify_stack_alignment() {
  assert(((intptr_t)os::current_stack_pointer() & (StackAlignmentInBytes-1)) == 0, "incorrect stack alignment");
}
#endif

int os::extra_bang_size_in_bytes() {
  // AArch64 does not require the additional stack bang.
  return 0;
}

extern "C" {
  int SpinPause() {
    return 0;
  }
};
