// Copyright (c) 2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
#include "SpoutHostTestIdentity.h"
#include "SpoutHostTestReport.h"
#include "SpoutModule.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/EngineVersion.h"
#include "HAL/PlatformProcess.h"
#include "HAL/FileManager.h"

bool SpoutHostTest::WriteRunIdentity()
{
	FString RunId, Output, ExpectedPlugin, ExpectedHost;
	if (!FParse::Value(FCommandLine::Get(), TEXT("SpoutRunId="), RunId)) return true;
	FParse::Value(FCommandLine::Get(), TEXT("SpoutAttestationPath="), Output);
	FParse::Value(FCommandLine::Get(), TEXT("SpoutSourceId="), ExpectedPlugin);
	FParse::Value(FCommandLine::Get(), TEXT("SpoutHostSourceId="), ExpectedHost);
	const FString PluginId = FSpoutModule::GetBuildSourceIdentity();
	const FString HostId = UTF8_TO_TCHAR(SPOUT_HOST_SOURCE_ID);
	const bool Match = !RunId.IsEmpty() && !Output.IsEmpty() && ExpectedPlugin == PluginId && ExpectedHost == HostId;
	const FString Json = FString::Printf(TEXT("{\"runId\":\"%s\",\"sourceId\":\"%s\",\"hostSourceId\":\"%s\",\"engineBuild\":\"%s\",\"processId\":%u,\"executable\":\"%s\",\"matched\":%s}"),
		*EscapeJson(RunId), *PluginId, *HostId, *EscapeJson(FEngineVersion::Current().ToString()),
		FPlatformProcess::GetCurrentProcessId(), *EscapeJson(FPlatformProcess::ExecutablePath()), Match ? TEXT("true") : TEXT("false"));
	if (Output.IsEmpty() || IFileManager::Get().FileExists(*Output)) return false;
	return FFileHelper::SaveStringToFile(Json, *Output, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM) && Match;
}
