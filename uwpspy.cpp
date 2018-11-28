// stubs for some uwp api's and interfaces
// useful for debugging and reverse engineering
// also a nice base to copypaste for hooking

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <roapi.h>
#include <activation.h>
#include <inspectable.h>
#include <Windows.UI.Core.h>
#include <Windows.ApplicationModel.Core.h>
#include <Windows.Foundation.Diagnostics.h>
#pragma comment (lib, "ole32.lib")

namespace abi {
  using namespace ABI::Windows::UI::Core;
  using namespace ABI::Windows::ApplicationModel;
  using namespace ABI::Windows::ApplicationModel::Core;
  using namespace ABI::Windows::Foundation;
  using namespace ABI::Windows::Foundation::Collections;
  using namespace ABI::Windows::Foundation::Diagnostics;
}

#include <intrin.h>
#pragma intrinsic(_ReturnAddress)

#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#define dbg \
  wprintf(L"[%hs:%d] %hs: ", __FILE__, __LINE__, __FUNCTION__), \
  wprintln

int wprintln(WCHAR* fmt, ...) {
  int res = 0;
  va_list va;
  va_start(va, fmt);
  res += vwprintf(fmt, va);
  va_end(va);
  res += wprintf(L"\n");
  return res;
}

// ------------------------------------------------------------------------

void log_init() {
  COORD bufsize;
  int success = (
    AttachConsole(ATTACH_PARENT_PROCESS) ||
    AttachConsole(GetCurrentProcessId()) ||
    AllocConsole()
  );
  if (success) {
    freopen("CONOUT$", "w", stdout);
    setvbuf(stdout, 0, _IONBF, 0);
    dbg(L"initialized console");
  } else {
    dbg(L"running in console mode");
  }
  bufsize.X = 80;
  bufsize.Y = 999;
  SetConsoleScreenBufferSize(GetStdHandle(STD_OUTPUT_HANDLE), bufsize);
}

typedef struct {
  void* addr;
  size_t n;
  DWORD fl;
} spy_protection_t;

int spy_protpush(spy_protection_t* prot, void* addr, size_t n) {
  DWORD oldprot;
  DWORD* poldprot;
  if (prot) {
    prot->addr = addr;
    prot->n = n;
    poldprot = &prot->fl;
  } else {
    poldprot = &oldprot;
  }
  if (!VirtualProtect(addr, n, PAGE_EXECUTE_READWRITE, poldprot)) {
    dbg(L"VirtualProtect %p failed: %08X", addr, GetLastError());
    return 0;
  }
  return 1;
}

int spy_protpop(spy_protection_t* prot) {
  DWORD dummy;
  if (!VirtualProtect(prot->addr, prot->n, prot->fl, &dummy)) {
    dbg(L"VirtualProtect failed: %08X", GetLastError());
    return 0;
  }
  return 1;
}

int spy_rva(void* base, DWORD* rva, void* hook) {
  spy_protection_t p;
  if (spy_protpush(&p, rva, sizeof(*rva))) {
    *rva = (DWORD)((char*)hook - (char*)base);
    return spy_protpop(&p);
  }
  return 0;
}

#if defined(_M_X64) && _M_X64 == 100
#define JMPABS_SIZE 12
#define JMPREL_SIZE 5

void jmpabs(void* p, void* dst) {
  unsigned char* code = (unsigned char*)p;
  code = (unsigned char*)p;
  code[0] = 0x4E; /* mov rax,imm64 */
  code[1] = 0xB8;
  *(UINT64*)&code[2] = (UINT64)dst;
  code[10] = 0xFF; /* jmp rax */
  code[11] = 0xE0;
}

void jmprel(void* p, void* dst) {
  unsigned char* code = (unsigned char*)p;
  code = (unsigned char*)p;
  code[0] = 0xE9;
  *(INT32*)&code[1] = (INT32)((char*)dst - (char*)p - 5);
}

void* rel_alloc(void* base, size_t n) {
  ULONG_PTR scan_addr;
  SYSTEM_INFO si;
  ULONG_PTR min_addr;
  ULONG_PTR max_addr;
  /* scan for unused memory blocks in rel32 range */
  GetSystemInfo(&si);
  min_addr = max((ULONG_PTR)si.lpMinimumApplicationAddress,
    (ULONG_PTR)base - 0x40000000);
  max_addr = min((ULONG_PTR)si.lpMaximumApplicationAddress,
    (ULONG_PTR)base + 0x40000000);
  scan_addr = min_addr;
  scan_addr -= scan_addr % si.dwAllocationGranularity;
  while (scan_addr < max_addr) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((void*)scan_addr, &mbi, sizeof(mbi))) {
      dbg(L"VirtualQuery failed: %08X", GetLastError());
      break;
    }
    if (mbi.State == MEM_FREE) {
      void* p = VirtualAlloc((void*)scan_addr, n,
        MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
      if (p) {
        return p;
      } else {
        dbg(L"VirtualAlloc failed: %08X", GetLastError());
      }
    }
    scan_addr = (ULONG_PTR)mbi.BaseAddress + mbi.RegionSize;
    scan_addr += si.dwAllocationGranularity - 1;
    scan_addr -= scan_addr % si.dwAllocationGranularity;
  }
  return 0;
}
#else
#error "arch not supported"
#endif

/*
 * using a short jump makes it easier to hook because it overwrites a lot
 * less code, which means I almost never have to hand-fix the trampoline
 *
 * target:
 *   jmp rel -> relay
 * ...
 * target_ret:
 * ...
 *
 * relay:
 *   mov rax,hook
 *   jmp rax
 *
 * trampoline:
 *   (orig code)
 *   mov rax,target_ret
 *   jmp rax
 */

