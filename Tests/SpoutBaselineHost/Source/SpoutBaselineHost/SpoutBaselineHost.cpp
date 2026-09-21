// Copyright (c) 2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Independent control host: no Spout API calls, module dependency, SDK or texture observer.
#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "Containers/Ticker.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/EngineVersion.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformMemory.h"
#include "HAL/PlatformTime.h"
#include "Serialization/JsonSerializer.h"
#include "Dom/JsonObject.h"
#include "EngineGlobals.h"
#include "RenderTimer.h"
#include "RHI.h"
#include "DynamicRHI.h"
#include "Runtime/Launch/Resources/Version.h"
#if ENGINE_MINOR_VERSION >= 6
#include "GPUProfiler.h"
#elif ENGINE_MINOR_VERSION == 5
#include "RHIGlobals.h"
#endif

namespace
{
    bool SaveJson(const TSharedRef<FJsonObject>& Object, const FString& Path)
    {
        FString Text;
        auto Writer = TJsonWriterFactory<>::Create(&Text);
        return FJsonSerializer::Serialize(Object, Writer) && FFileHelper::SaveStringToFile(Text, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
    }

    double Mean(const TArray<double>& Values)
    {
        double Sum = 0;
        for (double Value : Values) Sum += Value;
        return Values.Num() ? Sum / Values.Num() : 0;
    }

    double GpuMilliseconds()
    {
#if ENGINE_MINOR_VERSION >= 6
        static FRHIGPUFrameTimeHistory::FState State;
        uint64 Cycles = 0;
        double Latest = -1;
        while (State.PopFrameCycles(Cycles) != FRHIGPUFrameTimeHistory::EResult::Empty)
            Latest = FPlatformTime::ToMilliseconds64(Cycles);
        return Latest;
#else
        return GGPUFrameTime > 0 ? FPlatformTime::ToMilliseconds(GGPUFrameTime) : -1;
#endif
    }
}

class FSpoutBaselineModule : public FDefaultGameModuleImpl
{
    FTSTicker::FDelegateHandle Ticker;
    FString OutputDirectory;
    FString RunId;
    int32 WarmupSeconds = 5;
    int32 MeasureSeconds = 60;
    double Start = 0;
    double MeasurementStart = 0;
    double PreviousWork = 0;
    double NextWork = 0;
    bool Measuring = false;
    bool Finished = false;
    TArray<double> Frames, Game, Render, Gpu, Cpu;
    uint64 PeakMemory = 0;

    bool Attest()
    {
        FString Path, ExpectedSource, ExpectedHost;
        FParse::Value(FCommandLine::Get(), TEXT("SpoutRunId="), RunId);
        FParse::Value(FCommandLine::Get(), TEXT("SpoutAttestationPath="), Path);
        FParse::Value(FCommandLine::Get(), TEXT("SpoutSourceId="), ExpectedSource);
        FParse::Value(FCommandLine::Get(), TEXT("SpoutHostSourceId="), ExpectedHost);
        const FString HostId = UTF8_TO_TCHAR(SPOUT_CONTROL_SOURCE_ID);
        const bool Match = ExpectedSource == TEXT("no-plugin") && ExpectedHost == HostId && !RunId.IsEmpty()
            && !FModuleManager::Get().IsModuleLoaded(TEXT("SpoutPlugin"));
        auto Json = MakeShared<FJsonObject>();
        Json->SetStringField(TEXT("runId"), RunId);
        Json->SetStringField(TEXT("sourceId"), TEXT("no-plugin"));
        Json->SetStringField(TEXT("hostSourceId"), HostId);
        Json->SetStringField(TEXT("engineBuild"), FEngineVersion::Current().ToString());
        Json->SetNumberField(TEXT("processId"), FPlatformProcess::GetCurrentProcessId());
        Json->SetStringField(TEXT("executable"), FPlatformProcess::ExecutablePath());
        Json->SetBoolField(TEXT("matched"), Match);
        Json->SetBoolField(TEXT("spoutModuleLoaded"), FModuleManager::Get().IsModuleLoaded(TEXT("SpoutPlugin")));
        return !Path.IsEmpty() && !IFileManager::Get().FileExists(*Path) && SaveJson(Json, Path) && Match;
    }

