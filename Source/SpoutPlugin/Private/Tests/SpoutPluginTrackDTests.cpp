/**
 * Release-validation regression tests.
 *
 * SpoutPluginTests.cpp already covers the pure decision logic: the format policy, input
 * validation, the registration state machine, cancellation, receiver ownership, teardown
 * ordering, the frame protocol, the RHI contract and the colour-space rules. Repeating any of
 * that here would add nothing, so this file deliberately holds only the two things those tests
 * cannot reach:
 *
 *   1. RUNTIME DEPLOYMENT. Whether the plugin module is loaded, whether Spout.dll is genuinely
 *      mapped into this process, and - the part no other test can assert - whether the loader
 *      resolved it from one of the four locations the plugin declares, rather than from somewhere
 *      on PATH. That is the automated half of the packaged-host DLL check, and it is the only test
 *      in the suite that would fail if SpoutPlugin.Build.cs stopped staging the DLL.
 *
 *   2. REPEATED-CYCLE STABILITY. The lifecycle tests prove one close cancels one stream. These
 *      prove that doing it a hundred times in a row does not accumulate state, reuse a generation,
 *      or leave ownership behind - which is what the packaged host's ten-cycle smoke sequence
 *      exercises at runtime and what a leak would show up as first.
 *
 * Everything here runs unattended in a commandlet, needs no external Spout process, and mutates no
 * machine-global state.
 *
 * Compiled only when WITH_DEV_AUTOMATION_TESTS is set, so none of it ships in a Shipping build.
 */
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "SpoutBPFunctionLibrary.h"
#include "SpoutReceiverOwnership.h"
#include "SpoutSession.h"

#include "Engine/Texture2D.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Interfaces/IPluginManager.h"
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "UObject/Package.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <libloaderapi.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

#define SPOUT_TRACKD_TEST_FLAGS \
	(EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

namespace SpoutTrackDTests
{
	/** Absolute path of the Spout.dll currently mapped into this process, or empty. */
	FString GetLoadedSpoutDllPath()
	{
#if PLATFORM_WINDOWS
		// GetModuleHandle reports an already-mapped module and never loads one, so this observes the
		// loader rather than influencing it.
		HMODULE Handle = ::GetModuleHandleW(TEXT("Spout.dll"));
		if (Handle == nullptr)
		{
			return FString();
		}

		TCHAR Buffer[2048];
		const DWORD Written = ::GetModuleFileNameW(Handle, Buffer, UE_ARRAY_COUNT(Buffer));
		if (Written == 0 || Written >= UE_ARRAY_COUNT(Buffer))
		{
			return FString();
		}
		Buffer[Written] = TEXT('\0');

		FString Path = FPaths::ConvertRelativePathToFull(FString(Buffer));
		FPaths::NormalizeFilename(Path);
		return Path;
#else
		return FString();
#endif
	}

	/**
	 * The directories SpoutPlugin.Build.cs and FSpoutModule between them declare as legitimate
	 * homes for the runtime DLL.
	 *
	 * Kept in the same order the module's own candidate list uses. This is not a copy of the
	 * module's logic for its own sake: the assertion below is about which location the OS ended up
	 * resolving, which is an observable outcome of the build rules and the staging layout, not of
	 * this list.
	 */
	TArray<FString> GetDeclaredDllDirectories()
	{
		TArray<FString> Directories;

		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("SpoutPlugin"));
		if (Plugin.IsValid())
		{
			const FString BaseDir = FPaths::ConvertRelativePathToFull(Plugin->GetBaseDir());
			Directories.Add(FPaths::Combine(BaseDir, TEXT("Binaries"), TEXT("Win64")));
			Directories.Add(FPaths::Combine(BaseDir, TEXT("ThirdParty"), TEXT("Spout"), TEXT("lib"), TEXT("amd64")));
		}

		Directories.Add(FPaths::ConvertRelativePathToFull(
			FPaths::Combine(FPaths::ProjectDir(), TEXT("Binaries"), TEXT("Win64"))));
		Directories.Add(FPaths::ConvertRelativePathToFull(FString(FPlatformProcess::BaseDir())));

		for (FString& Directory : Directories)
		{
			FPaths::NormalizeDirectoryName(Directory);
		}
		return Directories;
	}

	UTexture2D* MakeTexture()
	{
		return UTexture2D::CreateTransient(4, 4, PF_B8G8R8A8);
	}

	UMaterialInstanceDynamic* MakeMaterial()
	{
		UMaterial* Parent = UMaterial::GetDefaultMaterial(MD_Surface);
		return Parent ? UMaterialInstanceDynamic::Create(Parent, GetTransientPackage()) : nullptr;
	}
}

