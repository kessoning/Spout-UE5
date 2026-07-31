#include "SpoutHostTestReport.h"
#include "SpoutHostTestModule.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/CommandLine.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"

namespace SpoutHostTest
{
	FReport* FReport::Instance = nullptr;

	FString EscapeJson(const FString& In)
	{
		FString Out;
		Out.Reserve(In.Len() + 16);
		for (int32 Index = 0; Index < In.Len(); ++Index)
		{
			const TCHAR Char = In[Index];
			switch (Char)
			{
			case TEXT('"'):  Out += TEXT("\\\""); break;
			case TEXT('\\'): Out += TEXT("\\\\"); break;
			case TEXT('\b'): Out += TEXT("\\b");  break;
			case TEXT('\f'): Out += TEXT("\\f");  break;
			case TEXT('\n'): Out += TEXT("\\n");  break;
			case TEXT('\r'): Out += TEXT("\\r");  break;
			case TEXT('\t'): Out += TEXT("\\t");  break;
			default:
				if (Char < 0x20)
				{
					Out += FString::Printf(TEXT("\\u%04x"), static_cast<int32>(Char));
				}
				else
				{
					Out.AppendChar(Char);
				}
				break;
			}
		}
		return Out;
	}

	void FReport::Initialize()
	{
		if (Instance != nullptr)
		{
			return;
		}

		Instance = new FReport();

		// The scripts always pass -SpoutResultDir so each matrix row writes into its own evidence
		// directory. The ProjectSavedDir fallback keeps a hand-run host usable without it.
		FString Dir;
		if (!FParse::Value(FCommandLine::Get(), TEXT("-SpoutResultDir="), Dir) || Dir.IsEmpty())
		{
			Dir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("SpoutHostTest"));
		}
		Dir = FPaths::ConvertRelativePathToFull(Dir);

		IFileManager& FileManager = IFileManager::Get();
		FileManager.MakeDirectory(*Dir, /*Tree*/ true);

		Instance->ResultDir = Dir;
		Instance->MarkerFilePath = FPaths::Combine(Dir, TEXT("markers.log"));
		Instance->ResultFilePath = FPaths::Combine(Dir, TEXT("result.json"));

		// Truncate any previous run's markers. A stale marker file from an earlier row would be read
		// as this row's evidence, producing a false pass.
		FFileHelper::SaveStringToFile(FString(), *Instance->MarkerFilePath);
		if (FileManager.FileExists(*Instance->ResultFilePath))
		{
			FileManager.Delete(*Instance->ResultFilePath, /*RequireExists*/ false, /*EvenReadOnly*/ true);
		}
	}

	void FReport::Shutdown()
	{
		delete Instance;
		Instance = nullptr;
	}

	void FReport::AppendMarkerLine(const FString& Line)
	{
		// Opened, written and closed per line rather than held open: the watchdog can kill this
		// process at any moment, and an unflushed buffered writer would take the evidence with it.
		const FString Stamped = FString::Printf(
			TEXT("[%s] %s"), *FDateTime::UtcNow().ToString(TEXT("%Y.%m.%d-%H.%M.%S")), *Line);

		FFileHelper::SaveStringToFile(
			Stamped + LINE_TERMINATOR,
			*MarkerFilePath,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM,
			&IFileManager::Get(),
			EFileWrite::FILEWRITE_Append);
	}

	void FReport::Mark(const FString& Marker)
	{
		EmittedMarkers.Add(Marker);
		AppendMarkerLine(Marker);

		// Compiled out in Shipping, which is the whole reason the file above exists. Kept because a
		// Development run is far easier to diagnose when the markers are interleaved with the
		// plugin's own LogSpoutPlugin output.
		UE_LOG(LogSpoutHostTest, Display, TEXT("%s"), *Marker);

		// Reaches a debugger and, when one is attached, the parent process. Best effort only; nothing
		// in the harness depends on it.
		FPlatformMisc::LowLevelOutputDebugStringf(TEXT("%s\n"), *Marker);
	}

	void FReport::MarkValue(const TCHAR* Marker, const FString& Value)
	{
		Mark(FString::Printf(TEXT("%s:%s"), Marker, *Value));
	}

	void FReport::SetField(const FString& Key, const FString& Value)
	{
		for (TPair<FString, FString>& Existing : Fields)
		{
			if (Existing.Key == Key)
			{
				Existing.Value = Value;
				return;
			}
		}
		Fields.Emplace(Key, Value);
	}

	void FReport::Fail(const FString& Reason)
	{
		if (bFailed)
		{
			// Keep the first reason. A cascade of consequences buries the cause.
			return;
		}
		bFailed = true;
		FailureReason = Reason;
		Mark(FString::Printf(TEXT("%s%s"), Markers::SmokeFailPrefix, *Reason));
	}

	void FReport::WriteResultJson(const FString& RunKind, bool bPassed)
	{
		FString Json;
		Json += TEXT("{\n");
		Json += TEXT("  \"schema\": \"spouthosttest-result/1\",\n");
		Json += FString::Printf(TEXT("  \"runKind\": \"%s\",\n"), *EscapeJson(RunKind));
		Json += FString::Printf(TEXT("  \"passed\": %s,\n"), bPassed ? TEXT("true") : TEXT("false"));
		Json += FString::Printf(TEXT("  \"failureReason\": %s,\n"),
			FailureReason.IsEmpty() ? TEXT("null") : *FString::Printf(TEXT("\"%s\""), *EscapeJson(FailureReason)));
		Json += FString::Printf(TEXT("  \"generatedUtc\": \"%s\",\n"), *FDateTime::UtcNow().ToIso8601());

		Json += TEXT("  \"markers\": [\n");
		for (int32 Index = 0; Index < EmittedMarkers.Num(); ++Index)
		{
			Json += FString::Printf(TEXT("    \"%s\"%s\n"),
				*EscapeJson(EmittedMarkers[Index]),
				(Index + 1 < EmittedMarkers.Num()) ? TEXT(",") : TEXT(""));
		}
		Json += TEXT("  ],\n");

		Json += TEXT("  \"fields\": {\n");
		for (int32 Index = 0; Index < Fields.Num(); ++Index)
		{
			Json += FString::Printf(TEXT("    \"%s\": \"%s\"%s\n"),
				*EscapeJson(Fields[Index].Key),
				*EscapeJson(Fields[Index].Value),
				(Index + 1 < Fields.Num()) ? TEXT(",") : TEXT(""));
		}
		Json += TEXT("  }\n");
		Json += TEXT("}\n");

		FFileHelper::SaveStringToFile(Json, *ResultFilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}
}
