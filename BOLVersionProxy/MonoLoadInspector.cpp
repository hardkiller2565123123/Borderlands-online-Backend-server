#include "MonoLoadInspector.h"
#include "Logger.h"

#include <windows.h>
#include <string>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace
{
    struct MonoDomain;
    struct MonoThread;
    struct MonoAssembly;
    struct MonoImage;
    struct MonoClass;
    struct MonoMethod;
    struct MonoObject;
    struct MonoString;

    using mono_get_root_domain_t = MonoDomain* (__cdecl*)();
    using mono_thread_attach_t = MonoThread* (__cdecl*)(MonoDomain*);
    using mono_domain_assembly_open_t = MonoAssembly* (__cdecl*)(MonoDomain*, const char*);
    using mono_assembly_get_image_t = MonoImage* (__cdecl*)(MonoAssembly*);
    using mono_class_from_name_t = MonoClass* (__cdecl*)(MonoImage*, const char*, const char*);
    using mono_class_get_parent_t = MonoClass* (__cdecl*)(MonoClass*);
    using mono_class_get_method_from_name_t = MonoMethod* (__cdecl*)(MonoClass*, const char*, int);
    using mono_runtime_invoke_t = MonoObject* (__cdecl*)(MonoMethod*, void*, void**, MonoObject**);
    using mono_object_unbox_t = void* (__cdecl*)(MonoObject*);
    using mono_string_to_utf8_t = char* (__cdecl*)(MonoString*);
    using mono_free_t = void (__cdecl*)(void*);

    struct MonoApi
    {
        mono_get_root_domain_t getRootDomain = nullptr;
        mono_thread_attach_t threadAttach = nullptr;
        mono_domain_assembly_open_t domainAssemblyOpen = nullptr;
        mono_assembly_get_image_t assemblyGetImage = nullptr;
        mono_class_from_name_t classFromName = nullptr;
        mono_class_get_parent_t classGetParent = nullptr;
        mono_class_get_method_from_name_t classGetMethodFromName = nullptr;
        mono_runtime_invoke_t runtimeInvoke = nullptr;

        // Optional on the old Unity/Mono build used by BOL. v63 has ABI-safe
        // fallbacks so absence of these helpers no longer disables the monitor.
        mono_object_unbox_t objectUnbox = nullptr;
        mono_string_to_utf8_t stringToUtf8 = nullptr;
        mono_free_t freeFn = nullptr;
    };

    struct StepSpec
    {
        int index;
        const char* token;
        const char* name;
        const char* completionGate;
    };

    // Exact order reconstructed from LevelLoadingStepController.Start().
    static const StepSpec kSteps[] =
    {
        {  1, "LevelLoadingStep_Init",                     "Init",                     "waiting for Application.LoadLevelAsync(\"ResourcesLoading\").isDone" },
        {  2, "LevelLoadingStep_DownloadAssetBundle",      "DownloadAssetBundle",      "waiting for AssetBundleManager download callback (m_finish=true); progress is weighted bundle download progress" },
        {  3, "LevelLoadingStep_LoadPlayerAudio",           "LoadPlayerAudio",           "waiting for player-audio preload callback (m_finish=true)" },
        {  4, "LevelLoadingStep_PreLoadPlayerBuildingKit",  "PreLoadPlayerBuildingKit",  "waiting for local-player building-kit preload callback (m_finished=true)" },
        {  5, "LevelLoadingStep_LoadMapSetting",            "LoadMapSetting",            "completion predicate is immediate=true; a stall here implies EnterStep exception/block" },
        {  6, "LevelLoadingStep_LoadResourceDependencyBundle","LoadResourceDependencyBundle","completion predicate is immediate=true; a stall here implies synchronous dependency-load exception/block" },
        {  7, "LevelLoadingStep_PreLoadNpcResources",       "PreLoadNpcResources",       "waiting for NPC resource preload callback (m_finish=true)" },
        {  8, "LevelLoadingStep_PreLoadWeapon",             "PreLoadWeapon",             "waiting for weapon preload callback (m_finish=true)" },
        {  9, "LevelLoadingStep_SyncLocalPlayer",           "SyncLocalPlayer",           "waiting for Session != null AND Session.PlayerInfoSync != null" },
        { 10, "LevelLoadingStep_UI",                        "UI",                        "waiting for every loaded uniswf_* InGameAssetBundle WWW request to report isDone" },
        { 11, "LevelLoadingStep_LoadLevelBundle",           "LoadLevelBundle",           "waiting for map AssetBundleRequest.isDone (map asset extracted from lvl_*_scn.assetbundle)" },
        { 12, "LevelLoadingStep_LoadLevel",                 "LoadLevel",                 "waiting for Application.LoadLevelAsync(MapsDesc @map).isDone" },
        { 13, "LevelLoadingStep_LoadLevelDataBundle",       "LoadLevelDataBundle",       "waiting for level-data AssetBundleRequest list to finish/drain" },
        { 14, "LevelLoadingStep_GenerateWeaponsData",       "GenerateWeaponsData",       "completion predicate is immediate=true" },
        { 15, "LevelLoadingStep_EndLoading",                "EndLoading",                "runs final unload/UI callback then completes; a stall implies cleanup/callback exception/block" },
    };

    template <typename T>
    bool LoadProc(HMODULE module, const char* name, T& out)
    {
        out = reinterpret_cast<T>(GetProcAddress(module, name));
        return out != nullptr;
    }

    bool ResolveMonoApi(HMODULE mono, MonoApi& api)
    {
        LoadProc(mono, "mono_get_root_domain", api.getRootDomain);
        LoadProc(mono, "mono_thread_attach", api.threadAttach);
        LoadProc(mono, "mono_domain_assembly_open", api.domainAssemblyOpen);
        LoadProc(mono, "mono_assembly_get_image", api.assemblyGetImage);
        LoadProc(mono, "mono_class_from_name", api.classFromName);
        LoadProc(mono, "mono_class_get_parent", api.classGetParent);
        LoadProc(mono, "mono_class_get_method_from_name", api.classGetMethodFromName);
        LoadProc(mono, "mono_runtime_invoke", api.runtimeInvoke);

        LoadProc(mono, "mono_object_unbox", api.objectUnbox);
        LoadProc(mono, "mono_string_to_utf8", api.stringToUtf8);
        LoadProc(mono, "mono_free", api.freeFn);

        return api.getRootDomain && api.threadAttach && api.domainAssemblyOpen &&
               api.assemblyGetImage && api.classFromName && api.classGetParent &&
               api.classGetMethodFromName && api.runtimeInvoke;
    }

    void LogExportState(const MonoApi& api)
    {
        bol_log::Write(
            "[MONO/LOADSTEP] export audit root=%s attach=%s open=%s image=%s class=%s parent=%s method=%s invoke=%s unbox=%s str8=%s free=%s",
            api.getRootDomain ? "OK" : "MISS",
            api.threadAttach ? "OK" : "MISS",
            api.domainAssemblyOpen ? "OK" : "MISS",
            api.assemblyGetImage ? "OK" : "MISS",
            api.classFromName ? "OK" : "MISS",
            api.classGetParent ? "OK" : "MISS",
            api.classGetMethodFromName ? "OK" : "MISS",
            api.runtimeInvoke ? "OK" : "MISS",
            api.objectUnbox ? "OK" : "fallback",
            api.stringToUtf8 ? "OK" : "fallback",
            api.freeFn ? "OK" : "leak-safe");
    }

    std::wstring GetGameDirectory()
    {
        wchar_t path[MAX_PATH]{};
        const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
        if (!length || length >= MAX_PATH)
            return L".";
        std::wstring result(path, length);
        const auto slash = result.find_last_of(L"\\/");
        if (slash != std::wstring::npos)
            result.resize(slash);
        return result;
    }

    std::string WideToUtf8(const std::wstring& value)
    {
        if (value.empty())
            return {};
        const int chars = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
        if (chars <= 0)
            return {};
        std::string result(static_cast<size_t>(chars), '\0');
        WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), &result[0], chars, nullptr, nullptr);
        return result;
    }

    // Classic Unity MonoObject layout on Win32 is two pointers followed by boxed data.
    void* UnboxCompat(const MonoApi& api, MonoObject* object)
    {
        if (!object)
            return nullptr;
        if (api.objectUnbox)
            return api.objectUnbox(object);
        return reinterpret_cast<unsigned char*>(object) + sizeof(void*) * 2;
    }

    std::string MonoStringToUtf8Compat(const MonoApi& api, MonoString* value)
    {
        if (!value)
            return {};

        if (api.stringToUtf8)
        {
            char* utf8 = api.stringToUtf8(value);
            if (!utf8)
                return {};
            std::string result(utf8);
            if (api.freeFn)
                api.freeFn(utf8);
            return result;
        }

        // Classic Unity MonoString: MonoObject, int32 length, UTF-16 chars[].
        const unsigned char* base = reinterpret_cast<const unsigned char*>(value);
        const int32_t length = *reinterpret_cast<const int32_t*>(base + sizeof(void*) * 2);
        if (length < 0 || length > 1024 * 1024)
            return {};
        if (length == 0)
            return {};
        const wchar_t* chars = reinterpret_cast<const wchar_t*>(base + sizeof(void*) * 2 + sizeof(int32_t));
        const int bytes = WideCharToMultiByte(CP_UTF8, 0, chars, length, nullptr, 0, nullptr, nullptr);
        if (bytes <= 0)
            return {};
        std::string result(static_cast<size_t>(bytes), '\0');
        WideCharToMultiByte(CP_UTF8, 0, chars, length, &result[0], bytes, nullptr, nullptr);
        return result;
    }

    bool InvokeObject(const MonoApi& api, MonoMethod* method, MonoObject* instance, MonoObject*& value)
    {
        value = nullptr;
        if (!method)
            return false;
        MonoObject* exc = nullptr;
        MonoObject* result = api.runtimeInvoke(method, instance, nullptr, &exc);
        if (exc || !result)
            return false;
        value = result;
        return true;
    }

    bool InvokeBool(const MonoApi& api, MonoMethod* method, MonoObject* instance, bool& value)
    {
        MonoObject* result = nullptr;
        if (!InvokeObject(api, method, instance, result))
            return false;
        void* raw = UnboxCompat(api, result);
        if (!raw)
            return false;
        value = *reinterpret_cast<unsigned char*>(raw) != 0;
        return true;
    }

    bool InvokeFloat(const MonoApi& api, MonoMethod* method, MonoObject* instance, float& value)
    {
        MonoObject* result = nullptr;
        if (!InvokeObject(api, method, instance, result))
            return false;
        void* raw = UnboxCompat(api, result);
        if (!raw)
            return false;
        value = *reinterpret_cast<float*>(raw);
        return true;
    }

    bool InvokeString(const MonoApi& api, MonoMethod* method, MonoObject* instance, std::string& value)
    {
        MonoObject* result = nullptr;
        if (!InvokeObject(api, method, instance, result))
            return false;
        value = MonoStringToUtf8Compat(api, reinterpret_cast<MonoString*>(result));
        return true;
    }

    const StepSpec* IdentifyStep(const std::string& raw)
    {
        for (const StepSpec& spec : kSteps)
        {
            if (raw.find(spec.token) != std::string::npos || raw.find(spec.name) != std::string::npos)
                return &spec;
        }
        return nullptr;
    }

    void LogStepTable()
    {
        bol_log::Write("[MONO/LOADSTEP] v63 exact client loading chain (read-only):");
        for (const StepSpec& spec : kSteps)
            bol_log::Write("[MONO/LOADSTEP]   %02d/15 %-30s gate=%s", spec.index, spec.name, spec.completionGate);
    }

    DWORD WINAPI MonitorThread(void*)
    {
        HMODULE mono = nullptr;
        for (int i = 0; i < 240 && !mono; ++i)
        {
            mono = GetModuleHandleW(L"mono.dll");
            if (!mono)
                Sleep(250);
        }
        if (!mono)
        {
            bol_log::Write("[MONO/LOADSTEP] mono.dll did not appear; managed loading-step monitor disabled");
            return 0;
        }

        MonoApi api{};
        bool resolved = false;
        for (int attempt = 0; attempt < 40 && !resolved; ++attempt)
        {
            api = MonoApi{};
            resolved = ResolveMonoApi(mono, api);
            if (!resolved)
                Sleep(250);
        }
        LogExportState(api);
        if (!resolved)
        {
            bol_log::Write("[MONO/LOADSTEP] core Mono embedding exports are unavailable; v63 cannot invoke managed code on this client");
            bol_log::Write("[MONO/LOADSTEP] keep native LOADTRACE enabled; export audit above identifies the exact missing Mono entrypoint(s)");
            return 0;
        }

        MonoDomain* domain = nullptr;
        for (int i = 0; i < 120 && !domain; ++i)
        {
            domain = api.getRootDomain();
            if (!domain)
                Sleep(250);
        }
        if (!domain)
        {
            bol_log::Write("[MONO/LOADSTEP] mono_get_root_domain stayed null");
            return 0;
        }
        api.threadAttach(domain);

        const std::wstring assemblyPathW = GetGameDirectory() + L"\\BOL_Data\\Managed\\Assembly-CSharp.dll";
        const std::string assemblyPath = WideToUtf8(assemblyPathW);
        MonoAssembly* assembly = api.domainAssemblyOpen(domain, assemblyPath.c_str());
        if (!assembly)
        {
            bol_log::Write("[MONO/LOADSTEP] could not open Assembly-CSharp.dll through Mono path=%s", assemblyPath.c_str());
            return 0;
        }

        MonoImage* image = api.assemblyGetImage(assembly);
        MonoClass* klass = image ? api.classFromName(image, "", "LevelLoadingManager") : nullptr;
        if (!klass)
        {
            bol_log::Write("[MONO/LOADSTEP] LevelLoadingManager class not found");
            return 0;
        }

        MonoClass* parent = api.classGetParent(klass);
        if (!parent)
        {
            bol_log::Write("[MONO/LOADSTEP] LevelLoadingManager generic singleton parent not found");
            return 0;
        }

        // Use the inherited singleton getter instead of static-field/vtable APIs.
        // Those APIs are not guaranteed exports in this old Unity Mono build.
        MonoMethod* getSingleton = api.classGetMethodFromName(parent, "get_c5ee19dc8d4cccf5ae2de225410458b86", 0);
        MonoMethod* getProgress = api.classGetMethodFromName(klass, "c0a687459ceef442f37c4d398cb12f99f", 0);
        MonoMethod* getDescription = api.classGetMethodFromName(klass, "c8b70afb12d12e93983260684a70f87c7", 0);
        MonoMethod* getActive = api.classGetMethodFromName(klass, "c739bc996d272aeac228d6b5198b164d2", 0);

        if (!getSingleton || !getProgress || !getDescription || !getActive)
        {
            bol_log::Write("[MONO/LOADSTEP] managed contract resolution failed singleton=%s progress=%s desc=%s active=%s",
                           getSingleton ? "OK" : "MISS",
                           getProgress ? "OK" : "MISS",
                           getDescription ? "OK" : "MISS",
                           getActive ? "OK" : "MISS");
            return 0;
        }

        bol_log::Write("[MONO/LOADSTEP] v63 managed contract resolved; waiting for LevelLoadingManager singleton");
        LogStepTable();

        MonoObject* instance = nullptr;
        for (int i = 0; i < 240 && !instance; ++i)
        {
            MonoObject* exc = nullptr;
            instance = api.runtimeInvoke(getSingleton, nullptr, nullptr, &exc);
            if (exc)
            {
                bol_log::Write("[MONO/LOADSTEP] singleton getter raised a managed exception; monitor disabled");
                return 0;
            }
            if (!instance)
                Sleep(250);
        }
        if (!instance)
        {
            bol_log::Write("[MONO/LOADSTEP] LevelLoadingManager singleton getter stayed null");
            return 0;
        }

        bol_log::Write("[MONO/LOADSTEP] singleton=%p; exact 15-step/stall polling active", instance);

        std::string lastRawStep;
        const StepSpec* lastSpec = nullptr;
        int lastPercent = -1;
        bool lastActive = false;
        bool sawActive = false;
        ULONGLONG stepEnteredAt = 0;
        ULONGLONG lastProgressAt = 0;
        ULONGLONG lastHeartbeatAt = 0;
        ULONGLONG lastStallReportAt = 0;
        unsigned int invokeFailures = 0;

        for (;;)
        {
            bool active = false;
            float progress = 0.0f;
            std::string rawStep;
            const bool okActive = InvokeBool(api, getActive, instance, active);
            const bool okProgress = InvokeFloat(api, getProgress, instance, progress);
            const bool okStep = InvokeString(api, getDescription, instance, rawStep);

            if (!okActive || !okProgress || !okStep)
            {
                ++invokeFailures;
                if (invokeFailures == 1 || (invokeFailures % 10) == 0)
                {
                    bol_log::Write("[MONO/LOADSTEP] invoke failure #%u active=%d progress=%d step=%d; retrying",
                                   invokeFailures, okActive ? 1 : 0, okProgress ? 1 : 0, okStep ? 1 : 0);
                }
                Sleep(500);
                continue;
            }
            invokeFailures = 0;

            if (!std::isfinite(progress))
                progress = 0.0f;
            int percent = static_cast<int>(progress * 100.0f + 0.5f);
            if (percent < 0) percent = 0;
            if (percent > 100) percent = 100;

            const ULONGLONG now = GetTickCount64();
            const StepSpec* spec = IdentifyStep(rawStep);
            const bool stepChanged = rawStep != lastRawStep || spec != lastSpec;
            const bool progressChanged = percent != lastPercent;
            const bool activeChanged = active != lastActive;

            if (active)
            {
                sawActive = true;
                if (stepChanged || !lastActive)
                {
                    stepEnteredAt = now;
                    lastProgressAt = now;
                    lastStallReportAt = 0;
                    if (spec)
                    {
                        bol_log::Write("[MONO/LOADSTEP] STEP %02d/15 ENTER name=%s totalProgress=%d%% raw=%s",
                                       spec->index, spec->name, percent, rawStep.c_str());
                        bol_log::Write("[MONO/LOADSTEP] STEP %02d/15 completion gate: %s", spec->index, spec->completionGate);
                    }
                    else
                    {
                        bol_log::Write("[MONO/LOADSTEP] STEP ?/15 ENTER totalProgress=%d%% raw=%s",
                                       percent, rawStep.empty() ? "<empty>" : rawStep.c_str());
                    }
                }
                else if (progressChanged)
                {
                    lastProgressAt = now;
                    if (spec)
                        bol_log::Write("[MONO/LOADSTEP] STEP %02d/15 progress=%d%%", spec->index, percent);
                    else
                        bol_log::Write("[MONO/LOADSTEP] progress=%d%% raw=%s", percent, rawStep.c_str());
                }

                const ULONGLONG unchangedMs = now - lastProgressAt;
                const ULONGLONG inStepMs = now - stepEnteredAt;
                if (unchangedMs >= 3000 && (lastStallReportAt == 0 || now - lastStallReportAt >= 5000))
                {
                    if (spec)
                    {
                        bol_log::Write("[MONO/LOADSTEP][STALL] STEP %02d/15 %s unchanged=%llums inStep=%llums totalProgress=%d%%",
                                       spec->index, spec->name,
                                       static_cast<unsigned long long>(unchangedMs),
                                       static_cast<unsigned long long>(inStepMs), percent);
                        bol_log::Write("[MONO/LOADSTEP][STALL] FIRST UNSATISFIED GATE candidate: %s", spec->completionGate);
                    }
                    else
                    {
                        bol_log::Write("[MONO/LOADSTEP][STALL] unknown step raw=%s unchanged=%llums totalProgress=%d%%",
                                       rawStep.c_str(), static_cast<unsigned long long>(unchangedMs), percent);
                    }
                    lastStallReportAt = now;
                }

                if (lastHeartbeatAt == 0 || now - lastHeartbeatAt >= 2000)
                {
                    if (spec)
                        bol_log::Write("[MONO/LOADSTEP] heartbeat step=%02d/15 %s progress=%d%% active=YES", spec->index, spec->name, percent);
                    else
                        bol_log::Write("[MONO/LOADSTEP] heartbeat step=? progress=%d%% active=YES raw=%s", percent, rawStep.c_str());
                    lastHeartbeatAt = now;
                }
            }
            else if (activeChanged || stepChanged || progressChanged)
            {
                if (sawActive)
                {
                    const bool endedAtFinal = lastSpec && lastSpec->index == 15;
                    bol_log::Write("[MONO/LOADSTEP] loading became inactive after %s; pipelineCompleteCandidate=%s",
                                   lastSpec ? lastSpec->name : (lastRawStep.empty() ? "<unknown>" : lastRawStep.c_str()),
                                   endedAtFinal ? "YES" : "NO");
                }
                else
                {
                    bol_log::Write("[MONO/LOADSTEP] monitor ready; LevelLoadingManager currently Not Loading");
                }
                lastHeartbeatAt = 0;
            }

            lastActive = active;
            lastRawStep = rawStep;
            lastSpec = spec;
            lastPercent = percent;
            Sleep(active ? 100 : 500);
        }
    }
}

namespace mono_load_inspector
{
    void Start()
    {
        HANDLE thread = CreateThread(nullptr, 0, MonitorThread, nullptr, 0, nullptr);
        if (thread)
        {
            CloseHandle(thread);
            bol_log::Write("[MONO/LOADSTEP] v63 managed exact-step monitor thread started (old-Mono compatible)");
        }
        else
        {
            bol_log::Write("[MONO/LOADSTEP] failed to start managed loading-step monitor thread error=%lu", GetLastError());
        }
    }
}