    void Finish(bool Passed)
    {
        Finished = true;
        TArray<double> Sorted = Frames;
        Sorted.Sort();
        auto Percentile = [&Sorted](double Fraction)
        {
            return Sorted.Num() ? Sorted[FMath::Clamp(FMath::RoundToInt(Fraction * (Sorted.Num()-1)),0,Sorted.Num()-1)] : 0;
        };
        auto Json = MakeShared<FJsonObject>();
        Json->SetStringField(TEXT("schema"), TEXT("spouthosttest-performance/1"));
        Json->SetStringField(TEXT("scenario"), TEXT("noplugin"));
        Json->SetStringField(TEXT("runId"), RunId);
        Json->SetBoolField(TEXT("passed"), Passed);
        Json->SetBoolField(TEXT("noPlugin"), !FModuleManager::Get().IsModuleLoaded(TEXT("SpoutPlugin")));
        Json->SetStringField(TEXT("failureReason"), Passed ? TEXT("") : TEXT("control host loaded Spout, lacked D3D12, or measured too few frames"));
        Json->SetStringField(TEXT("engineVersion"), FEngineVersion::Current().ToString());
        auto Configuration = MakeShared<FJsonObject>();
        Configuration->SetNumberField(TEXT("width"), 0);
        Configuration->SetNumberField(TEXT("height"), 0);
        Configuration->SetNumberField(TEXT("streamCount"), 0);
        Configuration->SetBoolField(TEXT("usesSender"), false);
        Configuration->SetBoolField(TEXT("usesReceiver"), false);
        Configuration->SetStringField(TEXT("receiverMode"), TEXT("no-plugin control"));
        Json->SetObjectField(TEXT("configuration"), Configuration);
        auto Delivery = MakeShared<FJsonObject>();
        Delivery->SetNumberField(TEXT("expected"), 0);
        Delivery->SetNumberField(TEXT("observed"), 0);
        Delivery->SetNumberField(TEXT("ratio"), 1);
        Delivery->SetNumberField(TEXT("corrupt"), 0);
        Delivery->SetNumberField(TEXT("observerSeconds"), 0);
        Json->SetObjectField(TEXT("delivery"), Delivery);
        auto Results = MakeShared<FJsonObject>();
        const double Seconds = FPlatformTime::Seconds() - MeasurementStart;
        Results->SetNumberField(TEXT("frameCount"), Frames.Num());
        Results->SetNumberField(TEXT("measuredSeconds"), Seconds);
        Results->SetNumberField(TEXT("averageFps"), Seconds > 0 ? Frames.Num()/Seconds : 0);
        Results->SetNumberField(TEXT("frameTimeMeanMs"), Mean(Frames));
        Results->SetNumberField(TEXT("frameTimeP95Ms"), Percentile(0.95));
        Results->SetNumberField(TEXT("frameTimeP99Ms"), Percentile(0.99));
        Results->SetNumberField(TEXT("frameTimePeakMs"), Sorted.Num() ? Sorted.Last() : 0);
        Results->SetNumberField(TEXT("gameThreadMeanMs"), Mean(Game));
        Results->SetNumberField(TEXT("renderThreadMeanMs"), Mean(Render));
        if (Gpu.Num()) Results->SetNumberField(TEXT("gpuMeanMs"), Mean(Gpu));
        else Results->SetField(TEXT("gpuMeanMs"), MakeShared<FJsonValueNull>());
        Results->SetNumberField(TEXT("gpuSampleCount"), Gpu.Num());
        Results->SetNumberField(TEXT("processCpuPercentMean"), Mean(Cpu));
        Results->SetNumberField(TEXT("peakWorkingSetBytes"), double(PeakMemory));
        Json->SetObjectField(TEXT("results"), Results);
        if (!SaveJson(Json, OutputDirectory / TEXT("perf-noplugin.json"))) Passed = false;
        FPlatformMisc::RequestExitWithStatus(!Passed, Passed ? 0 : 5);
    }

    bool Tick(float)
    {
        if (Finished) return true;
        const double Now = FPlatformTime::Seconds();
        if (Start == 0) Start = Now;
        if (Now < NextWork) return true;
        NextWork = FMath::Max(NextWork + 1.0/60.0, Now);
        const double Delta = PreviousWork > 0 ? (Now - PreviousWork)*1000 : 0;
        PreviousWork = Now;
        if (!Measuring)
        {
            if (Now - Start >= WarmupSeconds) { Measuring = true; MeasurementStart = Now; }
            return true;
        }
        Frames.Add(Delta);
        Game.Add(FPlatformTime::ToMilliseconds(GGameThreadTime));
        Render.Add(FPlatformTime::ToMilliseconds(GRenderThreadTime));
        const double GpuTime = GpuMilliseconds();
        if (GpuTime >= 0) Gpu.Add(GpuTime);
        Cpu.Add(FPlatformTime::GetCPUTime().CPUTimePct);
        PeakMemory = FMath::Max(PeakMemory, uint64(FPlatformMemory::GetStats().UsedPhysical));
        if (Now - MeasurementStart >= MeasureSeconds)
            Finish(Frames.Num() >= MeasureSeconds*54 && GDynamicRHI && FString(GDynamicRHI->GetName()) == TEXT("D3D12")
                && !FModuleManager::Get().IsModuleLoaded(TEXT("SpoutPlugin")));
        return true;
    }

public:
    void StartupModule() override
    {
        FString Scenario;
        if (!FParse::Value(FCommandLine::Get(), TEXT("SpoutPerf="), Scenario)) return;
        if (Scenario != TEXT("noplugin") || !Attest()) { FPlatformMisc::RequestExitWithStatus(true,21); return; }
        FParse::Value(FCommandLine::Get(), TEXT("SpoutResultDir="), OutputDirectory);
        FParse::Value(FCommandLine::Get(), TEXT("SpoutPerfSeconds="), MeasureSeconds);
        FParse::Value(FCommandLine::Get(), TEXT("SpoutPerfWarmupSeconds="), WarmupSeconds);
        Ticker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateRaw(this,&FSpoutBaselineModule::Tick));
    }
    void ShutdownModule() override
    {
        if (Ticker.IsValid()) FTSTicker::GetCoreTicker().RemoveTicker(Ticker);
    }
};
IMPLEMENT_PRIMARY_GAME_MODULE(FSpoutBaselineModule, SpoutBaselineHost, "SpoutHostTest");
