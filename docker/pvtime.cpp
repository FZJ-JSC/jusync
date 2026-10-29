// pvtime.cpp - libpvtime.so: hand ParaView's animation time to the ANARI
// renderer.
//
// Why this exists
// ---------------
// The ANARI mapper node takes the time it stamps on each ANARI geometry from
// `info->Get(vtkDataObject::DATA_TIME_STEP())`, where `info` is the *actor's
// property keys*. Nothing in VTK or in ParaView ever writes that key, so the
// node always ends up with NaN and the device (libanari_library_usd) has to
// guess the dataset time from the payload. That guess is what made Unreal's t
// differ from the value in ParaView's Time field.
//
// This small preload library closes the gap:
//
//   * `vtkPVView::SetViewTime(double)` is called by ParaView's animation scene
//     with exactly the time shown in the Time field. We remember it.
//   * `vtkInformation::Has/Get(vtkInformationDoubleKey*)` are the calls the
//     ANARI node makes for DATA_TIME_STEP. For that one key - and only when the
//     information object does not already carry it - we answer with the time
//     ParaView just set.
//
// The hooks must stay invisible to the rest of ParaView. Claiming that *every*
// information object carries DATA_TIME_STEP made the pipeline executive believe
// data was already computed for the requested time, so scrubbing the time
// stopped re-executing and the view froze. We therefore only answer calls whose
// caller lives inside the ANARI rendering plugin (checked with the return
// address + the loaded module ranges); everything else is passed straight
// through.
//
// Nothing is guessed and nothing is written into VTK's data structures: the
// device then receives the pipeline's own time as `usd::time` on the geometry.
//
// Build: g++ -O2 -shared -fPIC -o libpvtime.so pvtime.cpp -ldl
// Use:   LD_PRELOAD=/opt/pv61/lib/libpvtime.so paraview ...
// Debug: JUSYNC_PVTIME_DEBUG=1 prints the times it sees and serves.
//        JUSYNC_PVTIME_SCOPE=any disables the caller check (debug only).

#define _GNU_SOURCE
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <limits>
#include <link.h>

