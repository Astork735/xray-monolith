#define NOMINMAX
#define XR_NO_PROTOTYPES
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <windows.h>
#include <d3d11.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <cstdio>
#include <cstring>
#include <vector>
int wmain(int argc,wchar_t**argv)
{
    if(argc!=2){puts("usage: vr_runtime_probe <absolute path to openxr_loader.dll>");return 2;}
    HMODULE module=LoadLibraryExW(argv[1],nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if(!module){printf("OpenXR loader unavailable: Win32 %lu\n",GetLastError());return 0;}
    const auto proc=reinterpret_cast<PFN_xrGetInstanceProcAddr>(GetProcAddress(module,"xrGetInstanceProcAddr"));
    if(!proc){FreeLibrary(module);return 1;}
    PFN_xrCreateInstance create=nullptr;PFN_xrEnumerateInstanceExtensionProperties enumerate=nullptr;
    proc(XR_NULL_HANDLE,"xrCreateInstance",reinterpret_cast<PFN_xrVoidFunction*>(&create));
    proc(XR_NULL_HANDLE,"xrEnumerateInstanceExtensionProperties",reinterpret_cast<PFN_xrVoidFunction*>(&enumerate));
    uint32_t count=0;XrResult result=enumerate(nullptr,0,&count,nullptr);
    printf("Extension discovery: %d\n",result);
    if(XR_FAILED(result)){FreeLibrary(module);return 0;}
    std::vector<XrExtensionProperties>extensions(count,{XR_TYPE_EXTENSION_PROPERTIES});
    result=enumerate(nullptr,count,&count,extensions.data());
    bool d3d=false;for(const auto&e:extensions)if(!strcmp(e.extensionName,XR_KHR_D3D11_ENABLE_EXTENSION_NAME))d3d=true;
    if(XR_FAILED(result)||!d3d){puts("D3D11 binding unavailable");FreeLibrary(module);return 0;}
    const char*enabled[]={XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
    XrInstanceCreateInfo info{XR_TYPE_INSTANCE_CREATE_INFO};info.applicationInfo.apiVersion=XR_API_VERSION_1_0;
    strcpy_s(info.applicationInfo.applicationName,"X-Ray VR availability probe");
    info.enabledExtensionCount=1;info.enabledExtensionNames=enabled;
    XrInstance instance=XR_NULL_HANDLE;result=create(&info,&instance);printf("Instance creation: %d\n",result);
    if(XR_FAILED(result)){FreeLibrary(module);return 0;}
    PFN_xrDestroyInstance destroy=nullptr;PFN_xrGetSystem system=nullptr;PFN_xrGetInstanceProperties properties=nullptr;
    proc(instance,"xrDestroyInstance",reinterpret_cast<PFN_xrVoidFunction*>(&destroy));
    proc(instance,"xrGetSystem",reinterpret_cast<PFN_xrVoidFunction*>(&system));
    proc(instance,"xrGetInstanceProperties",reinterpret_cast<PFN_xrVoidFunction*>(&properties));
    XrInstanceProperties p{XR_TYPE_INSTANCE_PROPERTIES};properties(instance,&p);printf("Runtime: %s\n",p.runtimeName);
    XrSystemGetInfo request{XR_TYPE_SYSTEM_GET_INFO};request.formFactor=XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId id=XR_NULL_SYSTEM_ID;result=system(instance,&request,&id);printf("HMD availability: %d\n",result);
    if(XR_SUCCEEDED(result))
    {
        PFN_xrGetD3D11GraphicsRequirementsKHR graphics=nullptr;
        proc(instance,"xrGetD3D11GraphicsRequirementsKHR",reinterpret_cast<PFN_xrVoidFunction*>(&graphics));
        XrGraphicsRequirementsD3D11KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
        result=graphics(instance,id,&requirements);
        printf("D3D11 requirements: %d adapter=%08lx:%08lx feature=%x\n",result,
            requirements.adapterLuid.HighPart,requirements.adapterLuid.LowPart,requirements.minFeatureLevel);
    }
    destroy(instance);FreeLibrary(module);return 0;
}