int spy_hook(void* func, void* hook, size_t nops, void** ptrampoline) {
  spy_protection_t p;
  char* relay = 0;
  size_t hook_size = JMPREL_SIZE + nops;
  if (!spy_protpush(&p, func, hook_size)) {
    return 0;
  }
  relay = (char*)rel_alloc(func, JMPABS_SIZE);
  if (!relay) {
    dbg(L"couldn't find free memory in rel range for %p", func);
    return 0;
  }
  if (!ptrampoline || !*ptrampoline) {
    size_t trampoline_size = hook_size + JMPABS_SIZE;
    char* trampoline = (char*)malloc(trampoline_size);
    if (!spy_protpush(0, trampoline, trampoline_size)) {
      return 0;
    }
    memcpy(trampoline, func, hook_size);
    jmpabs(&trampoline[hook_size], (char*)func + hook_size);
    if (ptrampoline) {
      *ptrampoline = trampoline;
    }
  }
  jmpabs(relay, hook);
  jmprel(func, relay);
  memset((char*)func + JMPREL_SIZE, 0x90, nops);
  return spy_protpop(&p);
}

typedef struct {
  char* name;
  void* hook;
  void** trampoline;
  int nops;
} spy_export_t;

int spy_exports(char* module_name, int n_hooks, spy_export_t* hooks) {
  char* module;
  IMAGE_NT_HEADERS* nt;
  IMAGE_DATA_DIRECTORY* dir;
  IMAGE_EXPORT_DIRECTORY* exports;
  DWORD i;
  DWORD* name_off;
  DWORD* function_off;
  WORD* ordinals;

  module = (char*)GetModuleHandleA(module_name);
  if (!module) {
    dbg(L"%hs not found", module_name);
    return 0;
  }

  nt = (IMAGE_NT_HEADERS*)(module + PIMAGE_DOS_HEADER(module)->e_lfanew);
  dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
  exports = (IMAGE_EXPORT_DIRECTORY*)(module + dir->VirtualAddress);
  name_off = (DWORD*)(module + exports->AddressOfNames);
  function_off = (DWORD*)(module + exports->AddressOfFunctions);
  ordinals = (WORD*)(module + exports->AddressOfNameOrdinals);

  dbg(L":: %hs", module_name);

  for (i = 0; i < exports->NumberOfNames; ++i) {
    char* name = module + name_off[i];
    int ordinal = (int)ordinals[i];
    unsigned char* func = (unsigned char*)module + function_off[ordinal];
    int j;
    for (j = 0; j < n_hooks; ++j) {
      spy_export_t* hook = &hooks[j];
      if (!_stricmp(hook->name, name)) {
        LONG_PTR distance = (char*)hook->hook - module;
        if (!hook->hook) {
          if (hook->trampoline) {
            *hook->trampoline = func;
          }
          break;
        }
        dbg(L"%p -> %hs @%d", func, name, ordinal);
        if (distance & ~(LONG_PTR)0xFFFFFFFF) {
          spy_hook(func, hook->hook, hook->nops, hook->trampoline);
        } else {
          dbg(L"function is nearby, using simple iat hook");
          spy_rva(module, &function_off[ordinal], hook->hook);
          if (hook->trampoline) {
            *hook->trampoline = func;
          }
        }
        break;
      }
    }
  }

  return 1;
}

// ------------------------------------------------------------------------

PCWSTR (STDMETHODCALLTYPE* _WindowsGetStringRawBuffer)
  (HSTRING s, UINT32* len);

// ------------------------------------------------------------------------

#define dumpstr(x) dbg(L"" #x L"=%s", _WindowsGetStringRawBuffer(x, 0))
#define dumpptr(fmt, x) dbg(L"" #x L"=%p -> " fmt, (x), (x) ? *(x) : 0)
#define dumpiid(x) { \
  LPOLESTR iidstr; \
  if (StringFromIID(x, &iidstr) == S_OK) { \
    dbg(L"" #x L"=%s", iidstr); \
    CoTaskMemFree(iidstr); \
  } \
}
#define dump(fmt, x) dbg(L"" #x L"=" fmt, x)

// ------------------------------------------------------------------------

struct __declspec(uuid("00000035-0000-0000-C000-000000000046"))
_IActivationFactory : public IActivationFactory {
  IActivationFactory* orig;

  explicit _IActivationFactory(IActivationFactory* orig)
    : orig(orig)
  {
    dump("%p", orig);
  }

  // IUnknown

  virtual HRESULT STDMETHODCALLTYPE
    QueryInterface(REFIID riid, void** obj) override
  {
    dbg(L"called from %p", _ReturnAddress());
    dumpiid(riid);
    HRESULT hr = orig->QueryInterface(riid, obj);
    dump("%08X", hr);
    if (SUCCEEDED(hr)) {
      dumpptr(L"%p", obj);
    }
    return hr;
  }

  virtual ULONG STDMETHODCALLTYPE AddRef() override {
    dbg(L"called from %p", _ReturnAddress());
    ULONG refcount = orig->AddRef();
    dump(L"%u", refcount);
    return refcount;
  }

  virtual ULONG STDMETHODCALLTYPE Release() override {
    dbg(L"called from %p", _ReturnAddress());
    ULONG refcount = orig->Release();
    dump(L"%u", refcount);
    if (!refcount) {
      delete this;
    }
    return refcount;
  }

  // IInspectable

  virtual HRESULT STDMETHODCALLTYPE
    GetIids(ULONG* n_iids, IID** iids) override
  {
    dbg(L"called from %p", _ReturnAddress());
    HRESULT hr = orig->GetIids(n_iids, iids);
    dump("%08X", hr);
    if (SUCCEEDED(hr)) {
      for (ULONG i = 0; i < *n_iids; ++i) {
        dumpiid((*iids)[i]);
      }
    }
    return hr;
  }

  virtual HRESULT STDMETHODCALLTYPE
    GetRuntimeClassName(HSTRING* name) override
  {
    dbg(L"called from %p", _ReturnAddress());
    HRESULT hr = orig->GetRuntimeClassName(name);
    dump("%08X", hr);
    if (SUCCEEDED(hr)) {
      dumpstr(*name);
    }
    return hr;
  }

  virtual HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel* l) override {
    dbg(L"called from %p", _ReturnAddress());
    HRESULT hr = orig->GetTrustLevel(l);
    dump("%08X", hr);
    if (SUCCEEDED(hr)) {
      char* level = "Unknown";
      switch (*l) {
        #define c(x) case x: level = #x; break
        c(BaseTrust);
        c(PartialTrust);
        c(FullTrust);
        #undef c
      }
      dump(L"%hs", level);
    }
    return hr;
  }

  // IActivationFactory

  virtual HRESULT STDMETHODCALLTYPE
    ActivateInstance(IInspectable** instance) override
  {
    dbg(L"called from %p", _ReturnAddress());
    HRESULT hr = orig->ActivateInstance(instance);
    dump("%08X", hr);
    if (SUCCEEDED(hr)) {
      dumpptr("%p", instance);
    }
    return hr;
  }
};