// =================================================================================================
// Runtime deployment - the automated half of the packaged-host DLL check
// =================================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSpoutTrackDModuleLoadedTest,
	"SpoutPlugin.TrackD.Runtime.PluginModuleIsLoaded",
	SPOUT_TRACKD_TEST_FLAGS)

bool FSpoutTrackDModuleLoadedTest::RunTest(const FString& Parameters)
{
	// Trivial to state and not trivial to keep true: a descriptor typo, a platform allow-list
	// mistake or a module that throws during StartupModule all land here first.
	TestTrue(TEXT("the SpoutPlugin module is loaded"),
		FModuleManager::Get().IsModuleLoaded(TEXT("SpoutPlugin")));

	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("SpoutPlugin"));
	TestTrue(TEXT("IPluginManager can find the plugin"), Plugin.IsValid());

	if (Plugin.IsValid())
	{
		TestTrue(TEXT("the plugin reports itself enabled"), Plugin->IsEnabled());
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSpoutTrackDRuntimeDllLocationTest,
	"SpoutPlugin.TrackD.Runtime.RuntimeDllIsLoadedFromADeclaredLocation",
	SPOUT_TRACKD_TEST_FLAGS)

bool FSpoutTrackDRuntimeDllLocationTest::RunTest(const FString& Parameters)
{
	const FString LoadedPath = SpoutTrackDTests::GetLoadedSpoutDllPath();

	// Not loaded at all is a deployment failure, not a skip. Spout.dll is bundled with the plugin
	// and the module loads it explicitly at startup, so if it is absent here the build rules or the
	// staging layout have regressed.
	TestFalse(TEXT("Spout.dll is mapped into the process"), LoadedPath.IsEmpty());
	if (LoadedPath.IsEmpty())
	{
		AddError(TEXT("Spout.dll is not loaded. SpoutPlugin.Build.cs stages it via RuntimeDependencies ")
			TEXT("and FSpoutModule::StartupModule loads it by full path; check both before assuming this ")
			TEXT("is an environment problem."));
		return false;
	}

	const FString LoadedDirectory = FPaths::GetPath(LoadedPath);

	bool bDeclared = false;
	for (const FString& Directory : SpoutTrackDTests::GetDeclaredDllDirectories())
	{
		if (LoadedDirectory.Equals(Directory, ESearchCase::IgnoreCase))
		{
			bDeclared = true;
			break;
		}
	}

	// This is the assertion that matters. The module falls back to a bare-name load through the
	// OS search order when no declared candidate exists, which would silently bind to any Spout.dll
	// on PATH - a different build, a different version, or something else entirely. A pass here
	// means the resolved copy came from a location this plugin actually ships to.
	TestTrue(
		*FString::Printf(TEXT("Spout.dll resolved from a declared location, not the OS search order (got '%s')"), *LoadedPath),
		bDeclared);

	if (!bDeclared)
	{
		AddError(FString::Printf(
			TEXT("Spout.dll was loaded from '%s', which is none of the declared locations: %s"),
			*LoadedDirectory,
			*FString::Join(SpoutTrackDTests::GetDeclaredDllDirectories(), TEXT(", "))));
	}

	// A zero-byte or missing file that the loader nonetheless reports would mean the handle refers
	// to something other than the shipped binary.
	const int64 Size = IFileManager::Get().FileSize(*LoadedPath);
	TestTrue(TEXT("the resolved Spout.dll has a non-zero size on disk"), Size > 0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSpoutTrackDApiAnswersTest,
	"SpoutPlugin.TrackD.Runtime.SpoutApiAnswersDeterministically",
	SPOUT_TRACKD_TEST_FLAGS)

bool FSpoutTrackDApiAnswersTest::RunTest(const FString& Parameters)
{
	// The DLL being mapped proves deployment. This proves the plugin can USE it: GetMaxSenders
	// leaves its output at 0 whenever Spout is unavailable, so a positive limit is an answer that
	// came back through the DLL rather than a call that merely returned.
	int32 MaxSenders = -1;
	USpoutBPFunctionLibrary::GetMaxSenders(MaxSenders);
	TestTrue(TEXT("GetMaxSenders reports a usable limit, so Spout answered"), MaxSenders > 0);

	// Enumeration must clear its output on every path, including failure. Seeded with a sentinel so
	// a implementation that returns early without touching the array is caught.
	TArray<FSenderStruct> Senders;
	Senders.AddDefaulted();
	Senders[0].Name = FName(TEXT("SentinelThatMustNotSurvive"));

	USpoutBPFunctionLibrary::SpoutInfo(Senders);

	bool bSentinelSurvived = false;
	for (const FSenderStruct& Sender : Senders)
	{
		if (Sender.Name == FName(TEXT("SentinelThatMustNotSurvive")))
		{
			bSentinelSurvived = true;
			break;
		}
	}
	TestFalse(TEXT("SpoutInfo clears its output array before enumerating"), bSentinelSurvived);

	// A name that cannot exist: the struct must come back at its defaults rather than carrying
	// whatever the caller had in it.
	FSenderStruct Info;
	Info.Name = FName(TEXT("StaleValueFromAPreviousQuery"));
	Info.Width = 4321;
	Info.Height = 8765;
	Info.bIsAlive = true;

	const bool bFound = USpoutBPFunctionLibrary::GetSenderInfo(
		FName(TEXT("SpoutTrackD_NoSuchSender_9f3a1c")), Info);

	TestFalse(TEXT("a sender that does not exist is not reported as found"), bFound);
	TestEqual(TEXT("a failed lookup resets the width"), Info.Width, 0);
	TestEqual(TEXT("a failed lookup resets the height"), Info.Height, 0);
	TestFalse(TEXT("a failed lookup resets the alive flag"), Info.bIsAlive);

	return true;
}

// =================================================================================================
// Repeated-cycle stability - the automated counterpart of the packaged host's cycle stress
// =================================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSpoutTrackDRepeatedStreamCyclesTest,
	"SpoutPlugin.TrackD.Lifecycle.RepeatedOpenCloseCyclesStayCorrect",
	SPOUT_TRACKD_TEST_FLAGS)

