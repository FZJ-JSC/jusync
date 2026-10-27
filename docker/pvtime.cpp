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
// Nothing is guessed and nothing is written into VTK's data structures: the
// device then receives the pipeline's own time as `usd::time` on the geometry.
//
// Build: g++ -O2 -shared -fPIC -o libpvtime.so pvtime.cpp -ldl
// Use:   LD_PRELOAD=/opt/pv61/lib/libpvtime.so paraview ...
// Debug: JUSYNC_PVTIME_DEBUG=1 prints the times it sees and serves.

#define _GNU_SOURCE
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>

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

// The time ParaView is currently showing. NaN until SetViewTime is called.
double viewTime = std::numeric_limits<double>::quiet_NaN();
double lastLogged = -1e300;
unsigned long long served = 0;

const char* kSetViewTime = "_ZN9vtkPVView11SetViewTimeEd";
const char* kHas = "_ZN14vtkInformation3HasEP23vtkInformationDoubleKey";
const char* kGet = "_ZN14vtkInformation3GetEP23vtkInformationDoubleKey";
const char* kSet = "_ZN14vtkInformation3SetEP23vtkInformationDoubleKeyd";
const char* kDataTimeStep = "_ZN13vtkDataObject14DATA_TIME_STEPEv";

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

  if (debug)
    fprintf(stderr,
        "[pvtime] hooked SetViewTime=%p Has=%p Get=%p DATA_TIME_STEP=%p\n",
        (void*)realSetViewTime, (void*)realHas, (void*)realGet, dataTimeStepKey);
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
    return 1;
  return found;
}

extern "C" double _ZN14vtkInformation3GetEP23vtkInformationDoubleKey(void* info, void* key)
{
  Init();
  if (key && key == dataTimeStepKey && !std::isnan(viewTime))
  {
    const int found = realHas ? realHas(info, key) : 1;
    if (!found)
    {
      if (debug && ++served <= 8)
        fprintf(stderr, "[pvtime] serve DATA_TIME_STEP=%.6f\n", viewTime);
      return viewTime;
    }
  }
  return realGet ? realGet(info, key) : 0.0;
}