// ------------------------------------------------------------------------

struct __declspec(uuid("CF86461D-261E-4B72-9ACD-44ED2ACE6A29"))
_ICoreApplicationExit : public abi::ICoreApplicationExit {
  abi::ICoreApplicationExit* orig;

  explicit _ICoreApplicationExit(abi::ICoreApplicationExit* orig)
    : orig(orig)
  {
    dump("%p", orig);
  }

  // IUnknown

  virtual HRESULT STDMETHODCALLTYPE
    QueryInterface(REFIID riid, void** obj) override
  {
    dbg(L"called from %p", _ReturnAddress());
    dumpiid(riid);
    HRESULT hr = orig->QueryInterface(riid, obj);
    dump("%08X", hr);
    if (SUCCEEDED(hr)) {
      dumpptr(L"%p", obj);
    }
    return hr;
  }

  virtual ULONG STDMETHODCALLTYPE AddRef() override {
    dbg(L"called from %p", _ReturnAddress());
    ULONG refcount = orig->AddRef();
    dump(L"%u", refcount);
    return refcount;
  }

  virtual ULONG STDMETHODCALLTYPE Release() override {
    dbg(L"called from %p", _ReturnAddress());
    ULONG refcount = orig->Release();
    dump(L"%u", refcount);
    if (!refcount) {
      delete this;
    }
    return refcount;
  }

  // IInspectable

  virtual HRESULT STDMETHODCALLTYPE
    GetIids(ULONG* n_iids, IID** iids) override
  {
    dbg(L"called from %p", _ReturnAddress());
    HRESULT hr = orig->GetIids(n_iids, iids);
    dump("%08X", hr);
    if (SUCCEEDED(hr)) {
      for (ULONG i = 0; i < *n_iids; ++i) {
        dumpiid((*iids)[i]);
      }
    }
    return hr;
  }

  virtual HRESULT STDMETHODCALLTYPE
    GetRuntimeClassName(HSTRING* name) override
  {
    dbg(L"called from %p", _ReturnAddress());
    HRESULT hr = orig->GetRuntimeClassName(name);
    dump("%08X", hr);
    if (SUCCEEDED(hr)) {
      dumpstr(*name);
    }
    return hr;
  }

  virtual HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel* l) override {
    dbg(L"called from %p", _ReturnAddress());
    HRESULT hr = orig->GetTrustLevel(l);
    dump("%08X", hr);
    if (SUCCEEDED(hr)) {
      char* level = "Unknown";
      switch (*l) {
        #define c(x) case x: level = #x; break
        c(BaseTrust);
        c(PartialTrust);
        c(FullTrust);
        #undef c
      }
      dump(L"%hs", level);
    }
    return hr;
  }

  // ICoreApplicationExit

  virtual HRESULT STDMETHODCALLTYPE Exit() override {
    dbg(L"called from %p", _ReturnAddress());
    HRESULT hr = orig->Exit();
    dump("%08X", hr);
    return hr;
  }

  virtual HRESULT STDMETHODCALLTYPE add_Exiting(
    abi::IEventHandler<IInspectable*>* handler,
    EventRegistrationToken* token) override
  {
    dbg(L"called from %p", _ReturnAddress());
    HRESULT hr = orig->add_Exiting(handler, token);
    dump("%08X", hr);
    return hr;
  }

  virtual HRESULT STDMETHODCALLTYPE remove_Exiting(
    EventRegistrationToken token) override
  {
    dbg(L"called from %p", _ReturnAddress());
    HRESULT hr = orig->remove_Exiting(token);
    dump("%08X", hr);
    return hr;
  }
};

// ------------------------------------------------------------------------