bool FSpoutTrackDRepeatedStreamCyclesTest::RunTest(const FString& Parameters)
{
	// A local session, so nothing here can cancel a live stream in the same process.
	FSpoutSession Session;

	const FName Name(TEXT("TrackDCycles"));
	const int32 CycleCount = 100;

	TArray<FSpoutStreamToken> PreviousTokens;
	PreviousTokens.Reserve(CycleCount);

	uint64 LastGeneration = 0;

	for (int32 Cycle = 0; Cycle < CycleCount; ++Cycle)
	{
		const FSpoutStreamToken Token = Session.OpenStream(Name, ESpoutType::Sender);

		if (!Session.IsCurrent(Token))
		{
			AddError(FString::Printf(TEXT("cycle %d: a freshly issued token was already stale"), Cycle));
			return false;
		}

		// Every generation must be strictly newer than the last. A counter that wrapped or was reset
		// would make an OLD token current again, which is exactly the close-then-reopen hazard the
		// generation model exists to prevent.
		const uint64 Generation = Session.GetStreamGeneration(Name, ESpoutType::Sender);
		if (Cycle > 0 && Generation <= LastGeneration)
		{
			AddError(FString::Printf(
				TEXT("cycle %d: stream generation went from %llu to %llu; it must increase"),
				Cycle, LastGeneration, Generation));
			return false;
		}
		LastGeneration = Generation;

		Session.CancelStream(Name, ESpoutType::Sender);

		if (Session.IsCurrent(Token))
		{
			AddError(FString::Printf(TEXT("cycle %d: the token survived its own close"), Cycle));
			return false;
		}

		PreviousTokens.Add(Token);
	}

	// After a hundred cycles, not one of the earlier tokens may have become current again.
	int32 Resurrected = 0;
	for (const FSpoutStreamToken& Token : PreviousTokens)
	{
		if (Session.IsCurrent(Token))
		{
			++Resurrected;
		}
	}
	TestEqual(TEXT("no token from any earlier cycle is current again"), Resurrected, 0);

	// And a stream in the other role, never touched, is unaffected by all of it.
	const FSpoutStreamToken ReceiverToken = Session.OpenStream(Name, ESpoutType::Receiver);
	TestTrue(TEXT("the same name in the receiver role is still usable"), Session.IsCurrent(ReceiverToken));

	// One global teardown stales everything at once, including the receiver just opened.
	Session.BeginStop();
	TestFalse(TEXT("teardown stales the untouched receiver token too"), Session.IsCurrent(ReceiverToken));
	TestFalse(TEXT("no new work is issued during teardown"),
		Session.OpenStream(Name, ESpoutType::Sender).IsSet());

	Session.EndStop();
	TestTrue(TEXT("work is issued again once teardown finishes"),
		Session.OpenStream(Name, ESpoutType::Sender).IsSet());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSpoutTrackDRepeatedReceiverOwnershipTest,
	"SpoutPlugin.TrackD.Lifecycle.RepeatedReceiverCyclesLeaveNothingTracked",
	SPOUT_TRACKD_TEST_FLAGS)