namespace
{
typedef void (*SetViewTimeFn)(void*, double);
typedef int (*HasDoubleFn)(void*, void*);
typedef double (*GetDoubleFn)(void*, void*);
typedef void (*SetDoubleFn)(void*, void*, double);
typedef void* (*KeyFn)();

SetViewTimeFn realSetViewTime = nullptr;
HasDoubleFn realHas = nullptr;
GetDoubleFn realGet = nullptr;
SetDoubleFn realSet = nullptr;
KeyFn realDataTimeStep = nullptr;
void* dataTimeStepKey = nullptr;
bool inited = false;
bool debug = false;
bool scopeAny = false;

// The time ParaView is currently showing. NaN until SetViewTime is called.
double viewTime = std::numeric_limits<double>::quiet_NaN();
double lastLogged = -1e300;
unsigned long long served = 0;
unsigned long long skipped = 0;

// Address range of the ANARI rendering plugin, resolved on first use. The
// plugin is always loaded by the time it can call into us, so this succeeds.
struct ModuleRange
{
  uintptr_t lo = 0;
  uintptr_t hi = 0;
  bool resolved = false;
  char name[256] = { 0 };
} anari;

const char* kSetViewTime = "_ZN9vtkPVView11SetViewTimeEd";
const char* kHas = "_ZN14vtkInformation3HasEP23vtkInformationDoubleKey";
const char* kGet = "_ZN14vtkInformation3GetEP23vtkInformationDoubleKey";
const char* kSet = "_ZN14vtkInformation3SetEP23vtkInformationDoubleKeyd";
const char* kDataTimeStep = "_ZN13vtkDataObject14DATA_TIME_STEPEv";

int FindAnariModule(struct dl_phdr_info* info, size_t, void*)
{
  if (!info->dlpi_name || !*info->dlpi_name)
    return 0;
  if (!strstr(info->dlpi_name, "RenderingAnari"))
    return 0;

  uintptr_t lo = UINTPTR_MAX;
  uintptr_t hi = 0;
  for (int i = 0; i < info->dlpi_phnum; ++i)
  {
    const ElfW(Phdr)& ph = info->dlpi_phdr[i];
    if (ph.p_type != PT_LOAD)
      continue;
    const uintptr_t start = info->dlpi_addr + ph.p_vaddr;
    const uintptr_t end = start + ph.p_memsz;
    if (start < lo)
      lo = start;
    if (end > hi)
      hi = end;
  }
  anari.lo = lo;
  anari.hi = hi;
  anari.resolved = hi > lo;
  strncpy(anari.name, info->dlpi_name, sizeof(anari.name) - 1);
  return 1; // stop at the first match
}

// Is the immediate caller inside the ANARI plugin?
bool CallerIsAnari(void* caller)
{
  if (scopeAny)
    return true;
  if (!anari.resolved)
  {
    dl_iterate_phdr(FindAnariModule, nullptr);
    if (debug)
      fprintf(stderr, "[pvtime] anari module %s range [%p,%p)\n",
          anari.resolved ? anari.name : "(not found)", (void*)anari.lo, (void*)anari.hi);
  }
  if (!anari.resolved)
    return false;
  const uintptr_t pc = reinterpret_cast<uintptr_t>(caller);
  return pc >= anari.lo && pc < anari.hi;
}

void Init()
{
  if (inited)
    return;
  inited = true;

  realSetViewTime = (SetViewTimeFn)dlsym(RTLD_NEXT, kSetViewTime);
  realHas = (HasDoubleFn)dlsym(RTLD_NEXT, kHas);
  realGet = (GetDoubleFn)dlsym(RTLD_NEXT, kGet);
  realSet = (SetDoubleFn)dlsym(RTLD_NEXT, kSet);
  realDataTimeStep = (KeyFn)dlsym(RTLD_NEXT, kDataTimeStep);
  if (realDataTimeStep)
    dataTimeStepKey = realDataTimeStep();

  const char* env = getenv("JUSYNC_PVTIME_DEBUG");
  debug = env && *env && strcmp(env, "0") != 0;
  const char* scope = getenv("JUSYNC_PVTIME_SCOPE");
  scopeAny = scope && strcmp(scope, "any") == 0;

  if (debug)
    fprintf(stderr,
        "[pvtime] hooked SetViewTime=%p Has=%p Get=%p DATA_TIME_STEP=%p scope=%s\n",
        (void*)realSetViewTime, (void*)realHas, (void*)realGet, dataTimeStepKey,
        scopeAny ? "any" : "anari");
}
} // namespace

extern "C" void _ZN9vtkPVView11SetViewTimeEd(void* self, double t)
{
  Init();
  viewTime = t;
  if (debug && t != lastLogged)
  {
    lastLogged = t;
    fprintf(stderr, "[pvtime] view time %.6f\n", t);
  }
  if (realSetViewTime)
    realSetViewTime(self, t);
}

extern "C" int _ZN14vtkInformation3HasEP23vtkInformationDoubleKey(void* info, void* key)
{
  Init();
  const int found = realHas ? realHas(info, key) : 1;
  if (!found && key && key == dataTimeStepKey && !std::isnan(viewTime))
  {
    if (!CallerIsAnari(__builtin_return_address(0)))
    {
      if (debug && ++skipped <= 4)
        fprintf(stderr, "[pvtime] skip Has (caller %p not in ANARI)\n",
            __builtin_return_address(0));
      return found;
    }
    return 1;
  }
  return found;
}

extern "C" double _ZN14vtkInformation3GetEP23vtkInformationDoubleKey(void* info, void* key)
{
  Init();
  if (key && key == dataTimeStepKey && !std::isnan(viewTime))
  {
    const int found = realHas ? realHas(info, key) : 1;
    if (!found && CallerIsAnari(__builtin_return_address(0)))
    {
      if (debug && ++served <= 8)
        fprintf(stderr, "[pvtime] serve DATA_TIME_STEP=%.6f\n", viewTime);
      return viewTime;
    }
  }
  return realGet ? realGet(info, key) : 0.0;
}