struct __declspec(uuid("0AACF7A4-5E1D-49DF-8034-FB6A68BC5ED1"))
_ICoreApplication : public abi::ICoreApplication
{
  abi::ICoreApplication* orig;

  explicit _ICoreApplication(abi::ICoreApplication* orig)
    : orig(orig)
  {
    dump("%p", orig);
  }

  // IUnknown

  virtual HRESULT STDMETHODCALLTYPE
    QueryInterface(REFIID riid, void** obj) override
  {
    void* res;
    dbg(L"called from %p", _ReturnAddress());
    dumpiid(riid);
    HRESULT hr = orig->QueryInterface(riid, &res);
    dump("%08X", hr);
    if (FAILED(hr)) {
      *obj = res;
    } else if (riid == __uuidof(abi::ICoreApplicationExit)) {
      *obj = new _ICoreApplicationExit((abi::ICoreApplicationExit*)res);
    } else {
      *obj = res;
    }
    if (SUCCEEDED(hr)) {
      dumpptr(L"%p", obj);
    }
    return hr;
  }

  virtual ULONG STDMETHODCALLTYPE AddRef() override {
    dbg(L"called from %p", _ReturnAddress());
    ULONG refcount = orig->AddRef();
    dump(L"%u", refcount);
    return refcount;
  }

  virtual ULONG STDMETHODCALLTYPE Release() override {
    dbg(L"called from %p", _ReturnAddress());
    ULONG refcount = orig->Release();
    dump(L"%u", refcount);
    if (!refcount) {
      delete this;
    }
    return refcount;
  }

  // IInspectable

  virtual HRESULT STDMETHODCALLTYPE
    GetIids(ULONG* n_iids, IID** iids) override
  {
    dbg(L"called from %p", _ReturnAddress());
    HRESULT hr = orig->GetIids(n_iids, iids);
    dump("%08X", hr);
    if (SUCCEEDED(hr)) {
      for (ULONG i = 0; i < *n_iids; ++i) {
        dumpiid((*iids)[i]);
      }
    }
    return hr;
  }

  virtual HRESULT STDMETHODCALLTYPE
    GetRuntimeClassName(HSTRING* name) override
  {
    dbg(L"called from %p", _ReturnAddress());
    HRESULT hr = orig->GetRuntimeClassName(name);
    dump("%08X", hr);
    if (SUCCEEDED(hr)) {
      dumpstr(*name);
    }
    return hr;
  }

  virtual HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel* l) override {
    dbg(L"called from %p", _ReturnAddress());
    HRESULT hr = orig->GetTrustLevel(l);
    dump("%08X", hr);
    if (SUCCEEDED(hr)) {
      char* level = "Unknown";
      switch (*l) {
        #define c(x) case x: level = #x; break
        c(BaseTrust);
        c(PartialTrust);
        c(FullTrust);
        #undef c
      }
      dump(L"%hs", level);
    }
    return hr;
  }

  // ICoreApplication

  virtual HRESULT STDMETHODCALLTYPE get_Id(HSTRING* value) override {
    dbg(L"called from %p", _ReturnAddress());
    HRESULT hr = orig->get_Id(value);
    dump("%08X", hr);
    if (SUCCEEDED(hr)) {
      dumpstr(*value);
    }
    return hr;
  }

  virtual HRESULT STDMETHODCALLTYPE add_Suspending(
    abi::IEventHandler<abi::SuspendingEventArgs*>* h,
    EventRegistrationToken* token) override
  {
    dbg(L"called from %p", _ReturnAddress());
    HRESULT hr = orig->add_Suspending(h, token);
    dump("%08X", hr);
    return hr;
  }

  virtual HRESULT STDMETHODCALLTYPE remove_Suspending(
    EventRegistrationToken token) override
  {
    dbg(L"called from %p", _ReturnAddress());
    HRESULT hr = orig->remove_Suspending(token);
    dump("%08X", hr);
    return hr;
  }

  virtual HRESULT STDMETHODCALLTYPE add_Resuming(
    abi::IEventHandler<IInspectable*>* handler,
    EventRegistrationToken* token) override
  {
    dbg(L"called from %p", _ReturnAddress());
    HRESULT hr = orig->add_Resuming(handler, token);
    dump("%08X", hr);
    return hr;
  }

  virtual HRESULT STDMETHODCALLTYPE remove_Resuming(
    EventRegistrationToken token) override
  {
    dbg(L"called from %p", _ReturnAddress());
    HRESULT hr = orig->remove_Resuming(token);
    dump("%08X", hr);
    return hr;
  }

  virtual HRESULT STDMETHODCALLTYPE get_Properties(
    abi::IPropertySet** value) override
  {
    dbg(L"called from %p", _ReturnAddress());
    HRESULT hr = orig->get_Properties(value);
    dump("%08X", hr);
    return hr;
  }

  virtual HRESULT STDMETHODCALLTYPE GetCurrentView(
    abi::ICoreApplicationView** value) override
  {
    dbg(L"called from %p", _ReturnAddress());
    HRESULT hr = orig->GetCurrentView(value);
    dump("%08X", hr);
    return hr;
  }

  virtual HRESULT STDMETHODCALLTYPE Run(
    abi::IFrameworkViewSource* s) override
  {
    dbg(L"called from %p", _ReturnAddress());
    HRESULT hr = orig->Run(s);
    dump("%08X", hr);
    return hr;
  }

  virtual HRESULT STDMETHODCALLTYPE RunWithActivationFactories(
    abi::IGetActivationFactory* callback) override
  {
    dbg(L"called from %p", _ReturnAddress());
    HRESULT hr = orig->RunWithActivationFactories(callback);
    dump("%08X", hr);
    return hr;
  }
};

// ------------------------------------------------------------------------

struct __declspec(uuid("8A43ED9F-F4E6-4421-ACF9-1DAB2986820C"))
_IPropertySet : public abi::IPropertySet {
  IPropertySet* orig;

  explicit _IPropertySet(abi::IPropertySet* orig)
    : orig(orig)
  {
    dump("%p", orig);
  }

  // IUnknown

  virtual HRESULT STDMETHODCALLTYPE
    QueryInterface(REFIID riid, void** obj) override
  {
    dbg(L"called from %p", _ReturnAddress());
    dumpiid(riid);
    HRESULT hr = orig->QueryInterface(riid, obj);
    dump("%08X", hr);
    if (SUCCEEDED(hr)) {
      dumpptr(L"%p", obj);
    }
    return hr;
  }

  virtual ULONG STDMETHODCALLTYPE AddRef() override {
    dbg(L"called from %p", _ReturnAddress());
    ULONG refcount = orig->AddRef();
    dump(L"%u", refcount);
    return refcount;
  }

  virtual ULONG STDMETHODCALLTYPE Release() override {
    dbg(L"called from %p", _ReturnAddress());
    ULONG refcount = orig->Release();
    dump(L"%u", refcount);
    if (!refcount) {
      delete this;
    }
    return refcount;
  }

  // IInspectable

  virtual HRESULT STDMETHODCALLTYPE
    GetIids(ULONG* n_iids, IID** iids) override
  {
    dbg(L"called from %p", _ReturnAddress());
    HRESULT hr = orig->GetIids(n_iids, iids);
    dump("%08X", hr);
    if (SUCCEEDED(hr)) {
      for (ULONG i = 0; i < *n_iids; ++i) {
        dumpiid((*iids)[i]);
      }
    }
    return hr;
  }

  virtual HRESULT STDMETHODCALLTYPE
    GetRuntimeClassName(HSTRING* name) override
  {
    dbg(L"called from %p", _ReturnAddress());
    HRESULT hr = orig->GetRuntimeClassName(name);
    dump("%08X", hr);
    if (SUCCEEDED(hr)) {
      dumpstr(*name);
    }
    return hr;
  }

  virtual HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel* l) override {
    dbg(L"called from %p", _ReturnAddress());
    HRESULT hr = orig->GetTrustLevel(l);
    dump("%08X", hr);
    if (SUCCEEDED(hr)) {
      char* level = "Unknown";
      switch (*l) {
        #define c(x) case x: level = #x; break
        c(BaseTrust);
        c(PartialTrust);
        c(FullTrust);
        #undef c
      }
      dump(L"%hs", level);
    }
    return hr;
  }
};