bool FSpoutTrackDRepeatedReceiverOwnershipTest::RunTest(const FString& Parameters)
{
	// Local tracker: releasing the process-wide one would take out a live receiver.
	FSpoutReceiverOwnership Ownership;

	const FName ReceiverName(TEXT("TrackDReceiverCycles"));
	const FName ParameterName(TEXT("SpoutTexture"));
	const int32 CycleCount = 25;

	for (int32 Cycle = 0; Cycle < CycleCount; ++Cycle)
	{
		UTexture2D* Texture = SpoutTrackDTests::MakeTexture();
		UMaterialInstanceDynamic* Material = SpoutTrackDTests::MakeMaterial();

		if (Texture == nullptr || Material == nullptr)
		{
			AddError(FString::Printf(TEXT("cycle %d: could not create the test texture or material"), Cycle));
			return false;
		}

		Ownership.AdoptTexture(ReceiverName, Texture);
		Ownership.RegisterMaterialBinding(ReceiverName, Material, ParameterName, /*bPluginCreated*/ true);

		// Re-registering the same pair every "frame" must not grow the binding collection; a
		// per-frame Blueprint call would otherwise accumulate one binding per frame forever.
		for (int32 Repeat = 0; Repeat < 5; ++Repeat)
		{
			Ownership.RegisterMaterialBinding(ReceiverName, Material, ParameterName, /*bPluginCreated*/ true);
		}

		if (Ownership.NumMaterialBindings(ReceiverName) != 1)
		{
			AddError(FString::Printf(
				TEXT("cycle %d: re-registering the same (material, parameter) pair produced %d bindings, expected 1"),
				Cycle, Ownership.NumMaterialBindings(ReceiverName)));
			return false;
		}

		if (!Ownership.OwnsTexture(ReceiverName, Texture))
		{
			AddError(FString::Printf(TEXT("cycle %d: the adopted texture is not owned"), Cycle));
			return false;
		}

		const bool bReleased = Ownership.Release(ReceiverName);
		if (!bReleased)
		{
			AddError(FString::Printf(TEXT("cycle %d: Release reported nothing was tracked"), Cycle));
			return false;
		}

		// Nothing may survive the release, in either direction.
		if (Ownership.FindTexture(ReceiverName) != nullptr)
		{
			AddError(FString::Printf(TEXT("cycle %d: a texture is still tracked after Release"), Cycle));
			return false;
		}
		if (Ownership.NumMaterialBindings(ReceiverName) != 0)
		{
			AddError(FString::Printf(
				TEXT("cycle %d: %d material binding(s) survived Release"),
				Cycle, Ownership.NumMaterialBindings(ReceiverName)));
			return false;
		}

		// Idempotent: a second close of an already-closed session reports nothing tracked rather
		// than doing anything.
		if (Ownership.Release(ReceiverName))
		{
			AddError(FString::Printf(TEXT("cycle %d: a second Release claimed something was tracked"), Cycle));
			return false;
		}
	}

	TestEqual(TEXT("nothing is tracked after every cycle has closed"),
		Ownership.NumMaterialBindings(ReceiverName), 0);
	TestNull(TEXT("no texture is tracked after every cycle has closed"),
		Ownership.FindTexture(ReceiverName));

	// ReleaseAll over an already-empty tracker must be safe; module shutdown calls it
	// unconditionally after the per-name closes have run.
	Ownership.ReleaseAll();
	TestNull(TEXT("ReleaseAll on an empty tracker is safe"), Ownership.FindTexture(ReceiverName));

	return true;
}

#undef SPOUT_TRACKD_TEST_FLAGS

#endif // WITH_DEV_AUTOMATION_TESTS
