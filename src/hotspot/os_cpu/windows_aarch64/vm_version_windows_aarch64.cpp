/*
 * Copyright (c) 2020, 2021, Microsoft Corporation. All rights reserved.
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

#include "logging/log.hpp"
#include "runtime/os.hpp"
#include "runtime/vm_version.hpp"

#include <winreg.h>

// Try to read MIDR_EL1 via system register access.
// Some Windows versions do not emulate this EL1 register read from user mode
// and will throw EXCEPTION_ILLEGAL_INSTRUCTION, so we must guard with SEH.
static __int64 try_read_midr_el1() {
  __int64 midr = 0;
  __try {
    midr = _ReadStatusReg(0x4000 /* MIDR_EL1: o0=1,op1=0,CRn=0,CRm=0,op2=0 */);
  } __except(EXCEPTION_EXECUTE_HANDLER) {
    midr = 0;
  }
  return midr;
}

int VM_Version::get_current_sve_vector_length() {
  assert(_features & CPU_SVE, "should not call this");
  ShouldNotReachHere();
  return 0;
}

int VM_Version::set_and_get_current_sve_vector_length(int length) {
  assert(_features & CPU_SVE, "should not call this");
  ShouldNotReachHere();
  return 0;
}

void VM_Version::get_os_cpu_info() {

  if (IsProcessorFeaturePresent(PF_ARM_V8_CRC32_INSTRUCTIONS_AVAILABLE))   _features |= CPU_CRC32;
  if (IsProcessorFeaturePresent(PF_ARM_V8_CRYPTO_INSTRUCTIONS_AVAILABLE))  _features |= CPU_AES | CPU_SHA1 | CPU_SHA2;
  if (IsProcessorFeaturePresent(PF_ARM_VFP_32_REGISTERS_AVAILABLE))        _features |= CPU_ASIMD;
  // No check for CPU_PMULL, CPU_SVE, CPU_SVE2

  if (IsProcessorFeaturePresent(PF_ARM_V81_ATOMIC_INSTRUCTIONS_AVAILABLE)) {
    _features |= CPU_LSE;
  }

  __int64 dczid_el0 = _ReadStatusReg(0x5807 /* ARM64_DCZID_EL0 */);

  if (!(dczid_el0 & 0x10)) {
    _zva_length = 4 << (dczid_el0 & 0xf);
  }

  {
    PSYSTEM_LOGICAL_PROCESSOR_INFORMATION buffer = nullptr;
    DWORD returnLength = 0;

    // See https://docs.microsoft.com/en-us/windows/win32/api/sysinfoapi/nf-sysinfoapi-getlogicalprocessorinformation
    GetLogicalProcessorInformation(nullptr, &returnLength);
    assert(GetLastError() == ERROR_INSUFFICIENT_BUFFER, "Unexpected return from GetLogicalProcessorInformation");

    buffer = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION)os::malloc(returnLength, mtInternal);
    BOOL rc = GetLogicalProcessorInformation(buffer, &returnLength);
    assert(rc, "Unexpected return from GetLogicalProcessorInformation");

    _icache_line_size = _dcache_line_size = -1;
    for (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION ptr = buffer; ptr < buffer + returnLength / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION); ptr++) {
      switch (ptr->Relationship) {
      case RelationCache:
        // Cache data is in ptr->Cache, one CACHE_DESCRIPTOR structure for each cache.
        PCACHE_DESCRIPTOR Cache = &ptr->Cache;
        if (Cache->Level == 1) {
            _icache_line_size = _dcache_line_size = Cache->LineSize;
        }
        break;
      }
    }
    os::free(buffer);
  }

  // Identify CPU implementer, variant, part number, and revision.
  // First try reading MIDR_EL1 directly (works on some Windows builds).
  {
    __int64 midr = try_read_midr_el1();
    if (midr != 0) {
      _cpu      = (midr >> 24) & 0xff;
      _variant  = (midr >> 20) & 0xf;
      _model    = (midr >>  4) & 0xfff;
      _revision = (midr      ) & 0xf;
    } else {
      // Fallback: read "CP 4000" from the registry.
      // Windows stores raw system register values under
      // HKLM\HARDWARE\DESCRIPTION\System\CentralProcessor\0\CP <encoding>.
      // "CP 4000" is the raw MIDR_EL1 value (encoding 0x4000).
      // It is stored as REG_QWORD (64-bit) even though MIDR_EL1 only uses the low 32 bits.
      HKEY key;
      LONG rc = RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                        "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                        0, KEY_READ, &key);
      if (rc == ERROR_SUCCESS) {
        uint64_t cp4000 = 0;
        DWORD size = sizeof(cp4000);
        DWORD type = 0;
        LONG qrc = RegQueryValueExA(key, "CP 4000", nullptr, &type,
                             (LPBYTE)&cp4000, &size);
        if (qrc == ERROR_SUCCESS && (type == REG_QWORD || type == REG_DWORD) && cp4000 != 0) {
          _cpu      = (cp4000 >> 24) & 0xff;
          _variant  = (cp4000 >> 20) & 0xf;
          _model    = (cp4000 >>  4) & 0xfff;
          _revision = (cp4000      ) & 0xf;
        }
        RegCloseKey(key);
      }
      if (_cpu == 0) {
        log_info(os)("VM_Version: unable to identify CPU model");
      }
    }
  }
}

void VM_Version::get_compatible_board(char *buf, int buflen) {
  assert(buf != nullptr, "invalid argument");
  assert(buflen >= 1, "invalid argument");
  *buf = '\0';
}