// ------------------------------------------------------------------------

struct __declspec(uuid("4D239005-3C2A-41B1-9022-536BB9CF93B1"))
_ICoreWindowStatic : public abi::ICoreWindowStatic {
  abi::ICoreWindowStatic* orig;

  explicit _ICoreWindowStatic(abi::ICoreWindowStatic* orig)
    : orig(orig)
  {
    dump("%p", orig);
  }

  // IUnknown

  virtual HRESULT STDMETHODCALLTYPE
    QueryInterface(REFIID riid, void** obj) override
  {
    dbg(L"called from %p", _ReturnAddress());
    dumpiid(riid);
    HRESULT hr = orig->QueryInterface(riid, obj);
    dump("%08X", hr);
    if (SUCCEEDED(hr)) {
      dumpptr(L"%p", obj);
    }
    return hr;
  }

  virtual ULONG STDMETHODCALLTYPE AddRef() override {
    dbg(L"called from %p", _ReturnAddress());
    ULONG refcount = orig->AddRef();
    dump(L"%u", refcount);
    return refcount;
  }

  virtual ULONG STDMETHODCALLTYPE Release() override {
    dbg(L"called from %p", _ReturnAddress());
    ULONG refcount = orig->Release();
    dump(L"%u", refcount);
    if (!refcount) {
      delete this;
    }
    return refcount;
  }

  // IInspectable

  virtual HRESULT STDMETHODCALLTYPE
    GetIids(ULONG* n_iids, IID** iids) override
  {
    dbg(L"called from %p", _ReturnAddress());
    HRESULT hr = orig->GetIids(n_iids, iids);
    dump("%08X", hr);
    if (SUCCEEDED(hr)) {
      for (ULONG i = 0; i < *n_iids; ++i) {
        dumpiid((*iids)[i]);
      }
    }
    return hr;
  }

  virtual HRESULT STDMETHODCALLTYPE
    GetRuntimeClassName(HSTRING* name) override
  {
    dbg(L"called from %p", _ReturnAddress());
    HRESULT hr = orig->GetRuntimeClassName(name);
    dump("%08X", hr);
    if (SUCCEEDED(hr)) {
      dumpstr(*name);
    }
    return hr;
  }

  virtual HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel* l) override {
    dbg(L"called from %p", _ReturnAddress());
    HRESULT hr = orig->GetTrustLevel(l);
    dump("%08X", hr);
    if (SUCCEEDED(hr)) {
      char* level = "Unknown";
      switch (*l) {
        #define c(x) case x: level = #x; break
        c(BaseTrust);
        c(PartialTrust);
        c(FullTrust);
        #undef c
      }
      dump(L"%hs", level);
    }
    return hr;
  }

  // ICoreWindowStatic

  virtual HRESULT STDMETHODCALLTYPE GetForCurrentThread(
    abi::ICoreWindow** pwindow) override
  {
    dbg(L"called from %p", _ReturnAddress());
    HRESULT hr = orig->GetForCurrentThread(pwindow);
    if (SUCCEEDED(hr)) {
      dumpptr("%p", pwindow);
    }
    dump("%08X", hr);
    return hr;
  }
};

// ------------------------------------------------------------------------

struct __declspec(uuid("65A1ECC5-3FB5-4832-8CA9-F061B281D13A"))
_IDeferralFactory : public abi::IDeferralFactory {
  abi::IDeferralFactory* orig;

  explicit _IDeferralFactory(abi::IDeferralFactory* orig)
    : orig(orig)
  {
    dump("%p", orig);
  }

  // IUnknown

  virtual HRESULT STDMETHODCALLTYPE
    QueryInterface(REFIID riid, void** obj) override
  {
    dbg(L"called from %p", _ReturnAddress());
    dumpiid(riid);
    HRESULT hr = orig->QueryInterface(riid, obj);
    dump("%08X", hr);
    if (SUCCEEDED(hr)) {
      dumpptr(L"%p", obj);
    }
    return hr;
  }

  virtual ULONG STDMETHODCALLTYPE AddRef() override {
    dbg(L"called from %p", _ReturnAddress());
    ULONG refcount = orig->AddRef();
    dump(L"%u", refcount);
    return refcount;
  }

  virtual ULONG STDMETHODCALLTYPE Release() override {
    dbg(L"called from %p", _ReturnAddress());
    ULONG refcount = orig->Release();
    dump(L"%u", refcount);
    if (!refcount) {
      delete this;
    }
    return refcount;
  }

  // IInspectable

  virtual HRESULT STDMETHODCALLTYPE
    GetIids(ULONG* n_iids, IID** iids) override
  {
    dbg(L"called from %p", _ReturnAddress());
    HRESULT hr = orig->GetIids(n_iids, iids);
    dump("%08X", hr);
    if (SUCCEEDED(hr)) {
      for (ULONG i = 0; i < *n_iids; ++i) {
        dumpiid((*iids)[i]);
      }
    }
    return hr;
  }

  virtual HRESULT STDMETHODCALLTYPE
    GetRuntimeClassName(HSTRING* name) override
  {
    dbg(L"called from %p", _ReturnAddress());
    HRESULT hr = orig->GetRuntimeClassName(name);
    dump("%08X", hr);
    if (SUCCEEDED(hr)) {
      dumpstr(*name);
    }
    return hr;
  }

  virtual HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel* l) override {
    dbg(L"called from %p", _ReturnAddress());
    HRESULT hr = orig->GetTrustLevel(l);
    dump("%08X", hr);
    if (SUCCEEDED(hr)) {
      char* level = "Unknown";
      switch (*l) {
        #define c(x) case x: level = #x; break
        c(BaseTrust);
        c(PartialTrust);
        c(FullTrust);
        #undef c
      }
      dump(L"%hs", level);
    }
    return hr;
  }

  // IDeferralFactory

  virtual HRESULT STDMETHODCALLTYPE Create(
    abi::IDeferralCompletedHandler* handler, abi::IDeferral** result)
    override
  {
    dbg(L"called from %p", _ReturnAddress());
    HRESULT hr = orig->Create(handler, result);
    dump("%08X", hr);
    if (SUCCEEDED(hr)) {
      dumpptr("%p", result);
    }
    return hr;
  }
};

// ------------------------------------------------------------------------

struct __declspec(uuid("50850B26-267E-451B-A890-AB6A370245EE"))
_IAsyncCausalityTracerStatics : public abi::IAsyncCausalityTracerStatics {
  abi::IAsyncCausalityTracerStatics* orig;

  explicit _IAsyncCausalityTracerStatics(abi::IAsyncCausalityTracerStatics* orig)
    : orig(orig)
  {
    dump("%p", orig);
  }

  // IUnknown

  virtual HRESULT STDMETHODCALLTYPE
    QueryInterface(REFIID riid, void** obj) override
  {
    dbg(L"called from %p", _ReturnAddress());
    dumpiid(riid);
    HRESULT hr = orig->QueryInterface(riid, obj);
    dump("%08X", hr);
    if (SUCCEEDED(hr)) {
      dumpptr(L"%p", obj);
    }
    return hr;
  }

  virtual ULONG STDMETHODCALLTYPE AddRef() override {
    dbg(L"called from %p", _ReturnAddress());
    ULONG refcount = orig->AddRef();
    dump(L"%u", refcount);
    return refcount;
  }

  virtual ULONG STDMETHODCALLTYPE Release() override {
    dbg(L"called from %p", _ReturnAddress());
    ULONG refcount = orig->Release();
    dump(L"%u", refcount);
    if (!refcount) {
      delete this;
    }
    return refcount;
  }

  // IInspectable

  virtual HRESULT STDMETHODCALLTYPE
    GetIids(ULONG* n_iids, IID** iids) override
  {
    dbg(L"called from %p", _ReturnAddress());
    HRESULT hr = orig->GetIids(n_iids, iids);
    dump("%08X", hr);
    if (SUCCEEDED(hr)) {
      for (ULONG i = 0; i < *n_iids; ++i) {
        dumpiid((*iids)[i]);
      }
    }
    return hr;
  }

  virtual HRESULT STDMETHODCALLTYPE
    GetRuntimeClassName(HSTRING* name) override
  {
    dbg(L"called from %p", _ReturnAddress());
    HRESULT hr = orig->GetRuntimeClassName(name);
    dump("%08X", hr);
    if (SUCCEEDED(hr)) {
      dumpstr(*name);
    }
    return hr;
  }

  virtual HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel* l) override {
    dbg(L"called from %p", _ReturnAddress());
    HRESULT hr = orig->GetTrustLevel(l);
    dump("%08X", hr);
    if (SUCCEEDED(hr)) {
      char* level = "Unknown";
      switch (*l) {
        #define c(x) case x: level = #x; break
        c(BaseTrust);
        c(PartialTrust);
        c(FullTrust);
        #undef c
      }
      dump(L"%hs", level);
    }
    return hr;
  }

  // IAsyncCausalityTracerStatics

  static char const* TraceLevelStr(abi::CausalityTraceLevel trace_level) {
    #define c(x) case x: return #x;
    switch (trace_level) {
      c(abi::CausalityTraceLevel_Required);
      c(abi::CausalityTraceLevel_Important);
      c(abi::CausalityTraceLevel_Verbose);
    }
    #undef c
    return "?";
  }

  static char const* SourceStr(abi::CausalitySource source) {
    #define c(x) case x: return #x;
    switch (source) {
      c(abi::CausalitySource_Application);
      c(abi::CausalitySource_Library);
      c(abi::CausalitySource_System);
    }
    #undef c
    return "?";
  }

  static char const* AsyncStatusStr(AsyncStatus status) {
    #define c(x) case x: return #x;
    switch (status) {
      c(Canceled);
      c(Completed);
      c(Error);
      c(Started);
    }
    #undef c
    return "?";
  }

  static char const* CausalityRelationStr(abi::CausalityRelation rel) {
    #define c(x) case x: return #x;
    switch (rel) {
      c(abi::CausalityRelation_AssignDelegate);
      c(abi::CausalityRelation_Join);
      c(abi::CausalityRelation_Choice);
      c(abi::CausalityRelation_Cancel);
      c(abi::CausalityRelation_Error);
    }
    #undef c
    return "?";
  }

  static char const* CausalitySynchronousWorkStr(
    abi::CausalitySynchronousWork work) {
    #define c(x) case x: return #x;
    switch (work) {
      c(abi::CausalitySynchronousWork_CompletionNotification);
      c(abi::CausalitySynchronousWork_ProgressNotification);
      c(abi::CausalitySynchronousWork_Execution);
    }
    #undef c
    return "?";
  }

  virtual HRESULT STDMETHODCALLTYPE TraceOperationCreation(
    abi::CausalityTraceLevel trace_level, abi::CausalitySource source,
    GUID platform_id, UINT64 operation_id,
    HSTRING operation_name, UINT64 related_context) override
  {
    dbg(L"called from %p", _ReturnAddress());
    dump(L"%hs", TraceLevelStr(trace_level));
    dump(L"%hs", SourceStr(source));
    dumpiid(platform_id);
    dump("%I64d", operation_id);
    dumpstr(operation_name);
    dump("%I64u", related_context);
    HRESULT hr = orig->TraceOperationCreation(trace_level, source,
      platform_id, operation_id, operation_name, related_context);
    dump("%08X", hr);
    return hr;
  }

  virtual HRESULT STDMETHODCALLTYPE TraceOperationCompletion(
    abi::CausalityTraceLevel trace_level, abi::CausalitySource source,
    GUID platform_id, UINT64 operation_id, AsyncStatus status) override
  {
    dbg(L"called from %p", _ReturnAddress());
    dump(L"%hs", TraceLevelStr(trace_level));
    dump(L"%hs", SourceStr(source));
    dumpiid(platform_id);
    dump("%I64d", operation_id);
    dump(L"%hs", AsyncStatusStr(status));
    HRESULT hr = orig->TraceOperationCompletion(trace_level, source,
      platform_id, operation_id, status);
    dump("%08X", hr);
    return hr;
  }

  virtual HRESULT STDMETHODCALLTYPE TraceOperationRelation(
    abi::CausalityTraceLevel trace_level, abi::CausalitySource source,
    GUID platform_id, UINT64 operation_id, abi::CausalityRelation relation)
    override
  {
    dbg(L"called from %p", _ReturnAddress());
    dump(L"%hs", TraceLevelStr(trace_level));
    dump(L"%hs", SourceStr(source));
    dumpiid(platform_id);
    dump("%I64d", operation_id);
    dump(L"%hs", CausalityRelationStr(relation));
    HRESULT hr = orig->TraceOperationRelation(trace_level, source,
      platform_id, operation_id, relation);
    dump("%08X", hr);
    return hr;
  }

  virtual HRESULT STDMETHODCALLTYPE TraceSynchronousWorkStart(
    abi::CausalityTraceLevel trace_level, abi::CausalitySource source,
    GUID platform_id, UINT64 operation_id,
    abi::CausalitySynchronousWork work) override
  {
    dbg(L"called from %p", _ReturnAddress());
    dump(L"%hs", TraceLevelStr(trace_level));
    dump(L"%hs", SourceStr(source));
    dumpiid(platform_id);
    dump("%I64d", operation_id);
    dump(L"%hs", CausalitySynchronousWorkStr(work));
    HRESULT hr = orig->TraceSynchronousWorkStart(trace_level, source,
      platform_id, operation_id, work);
    dump("%08X", hr);
    return hr;
  }

  virtual HRESULT STDMETHODCALLTYPE TraceSynchronousWorkCompletion(
    abi::CausalityTraceLevel trace_level, abi::CausalitySource source,
    abi::CausalitySynchronousWork work) override
  {
    dbg(L"called from %p", _ReturnAddress());
    dump(L"%hs", TraceLevelStr(trace_level));
    dump(L"%hs", SourceStr(source));
    dump(L"%hs", CausalitySynchronousWorkStr(work));
    HRESULT hr =
      orig->TraceSynchronousWorkCompletion(trace_level, source, work);
    dump("%08X", hr);
    return hr;
  }

  virtual HRESULT STDMETHODCALLTYPE add_TracingStatusChanged(
    abi::IEventHandler<abi::TracingStatusChangedEventArgs*>* handler,
    EventRegistrationToken* cookie) override
  {
    dbg(L"called from %p", _ReturnAddress());
    HRESULT hr = orig->add_TracingStatusChanged(handler, cookie);
    dump("%08X", hr);
    return hr;
  }

  virtual HRESULT STDMETHODCALLTYPE remove_TracingStatusChanged(
    EventRegistrationToken cookie) override
  {
    dbg(L"called from %p", _ReturnAddress());
    HRESULT hr = orig->remove_TracingStatusChanged(cookie);
    dump("%08X", hr);
    return hr;
  }
};

// ------------------------------------------------------------------------

HRESULT (STDMETHODCALLTYPE* orig_RoGetActivationFactory)(
  HSTRING id, REFIID iid, void** pfactory);

STDAPI _RoGetActivationFactory(HSTRING id, REFIID iid, void** pfactory) {
  HRESULT hr;
  void* res;
  dbg(L"called from %p", _ReturnAddress());
  dumpstr(id);
  dumpiid(iid);
  hr = orig_RoGetActivationFactory(id, iid, &res);
  dump("%08X", hr);
  if (FAILED(hr)) {
    *pfactory = res;
  } else if (iid == __uuidof(IActivationFactory)) {
    *pfactory = new _IActivationFactory((IActivationFactory*)res);
  } else if (iid == __uuidof(abi::ICoreApplication)) {
    *pfactory = new _ICoreApplication((abi::ICoreApplication*)res);
  } else if (iid == __uuidof(abi::ICoreWindowStatic)) {
    *pfactory = new _ICoreWindowStatic((abi::ICoreWindowStatic*)res);
  } else if (iid == __uuidof(abi::IDeferralFactory)) {
    *pfactory = new _IDeferralFactory((abi::IDeferralFactory*)res);
  } else if (iid == __uuidof(abi::IAsyncCausalityTracerStatics)) {
    *pfactory = new _IAsyncCausalityTracerStatics(
      (abi::IAsyncCausalityTracerStatics*)res);
  } else {
    *pfactory = res;
  }
  return hr;
}

// ------------------------------------------------------------------------

HRESULT (STDMETHODCALLTYPE* orig_RoActivateInstance)
  (HSTRING id, IInspectable** instance);

STDAPI _RoActivateInstance(HSTRING id, IInspectable** instance) {
  IInspectable* res;
  HRESULT hr;
  PCWSTR s;
  dbg(L"called from %p", _ReturnAddress());
  dumpstr(id);
  hr = orig_RoActivateInstance(id, &res);
  dump("%08X", hr);
  s = _WindowsGetStringRawBuffer(id, 0);
  if (FAILED(hr)) {
    *instance = res;
  } else if (!wcscmp(s, L"Windows.Foundation.Collections.PropertySet")) {
    *instance = new _IPropertySet((abi::IPropertySet*)res);
  } else {
    *instance = res;
  }
  if (SUCCEEDED(hr)) {
    dumpptr("%p", instance);
  }
  return hr;
}

// ------------------------------------------------------------------------

HRESULT (STDMETHODCALLTYPE* orig_RoGetApartmentIdentifier)
  (UINT64* identifier);

STDAPI _RoGetApartmentIdentifier(UINT64* identifier) {
  HRESULT hr;
  dbg(L"called from %p", _ReturnAddress());
  hr = orig_RoGetApartmentIdentifier(identifier);
  dump("%08X", hr);
  if (SUCCEEDED(hr)) {
    dumpptr("%I64d", identifier);
  }
  return hr;
}

// ------------------------------------------------------------------------

HRESULT (STDMETHODCALLTYPE* orig_RoInitialize)(RO_INIT_TYPE t);

STDAPI _RoInitialize(RO_INIT_TYPE t) {
  HRESULT hr;
  dbg(L"called from %p", _ReturnAddress());
  #define c(x) case x: dbg(L"" #x)
  switch (t) {
    c(RO_INIT_SINGLETHREADED); break;
    c(RO_INIT_MULTITHREADED); break;
  }
  #undef c
  hr = orig_RoInitialize(t);
  dump("%08X", hr);
  return hr;
}

// ------------------------------------------------------------------------

HRESULT (STDMETHODCALLTYPE* orig_RoRegisterActivationFactories)(
  HSTRING* ids, PFNGETACTIVATIONFACTORY* callbacks, UINT32 count,
  RO_REGISTRATION_COOKIE* cookie);

STDAPI _RoRegisterActivationFactories(HSTRING* ids,
  PFNGETACTIVATIONFACTORY* callbacks, UINT32 count,
  RO_REGISTRATION_COOKIE* cookie)
{
  HRESULT hr;
  UINT32 i;
  dbg(L"called from %p", _ReturnAddress());
  for (i = 0; i < count; ++i) {
    dumpstr(ids[i]);
    dump("%p", callbacks[i]);
  }
  dump("%p", cookie);
  hr = orig_RoRegisterActivationFactories(ids, callbacks, count, cookie);
  dump("%08X", hr);
  return hr;
}

// ------------------------------------------------------------------------

HRESULT (STDMETHODCALLTYPE* orig_RoRegisterForApartmentShutdown)(
  IApartmentShutdown* obj, UINT64* identifier,
  APARTMENT_SHUTDOWN_REGISTRATION_COOKIE* cookie);

STDAPI _RoRegisterForApartmentShutdown(IApartmentShutdown* obj,
  UINT64* identifier, APARTMENT_SHUTDOWN_REGISTRATION_COOKIE* cookie)
{
  HRESULT hr;
  dbg(L"called from %p", _ReturnAddress());
  dump("%p", obj);
  hr = orig_RoRegisterForApartmentShutdown(obj, identifier, cookie);
  dump("%08X", hr);
  if (SUCCEEDED(hr)) {
    dumpptr("%I64d", identifier);
    dump("%p", cookie);
  }
  return hr;
}

// ------------------------------------------------------------------------

void (STDMETHODCALLTYPE* orig_RoRevokeActivationFactories)
  (RO_REGISTRATION_COOKIE cookie);

STDAPI_(void) _RoRevokeActivationFactories(RO_REGISTRATION_COOKIE cookie) {
  dbg(L"called from %p", _ReturnAddress());
  dump("%p", cookie);
  orig_RoRevokeActivationFactories(cookie);
}

// ------------------------------------------------------------------------

void (STDMETHODCALLTYPE* orig_RoUninitialize)();

STDAPI_(void) _RoUninitialize() {
  dbg(L"called from %p", _ReturnAddress());
  orig_RoUninitialize();
}

// ------------------------------------------------------------------------

HRESULT (STDMETHODCALLTYPE* orig_RoUnregisterForApartmentShutdown)
  (APARTMENT_SHUTDOWN_REGISTRATION_COOKIE cookie);

STDAPI _RoUnregisterForApartmentShutdown(
  APARTMENT_SHUTDOWN_REGISTRATION_COOKIE cookie)
{
  HRESULT hr;
  dbg(L"called from %p", _ReturnAddress());
  dump("%p", cookie);
  hr = orig_RoUnregisterForApartmentShutdown(cookie);
  dump("%08X", hr);
  return hr;
}

// ------------------------------------------------------------------------

#define h(x, nops) { #x, _##x, (void**)&orig_##x, nops }
#define t(x) { #x, 0, (void**)&_##x, 0 }
spy_export_t hooks[] = {
  t(WindowsGetStringRawBuffer),
  h(RoActivateInstance, 0),
  h(RoGetActivationFactory, 1),
  h(RoGetApartmentIdentifier, 0),
  h(RoInitialize, 1),
  h(RoRegisterActivationFactories, 0),
  h(RoRegisterForApartmentShutdown, 0),
  h(RoRevokeActivationFactories, 1),
  h(RoUninitialize, 0),
  h(RoUnregisterForApartmentShutdown, 1),
};
#undef h

void spy_init() {
  log_init();
  spy_exports("api-ms-win-core-winrt-l1-1-0.dll", _countof(hooks), hooks);
}

/*
 * NOTE: it's not a good idea to do complex init in DllMain but I don't
 * wanna have spinlocks in every hook to wait in case the init thread
 * hasn't finished
 */

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved) {
  (void)reserved;
  (void)hinst;
  if (reason == DLL_PROCESS_ATTACH) {
    spy_init();
  }
  return TRUE;
}
