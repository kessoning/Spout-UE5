// Copyright (c) 2025-2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

/**
 * Automation tests for the plugin's correctness-critical logic.
 *
 * These are deterministic unit tests over the plugin's pure helpers: the supported-format policy,
 * input and external-metadata validation, the sender-registration state machine (driven through a
 * fake SDK backend), the output-render-target compatibility rule, and the deterministic-output
 * contract of the Blueprint API.
 *
 * They require no GPU work, no shared texture, and no external Spout process, so they run in an
 * unattended commandlet. The parts that genuinely need a GPU and a peer process (an actual
 * shared-texture copy, real registration against Spout's shared memory, interoperability) are
 * runtime validation and are recorded as such rather than approximated here.
 *
 * Deliberate constraints:
 *   - Only TestTrue / TestFalse / TestNull / TestEqual(int32) / TestEqual(FString) are used. Those
 *     overloads are present and stable across UE 5.3 to 5.8; TestNotEqual and the templated
 *     TestEqual changed shape between those versions.
 *   - Nothing here mutates process-global or machine-global Spout state. In particular no test
 *     calls SetMaxSenders with an accepted value, because that writes a persistent limit to the
 *     machine; only its rejection paths, which return before touching Spout, are exercised.
 *
 * Compiled only when WITH_DEV_AUTOMATION_TESTS is set, so none of this ships in a Shipping build.
 */
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "SpoutBPFunctionLibrary.h"
#include "SpoutFormatPolicy.h"
#include "SpoutFrameSync.h"
#include "SpoutReceiver.h"
#include "SpoutReceiverOwnership.h"
#include "SpoutRHIState.h"
#include "SpoutSenderRegistration.h"
#include "SpoutSession.h"
#include "SpoutTextureUtils.h"
#include "SpoutValidation.h"

#include "Containers/StringConv.h"
#include "Engine/Texture2D.h"
// MaterialDomain.h, not just Material.h: on UE 5.3 the latter only forward-declares EMaterialDomain,
// so MD_Surface is undeclared without this.
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

// Every test in this file is safe in the editor and in a commandlet, and none of them need a
// non-null RHI.
#define SPOUT_TEST_FLAGS \
	(EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

namespace SpoutPluginTests
{
	/**
	 * Scriptable stand-in for spoutSenderNames.
	 *
	 * Records every call and lets a test force any individual operation to fail, which is how the
	 * failure, retry and rollback paths are exercised without needing Spout's sender list to
	 * actually be full or otherwise broken.
	 */
	class FFakeSenderNameBackend final : public ISpoutSenderNameBackend
	{
	public:
		// Failure switches.
		bool bFailCreate = false;
		bool bFailUpdate = false;

		// Call counters.
		int32 CreateCalls = 0;
		int32 UpdateCalls = 0;
		int32 ReleaseCalls = 0;

		// Last values handed to the SDK, so tests can assert what was actually advertised.
		FString LastName;
		uint32 LastWidth = 0;
		uint32 LastHeight = 0;
		void* LastHandle = nullptr;
		uint32 LastFormat = 0;

		/** The exact name the last ReleaseSenderName was called with. */
		FString LastReleasedName;

		/** Names currently present in the fake sender list. */
		TSet<FString> RegisteredNames;

		virtual bool CreateSender(const ANSICHAR* Utf8Name, uint32 Width, uint32 Height, void* SharedHandle, uint32 Format) override
		{
			++CreateCalls;
			Record(Utf8Name, Width, Height, SharedHandle, Format);
			if (bFailCreate)
			{
				return false;
			}
			RegisteredNames.Add(LastName);
			return true;
		}

		virtual bool UpdateSender(const ANSICHAR* Utf8Name, uint32 Width, uint32 Height, void* SharedHandle, uint32 Format) override
		{
			++UpdateCalls;
			Record(Utf8Name, Width, Height, SharedHandle, Format);
			return !bFailUpdate;
		}

		virtual void ReleaseSenderName(const ANSICHAR* Utf8Name) override
		{
			++ReleaseCalls;
			LastReleasedName = FString(UTF8_TO_TCHAR(Utf8Name));
			RegisteredNames.Remove(LastReleasedName);
		}

	private:
		void Record(const ANSICHAR* Utf8Name, uint32 Width, uint32 Height, void* SharedHandle, uint32 Format)
		{
			LastName = FString(UTF8_TO_TCHAR(Utf8Name));
			LastWidth = Width;
			LastHeight = Height;
			LastHandle = SharedHandle;
			LastFormat = Format;
		}
	};

	/** Distinct non-null pointers standing in for shared texture handles. */
	void* const HandleA = reinterpret_cast<void*>(static_cast<UPTRINT>(0x1000));
	void* const HandleB = reinterpret_cast<void*>(static_cast<UPTRINT>(0x2000));

	/** Encodes a name the way the plugin does, for feeding the state machine directly. */
	TArray<ANSICHAR> Utf8(const TCHAR* Name)
	{
		TArray<ANSICHAR> Buffer;
		SpoutValidation::ValidateName(FName(Name), &Buffer);
		return Buffer;
	}

	/**
	 * Records which lifecycle steps ran and in what order.
	 *
	 * Shutdown correctness is purely a question of order, so the test for it has to be able to
	 * see that order. This substitutes for the production steps and does nothing else.
	 */
	class FRecordingLifecycleSteps final : public ISpoutLifecycleSteps
	{
	public:
		TArray<FString> Calls;

		virtual void EnterStoppingState() override     { Calls.Add(TEXT("EnterStoppingState")); }
		virtual void DetachProducers() override        { Calls.Add(TEXT("DetachProducers")); }
		virtual void ClearPendingWork() override       { Calls.Add(TEXT("ClearPendingWork")); }
		virtual void FlushRenderingWork() override     { Calls.Add(TEXT("FlushRenderingWork")); }
		virtual void ReleaseNativeResources() override { Calls.Add(TEXT("ReleaseNativeResources")); }
		virtual void ReleaseManagedObjects() override  { Calls.Add(TEXT("ReleaseManagedObjects")); }
		virtual void LeaveStoppingState() override     { Calls.Add(TEXT("LeaveStoppingState")); }

		int32 IndexOf(const TCHAR* Step) const { return Calls.IndexOfByKey(FString(Step)); }
	};

	/**
	 * A transient texture with no render resource.
	 *
	 * UpdateResource() is deliberately not called: these tests are about UObject ownership, and a
	 * texture with no RHI resource has nothing to release on the render thread, which keeps the
	 * tests independent of the RHI and safe to abandon to the collector.
	 */
	UTexture2D* MakeTestTexture()
	{
		return UTexture2D::CreateTransient(4, 4, PF_B8G8R8A8);
	}

	/** A MID over the engine default material, which is always loaded. */
	UMaterialInstanceDynamic* MakeTestMaterial()
	{
		UMaterial* Parent = UMaterial::GetDefaultMaterial(MD_Surface);
		return Parent ? UMaterialInstanceDynamic::Create(Parent, GetTransientPackage()) : nullptr;
	}

	/**
	 * A fresh transient UMaterial, to stand in for one consumer's InputMaterial.
	 *
	 * Created rather than taken from the engine's defaults because two consumers need two DISTINCT
	 * parents and UMaterial::GetDefaultMaterial returns the same object for every domain available
	 * here. A material instance cannot be a parent either -- the engine rejects it with "Only
	 * Materials and MaterialInstanceConstants are valid parents for a material instance" -- so a
	 * plain UMaterial is the only option. Never rendered, so it needs no shader map.
	 */
	UMaterial* MakeTestParentMaterial()
	{
		return NewObject<UMaterial>(GetTransientPackage());
	}

	/**
	 * One consumer's tick, driving the REAL production decision.
	 *
	 * SpoutReceiverInternal::ResolveConsumerMaterial is the function FSpoutReceiver::Receive calls,
	 * not a copy of it, so a regression in the per-consumer material rule fails these tests.
	 *
	 * LIMITATION, recorded deliberately: FSpoutReceiver::Receive itself still cannot be unit-tested,
	 * because it needs a live external Spout sender to get as far as the material code. What is
	 * covered here is the decision and the ownership bookkeeping around it; the surrounding Receive
	 * plumbing (registry lookup, shared-handle open, render-command enqueue) is runtime validation.
	 */
	UMaterialInstanceDynamic* TickConsumer(
		FSpoutReceiverOwnership& Ownership,
		FName ReceiverName,
		UMaterialInstanceDynamic* CallerOutMat,
		UMaterialInterface* InputMaterial,
		FName ParameterName,
		UTexture2D* SessionTexture)
	{
		bool bPluginCreated = false;
		UMaterialInstanceDynamic* Material = SpoutReceiverInternal::ResolveConsumerMaterial(
			Ownership, ReceiverName, CallerOutMat, InputMaterial, bPluginCreated);

		if (Material)
		{
			Material->SetTextureParameterValue(ParameterName, SessionTexture);
			Ownership.RegisterMaterialBinding(ReceiverName, Material, ParameterName, bPluginCreated);
		}

		return Material;
	}

	/** The value the MID currently overrides ParameterName with, and whether an override exists. */
	bool FindTextureOverride(const UMaterialInstanceDynamic* Material, FName ParameterName, UTexture*& OutValue)
	{
		OutValue = nullptr;
		if (!Material)
		{
			return false;
		}

		for (const FTextureParameterValue& Value : Material->TextureParameterValues)
		{
			if (Value.ParameterInfo.Name == ParameterName)
			{
				OutValue = Value.ParameterValue;
				return true;
			}
		}
		return false;
	}
}

// =================================================================================================
// Supported-format policy
// =================================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSpoutFormatPolicySupportedTest,
	"SpoutPlugin.FormatPolicy.SupportedFormatsRoundTrip",
	SPOUT_TEST_FLAGS)

bool FSpoutFormatPolicySupportedTest::RunTest(const FString& Parameters)
{
	using namespace SpoutFormatPolicy;

	struct FCase
	{
		DXGI_FORMAT Native;
		DXGI_FORMAT ExpectedShared;
		EPixelFormat ExpectedPixel;
		bool bExpectedSRGB;
	};

	// Every format the policy claims to support directly. 8-bit _UNORM_SRGB sources are shared as
	// _UNORM with unchanged bytes (SpoutGL receivers refuse _UNORM_SRGB), and are still received.
	const FCase Cases[] =
	{
		{ DXGI_FORMAT_R8G8B8A8_UNORM,      DXGI_FORMAT_R8G8B8A8_UNORM,      PF_R8G8B8A8,    true  },
		{ DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM,      PF_R8G8B8A8,    true  },
		{ DXGI_FORMAT_B8G8R8A8_UNORM,      DXGI_FORMAT_B8G8R8A8_UNORM,      PF_B8G8R8A8,    true  },
		{ DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM,      PF_B8G8R8A8,    true  },
		{ DXGI_FORMAT_R10G10B10A2_UNORM,   DXGI_FORMAT_R10G10B10A2_UNORM,   PF_A2B10G10R10, false },
		{ DXGI_FORMAT_R16G16B16A16_FLOAT,  DXGI_FORMAT_R16G16B16A16_FLOAT,  PF_FloatRGBA,   false },
	};

	for (const FCase& Case : Cases)
	{
		const FString Label = DescribeDxgiFormat(static_cast<uint32>(Case.Native));

		const FSenderFormat SenderResult = ResolveSenderFormat(Case.Native);
		TestEqual(FString::Printf(TEXT("%s sender support"), *Label), (int32)SenderResult.Support, (int32)ESupport::Supported);
		TestTrue(FString::Printf(TEXT("%s sender usable"), *Label), SenderResult.IsUsable());
		TestEqual(FString::Printf(TEXT("%s shared format"), *Label), (int32)SenderResult.SharedFormat, (int32)Case.ExpectedShared);

		const FReceiverFormat ReceiverResult = ResolveReceiverFormat(static_cast<uint32>(Case.Native));
		TestEqual(FString::Printf(TEXT("%s receiver support"), *Label), (int32)ReceiverResult.Support, (int32)ESupport::Supported);
		TestTrue(FString::Printf(TEXT("%s receiver usable"), *Label), ReceiverResult.IsUsable());
		TestEqual(FString::Printf(TEXT("%s pixel format"), *Label), (int32)ReceiverResult.PixelFormat, (int32)Case.ExpectedPixel);
		TestEqual(FString::Printf(TEXT("%s sRGB preference"), *Label), (int32)ReceiverResult.bPrefersSRGB, (int32)Case.bExpectedSRGB);

		// Symmetry: a pixel format the receiver can produce must be one the sender accepts.
		TestTrue(FString::Printf(TEXT("%s pixel format is sendable"), *Label),
			IsSupportedSendPixelFormat(ReceiverResult.PixelFormat));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSpoutFormatPolicyTypelessTest,
	"SpoutPlugin.FormatPolicy.TypelessFormatsMapToTypedEquivalents",
	SPOUT_TEST_FLAGS)

bool FSpoutFormatPolicyTypelessTest::RunTest(const FString& Parameters)
{
	using namespace SpoutFormatPolicy;

	struct FCase
	{
		DXGI_FORMAT Typeless;
		DXGI_FORMAT ExpectedTyped;
		EPixelFormat ExpectedPixel;
	};

	// The deliberate typeless-to-typed mappings.
	const FCase Cases[] =
	{
		{ DXGI_FORMAT_R8G8B8A8_TYPELESS,     DXGI_FORMAT_R8G8B8A8_UNORM,     PF_R8G8B8A8    },
		{ DXGI_FORMAT_B8G8R8A8_TYPELESS,     DXGI_FORMAT_B8G8R8A8_UNORM,     PF_B8G8R8A8    },
		{ DXGI_FORMAT_R10G10B10A2_TYPELESS,  DXGI_FORMAT_R10G10B10A2_UNORM,  PF_A2B10G10R10 },
		{ DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_FLOAT, PF_FloatRGBA   },
	};

	for (const FCase& Case : Cases)
	{
		const FString Label = DescribeDxgiFormat(static_cast<uint32>(Case.Typeless));

		const FSenderFormat SenderResult = ResolveSenderFormat(Case.Typeless);
		// Classified as an intentional mapping rather than plain "Supported": the distinction is
		// what makes the reinterpretation visible in diagnostics.
		TestEqual(FString::Printf(TEXT("%s is MappedTyped"), *Label), (int32)SenderResult.Support, (int32)ESupport::MappedTyped);
		TestTrue(FString::Printf(TEXT("%s usable"), *Label), SenderResult.IsUsable());
		TestEqual(FString::Printf(TEXT("%s maps to typed format"), *Label), (int32)SenderResult.SharedFormat, (int32)Case.ExpectedTyped);

		const FReceiverFormat ReceiverResult = ResolveReceiverFormat(static_cast<uint32>(Case.Typeless));
		TestEqual(FString::Printf(TEXT("%s receiver is MappedTyped"), *Label), (int32)ReceiverResult.Support, (int32)ESupport::MappedTyped);
		TestEqual(FString::Printf(TEXT("%s receiver pixel format"), *Label), (int32)ReceiverResult.PixelFormat, (int32)Case.ExpectedPixel);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSpoutFormatPolicyRejectionTest,
	"SpoutPlugin.FormatPolicy.UnknownAndUnsupportedAreRejected",
	SPOUT_TEST_FLAGS)

bool FSpoutFormatPolicyRejectionTest::RunTest(const FString& Parameters)
{
	using namespace SpoutFormatPolicy;

	// DXGI_FORMAT_UNKNOWN. Substituting BGRA8 here would advertise a layout the source does not
	// have.
	{
		const FSenderFormat SenderResult = ResolveSenderFormat(DXGI_FORMAT_UNKNOWN);
		TestEqual(TEXT("UNKNOWN sender support"), (int32)SenderResult.Support, (int32)ESupport::Unknown);
		TestFalse(TEXT("UNKNOWN is not usable by the sender"), SenderResult.IsUsable());
		TestEqual(TEXT("UNKNOWN yields no shared format"), (int32)SenderResult.SharedFormat, (int32)DXGI_FORMAT_UNKNOWN);

		const FReceiverFormat ReceiverResult = ResolveReceiverFormat(static_cast<uint32>(DXGI_FORMAT_UNKNOWN));
		TestEqual(TEXT("UNKNOWN receiver support"), (int32)ReceiverResult.Support, (int32)ESupport::Unknown);
		TestFalse(TEXT("UNKNOWN is not usable by the receiver"), ReceiverResult.IsUsable());
		// The regression that matters here: this must NOT come back as PF_B8G8R8A8.
		TestTrue(TEXT("UNKNOWN must not become PF_B8G8R8A8"), ReceiverResult.PixelFormat != PF_B8G8R8A8);
		TestEqual(TEXT("UNKNOWN yields PF_Unknown"), (int32)ReceiverResult.PixelFormat, (int32)PF_Unknown);
	}

	// Representative unsupported formats: block-compressed, planar/video, depth, and wider or
	// narrower colour layouts. None of these may be reinterpreted as BGRA8 on receive.
	const DXGI_FORMAT Unsupported[] =
	{
		DXGI_FORMAT_BC1_UNORM,
		DXGI_FORMAT_BC3_UNORM,
		DXGI_FORMAT_BC7_UNORM,
		DXGI_FORMAT_NV12,
		DXGI_FORMAT_P010,
		DXGI_FORMAT_D32_FLOAT,
		DXGI_FORMAT_R32G32B32A32_FLOAT,
		DXGI_FORMAT_R11G11B10_FLOAT,
		DXGI_FORMAT_R16G16B16A16_UNORM,
		DXGI_FORMAT_R8_UNORM,
		DXGI_FORMAT_R32_FLOAT,
	};

	for (const DXGI_FORMAT Format : Unsupported)
	{
		const FString Label = DescribeDxgiFormat(static_cast<uint32>(Format));

		const FSenderFormat SenderResult = ResolveSenderFormat(Format);
		TestEqual(FString::Printf(TEXT("%s sender support"), *Label), (int32)SenderResult.Support, (int32)ESupport::Unsupported);
		TestFalse(FString::Printf(TEXT("%s not sender-usable"), *Label), SenderResult.IsUsable());

		const FReceiverFormat ReceiverResult = ResolveReceiverFormat(static_cast<uint32>(Format));
		TestEqual(FString::Printf(TEXT("%s receiver support"), *Label), (int32)ReceiverResult.Support, (int32)ESupport::Unsupported);
		TestFalse(FString::Printf(TEXT("%s not receiver-usable"), *Label), ReceiverResult.IsUsable());
		TestTrue(FString::Printf(TEXT("%s must not become PF_B8G8R8A8"), *Label),
			ReceiverResult.PixelFormat != PF_B8G8R8A8);
	}

	// Pixel formats outside the policy must be refused by the send-side check too.
	TestFalse(TEXT("PF_Unknown is not sendable"), IsSupportedSendPixelFormat(PF_Unknown));
	TestFalse(TEXT("PF_DXT1 is not sendable"), IsSupportedSendPixelFormat(PF_DXT1));
	TestFalse(TEXT("PF_G8 is not sendable"), IsSupportedSendPixelFormat(PF_G8));
	TestFalse(TEXT("PF_A32B32G32R32F is not sendable"), IsSupportedSendPixelFormat(PF_A32B32G32R32F));
	TestFalse(TEXT("PF_FloatRGB is not sendable"), IsSupportedSendPixelFormat(PF_FloatRGB));

	TestTrue(TEXT("PF_B8G8R8A8 is sendable"), IsSupportedSendPixelFormat(PF_B8G8R8A8));
	TestTrue(TEXT("PF_R8G8B8A8 is sendable"), IsSupportedSendPixelFormat(PF_R8G8B8A8));
	TestTrue(TEXT("PF_A2B10G10R10 is sendable"), IsSupportedSendPixelFormat(PF_A2B10G10R10));
	TestTrue(TEXT("PF_FloatRGBA is sendable"), IsSupportedSendPixelFormat(PF_FloatRGBA));

	return true;
}

// =================================================================================================
// Optional output render target compatibility
// =================================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSpoutOutputTargetCompatibilityTest,
	"SpoutPlugin.OutputTarget.MismatchIsDetectedAndClassified",
	SPOUT_TEST_FLAGS)

bool FSpoutOutputTargetCompatibilityTest::RunTest(const FString& Parameters)
{
	using namespace SpoutFormatPolicy;

	// A fully matching target is the only case that may be copied into.
	TestEqual(TEXT("exact match is compatible"),
		(int32)CheckOutputTarget(1920, 1080, PF_B8G8R8A8, 1920, 1080, PF_B8G8R8A8, 1),
		(int32)EOutputTargetStatus::Compatible);

	// Size.
	TestEqual(TEXT("width mismatch"),
		(int32)CheckOutputTarget(1920, 1080, PF_B8G8R8A8, 1280, 1080, PF_B8G8R8A8, 1),
		(int32)EOutputTargetStatus::SizeMismatch);
	TestEqual(TEXT("height mismatch"),
		(int32)CheckOutputTarget(1920, 1080, PF_B8G8R8A8, 1920, 720, PF_B8G8R8A8, 1),
		(int32)EOutputTargetStatus::SizeMismatch);

	// Format. A mismatch is refused, not warned about and then copied anyway.
	TestEqual(TEXT("format mismatch"),
		(int32)CheckOutputTarget(1920, 1080, PF_B8G8R8A8, 1920, 1080, PF_R8G8B8A8, 1),
		(int32)EOutputTargetStatus::FormatMismatch);
	TestEqual(TEXT("float vs 8-bit format mismatch"),
		(int32)CheckOutputTarget(1920, 1080, PF_FloatRGBA, 1920, 1080, PF_B8G8R8A8, 1),
		(int32)EOutputTargetStatus::FormatMismatch);

	// Sample count. CopyResource requires matching sample counts and the shared texture is always
	// single-sampled, so a multisampled destination can never take the copy.
	TestEqual(TEXT("MSAA destination rejected"),
		(int32)CheckOutputTarget(1920, 1080, PF_B8G8R8A8, 1920, 1080, PF_B8G8R8A8, 4),
		(int32)EOutputTargetStatus::SampleCountMismatch);
	TestEqual(TEXT("zero sample count rejected"),
		(int32)CheckOutputTarget(1920, 1080, PF_B8G8R8A8, 1920, 1080, PF_B8G8R8A8, 0),
		(int32)EOutputTargetStatus::SampleCountMismatch);

	// Size is reported ahead of format, so the actionable "a resize fixes this" case is named first
	// when both differ.
	TestEqual(TEXT("size takes precedence over format"),
		(int32)CheckOutputTarget(1920, 1080, PF_B8G8R8A8, 640, 480, PF_FloatRGBA, 1),
		(int32)EOutputTargetStatus::SizeMismatch);

	return true;
}

// =================================================================================================
// Input and external-metadata validation
// =================================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSpoutValidationNameTest,
	"SpoutPlugin.Validation.Names",
	SPOUT_TEST_FLAGS)

bool FSpoutValidationNameTest::RunTest(const FString& Parameters)
{
	using namespace SpoutValidation;

	// Rejections.
	TestEqual(TEXT("NAME_None rejected"), (int32)ValidateName(NAME_None), (int32)EResult::EmptyName);
	TestEqual(TEXT("empty name rejected"), (int32)ValidateName(FName(TEXT(""))), (int32)EResult::EmptyName);
	TestEqual(TEXT("whitespace-only name rejected"), (int32)ValidateName(FName(TEXT("   "))), (int32)EResult::EmptyName);

	// Acceptance, and the encoded buffer comes back null-terminated.
	{
		TArray<ANSICHAR> Encoded;
		TestEqual(TEXT("ordinary name accepted"), (int32)ValidateName(FName(TEXT("MySender")), &Encoded), (int32)EResult::Ok);
		TestEqual(TEXT("encoded length includes terminator"), Encoded.Num(), 9);
		TestEqual(TEXT("encoded buffer is terminated"), (int32)Encoded[8], 0);
		TestEqual(TEXT("encoded content round-trips"), FString(UTF8_TO_TCHAR(Encoded.GetData())), FString(TEXT("MySender")));
	}

	// Boundary: exactly the maximum, and one byte over, in single-byte characters.
	{
		TArray<ANSICHAR> Encoded;
		const FString AtLimit = FString::ChrN(MaxSenderNameUtf8Bytes, TEXT('a'));
		TestEqual(TEXT("name at the byte limit accepted"), (int32)ValidateName(FName(*AtLimit), &Encoded), (int32)EResult::Ok);
		TestEqual(TEXT("encoded at-limit length"), Encoded.Num(), MaxSenderNameUtf8Bytes + 1);

		const FString OverLimit = FString::ChrN(MaxSenderNameUtf8Bytes + 1, TEXT('a'));
		TestEqual(TEXT("name one byte over the limit rejected"), (int32)ValidateName(FName(*OverLimit), &Encoded), (int32)EResult::NameTooLong);
		// A rejected name must not leave a partially encoded buffer behind.
		TestEqual(TEXT("rejected name leaves the buffer empty"), Encoded.Num(), 0);
	}

	// Why UTF-8 bytes and not TCHAR count: a name well under any character limit can still overflow
	// Spout's fixed 256-byte field once encoded.
	{
		TArray<ANSICHAR> Encoded;
		// U+00E9 (e-acute) encodes to two UTF-8 bytes, so 200 of them is 400 bytes.
		const FString MultiByte = FString::ChrN(200, TCHAR(0x00E9));
		TestEqual(TEXT("200 two-byte characters is 400 bytes and is rejected"),
			(int32)ValidateName(FName(*MultiByte), &Encoded), (int32)EResult::NameTooLong);
		TestEqual(TEXT("rejected multi-byte name leaves the buffer empty"), Encoded.Num(), 0);

		// 127 two-byte characters is 254 bytes, which fits.
		const FString FitsMultiByte = FString::ChrN(127, TCHAR(0x00E9));
		TestEqual(TEXT("127 two-byte characters fits"),
			(int32)ValidateName(FName(*FitsMultiByte), &Encoded), (int32)EResult::Ok);
		TestEqual(TEXT("encoded multi-byte length is 254 plus terminator"), Encoded.Num(), 255);
		TestEqual(TEXT("encoded multi-byte buffer is terminated"), (int32)Encoded[254], 0);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSpoutValidationDimensionsTest,
	"SpoutPlugin.Validation.Dimensions",
	SPOUT_TEST_FLAGS)

bool FSpoutValidationDimensionsTest::RunTest(const FString& Parameters)
{
	using namespace SpoutValidation;

	// Valid.
	TestEqual(TEXT("1x1 accepted"), (int32)ValidateDimensions(1, 1), (int32)EResult::Ok);
	TestEqual(TEXT("1920x1080 accepted"), (int32)ValidateDimensions(1920, 1080), (int32)EResult::Ok);
	TestEqual(TEXT("max square accepted"), (int32)ValidateDimensions(MaxTextureDimension, MaxTextureDimension), (int32)EResult::Ok);

	// Zero and negative.
	TestEqual(TEXT("0x0 rejected"), (int32)ValidateDimensions(0, 0), (int32)EResult::InvalidDimensions);
	TestEqual(TEXT("zero width rejected"), (int32)ValidateDimensions(0, 1080), (int32)EResult::InvalidDimensions);
	TestEqual(TEXT("zero height rejected"), (int32)ValidateDimensions(1920, 0), (int32)EResult::InvalidDimensions);
	TestEqual(TEXT("negative width rejected"), (int32)ValidateDimensions(-1920, 1080), (int32)EResult::InvalidDimensions);
	TestEqual(TEXT("negative height rejected"), (int32)ValidateDimensions(1920, -1080), (int32)EResult::InvalidDimensions);
	TestEqual(TEXT("both negative rejected"), (int32)ValidateDimensions(-1, -1), (int32)EResult::InvalidDimensions);

	// Excessive.
	TestEqual(TEXT("width over the cap rejected"),
		(int32)ValidateDimensions(MaxTextureDimension + 1, 1080), (int32)EResult::DimensionsTooLarge);
	TestEqual(TEXT("height over the cap rejected"),
		(int32)ValidateDimensions(1920, MaxTextureDimension + 1), (int32)EResult::DimensionsTooLarge);

	// The sign-change case: INT32_MIN as a dimension. Widened to int64 it stays negative instead
	// of wrapping into a plausible unsigned extent.
	TestEqual(TEXT("INT32_MIN rejected as negative, not wrapped"),
		(int32)ValidateDimensions(static_cast<int64>(MIN_int32), 1080), (int32)EResult::InvalidDimensions);

	// An unsigned value arriving from another process. As uint32 this is 4294967295; widened to
	// int64 it stays large and is rejected rather than becoming -1.
	TestEqual(TEXT("uint32 max rejected as too large"),
		(int32)ValidateDimensions(static_cast<int64>(static_cast<uint32>(0xFFFFFFFFu)), 1080),
		(int32)EResult::DimensionsTooLarge);
	TestEqual(TEXT("INT32_MAX rejected as too large"),
		(int32)ValidateDimensions(static_cast<int64>(MAX_int32), static_cast<int64>(MAX_int32)),
		(int32)EResult::DimensionsTooLarge);

	// Overflow guard: 65536 * 65536 wraps to zero in 32-bit arithmetic. Both operands are caught by
	// the dimension cap before the multiply, and the int64 product check backs that up.
	TestEqual(TEXT("65536x65536 rejected"),
		(int32)ValidateDimensions(65536, 65536), (int32)EResult::DimensionsTooLarge);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSpoutValidationHandleFormatTest,
	"SpoutPlugin.Validation.HandlesFormatsAndRanges",
	SPOUT_TEST_FLAGS)

bool FSpoutValidationHandleFormatTest::RunTest(const FString& Parameters)
{
	using namespace SpoutValidation;

	// Handles.
	TestEqual(TEXT("null handle rejected"), (int32)ValidateSharedHandle(nullptr), (int32)EResult::InvalidHandle);
	TestEqual(TEXT("non-null handle accepted"), (int32)ValidateSharedHandle(SpoutPluginTests::HandleA), (int32)EResult::Ok);
	TestEqual(TEXT("INVALID_HANDLE_VALUE rejected"),
		(int32)ValidateSharedHandle(reinterpret_cast<const void*>(static_cast<intptr_t>(-1))),
		(int32)EResult::InvalidHandle);

	// Sample counts.
	TestEqual(TEXT("single sample accepted"), (int32)ValidateSampleCount(1), (int32)EResult::Ok);
	TestEqual(TEXT("zero samples rejected"), (int32)ValidateSampleCount(0), (int32)EResult::InvalidSampleCount);
	TestEqual(TEXT("MSAA 4x rejected"), (int32)ValidateSampleCount(4), (int32)EResult::InvalidSampleCount);
	TestEqual(TEXT("negative samples rejected"), (int32)ValidateSampleCount(-1), (int32)EResult::InvalidSampleCount);

	// Pixel formats.
	TestEqual(TEXT("PF_B8G8R8A8 accepted"), (int32)ValidatePixelFormat(PF_B8G8R8A8), (int32)EResult::Ok);
	TestEqual(TEXT("PF_FloatRGBA accepted"), (int32)ValidatePixelFormat(PF_FloatRGBA), (int32)EResult::Ok);
	TestEqual(TEXT("PF_Unknown rejected"), (int32)ValidatePixelFormat(PF_Unknown), (int32)EResult::UnsupportedFormat);
	TestEqual(TEXT("PF_DXT1 rejected"), (int32)ValidatePixelFormat(PF_DXT1), (int32)EResult::UnsupportedFormat);
	TestEqual(TEXT("PF_G8 rejected"), (int32)ValidatePixelFormat(PF_G8), (int32)EResult::UnsupportedFormat);

	// SetMaxSenders range.
	TestEqual(TEXT("minimum accepted"), (int32)ValidateMaxSenders(MinMaxSenders), (int32)EResult::Ok);
	TestEqual(TEXT("maximum accepted"), (int32)ValidateMaxSenders(MaxMaxSenders), (int32)EResult::Ok);
	TestEqual(TEXT("10 accepted"), (int32)ValidateMaxSenders(10), (int32)EResult::Ok);
	TestEqual(TEXT("zero rejected"), (int32)ValidateMaxSenders(0), (int32)EResult::OutOfRange);
	TestEqual(TEXT("negative rejected"), (int32)ValidateMaxSenders(-5), (int32)EResult::OutOfRange);
	TestEqual(TEXT("above maximum rejected"), (int32)ValidateMaxSenders(MaxMaxSenders + 1), (int32)EResult::OutOfRange);
	TestEqual(TEXT("INT32_MAX rejected"), (int32)ValidateMaxSenders(MAX_int32), (int32)EResult::OutOfRange);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSpoutValidationExternalMetadataTest,
	"SpoutPlugin.Validation.ExternalSenderMetadata",
	SPOUT_TEST_FLAGS)

bool FSpoutValidationExternalMetadataTest::RunTest(const FString& Parameters)
{
	using namespace SpoutValidation;

	const uint32 GoodFormat = static_cast<uint32>(DXGI_FORMAT_B8G8R8A8_UNORM);

	TestEqual(TEXT("valid metadata accepted"),
		(int32)ValidateExternalSenderMetadata(1920, 1080, SpoutPluginTests::HandleA, GoodFormat),
		(int32)EResult::Ok);

	// Each field rejected independently, before any resource is opened.
	TestEqual(TEXT("zero extent rejected"),
		(int32)ValidateExternalSenderMetadata(0, 0, SpoutPluginTests::HandleA, GoodFormat),
		(int32)EResult::InvalidDimensions);
	TestEqual(TEXT("excessive extent rejected"),
		(int32)ValidateExternalSenderMetadata(999999, 999999, SpoutPluginTests::HandleA, GoodFormat),
		(int32)EResult::DimensionsTooLarge);
	TestEqual(TEXT("null handle rejected"),
		(int32)ValidateExternalSenderMetadata(1920, 1080, nullptr, GoodFormat),
		(int32)EResult::InvalidHandle);
	TestEqual(TEXT("unknown format rejected"),
		(int32)ValidateExternalSenderMetadata(1920, 1080, SpoutPluginTests::HandleA, static_cast<uint32>(DXGI_FORMAT_UNKNOWN)),
		(int32)EResult::UnsupportedFormat);
	TestEqual(TEXT("compressed format rejected"),
		(int32)ValidateExternalSenderMetadata(1920, 1080, SpoutPluginTests::HandleA, static_cast<uint32>(DXGI_FORMAT_BC7_UNORM)),
		(int32)EResult::UnsupportedFormat);

	// Typeless metadata is accepted, because the policy maps it deliberately.
	TestEqual(TEXT("typeless format accepted"),
		(int32)ValidateExternalSenderMetadata(1920, 1080, SpoutPluginTests::HandleA, static_cast<uint32>(DXGI_FORMAT_B8G8R8A8_TYPELESS)),
		(int32)EResult::Ok);

	return true;
}

// =================================================================================================
// Sender registration state machine
// =================================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSpoutRegistrationSuccessTest,
	"SpoutPlugin.Registration.InitialRegistrationSucceeds",
	SPOUT_TEST_FLAGS)

bool FSpoutRegistrationSuccessTest::RunTest(const FString& Parameters)
{
	using namespace SpoutPluginTests;

	FFakeSenderNameBackend Backend;
	FSpoutSenderRegistration Registration;
	const TArray<ANSICHAR> Name = Utf8(TEXT("SenderA"));

	TestEqual(TEXT("starts Unregistered"), (int32)Registration.GetState(), (int32)ESpoutRegistrationState::Unregistered);
	TestFalse(TEXT("starts not owning the name"), Registration.OwnsName());
	TestFalse(TEXT("starts inactive"), Registration.IsActive());

	TestTrue(TEXT("publish succeeds"),
		Registration.Publish(Backend, Name.GetData(), 1920, 1080, HandleA, 87));

	TestEqual(TEXT("becomes Active"), (int32)Registration.GetState(), (int32)ESpoutRegistrationState::Active);
	TestTrue(TEXT("is active"), Registration.IsActive());
	TestTrue(TEXT("owns the name"), Registration.OwnsName());
	TestEqual(TEXT("no failures recorded"), Registration.GetConsecutiveFailureCount(), 0);

	TestEqual(TEXT("CreateSender called once"), Backend.CreateCalls, 1);
	TestEqual(TEXT("UpdateSender not called"), Backend.UpdateCalls, 0);
	TestEqual(TEXT("ReleaseSenderName not called"), Backend.ReleaseCalls, 0);
	TestEqual(TEXT("advertised name"), Backend.LastName, FString(TEXT("SenderA")));
	TestEqual(TEXT("advertised width"), (int32)Backend.LastWidth, 1920);
	TestEqual(TEXT("advertised height"), (int32)Backend.LastHeight, 1080);

	// Republishing identical values must not call the SDK again: a steady-state frame is a
	// comparison and nothing more.
	TestTrue(TEXT("idempotent republish succeeds"),
		Registration.Publish(Backend, Name.GetData(), 1920, 1080, HandleA, 87));
	TestEqual(TEXT("CreateSender still called once"), Backend.CreateCalls, 1);
	TestEqual(TEXT("UpdateSender still not called"), Backend.UpdateCalls, 0);
	TestTrue(TEXT("still active"), Registration.IsActive());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSpoutRegistrationFailureAndRetryTest,
	"SpoutPlugin.Registration.InitialFailureIsRetryable",
	SPOUT_TEST_FLAGS)

bool FSpoutRegistrationFailureAndRetryTest::RunTest(const FString& Parameters)
{
	using namespace SpoutPluginTests;

	FFakeSenderNameBackend Backend;
	Backend.bFailCreate = true;

	FSpoutSenderRegistration Registration;
	const TArray<ANSICHAR> Name = Utf8(TEXT("SenderB"));

	// Frame 1: registration fails.
	TestFalse(TEXT("failed publish returns false"),
		Registration.Publish(Backend, Name.GetData(), 1920, 1080, HandleA, 87));
	TestEqual(TEXT("state is Failed"), (int32)Registration.GetState(), (int32)ESpoutRegistrationState::Failed);
	TestFalse(TEXT("not active"), Registration.IsActive());
	// The core invariant: a failed registration never owns the name, so nothing stale is advertised
	// and no foreign name can be released later.
	TestFalse(TEXT("does not own the name after failure"), Registration.OwnsName());
	TestEqual(TEXT("one failure recorded"), Registration.GetConsecutiveFailureCount(), 1);
	TestEqual(TEXT("no release attempted for an unowned name"), Backend.ReleaseCalls, 0);

	// Frame 2: still failing. A failed registration is retried on every publish, even when the
	// texture size has not changed.
	TestFalse(TEXT("second attempt also fails"),
		Registration.Publish(Backend, Name.GetData(), 1920, 1080, HandleA, 87));
	TestEqual(TEXT("CreateSender retried"), Backend.CreateCalls, 2);
	TestEqual(TEXT("two failures recorded"), Registration.GetConsecutiveFailureCount(), 2);

	// Frame 3: the underlying cause clears and the retry succeeds.
	Backend.bFailCreate = false;
	TestTrue(TEXT("retry succeeds once the cause clears"),
		Registration.Publish(Backend, Name.GetData(), 1920, 1080, HandleA, 87));
	TestEqual(TEXT("CreateSender called a third time"), Backend.CreateCalls, 3);
	TestEqual(TEXT("becomes Active"), (int32)Registration.GetState(), (int32)ESpoutRegistrationState::Active);
	TestTrue(TEXT("owns the name after recovery"), Registration.OwnsName());
	TestEqual(TEXT("failure count reset"), Registration.GetConsecutiveFailureCount(), 0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSpoutRegistrationResizeTest,
	"SpoutPlugin.Registration.ResizeUpdatesTheAdvertisement",
	SPOUT_TEST_FLAGS)

bool FSpoutRegistrationResizeTest::RunTest(const FString& Parameters)
{
	using namespace SpoutPluginTests;

	FFakeSenderNameBackend Backend;
	FSpoutSenderRegistration Registration;
	const TArray<ANSICHAR> Name = Utf8(TEXT("SenderC"));

	TestTrue(TEXT("initial publish"), Registration.Publish(Backend, Name.GetData(), 1920, 1080, HandleA, 87));

	// A resize replaces the shared texture, so both the extent and the handle change, and the
	// advertisement must follow them rather than go stale.
	TestTrue(TEXT("resize publish succeeds"),
		Registration.Publish(Backend, Name.GetData(), 3840, 2160, HandleB, 87));
	TestEqual(TEXT("UpdateSender called"), Backend.UpdateCalls, 1);
	TestEqual(TEXT("CreateSender not called again"), Backend.CreateCalls, 1);
	TestTrue(TEXT("still active after resize"), Registration.IsActive());
	TestEqual(TEXT("advertised new width"), (int32)Backend.LastWidth, 3840);
	TestEqual(TEXT("advertised new height"), (int32)Backend.LastHeight, 2160);
	TestTrue(TEXT("advertised new handle"), Backend.LastHandle == HandleB);

	// A handle change at the same extent must also republish: it means the shared texture was
	// recreated, so the old handle refers to a dropped resource.
	TestTrue(TEXT("handle-only change republishes"),
		Registration.Publish(Backend, Name.GetData(), 3840, 2160, HandleA, 87));
	TestEqual(TEXT("UpdateSender called again"), Backend.UpdateCalls, 2);

	// A format change at the same extent and handle must republish too.
	TestTrue(TEXT("format-only change republishes"),
		Registration.Publish(Backend, Name.GetData(), 3840, 2160, HandleA, 10));
	TestEqual(TEXT("UpdateSender called a third time"), Backend.UpdateCalls, 3);
	TestEqual(TEXT("advertised new format"), (int32)Backend.LastFormat, 10);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSpoutRegistrationUpdateFailureTest,
	"SpoutPlugin.Registration.UpdateFailureRollsBackAndRecreates",
	SPOUT_TEST_FLAGS)

bool FSpoutRegistrationUpdateFailureTest::RunTest(const FString& Parameters)
{
	using namespace SpoutPluginTests;

	FFakeSenderNameBackend Backend;
	FSpoutSenderRegistration Registration;
	const TArray<ANSICHAR> Name = Utf8(TEXT("SenderD"));

	TestTrue(TEXT("initial publish"), Registration.Publish(Backend, Name.GetData(), 1920, 1080, HandleA, 87));
	TestTrue(TEXT("name is in the sender list"), Backend.RegisteredNames.Contains(TEXT("SenderD")));

	// UpdateSender fails during a resize. Its result must not be discarded: Spout would keep
	// advertising a handle whose texture had already been dropped.
	Backend.bFailUpdate = true;
	TestFalse(TEXT("failed update returns false"),
		Registration.Publish(Backend, Name.GetData(), 3840, 2160, HandleB, 87));

	TestEqual(TEXT("state is Failed"), (int32)Registration.GetState(), (int32)ESpoutRegistrationState::Failed);
	TestFalse(TEXT("not active"), Registration.IsActive());
	// The rollback: the name is released rather than left pointing at a dead handle.
	TestFalse(TEXT("ownership dropped"), Registration.OwnsName());
	TestEqual(TEXT("name released exactly once"), Backend.ReleaseCalls, 1);
	TestFalse(TEXT("name removed from the sender list"), Backend.RegisteredNames.Contains(TEXT("SenderD")));

	// Recovery takes the clean CreateSender path, not another Update.
	Backend.bFailUpdate = false;
	const int32 UpdateCallsBefore = Backend.UpdateCalls;
	TestTrue(TEXT("recovery publish succeeds"),
		Registration.Publish(Backend, Name.GetData(), 3840, 2160, HandleB, 87));
	TestEqual(TEXT("recovered via CreateSender"), Backend.CreateCalls, 2);
	TestEqual(TEXT("no further UpdateSender attempt"), Backend.UpdateCalls, UpdateCallsBefore);
	TestTrue(TEXT("active again"), Registration.IsActive());
	TestTrue(TEXT("name back in the sender list"), Backend.RegisteredNames.Contains(TEXT("SenderD")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSpoutRegistrationInvalidateTest,
	"SpoutPlugin.Registration.InvalidateReleasesAndStaysRetryable",
	SPOUT_TEST_FLAGS)

bool FSpoutRegistrationInvalidateTest::RunTest(const FString& Parameters)
{
	using namespace SpoutPluginTests;

	// Invalidate models "the resources behind the registration are gone" - a failed shared-texture
	// recreate. The advertisement must go with them.
	FFakeSenderNameBackend Backend;
	FSpoutSenderRegistration Registration;
	const TArray<ANSICHAR> Name = Utf8(TEXT("SenderE"));

	TestTrue(TEXT("initial publish"), Registration.Publish(Backend, Name.GetData(), 1920, 1080, HandleA, 87));

	Registration.Invalidate(Backend);
	TestEqual(TEXT("state is Failed"), (int32)Registration.GetState(), (int32)ESpoutRegistrationState::Failed);
	TestFalse(TEXT("ownership dropped"), Registration.OwnsName());
	TestEqual(TEXT("owned name released"), Backend.ReleaseCalls, 1);
	TestFalse(TEXT("removed from the sender list"), Backend.RegisteredNames.Contains(TEXT("SenderE")));
	TestEqual(TEXT("failure counted"), Registration.GetConsecutiveFailureCount(), 1);

	// Invalidating something never registered must not call the SDK at all.
	FFakeSenderNameBackend FreshBackend;
	FSpoutSenderRegistration FreshRegistration;
	FreshRegistration.Invalidate(FreshBackend);
	TestEqual(TEXT("unowned name is not released"), FreshBackend.ReleaseCalls, 0);
	TestEqual(TEXT("fresh state is Failed"), (int32)FreshRegistration.GetState(), (int32)ESpoutRegistrationState::Failed);

	// Still retryable afterwards.
	TestTrue(TEXT("publish after invalidate succeeds"),
		Registration.Publish(Backend, Name.GetData(), 1920, 1080, HandleA, 87));
	TestTrue(TEXT("active again"), Registration.IsActive());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSpoutRegistrationReleaseOwnershipTest,
	"SpoutPlugin.Registration.ReleaseOnlyWhenOwned",
	SPOUT_TEST_FLAGS)

bool FSpoutRegistrationReleaseOwnershipTest::RunTest(const FString& Parameters)
{
	using namespace SpoutPluginTests;

	const TArray<ANSICHAR> Name = Utf8(TEXT("SenderF"));

	// A registered sender releases its name exactly once.
	{
		FFakeSenderNameBackend Backend;
		FSpoutSenderRegistration Registration;
		TestTrue(TEXT("publish"), Registration.Publish(Backend, Name.GetData(), 1920, 1080, HandleA, 87));

		Registration.Release(Backend);
		TestEqual(TEXT("state is Released"), (int32)Registration.GetState(), (int32)ESpoutRegistrationState::Released);
		TestFalse(TEXT("no longer owns the name"), Registration.OwnsName());
		TestEqual(TEXT("released once"), Backend.ReleaseCalls, 1);
		TestFalse(TEXT("removed from the sender list"), Backend.RegisteredNames.Contains(TEXT("SenderF")));

		// A second Release must not touch the SDK again.
		Registration.Release(Backend);
		TestEqual(TEXT("still released only once"), Backend.ReleaseCalls, 1);
	}

	// A sender that never registered must NOT release the name. Releasing unconditionally on
	// destruction could remove a name owned by another process.
	{
		FFakeSenderNameBackend Backend;
		FSpoutSenderRegistration Registration;

		Registration.Release(Backend);
		TestEqual(TEXT("never-registered name is not released"), Backend.ReleaseCalls, 0);
		TestEqual(TEXT("state is Released"), (int32)Registration.GetState(), (int32)ESpoutRegistrationState::Released);
	}

	// A sender whose registration failed must not release the name either.
	{
		FFakeSenderNameBackend Backend;
		Backend.bFailCreate = true;
		FSpoutSenderRegistration Registration;
		TestFalse(TEXT("publish fails"), Registration.Publish(Backend, Name.GetData(), 1920, 1080, HandleA, 87));

		Registration.Release(Backend);
		TestEqual(TEXT("failed registration does not release"), Backend.ReleaseCalls, 0);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSpoutRegistrationRestartTest,
	"SpoutPlugin.Registration.RestartAfterRelease",
	SPOUT_TEST_FLAGS)

bool FSpoutRegistrationRestartTest::RunTest(const FString& Parameters)
{
	using namespace SpoutPluginTests;

	FFakeSenderNameBackend Backend;
	FSpoutSenderRegistration Registration;
	const TArray<ANSICHAR> Name = Utf8(TEXT("SenderG"));

	TestTrue(TEXT("publish"), Registration.Publish(Backend, Name.GetData(), 1920, 1080, HandleA, 87));
	Registration.Release(Backend);
	TestEqual(TEXT("state is Released"), (int32)Registration.GetState(), (int32)ESpoutRegistrationState::Released);

	// Restarting from Released takes the CreateSender path, because ownership was dropped.
	TestTrue(TEXT("restart succeeds"), Registration.Publish(Backend, Name.GetData(), 1280, 720, HandleB, 87));
	TestEqual(TEXT("CreateSender used for the restart"), Backend.CreateCalls, 2);
	TestEqual(TEXT("UpdateSender not used"), Backend.UpdateCalls, 0);
	TestTrue(TEXT("active"), Registration.IsActive());
	TestTrue(TEXT("name back in the sender list"), Backend.RegisteredNames.Contains(TEXT("SenderG")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSpoutRegistrationGuardTest,
	"SpoutPlugin.Registration.RejectsUnusableArguments",
	SPOUT_TEST_FLAGS)

bool FSpoutRegistrationGuardTest::RunTest(const FString& Parameters)
{
	using namespace SpoutPluginTests;

	const TArray<ANSICHAR> Name = Utf8(TEXT("SenderH"));

	// An empty name, a null handle, or a zero extent must never reach the SDK.
	{
		FFakeSenderNameBackend Backend;
		FSpoutSenderRegistration Registration;
		TestFalse(TEXT("null name rejected"), Registration.Publish(Backend, nullptr, 1920, 1080, HandleA, 87));
		TestEqual(TEXT("no SDK call for a null name"), Backend.CreateCalls, 0);
	}
	{
		FFakeSenderNameBackend Backend;
		FSpoutSenderRegistration Registration;
		TestFalse(TEXT("empty name rejected"), Registration.Publish(Backend, "", 1920, 1080, HandleA, 87));
		TestEqual(TEXT("no SDK call for an empty name"), Backend.CreateCalls, 0);
	}
	{
		FFakeSenderNameBackend Backend;
		FSpoutSenderRegistration Registration;
		TestFalse(TEXT("null handle rejected"), Registration.Publish(Backend, Name.GetData(), 1920, 1080, nullptr, 87));
		TestEqual(TEXT("no SDK call for a null handle"), Backend.CreateCalls, 0);
	}
	{
		FFakeSenderNameBackend Backend;
		FSpoutSenderRegistration Registration;
		TestFalse(TEXT("zero extent rejected"), Registration.Publish(Backend, Name.GetData(), 0, 1080, HandleA, 87));
		TestEqual(TEXT("no SDK call for a zero extent"), Backend.CreateCalls, 0);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSpoutRegistrationRejectedArgumentsReleaseTest,
	"SpoutPlugin.Registration.RejectedArgumentsStillReleaseAnOwnedName",
	SPOUT_TEST_FLAGS)

bool FSpoutRegistrationRejectedArgumentsReleaseTest::RunTest(const FString& Parameters)
{
	using namespace SpoutPluginTests;

	// The class invariant is "Failed implies the name is not owned". The argument-rejection guard
	// is the one path that can be reached while a registration is already live -- a shared texture
	// that fails to recreate can arrive here with a null handle -- so it has to give the name back
	// like every other route into Failed. Without that release the entry sits in Failed while Spout
	// still advertises it, and the next Publish takes the create path for a name it already owns.
	const TArray<ANSICHAR> Name = Utf8(TEXT("SenderI"));

	struct FCase
	{
		const TCHAR* What;
		const ANSICHAR* NameArg;
		uint32 Width;
		uint32 Height;
		void* Handle;
	};

	const FCase Cases[] =
	{
		{ TEXT("null handle"),  Name.GetData(), 1920, 1080, nullptr },
		{ TEXT("zero width"),   Name.GetData(),    0, 1080, HandleA },
		{ TEXT("zero height"),  Name.GetData(), 1920,    0, HandleA },
		{ TEXT("null name"),    nullptr,        1920, 1080, HandleA },
		{ TEXT("empty name"),   "",             1920, 1080, HandleA },
	};

	for (const FCase& Case : Cases)
	{
		FFakeSenderNameBackend Backend;
		FSpoutSenderRegistration Registration;

		TestTrue(*FString::Printf(TEXT("%s: initial publish succeeds"), Case.What),
			Registration.Publish(Backend, Name.GetData(), 1920, 1080, HandleA, 87));
		TestTrue(*FString::Printf(TEXT("%s: name is owned before the bad call"), Case.What), Registration.OwnsName());
		TestTrue(*FString::Printf(TEXT("%s: name is in the sender list"), Case.What),
			Backend.RegisteredNames.Contains(TEXT("SenderI")));

		TestFalse(*FString::Printf(TEXT("%s: rejected publish returns false"), Case.What),
			Registration.Publish(Backend, Case.NameArg, Case.Width, Case.Height, Case.Handle, 87));

		TestEqual(*FString::Printf(TEXT("%s: state is Failed"), Case.What),
			(int32)Registration.GetState(), (int32)ESpoutRegistrationState::Failed);
		// The discriminating assertions: ownership must not survive a rejected argument set, and the
		// name must not stay advertised.
		TestFalse(*FString::Printf(TEXT("%s: ownership was dropped"), Case.What), Registration.OwnsName());
		TestEqual(*FString::Printf(TEXT("%s: the owned name was released exactly once"), Case.What),
			Backend.ReleaseCalls, 1);
		TestFalse(*FString::Printf(TEXT("%s: name removed from the sender list"), Case.What),
			Backend.RegisteredNames.Contains(TEXT("SenderI")));

		// Still retryable, and the retry takes the create path because ownership is gone.
		const int32 CreateCallsBefore = Backend.CreateCalls;
		TestTrue(*FString::Printf(TEXT("%s: recovery publish succeeds"), Case.What),
			Registration.Publish(Backend, Name.GetData(), 1920, 1080, HandleA, 87));
		TestEqual(*FString::Printf(TEXT("%s: recovery used CreateSender"), Case.What),
			Backend.CreateCalls, CreateCallsBefore + 1);
		TestTrue(*FString::Printf(TEXT("%s: active again"), Case.What), Registration.IsActive());
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSpoutRegistrationOwnsItsIdentityTest,
	"SpoutPlugin.Registration.ReleasesTheNameItRegistered",
	SPOUT_TEST_FLAGS)

bool FSpoutRegistrationOwnsItsIdentityTest::RunTest(const FString& Parameters)
{
	using namespace SpoutPluginTests;

	const TArray<ANSICHAR> NameOne = Utf8(TEXT("SenderJ"));
	const TArray<ANSICHAR> NameTwo = Utf8(TEXT("SenderK"));

	// Release identity comes from what CreateSender accepted, not from whatever the caller happens
	// to hold at teardown time. A caller that has lost or corrupted its own copy of the name must
	// still be able to give the registration back.
	{
		FFakeSenderNameBackend Backend;
		FSpoutSenderRegistration Registration;

		TestTrue(TEXT("publish"), Registration.Publish(Backend, NameOne.GetData(), 1920, 1080, HandleA, 87));
		TestEqual(TEXT("owned name matches what was registered"),
			FString(UTF8_TO_TCHAR(Registration.GetOwnedName())), FString(TEXT("SenderJ")));

		// Release takes no name at all, so there is nothing for a caller to get wrong.
		Registration.Release(Backend);
		TestEqual(TEXT("released once"), Backend.ReleaseCalls, 1);
		TestEqual(TEXT("released the registered name"), Backend.LastReleasedName, FString(TEXT("SenderJ")));
		TestFalse(TEXT("no longer owned"), Registration.OwnsName());
		TestEqual(TEXT("owned name cleared"), FString(UTF8_TO_TCHAR(Registration.GetOwnedName())), FString(TEXT("")));
	}

	// Publishing a different name onto an owned registration gives the first name back rather than
	// leaving two identities advertised.
	{
		FFakeSenderNameBackend Backend;
		FSpoutSenderRegistration Registration;

		TestTrue(TEXT("publish first name"), Registration.Publish(Backend, NameOne.GetData(), 1920, 1080, HandleA, 87));
		TestTrue(TEXT("publish second name"), Registration.Publish(Backend, NameTwo.GetData(), 1920, 1080, HandleA, 87));

		TestEqual(TEXT("first name was released"), Backend.ReleaseCalls, 1);
		TestEqual(TEXT("the released name was the first one"), Backend.LastReleasedName, FString(TEXT("SenderJ")));
		TestFalse(TEXT("first name is no longer advertised"), Backend.RegisteredNames.Contains(TEXT("SenderJ")));
		TestTrue(TEXT("second name is advertised"), Backend.RegisteredNames.Contains(TEXT("SenderK")));
		TestEqual(TEXT("owned name is now the second"),
			FString(UTF8_TO_TCHAR(Registration.GetOwnedName())), FString(TEXT("SenderK")));
		TestTrue(TEXT("active on the new name"), Registration.IsActive());
	}

	// An update after a resize must republish under the owned name.
	{
		FFakeSenderNameBackend Backend;
		FSpoutSenderRegistration Registration;

		TestTrue(TEXT("publish"), Registration.Publish(Backend, NameOne.GetData(), 1920, 1080, HandleA, 87));
		TestTrue(TEXT("resize"), Registration.Publish(Backend, NameOne.GetData(), 3840, 2160, HandleB, 87));
		TestEqual(TEXT("UpdateSender used the owned name"), Backend.LastName, FString(TEXT("SenderJ")));
		TestEqual(TEXT("no release during a normal resize"), Backend.ReleaseCalls, 0);
	}

	return true;
}

// =================================================================================================
// Deterministic Blueprint outputs
//
// These call the real Blueprint entry points. They only assert the properties that must hold
// regardless of whether a Spout context could be created in this environment, which is the whole
// point: outputs must be deterministic on the failure path rather than left holding the caller's
// previous values.
//
// Nothing here mutates machine-global Spout state.
// =================================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSpoutBlueprintOutputResetTest,
	"SpoutPlugin.BlueprintOutputs.NoStaleDataOnFailure",
	SPOUT_TEST_FLAGS)

bool FSpoutBlueprintOutputResetTest::RunTest(const FString& Parameters)
{
	// SpoutInfo: pre-existing entries must be gone whether or not enumeration succeeds.
	{
		TArray<FSenderStruct> Senders;
		FSenderStruct Stale;
		Stale.Name = TEXT("SpoutPluginTests_StaleSender");
		Stale.Width = 4096;
		Stale.Height = 4096;
		Stale.bIsAlive = true;
		Senders.Add(Stale);
		Senders.Add(Stale);

		const bool bResult = USpoutBPFunctionLibrary::SpoutInfo(Senders);

		for (const FSenderStruct& Entry : Senders)
		{
			TestTrue(TEXT("no stale entry survives enumeration"),
				Entry.Name != FName(TEXT("SpoutPluginTests_StaleSender")));
		}

		if (!bResult)
		{
			TestEqual(TEXT("failed enumeration yields an empty array"), Senders.Num(), 0);
		}
	}

	// GetSenderInfo: the struct must be reset before the lookup can fail.
	{
		FSenderStruct Info;
		Info.Name = TEXT("SpoutPluginTests_StaleSender");
		Info.Width = 1234;
		Info.Height = 5678;
		Info.bIsAlive = true;

		// A name that cannot plausibly exist, so this fails wherever it runs.
		const bool bResult = USpoutBPFunctionLibrary::GetSenderInfo(
			FName(TEXT("SpoutPluginTests_NoSuchSender_9f3a1c")), Info);

		TestFalse(TEXT("lookup of a nonexistent sender fails"), bResult);
		TestTrue(TEXT("stale name cleared"), Info.Name.IsNone());
		TestEqual(TEXT("stale width cleared"), Info.Width, 0);
		TestEqual(TEXT("stale height cleared"), Info.Height, 0);
		TestFalse(TEXT("stale alive flag cleared"), Info.bIsAlive);
		TestNull(TEXT("texture reference cleared"), Info.TextureColor);
		TestNull(TEXT("material reference cleared"), Info.MaterialInstanceColor);
	}

	// GetSenderInfo with an invalid name: rejected before any Spout call, and still deterministic.
	{
		FSenderStruct Info;
		Info.Name = TEXT("SpoutPluginTests_StaleSender");
		Info.Width = 999;

		TestFalse(TEXT("NAME_None lookup fails"), USpoutBPFunctionLibrary::GetSenderInfo(NAME_None, Info));
		TestTrue(TEXT("output reset for an invalid name"), Info.Name.IsNone());
		TestEqual(TEXT("width reset for an invalid name"), Info.Width, 0);
	}

	// GetMaxSenders: the caller's value must not survive, whether the read succeeded or not.
	{
		const int32 Sentinel = 12345;
		int32 Max = Sentinel;
		USpoutBPFunctionLibrary::GetMaxSenders(Max);
		// Either Spout answered with a real limit, or the value was reset to 0. The one thing it must
		// never be is the caller's previous value. The sentinel is outside the accepted range, so a
		// genuine limit can never coincide with it.
		TestTrue(TEXT("caller's previous value does not survive"), Max != Sentinel);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSpoutSetMaxSendersRejectionTest,
	"SpoutPlugin.BlueprintOutputs.SetMaxSendersDoesNotEchoRejectedValues",
	SPOUT_TEST_FLAGS)

bool FSpoutSetMaxSendersRejectionTest::RunTest(const FString& Parameters)
{
	// Out-of-range arguments are rejected before any Spout call and must not be echoed back.
	// Echoing would return the requested number regardless of what actually happened.
	//
	// Only rejection paths are exercised. An accepted value would write a persistent machine-wide
	// sender limit, which a test must not do.
	TestEqual(TEXT("zero is not echoed back"), USpoutBPFunctionLibrary::SetMaxSenders(0), 0);
	TestEqual(TEXT("negative is not echoed back"), USpoutBPFunctionLibrary::SetMaxSenders(-7), 0);
	TestEqual(TEXT("absurdly large is not echoed back"), USpoutBPFunctionLibrary::SetMaxSenders(100000), 0);
	TestEqual(TEXT("INT32_MAX is not echoed back"), USpoutBPFunctionLibrary::SetMaxSenders(MAX_int32), 0);
	TestEqual(TEXT("INT32_MIN is not echoed back"), USpoutBPFunctionLibrary::SetMaxSenders(MIN_int32), 0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSpoutRenderTargetFactoryValidationTest,
	"SpoutPlugin.BlueprintOutputs.RenderTargetFactoryRejectsInvalidInput",
	SPOUT_TEST_FLAGS)

bool FSpoutRenderTargetFactoryValidationTest::RunTest(const FString& Parameters)
{
	using namespace SpoutValidation;

	// Every rejection below returns before NewObject and before InitCustomFormat, so these are safe
	// without a render device.
	TestNull(TEXT("zero size rejected"),
		USpoutBPFunctionLibrary::CreateTextureRenderTarget2D(0, 0, PF_B8G8R8A8, true));
	TestNull(TEXT("negative width rejected"),
		USpoutBPFunctionLibrary::CreateTextureRenderTarget2D(-1024, 768, PF_B8G8R8A8, true));
	TestNull(TEXT("negative height rejected"),
		USpoutBPFunctionLibrary::CreateTextureRenderTarget2D(1024, -768, PF_B8G8R8A8, true));
	TestNull(TEXT("INT32_MIN rejected"),
		USpoutBPFunctionLibrary::CreateTextureRenderTarget2D(MIN_int32, 768, PF_B8G8R8A8, true));
	TestNull(TEXT("excessive width rejected"),
		USpoutBPFunctionLibrary::CreateTextureRenderTarget2D(MaxTextureDimension + 1, 768, PF_B8G8R8A8, true));
	TestNull(TEXT("INT32_MAX rejected"),
		USpoutBPFunctionLibrary::CreateTextureRenderTarget2D(MAX_int32, MAX_int32, PF_B8G8R8A8, true));
	TestNull(TEXT("unsupported compressed format rejected"),
		USpoutBPFunctionLibrary::CreateTextureRenderTarget2D(1024, 768, PF_DXT1, true));
	TestNull(TEXT("PF_Unknown rejected"),
		USpoutBPFunctionLibrary::CreateTextureRenderTarget2D(1024, 768, PF_Unknown, true));
	TestNull(TEXT("PF_G8 rejected"),
		USpoutBPFunctionLibrary::CreateTextureRenderTarget2D(1024, 768, PF_G8, true));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSpoutSendReceiveNameRejectionTest,
	"SpoutPlugin.BlueprintOutputs.SendAndReceiveRejectInvalidNames",
	SPOUT_TEST_FLAGS)

bool FSpoutSendReceiveNameRejectionTest::RunTest(const FString& Parameters)
{
	using namespace SpoutValidation;

	const FString OverLongName = FString::ChrN(MaxSenderNameUtf8Bytes + 10, TEXT('x'));

	// Sender: rejected on the game thread, before the interop context is touched and before any
	// render command is queued.
	TestFalse(TEXT("NAME_None send rejected"),
		USpoutBPFunctionLibrary::SpoutSender(NAME_None, ESpoutSendTextureFrom::TextureRenderTarget2D, nullptr));
	TestFalse(TEXT("empty-name send rejected"),
		USpoutBPFunctionLibrary::SpoutSender(FName(TEXT("")), ESpoutSendTextureFrom::TextureRenderTarget2D, nullptr));
	TestFalse(TEXT("overlong-name send rejected"),
		USpoutBPFunctionLibrary::SpoutSender(FName(*OverLongName), ESpoutSendTextureFrom::TextureRenderTarget2D, nullptr));

	// Receiver: same, and the in/out ownership parameters are left exactly as passed in. Clearing
	// them on failure would orphan a texture the caller is still responsible for closing.
	{
		UMaterialInstanceDynamic* Mat = nullptr;
		UTexture2D* Texture = nullptr;

		TestFalse(TEXT("NAME_None receive rejected"),
			USpoutBPFunctionLibrary::SpoutReceiver(NAME_None, nullptr, NAME_None, Mat, Texture, nullptr));
		TestNull(TEXT("material untouched"), Mat);
		TestNull(TEXT("texture untouched"), Texture);

		TestFalse(TEXT("overlong-name receive rejected"),
			USpoutBPFunctionLibrary::SpoutReceiver(FName(*OverLongName), nullptr, NAME_None, Mat, Texture, nullptr));
		TestNull(TEXT("material still untouched"), Mat);
		TestNull(TEXT("texture still untouched"), Texture);
	}

	return true;
}

// =================================================================================================
// Cancellation and generation validation
//
// Every test below drives a LOCAL FSpoutSession rather than FSpoutSession::Get(), except where an
// integration point is explicitly the thing under test. Driving the process-wide session would mean
// a test could stop a real stream running in the same editor.
// =================================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSpoutSessionCancelTest,
	"SpoutPlugin.Session.ClosingAStreamStalesItsQueuedWork",
	SPOUT_TEST_FLAGS)

bool FSpoutSessionCancelTest::RunTest(const FString& Parameters)
{
	FSpoutSession Session;

	const FName StreamA(TEXT("SessionA"));
	const FName StreamB(TEXT("SessionB"));

	// The scenario: work is queued, the stream is closed before the work runs, and the work must
	// then do nothing. A queued command holding only a name would happily create the registry
	// entry and publish the sender after Close had returned.
	const FSpoutStreamToken Queued = Session.OpenStream(StreamA, ESpoutType::Sender);
	TestTrue(TEXT("a token was issued"), Queued.IsSet());
	TestTrue(TEXT("queued work is live before the close"), Session.IsCurrent(Queued));

	Session.CancelStream(StreamA, ESpoutType::Sender);
	TestFalse(TEXT("queued work is dead after the close"), Session.IsCurrent(Queued));

	// Closing is not permanent: reopening the same name works, and only the new token is live.
	const FSpoutStreamToken Reopened = Session.OpenStream(StreamA, ESpoutType::Sender);
	TestTrue(TEXT("the same name can be reopened"), Session.IsCurrent(Reopened));
	TestFalse(TEXT("the token from before the close stays dead"), Session.IsCurrent(Queued));

	// Cancellation is per stream. A second sender is untouched.
	const FSpoutStreamToken OtherSender = Session.OpenStream(StreamB, ESpoutType::Sender);
	Session.CancelStream(StreamA, ESpoutType::Sender);
	TestFalse(TEXT("the closed sender is dead"), Session.IsCurrent(Reopened));
	TestTrue(TEXT("an unrelated sender is unaffected"), Session.IsCurrent(OtherSender));

	// ... and per role, matching the registry's (Name, Type) keying. Closing a sender must not
	// cancel a receiver that happens to use the same FName.
	const FSpoutStreamToken SameNameReceiver = Session.OpenStream(StreamB, ESpoutType::Receiver);
	Session.CancelStream(StreamB, ESpoutType::Sender);
	TestFalse(TEXT("the sender was cancelled"), Session.IsCurrent(OtherSender));
	TestTrue(TEXT("the receiver with the same name survives"), Session.IsCurrent(SameNameReceiver));

	// A token nobody issued is never current, so "no token" and "stale token" collapse into one
	// check at every use site.
	TestFalse(TEXT("a default token is never current"), Session.IsCurrent(FSpoutStreamToken()));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSpoutSessionTeardownTest,
	"SpoutPlugin.Session.TeardownStalesEveryTokenAndRefusesNewWork",
	SPOUT_TEST_FLAGS)

bool FSpoutSessionTeardownTest::RunTest(const FString& Parameters)
{
	FSpoutSession Session;

	const FSpoutStreamToken SenderToken = Session.OpenStream(FName(TEXT("TeardownA")), ESpoutType::Sender);
	const FSpoutStreamToken ReceiverToken = Session.OpenStream(FName(TEXT("TeardownB")), ESpoutType::Receiver);
	TestTrue(TEXT("sender work is live"), Session.IsCurrent(SenderToken));
	TestTrue(TEXT("receiver work is live"), Session.IsCurrent(ReceiverToken));

	const uint64 GenerationBefore = Session.GetSessionGeneration();

	Session.BeginStop();

	// One bump kills everything outstanding, whatever stream it belonged to. This is what makes
	// "no late work after teardown" a property of the data rather than of the teardown's ordering.
	TestFalse(TEXT("session is no longer running"), Session.IsRunning());
	TestTrue(TEXT("session generation advanced"), Session.GetSessionGeneration() > GenerationBefore);
	TestFalse(TEXT("sender work is cancelled by teardown"), Session.IsCurrent(SenderToken));
	TestFalse(TEXT("receiver work is cancelled by teardown"), Session.IsCurrent(ReceiverToken));

	// And nothing new can be queued INTO the teardown, which is the race a Blueprint tick running
	// alongside a PIE stop would otherwise win.
	TestFalse(TEXT("no token is issued while stopping"),
		Session.OpenStream(FName(TEXT("TeardownC")), ESpoutType::Sender).IsSet());

	Session.EndStop();

	TestTrue(TEXT("session runs again"), Session.IsRunning());
	const FSpoutStreamToken AfterToken = Session.OpenStream(FName(TEXT("TeardownA")), ESpoutType::Sender);
	TestTrue(TEXT("work can be queued after the teardown"), Session.IsCurrent(AfterToken));
	// A reused name must not resurrect the pre-teardown token, which is why the session generation
	// is compared as well as the per-stream one.
	TestFalse(TEXT("the pre-teardown token stays dead"), Session.IsCurrent(SenderToken));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSpoutPendingSendersTest,
	"SpoutPlugin.Session.ViewportNamesAreIndividuallyRemovable",
	SPOUT_TEST_FLAGS)

bool FSpoutPendingSendersTest::RunTest(const FString& Parameters)
{
	FSpoutSession Session;
	FSpoutPendingSenders Pending;

	const FName NameA(TEXT("ViewportA"));
	const FName NameB(TEXT("ViewportB"));
	const FName NameC(TEXT("ViewportC"));

	Pending.Add(Session.OpenStream(NameA, ESpoutType::Sender));
	Pending.Add(Session.OpenStream(NameB, ESpoutType::Sender));
	Pending.Add(Session.OpenStream(NameC, ESpoutType::Sender));
	TestEqual(TEXT("three names queued"), Pending.Num(), 3);

	// A viewport name must not survive its own close: individual removal has to work.
	TestTrue(TEXT("closing one name removes it"), Pending.Remove(NameB));
	TestFalse(TEXT("the closed name is gone"), Pending.Contains(NameB));
	TestTrue(TEXT("the other names are untouched"), Pending.Contains(NameA) && Pending.Contains(NameC));
	TestEqual(TEXT("two names left"), Pending.Num(), 2);

	// Removing something that is not queued is not an error and changes nothing.
	TestFalse(TEXT("removing an absent name reports false"), Pending.Remove(NameB));
	TestEqual(TEXT("still two names"), Pending.Num(), 2);

	// Re-queueing a name replaces its token rather than duplicating the entry, so a per-frame
	// Blueprint call cannot grow the set.
	Pending.Add(Session.OpenStream(NameA, ESpoutType::Sender));
	TestEqual(TEXT("re-queueing does not duplicate"), Pending.Num(), 2);

	TArray<FSpoutStreamToken> Drained;
	Pending.Drain(Drained);
	TestEqual(TEXT("drain returns every queued token"), Drained.Num(), 2);
	TestEqual(TEXT("drain empties the set"), Pending.Num(), 0);
	for (const FSpoutStreamToken& Token : Drained)
	{
		TestTrue(TEXT("drained tokens are still live"), Session.IsCurrent(Token));
	}

	// A name removed while queued must not be published when the drained batch runs.
	Pending.Add(Session.OpenStream(NameA, ESpoutType::Sender));
	Pending.Add(Session.OpenStream(NameC, ESpoutType::Sender));
	Pending.Drain(Drained);
	Session.CancelStream(NameC, ESpoutType::Sender);

	int32 LiveCount = 0;
	for (const FSpoutStreamToken& Token : Drained)
	{
		if (Session.IsCurrent(Token))
		{
			++LiveCount;
			TestEqual(TEXT("the surviving name is the one that was not closed"), Token.Name.ToString(), NameA.ToString());
		}
	}
	TestEqual(TEXT("exactly one of the drained names survives the close"), LiveCount, 1);

	Pending.Reset();
	TestEqual(TEXT("reset empties the set"), Pending.Num(), 0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSpoutCloseSenderCancelsTest,
	"SpoutPlugin.Session.ClosePublicApiCancelsThatStreamOnly",
	SPOUT_TEST_FLAGS)

bool FSpoutCloseSenderCancelsTest::RunTest(const FString& Parameters)
{
	// The one test here that touches the process-wide session, because the wiring from the public
	// Blueprint API into the cancellation model is exactly what is being checked. It uses a name no
	// real stream can be using and only ever increments a counter for that name.
	FSpoutSession& Session = FSpoutSession::Get();
	const FName StreamName(TEXT("SpoutPluginTests_CloseCancels_7b1e"));

	const FSpoutStreamToken SenderToken = Session.OpenStream(StreamName, ESpoutType::Sender);
	const FSpoutStreamToken ReceiverToken = Session.OpenStream(StreamName, ESpoutType::Receiver);
	TestTrue(TEXT("sender token issued"), SenderToken.IsSet());
	TestTrue(TEXT("sender work is live before the close"), Session.IsCurrent(SenderToken));
	TestTrue(TEXT("receiver work is live before the close"), Session.IsCurrent(ReceiverToken));

	USpoutBPFunctionLibrary::CloseSender(StreamName);

	// Close cancels. Removing only an already-existing registry entry would let a Send queued in
	// the same frame still run and publish the sender afterwards.
	TestFalse(TEXT("CloseSender cancels the queued send"), Session.IsCurrent(SenderToken));
	// And it is scoped to the sender role; the registry is keyed by (Name, Type) and cancellation
	// has to match that or closing a sender would silently break a receiver of the same name.
	TestTrue(TEXT("CloseSender leaves a same-named receiver alone"), Session.IsCurrent(ReceiverToken));

	// The receiver half of the same wiring: CloseReceiver must cancel its own queued work, so a
	// receive queued earlier in the frame cannot copy into a texture the caller has just been told
	// is finished with. This also leaves the session with no live token for the test's name.
	UTexture2D* NoTexture = nullptr;
	UMaterialInstanceDynamic* NoMaterial = nullptr;
	USpoutBPFunctionLibrary::CloseReceiver(StreamName, NoTexture, NoMaterial);

	TestFalse(TEXT("CloseReceiver cancels the queued receive"), Session.IsCurrent(ReceiverToken));

	return true;
}

// =================================================================================================
// Ordered teardown
// =================================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSpoutLifecycleOrderTest,
	"SpoutPlugin.Lifecycle.ShutdownStepsRunInTheRequiredOrder",
	SPOUT_TEST_FLAGS)

bool FSpoutLifecycleOrderTest::RunTest(const FString& Parameters)
{
	using namespace SpoutPluginTests;

	FRecordingLifecycleSteps Recorder;
	SpoutLifecycle::RunOrderedShutdown(Recorder);

	TestEqual(TEXT("every step ran exactly once"), Recorder.Calls.Num(), 7);

	for (int32 Index = 0; Index < Recorder.Calls.Num() && Index < 7; ++Index)
	{
		TestEqual(
			*FString::Printf(TEXT("step %d"), Index),
			Recorder.Calls[Index],
			FString(SpoutLifecycle::OrderedStepNames[Index]));
	}

	// Named individually as well, because these are the inversions that actually break teardown
	// and a list comparison alone does not say which relationship broke.
	const int32 Detach = Recorder.IndexOf(TEXT("DetachProducers"));
	const int32 ClearPending = Recorder.IndexOf(TEXT("ClearPendingWork"));
	const int32 Flush = Recorder.IndexOf(TEXT("FlushRenderingWork"));
	const int32 ReleaseNative = Recorder.IndexOf(TEXT("ReleaseNativeResources"));
	const int32 ReleaseManaged = Recorder.IndexOf(TEXT("ReleaseManagedObjects"));
	const int32 Stopping = Recorder.IndexOf(TEXT("EnterStoppingState"));

	TestTrue(TEXT("stopping state is entered before anything is detached"), Stopping < Detach);
	// The inversion that matters: flushing first and detaching afterwards lets the render thread
	// pass the flush marker and enqueue more work behind it.
	TestTrue(TEXT("producers are detached BEFORE the render flush"), Detach < Flush);
	TestTrue(TEXT("pending work is cleared before the render flush"), ClearPending < Flush);
	// Native resources must outlive every command the flush is draining.
	TestTrue(TEXT("native resources are released AFTER the render flush"), Flush < ReleaseNative);
	// Managed UObjects go last so their RHI resources outlive the commands that captured them.
	TestTrue(TEXT("managed ownership is released last"), ReleaseNative < ReleaseManaged);

	return true;
}

// =================================================================================================
// Managed receiver ownership
// =================================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSpoutReceiverOwnershipTrackingTest,
	"SpoutPlugin.ReceiverOwnership.TracksTexturesWithoutRootingThem",
	SPOUT_TEST_FLAGS)

bool FSpoutReceiverOwnershipTrackingTest::RunTest(const FString& Parameters)
{
	using namespace SpoutPluginTests;

	FSpoutReceiverOwnership Ownership;

	const FName ReceiverA(TEXT("OwnedA"));
	const FName ReceiverB(TEXT("OwnedB"));

	UTexture2D* TextureA = MakeTestTexture();
	UTexture2D* TextureB = MakeTestTexture();
	if (!TextureA || !TextureB)
	{
		AddError(TEXT("could not create the transient textures the test needs"));
		return false;
	}

	Ownership.AdoptTexture(ReceiverA, TextureA);

	// Rooting is what has to be avoided: the root set cannot be enumerated by any code path here,
	// so a lost reference or a forgotten close would leak the texture permanently.
	TestFalse(TEXT("an owned texture is not rooted"), TextureA->IsRooted());

	TestTrue(TEXT("the texture is owned by its receiver"), Ownership.OwnsTexture(ReceiverA, TextureA));
	// The core primitive: ownership answers "is this mine", so the close path never has to assume
	// that whatever pointer it was handed is fair game.
	TestFalse(TEXT("a different texture is not owned by that receiver"), Ownership.OwnsTexture(ReceiverA, TextureB));
	TestFalse(TEXT("the texture is not owned by a different receiver"), Ownership.OwnsTexture(ReceiverB, TextureA));
	TestFalse(TEXT("null is never owned"), Ownership.OwnsTexture(ReceiverA, nullptr));
	TestEqual(TEXT("one receiver tracked"), Ownership.Num(), 1);

	// Replacing on resize: the old texture loses this reference and nothing else happens to it.
	Ownership.AdoptTexture(ReceiverA, TextureB);
	TestFalse(TEXT("the replaced texture is no longer owned"), Ownership.OwnsTexture(ReceiverA, TextureA));
	TestTrue(TEXT("the replacement is owned"), Ownership.OwnsTexture(ReceiverA, TextureB));
	TestEqual(TEXT("still one receiver tracked"), Ownership.Num(), 1);
	// Releasing is dropping a reference, not forcing destruction. The replaced object is still a
	// valid UObject; the engine collects it when nothing points at it.
	TestTrue(TEXT("the replaced texture was not destroyed"), IsValid(TextureA));

	TestTrue(TEXT("release reports that something was tracked"), Ownership.Release(ReceiverA));
	TestFalse(TEXT("the receiver is no longer tracked"), Ownership.Contains(ReceiverA));
	TestEqual(TEXT("nothing tracked"), Ownership.Num(), 0);
	TestFalse(TEXT("releasing twice reports nothing to release"), Ownership.Release(ReceiverA));
	TestTrue(TEXT("the released texture was not destroyed either"), IsValid(TextureB));
	TestFalse(TEXT("and it is not left rooted"), TextureB->IsRooted());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSpoutReceiverOwnershipReleaseAllTest,
	"SpoutPlugin.ReceiverOwnership.ShutdownReleasesEveryReceiver",
	SPOUT_TEST_FLAGS)

bool FSpoutReceiverOwnershipReleaseAllTest::RunTest(const FString& Parameters)
{
	using namespace SpoutPluginTests;

	FSpoutReceiverOwnership Ownership;

	// The requirement: no receiver textures remain rooted or referenced after a complete
	// shutdown, for every receiver, including ones whose Blueprint never called CloseReceiver.
	TArray<UTexture2D*> Textures;
	for (int32 Index = 0; Index < 4; ++Index)
	{
		UTexture2D* Texture = MakeTestTexture();
		if (!Texture)
		{
			AddError(TEXT("could not create a transient texture"));
			return false;
		}
		Textures.Add(Texture);
		Ownership.AdoptTexture(FName(*FString::Printf(TEXT("ShutdownReceiver%d"), Index)), Texture);
	}

	TestEqual(TEXT("four receivers tracked"), Ownership.Num(), 4);

	Ownership.ReleaseAll();

	TestEqual(TEXT("nothing tracked after shutdown"), Ownership.Num(), 0);
	for (int32 Index = 0; Index < Textures.Num(); ++Index)
	{
		TestFalse(
			*FString::Printf(TEXT("texture %d is not rooted after shutdown"), Index),
			Textures[Index]->IsRooted());
		TestTrue(
			*FString::Printf(TEXT("texture %d was not force-destroyed"), Index),
			IsValid(Textures[Index]));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSpoutReceiverOwnershipCollectionTest,
	"SpoutPlugin.ReceiverOwnership.ReleasedTexturesAreActuallyCollected",
	SPOUT_TEST_FLAGS)

bool FSpoutReceiverOwnershipCollectionTest::RunTest(const FString& Parameters)
{
	using namespace SpoutPluginTests;

	// The requirement is that no receiver texture is left rooted after shutdown. Asserting
	// IsRooted() only shows the flag is clear; this runs the collector and observes the object
	// actually going away, which is the property that actually matters.
	FSpoutReceiverOwnership Ownership;
	const FName ReceiverName(TEXT("CollectableReceiver"));

	TWeakObjectPtr<UTexture2D> Weak;
	{
		UTexture2D* Texture = MakeTestTexture();
		if (!Texture)
		{
			AddError(TEXT("could not create the transient texture the test needs"));
			return false;
		}
		Weak = Texture;
		Ownership.AdoptTexture(ReceiverName, Texture);
	}

	// Managed, not rooted -- but managed still has to mean "survives a collection". If this fails,
	// the reference is not being reported to the collector and receivers would lose their texture
	// mid-session.
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS, true);
	TestTrue(TEXT("an owned texture survives garbage collection"), Weak.IsValid());

	Ownership.Release(ReceiverName);

	// And released has to mean actually reclaimable. Under a root-based scheme this object would
	// still be alive here, which is the leak.
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS, true);
	TestFalse(TEXT("a released texture is collected"), Weak.IsValid());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSpoutReceiverOwnershipMaterialTest,
	"SpoutPlugin.ReceiverOwnership.CloseDetachesTheMaterialParameter",
	SPOUT_TEST_FLAGS)

bool FSpoutReceiverOwnershipMaterialTest::RunTest(const FString& Parameters)
{
	using namespace SpoutPluginTests;

	UTexture2D* Texture = MakeTestTexture();
	UMaterialInstanceDynamic* Material = MakeTestMaterial();
	if (!Texture || !Material)
	{
		AddError(TEXT("could not create the texture or material the test needs"));
		return false;
	}

	const FName ReceiverName(TEXT("MaterialReceiver"));
	const FName ParameterName(TEXT("SpoutTexture"));

	FSpoutReceiverOwnership Ownership;
	Ownership.AdoptTexture(ReceiverName, Texture);
	Material->SetTextureParameterValue(ParameterName, Texture);
	Ownership.RegisterMaterialBinding(ReceiverName, Material, ParameterName, /*bPluginCreated*/ true);

	UTexture* Bound = nullptr;
	TestTrue(TEXT("the parameter override exists"), FindTextureOverride(Material, ParameterName, Bound));
	TestTrue(TEXT("the parameter points at the received texture"), Bound == Texture);
	TestTrue(TEXT("a plugin-created material is owned"), Ownership.OwnsMaterial(ReceiverName, Material));

	Ownership.Release(ReceiverName);

	// Releasing must detach the material as well. Setting only the caller's variable to null would
	// leave the material still sampling the texture, and every other reference to that material would
	// keep the binding -- and, since a material parameter is a strong reference, keep the texture alive.
	//
	// The assertion is "no longer references the received texture", not "is null": UMaterialInstance
	// ignores a null texture value, so the detach rebinds to the engine default texture instead.
	TestTrue(TEXT("the override still exists"), FindTextureOverride(Material, ParameterName, Bound));
	TestTrue(TEXT("the material no longer references the received texture"), Bound != Texture);
	TestTrue(TEXT("and is bound to a placeholder rather than left dangling"), Bound != nullptr);
	TestTrue(TEXT("the material itself was not destroyed"), IsValid(Material));

	// A caller-supplied material is recorded so its parameter can be detached, but never owned.
	{
		UMaterialInstanceDynamic* CallerMaterial = MakeTestMaterial();
		UTexture2D* CallerTexture = MakeTestTexture();
		if (!CallerMaterial || !CallerTexture)
		{
			AddError(TEXT("could not create the caller-owned objects the test needs"));
			return false;
		}

		const FName OtherReceiver(TEXT("CallerMaterialReceiver"));
		Ownership.AdoptTexture(OtherReceiver, CallerTexture);
		CallerMaterial->SetTextureParameterValue(ParameterName, CallerTexture);
		Ownership.RegisterMaterialBinding(OtherReceiver, CallerMaterial, ParameterName, /*bPluginCreated*/ false);

		TestFalse(TEXT("a caller-supplied material is not owned"), Ownership.OwnsMaterial(OtherReceiver, CallerMaterial));
		TestTrue(TEXT("it is still registered, so close can detach its parameter"),
			Ownership.HasMaterialBinding(OtherReceiver, CallerMaterial, ParameterName));

		Ownership.Release(OtherReceiver);
		TestTrue(TEXT("the caller's parameter override still exists"), FindTextureOverride(CallerMaterial, ParameterName, Bound));
		TestTrue(TEXT("the caller's material no longer references the received texture"), Bound != CallerTexture);
		TestTrue(TEXT("the caller's material was not destroyed"), IsValid(CallerMaterial));
	}

	return true;
}

// =================================================================================================
// CloseReceiver may not destroy what it does not own
// =================================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSpoutCloseReceiverForeignTextureTest,
	"SpoutPlugin.CloseReceiver.LeavesATextureItDoesNotOwnAlone",
	SPOUT_TEST_FLAGS)

bool FSpoutCloseReceiverForeignTextureTest::RunTest(const FString& Parameters)
{
	using namespace SpoutPluginTests;

	// Drives the real Blueprint entry point against the real ownership tracker, because what is
	// under test is that specific function's authority to destroy things.
	FSpoutReceiverOwnership& Ownership = FSpoutReceiverOwnership::Get();

	const FName ReceiverName(TEXT("SpoutPluginTests_CloseOwned_3d90"));
	const FName ForeignName(TEXT("SpoutPluginTests_CloseForeign_3d90"));

	UTexture2D* OwnedTexture = MakeTestTexture();
	UTexture2D* ForeignTexture = MakeTestTexture();
	if (!OwnedTexture || !ForeignTexture)
	{
		AddError(TEXT("could not create the transient textures the test needs"));
		return false;
	}

	// Somebody else's texture, tracked under a different receiver so it is genuinely live state.
	Ownership.AdoptTexture(ReceiverName, OwnedTexture);
	Ownership.AdoptTexture(ForeignName, ForeignTexture);

	// The mismatched pair: the right name, the wrong pointer. Trusting the pointer would unroot,
	// BeginReleaseResource and MarkAsGarbage the foreign texture and null the caller's variable.
	{
		UTexture2D* CallerTexture = ForeignTexture;
		UMaterialInstanceDynamic* CallerMaterial = nullptr;
		USpoutBPFunctionLibrary::CloseReceiver(ReceiverName, CallerTexture, CallerMaterial);

		TestTrue(TEXT("the foreign texture was not destroyed"), IsValid(ForeignTexture));
		TestFalse(TEXT("the foreign texture was not left rooted"), ForeignTexture->IsRooted());
		// Not cleared either: dropping the caller's last reference would destroy it by a slower
		// route, which is the same harm.
		TestTrue(TEXT("the caller's reference to it was left untouched"), CallerTexture == ForeignTexture);
		// The other receiver keeps working; closing one receiver cannot affect another's texture.
		TestTrue(TEXT("the other receiver still owns its texture"), Ownership.OwnsTexture(ForeignName, ForeignTexture));
		// The named receiver really was closed.
		TestFalse(TEXT("the named receiver was released"), Ownership.Contains(ReceiverName));
	}

	// The matched pair: the caller's variable is cleared, and the object is still merely
	// unreferenced rather than force-destroyed.
	{
		UTexture2D* SecondTexture = MakeTestTexture();
		if (!SecondTexture)
		{
			AddError(TEXT("could not create the second transient texture"));
			return false;
		}

		Ownership.AdoptTexture(ReceiverName, SecondTexture);

		UTexture2D* CallerTexture = SecondTexture;
		UMaterialInstanceDynamic* CallerMaterial = nullptr;
		USpoutBPFunctionLibrary::CloseReceiver(ReceiverName, CallerTexture, CallerMaterial);

		TestNull(TEXT("the caller's reference to its own texture is cleared"), CallerTexture);
		TestTrue(TEXT("the texture is still a valid object, just unreferenced"), IsValid(SecondTexture));
		TestFalse(TEXT("it is not rooted"), SecondTexture->IsRooted());
		TestFalse(TEXT("the receiver is no longer tracked"), Ownership.Contains(ReceiverName));
	}

	// Closing a name that was never opened is a no-op, not a crash and not a destruction.
	{
		UTexture2D* CallerTexture = nullptr;
		UMaterialInstanceDynamic* CallerMaterial = nullptr;
		USpoutBPFunctionLibrary::CloseReceiver(FName(TEXT("SpoutPluginTests_NeverOpened_3d90")), CallerTexture, CallerMaterial);
		TestNull(TEXT("nothing was produced"), CallerTexture);
	}

	// Leave the tracker as the test found it.
	Ownership.Release(ForeignName);
	TestFalse(TEXT("test state cleaned up"), Ownership.Contains(ForeignName));

	return true;
}

// =================================================================================================
// Same-name receiver semantics
//
// Receiver state is keyed by sender name, at every layer: the registry is keyed by (Name, Type),
// and cancellation and UObject ownership are keyed the same way. The consequence is one receiver
// session per sender name, shared by every consumer of that name. These tests pin what that does
// and does not guarantee.
//
// The contract, deliberately preserved:
//   - one receiver session per sender name;
//   - every consumer of a name shares the same received UTexture2D;
//   - each consumer has its OWN material instance, from its OWN InputMaterial;
//   - CloseReceiver(Name) closes the shared session for every consumer of it, detaching every
//     registered material binding;
//   - different receiver names are fully independent;
//   - no reference counting and no public consumer handles.
// =================================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSpoutReceiverNamesIndependentTest,
	"SpoutPlugin.ReceiverOwnership.DifferentNamesAreFullyIndependent",
	SPOUT_TEST_FLAGS)

bool FSpoutReceiverNamesIndependentTest::RunTest(const FString& Parameters)
{
	using namespace SpoutPluginTests;

	FSpoutReceiverOwnership Ownership;

	const FName NameOne(TEXT("IndependentR1"));
	const FName NameTwo(TEXT("IndependentR2"));
	const FName ParameterName(TEXT("SpoutTexture"));

	UTexture2D* TextureOne = MakeTestTexture();
	UTexture2D* TextureTwo = MakeTestTexture();
	UMaterialInstanceDynamic* MaterialOne = MakeTestMaterial();
	UMaterialInstanceDynamic* MaterialTwo = MakeTestMaterial();
	if (!TextureOne || !TextureTwo || !MaterialOne || !MaterialTwo)
	{
		AddError(TEXT("could not create the objects the test needs"));
		return false;
	}

	Ownership.AdoptTexture(NameOne, TextureOne);
	MaterialOne->SetTextureParameterValue(ParameterName, TextureOne);
	Ownership.RegisterMaterialBinding(NameOne, MaterialOne, ParameterName, /*bPluginCreated*/ true);

	Ownership.AdoptTexture(NameTwo, TextureTwo);
	MaterialTwo->SetTextureParameterValue(ParameterName, TextureTwo);
	Ownership.RegisterMaterialBinding(NameTwo, MaterialTwo, ParameterName, /*bPluginCreated*/ true);

	TestEqual(TEXT("two receivers tracked"), Ownership.Num(), 2);
	// Distinct names never share a texture: the two entries are separate slots, not one.
	TestFalse(TEXT("name one does not own name two's texture"), Ownership.OwnsTexture(NameOne, TextureTwo));
	TestFalse(TEXT("name two does not own name one's texture"), Ownership.OwnsTexture(NameTwo, TextureOne));

	Ownership.Release(NameOne);

	TestFalse(TEXT("the released name is gone"), Ownership.Contains(NameOne));
	TestTrue(TEXT("the other name survives"), Ownership.Contains(NameTwo));
	TestTrue(TEXT("the other name keeps owning its texture"), Ownership.OwnsTexture(NameTwo, TextureTwo));
	TestTrue(TEXT("the other name keeps owning its material"), Ownership.OwnsMaterial(NameTwo, MaterialTwo));

	// The surviving receiver's material must still be bound to its own texture: releasing one name
	// must not detach another name's parameter.
	UTexture* Bound = nullptr;
	TestTrue(TEXT("the other name's override exists"), FindTextureOverride(MaterialTwo, ParameterName, Bound));
	TestTrue(TEXT("the other name's material still shows its own texture"), Bound == TextureTwo);

	// And the released one was detached.
	TestTrue(TEXT("the released name's override exists"), FindTextureOverride(MaterialOne, ParameterName, Bound));
	TestTrue(TEXT("the released name's material was detached"), Bound != TextureOne);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSpoutCloseReceiverNameScopeTest,
	"SpoutPlugin.CloseReceiver.ClosingOneNameLeavesOtherNamesAlone",
	SPOUT_TEST_FLAGS)

bool FSpoutCloseReceiverNameScopeTest::RunTest(const FString& Parameters)
{
	using namespace SpoutPluginTests;

	// Through the real Blueprint entry point and the real tracker, because the question is about the
	// public API's blast radius.
	FSpoutReceiverOwnership& Ownership = FSpoutReceiverOwnership::Get();

	const FName Closed(TEXT("SpoutPluginTests_ScopeClosed_5a2f"));
	const FName Kept(TEXT("SpoutPluginTests_ScopeKept_5a2f"));

	UTexture2D* ClosedTexture = MakeTestTexture();
	UTexture2D* KeptTexture = MakeTestTexture();
	if (!ClosedTexture || !KeptTexture)
	{
		AddError(TEXT("could not create the transient textures the test needs"));
		return false;
	}

	Ownership.AdoptTexture(Closed, ClosedTexture);
	Ownership.AdoptTexture(Kept, KeptTexture);

	UTexture2D* CallerTexture = ClosedTexture;
	UMaterialInstanceDynamic* CallerMaterial = nullptr;
	USpoutBPFunctionLibrary::CloseReceiver(Closed, CallerTexture, CallerMaterial);

	TestFalse(TEXT("the closed name was released"), Ownership.Contains(Closed));
	TestNull(TEXT("the closing caller's variable was cleared"), CallerTexture);

	// The whole point: closing is scoped to one name.
	TestTrue(TEXT("the other name is untouched"), Ownership.Contains(Kept));
	TestTrue(TEXT("the other name still owns its texture"), Ownership.OwnsTexture(Kept, KeptTexture));

	Ownership.Release(Kept);
	TestFalse(TEXT("test state cleaned up"), Ownership.Contains(Kept));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSpoutSharedNameSingleTextureTest,
	"SpoutPlugin.ReceiverOwnership.OneSessionPerNameSharesASingleTexture",
	SPOUT_TEST_FLAGS)

bool FSpoutSharedNameSingleTextureTest::RunTest(const FString& Parameters)
{
	using namespace SpoutPluginTests;

	// Intended behaviour. Whichever consumer of a name asks, the answer is the same object: there is
	// one receiver session per sender name, so there is one destination texture for it.
	FSpoutReceiverOwnership Ownership;
	const FName SharedName(TEXT("SharedNameR"));

	UTexture2D* Texture = MakeTestTexture();
	if (!Texture)
	{
		AddError(TEXT("could not create the transient texture the test needs"));
		return false;
	}

	Ownership.AdoptTexture(SharedName, Texture);

	UTexture2D* ConsumerOneSees = Ownership.FindTexture(SharedName);
	UTexture2D* ConsumerTwoSees = Ownership.FindTexture(SharedName);

	TestTrue(TEXT("consumer one gets the session texture"), ConsumerOneSees == Texture);
	TestTrue(TEXT("consumer two gets the same object, not a copy"), ConsumerTwoSees == Texture);
	TestEqual(TEXT("one session, not one per consumer"), Ownership.Num(), 1);

	// A resize replaces the single session texture, so every consumer's next call moves to the
	// replacement together. Neither consumer keeps the old object alive through the tracker.
	UTexture2D* Replacement = MakeTestTexture();
	if (!Replacement)
	{
		AddError(TEXT("could not create the replacement texture"));
		return false;
	}

	Ownership.AdoptTexture(SharedName, Replacement);
	TestTrue(TEXT("both consumers move to the replacement"), Ownership.FindTexture(SharedName) == Replacement);
	TestFalse(TEXT("the previous texture is no longer owned"), Ownership.OwnsTexture(SharedName, Texture));
	TestEqual(TEXT("still one session"), Ownership.Num(), 1);

	return true;
}

// -------------------------------------------------------------------------------------------------
// Regression tests for the two same-name ownership defects.
//
// Each test asserts the correct behaviour and fails if its defect returns.
// -------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSpoutEachConsumerOwnMaterialTest,
	"SpoutPlugin.ReceiverOwnership.EachConsumerGetsItsOwnMaterialInstance",
	SPOUT_TEST_FLAGS)

bool FSpoutEachConsumerOwnMaterialTest::RunTest(const FString& Parameters)
{
	using namespace SpoutPluginTests;

	// REGRESSION. Receive must not prefer the caller's OutMat and then fall back to whatever
	// material is tracked for the NAME. With two consumers of one name, that hands the second
	// consumer -- whose OutMat is null, exactly as a fresh Blueprint variable is -- the FIRST
	// consumer's instance, and its own InputMaterial is never used. Winner decided by tick order.
	//
	// Two distinct UMaterial objects stand in for two consumers' different InputMaterials.
	UMaterialInterface* InputMaterialOne = MakeTestParentMaterial();
	UMaterialInterface* InputMaterialTwo = MakeTestParentMaterial();
	if (!InputMaterialOne || !InputMaterialTwo || InputMaterialOne == InputMaterialTwo)
	{
		AddError(TEXT("could not create two distinct input materials"));
		return false;
	}

	FSpoutReceiverOwnership Ownership;
	const FName SharedName(TEXT("PerConsumerMaterials"));
	const FName ParameterName(TEXT("SpoutTexture"));

	UTexture2D* SessionTexture = MakeTestTexture();
	if (!SessionTexture)
	{
		AddError(TEXT("could not create the session texture"));
		return false;
	}
	Ownership.AdoptTexture(SharedName, SessionTexture);

	// Both consumers start with a null OutMat, the case a name-keyed fallback gets wrong.
	UMaterialInstanceDynamic* ConsumerOneMaterial =
		TickConsumer(Ownership, SharedName, nullptr, InputMaterialOne, ParameterName, SessionTexture);
	UMaterialInstanceDynamic* ConsumerTwoMaterial =
		TickConsumer(Ownership, SharedName, nullptr, InputMaterialTwo, ParameterName, SessionTexture);

	if (!ConsumerOneMaterial || !ConsumerTwoMaterial)
	{
		AddError(TEXT("a consumer did not receive a material"));
		return false;
	}

	// The regression assertions: distinct instances, each from its own InputMaterial.
	TestTrue(TEXT("the two consumers get distinct material instances"), ConsumerOneMaterial != ConsumerTwoMaterial);
	TestTrue(TEXT("consumer one's material uses consumer one's input material"),
		ConsumerOneMaterial->Parent == InputMaterialOne);
	TestTrue(TEXT("consumer two's material uses consumer two's input material"),
		ConsumerTwoMaterial->Parent == InputMaterialTwo);

	// One session per name is preserved: the texture is shared even though the materials are not.
	TestTrue(TEXT("both consumers share the one session texture"), Ownership.FindTexture(SharedName) == SessionTexture);
	TestEqual(TEXT("one session, not one per consumer"), Ownership.Num(), 1);

	// Both materials are registered against the one session, and both were created by the plugin.
	TestEqual(TEXT("both consumers are registered"), Ownership.NumMaterialBindings(SharedName), 2);
	TestTrue(TEXT("consumer one's material is owned"), Ownership.OwnsMaterial(SharedName, ConsumerOneMaterial));
	TestTrue(TEXT("consumer two's material is owned"), Ownership.OwnsMaterial(SharedName, ConsumerTwoMaterial));

	// A consumer that already holds a material keeps reusing it: repeated ticks must not churn.
	UMaterialInstanceDynamic* ConsumerOneAgain =
		TickConsumer(Ownership, SharedName, ConsumerOneMaterial, InputMaterialOne, ParameterName, SessionTexture);
	TestTrue(TEXT("a consumer's repeated tick reuses its own material"), ConsumerOneAgain == ConsumerOneMaterial);
	TestEqual(TEXT("and does not add a binding"), Ownership.NumMaterialBindings(SharedName), 2);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSpoutAllBindingsDetachedTest,
	"SpoutPlugin.ReceiverOwnership.EveryMaterialBindingIsDetachedOnClose",
	SPOUT_TEST_FLAGS)

bool FSpoutAllBindingsDetachedTest::RunTest(const FString& Parameters)
{
	using namespace SpoutPluginTests;

	// REGRESSION. An entry holding ONE material, ONE parameter name and ONE ownership slot,
	// overwritten on every call, would let close detach only whichever consumer ticked last. Every
	// other consumer's material would keep the released texture bound -- and a material parameter
	// is a strong reference, so it would keep that texture alive.
	FSpoutReceiverOwnership Ownership;

	const FName SharedName(TEXT("MultiBindingSession"));
	const FName ParameterOne(TEXT("SpoutTexture"));
	const FName ParameterTwo(TEXT("VideoIn"));

	UTexture2D* Texture = MakeTestTexture();
	UMaterialInstanceDynamic* MaterialOne = MakeTestMaterial();
	UMaterialInstanceDynamic* MaterialTwo = MakeTestMaterial();
	UMaterialInstanceDynamic* MaterialThree = MakeTestMaterial();
	if (!Texture || !MaterialOne || !MaterialTwo || !MaterialThree)
	{
		AddError(TEXT("could not create the objects the test needs"));
		return false;
	}

	Ownership.AdoptTexture(SharedName, Texture);

	// Three consumers: one plugin-created, one caller-owned, one plugin-created on a different
	// parameter name.
	MaterialOne->SetTextureParameterValue(ParameterOne, Texture);
	Ownership.RegisterMaterialBinding(SharedName, MaterialOne, ParameterOne, /*bPluginCreated*/ true);

	MaterialTwo->SetTextureParameterValue(ParameterOne, Texture);
	Ownership.RegisterMaterialBinding(SharedName, MaterialTwo, ParameterOne, /*bPluginCreated*/ false);

	MaterialThree->SetTextureParameterValue(ParameterTwo, Texture);
	Ownership.RegisterMaterialBinding(SharedName, MaterialThree, ParameterTwo, /*bPluginCreated*/ true);

	TestEqual(TEXT("three bindings registered"), Ownership.NumMaterialBindings(SharedName), 3);

	// A later caller-owned registration must not displace the plugin-owned ones.
	TestTrue(TEXT("the first plugin-created material is still owned"), Ownership.OwnsMaterial(SharedName, MaterialOne));
	TestTrue(TEXT("the third plugin-created material is owned"), Ownership.OwnsMaterial(SharedName, MaterialThree));
	TestFalse(TEXT("the caller-owned material is not owned"), Ownership.OwnsMaterial(SharedName, MaterialTwo));

	Ownership.Release(SharedName);

	// The regression assertions: EVERY binding was detached, not just the last.
	UTexture* Bound = nullptr;
	TestTrue(TEXT("binding one's override exists"), FindTextureOverride(MaterialOne, ParameterOne, Bound));
	TestTrue(TEXT("binding one no longer references the released texture"), Bound != Texture);
	TestTrue(TEXT("binding one is bound to a real fallback, not left dangling"), Bound != nullptr);

	TestTrue(TEXT("binding two's override exists"), FindTextureOverride(MaterialTwo, ParameterOne, Bound));
	TestTrue(TEXT("binding two no longer references the released texture"), Bound != Texture);

	TestTrue(TEXT("binding three's override exists"), FindTextureOverride(MaterialThree, ParameterTwo, Bound));
	TestTrue(TEXT("binding three no longer references the released texture"), Bound != Texture);

	// Nothing was destroyed, and the session's records are gone.
	TestTrue(TEXT("no material was destroyed"), IsValid(MaterialOne) && IsValid(MaterialTwo) && IsValid(MaterialThree));
	TestEqual(TEXT("the binding collection is empty after close"), Ownership.NumMaterialBindings(SharedName), 0);
	TestFalse(TEXT("the session is gone"), Ownership.Contains(SharedName));
	TestFalse(TEXT("plugin ownership of the created materials was released"),
		Ownership.OwnsMaterial(SharedName, MaterialOne));

	// Closing again is a no-op, not an error.
	TestFalse(TEXT("closing an already-closed session reports nothing to release"), Ownership.Release(SharedName));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSpoutBindingDeduplicationTest,
	"SpoutPlugin.ReceiverOwnership.RepeatedRegistrationDoesNotGrowTheBindingList",
	SPOUT_TEST_FLAGS)

bool FSpoutBindingDeduplicationTest::RunTest(const FString& Parameters)
{
	using namespace SpoutPluginTests;

	// RegisterMaterialBinding is called once per consumer per tick, so it has to be idempotent on
	// (material, parameter) or the collection grows without bound during normal playback.
	FSpoutReceiverOwnership Ownership;

	const FName SharedName(TEXT("DedupSession"));
	const FName ParameterOne(TEXT("SpoutTexture"));
	const FName ParameterTwo(TEXT("VideoIn"));

	UTexture2D* Texture = MakeTestTexture();
	UMaterialInstanceDynamic* Material = MakeTestMaterial();
	if (!Texture || !Material)
	{
		AddError(TEXT("could not create the objects the test needs"));
		return false;
	}

	Ownership.AdoptTexture(SharedName, Texture);

	for (int32 Tick = 0; Tick < 120; ++Tick)
	{
		Ownership.RegisterMaterialBinding(SharedName, Material, ParameterOne, /*bPluginCreated*/ true);
	}

	TestEqual(TEXT("120 ticks produce one binding"), Ownership.NumMaterialBindings(SharedName), 1);

	// The same material through a genuinely different parameter is a genuinely different binding.
	Ownership.RegisterMaterialBinding(SharedName, Material, ParameterTwo, /*bPluginCreated*/ true);
	TestEqual(TEXT("a different parameter adds a binding"), Ownership.NumMaterialBindings(SharedName), 2);
	TestTrue(TEXT("the first parameter is still registered"), Ownership.HasMaterialBinding(SharedName, Material, ParameterOne));
	TestTrue(TEXT("the second parameter is registered"), Ownership.HasMaterialBinding(SharedName, Material, ParameterTwo));

	// Re-registering the same pair still does not duplicate.
	Ownership.RegisterMaterialBinding(SharedName, Material, ParameterTwo, /*bPluginCreated*/ true);
	TestEqual(TEXT("still two bindings"), Ownership.NumMaterialBindings(SharedName), 2);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSpoutOwnedVersusCallerMaterialTest,
	"SpoutPlugin.ReceiverOwnership.CallerOwnedMaterialsNeverDisplacePluginOwnedOnes",
	SPOUT_TEST_FLAGS)

bool FSpoutOwnedVersusCallerMaterialTest::RunTest(const FString& Parameters)
{
	using namespace SpoutPluginTests;

	// A single-slot entry that wrote OwnedMaterial = bPluginCreated ? Material : nullptr on every
	// call would let a consumer supplying its own material silently drop plugin ownership of a
	// material the plugin had created for a different consumer.
	FSpoutReceiverOwnership Ownership;

	const FName SharedName(TEXT("OwnershipMixSession"));
	const FName ParameterName(TEXT("SpoutTexture"));

	UTexture2D* Texture = MakeTestTexture();
	UMaterialInstanceDynamic* PluginMaterial = MakeTestMaterial();
	UMaterialInstanceDynamic* CallerMaterial = MakeTestMaterial();
	if (!Texture || !PluginMaterial || !CallerMaterial)
	{
		AddError(TEXT("could not create the objects the test needs"));
		return false;
	}

	Ownership.AdoptTexture(SharedName, Texture);
	Ownership.RegisterMaterialBinding(SharedName, PluginMaterial, ParameterName, /*bPluginCreated*/ true);
	TestTrue(TEXT("the plugin-created material is owned"), Ownership.OwnsMaterial(SharedName, PluginMaterial));

	Ownership.RegisterMaterialBinding(SharedName, CallerMaterial, ParameterName, /*bPluginCreated*/ false);

	// The regression assertion: registering a caller-owned material leaves plugin ownership alone.
	TestTrue(TEXT("the plugin-created material is STILL owned"), Ownership.OwnsMaterial(SharedName, PluginMaterial));
	TestFalse(TEXT("the caller-owned material is not owned"), Ownership.OwnsMaterial(SharedName, CallerMaterial));
	TestTrue(TEXT("but it is registered, so close will detach it"),
		Ownership.HasMaterialBinding(SharedName, CallerMaterial, ParameterName));
	TestEqual(TEXT("two bindings"), Ownership.NumMaterialBindings(SharedName), 2);

	// Re-asserting plugin creation for an existing binding is idempotent and does not duplicate.
	Ownership.RegisterMaterialBinding(SharedName, PluginMaterial, ParameterName, /*bPluginCreated*/ true);
	TestTrue(TEXT("still owned after re-registration"), Ownership.OwnsMaterial(SharedName, PluginMaterial));
	TestEqual(TEXT("still two bindings"), Ownership.NumMaterialBindings(SharedName), 2);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSpoutMaterialLifetimeTest,
	"SpoutPlugin.ReceiverOwnership.PluginMaterialsSurviveGCUntilCloseAndCallerMaterialsAreNotKeptAlive",
	SPOUT_TEST_FLAGS)

bool FSpoutMaterialLifetimeTest::RunTest(const FString& Parameters)
{
	using namespace SpoutPluginTests;

	// Two halves of the same rule, both proved by running the collector rather than by inspecting a
	// flag: a plugin-created material must survive GC for as long as the session is open, and a
	// caller-owned material must NOT be kept alive merely because it was once bound.
	FSpoutReceiverOwnership Ownership;

	const FName SharedName(TEXT("MaterialLifetimeSession"));
	const FName ParameterName(TEXT("SpoutTexture"));

	UTexture2D* Texture = MakeTestTexture();
	if (!Texture)
	{
		AddError(TEXT("could not create the session texture"));
		return false;
	}
	Ownership.AdoptTexture(SharedName, Texture);

	TWeakObjectPtr<UMaterialInstanceDynamic> WeakPluginMaterial;
	TWeakObjectPtr<UMaterialInstanceDynamic> WeakCallerMaterial;
	{
		UMaterialInstanceDynamic* PluginMaterial = MakeTestMaterial();
		UMaterialInstanceDynamic* CallerMaterial = MakeTestMaterial();
		if (!PluginMaterial || !CallerMaterial)
		{
			AddError(TEXT("could not create the materials the test needs"));
			return false;
		}

		WeakPluginMaterial = PluginMaterial;
		WeakCallerMaterial = CallerMaterial;

		Ownership.RegisterMaterialBinding(SharedName, PluginMaterial, ParameterName, /*bPluginCreated*/ true);
		Ownership.RegisterMaterialBinding(SharedName, CallerMaterial, ParameterName, /*bPluginCreated*/ false);
	}

	// No strong reference outside the tracker now holds either material.
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS, true);

	TestTrue(TEXT("a plugin-created material survives GC while the session is open"), WeakPluginMaterial.IsValid());
	// The caller dropped its only reference, so nothing should be holding this one. Being kept alive
	// here would mean the plugin had silently adopted somebody else's object.
	TestFalse(TEXT("a caller-owned material is not kept alive by having been bound"), WeakCallerMaterial.IsValid());

	// The dead weak reference must not be left in the collection.
	TestEqual(TEXT("dead bindings are pruned"), Ownership.NumMaterialBindings(SharedName), 1);

	Ownership.Release(SharedName);

	// Released: nothing references it any more, so it must actually go.
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS, true);
	TestFalse(TEXT("a plugin-created material is collected after close"), WeakPluginMaterial.IsValid());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSpoutReopenAfterCloseTest,
	"SpoutPlugin.ReceiverOwnership.ReopeningANameStartsWithNoStaleBindings",
	SPOUT_TEST_FLAGS)

bool FSpoutReopenAfterCloseTest::RunTest(const FString& Parameters)
{
	using namespace SpoutPluginTests;

	FSpoutReceiverOwnership Ownership;

	const FName SharedName(TEXT("ReopenSession"));
	const FName ParameterName(TEXT("SpoutTexture"));

	UTexture2D* FirstTexture = MakeTestTexture();
	UMaterialInstanceDynamic* FirstMaterial = MakeTestMaterial();
	if (!FirstTexture || !FirstMaterial)
	{
		AddError(TEXT("could not create the first session's objects"));
		return false;
	}

	Ownership.AdoptTexture(SharedName, FirstTexture);
	FirstMaterial->SetTextureParameterValue(ParameterName, FirstTexture);
	Ownership.RegisterMaterialBinding(SharedName, FirstMaterial, ParameterName, /*bPluginCreated*/ true);

	Ownership.Release(SharedName);

	// Reopen the same name, as a Blueprint calling Spout Receiver again after Close Receiver does.
	UTexture2D* SecondTexture = MakeTestTexture();
	if (!SecondTexture)
	{
		AddError(TEXT("could not create the second session's texture"));
		return false;
	}
	Ownership.AdoptTexture(SharedName, SecondTexture);

	TestEqual(TEXT("the reopened session starts with no bindings"), Ownership.NumMaterialBindings(SharedName), 0);
	TestFalse(TEXT("the old material is not registered against the new session"),
		Ownership.HasMaterialBinding(SharedName, FirstMaterial, ParameterName));
	TestFalse(TEXT("the old material is not owned by the new session"), Ownership.OwnsMaterial(SharedName, FirstMaterial));
	TestTrue(TEXT("the new session owns the new texture"), Ownership.OwnsTexture(SharedName, SecondTexture));
	TestFalse(TEXT("the new session does not own the old texture"), Ownership.OwnsTexture(SharedName, FirstTexture));

	// The old material stays detached from the old texture; reopening must not resurrect the binding.
	UTexture* Bound = nullptr;
	TestTrue(TEXT("the old material's override exists"), FindTextureOverride(FirstMaterial, ParameterName, Bound));
	TestTrue(TEXT("the old material is still detached from the old texture"), Bound != FirstTexture);
	TestTrue(TEXT("and was not bound to the new texture either"), Bound != SecondTexture);

	return true;
}


// =================================================================================================
// Spout cross-process access locking and frame signalling
//
// The protocol's whole behaviour is driven through ISpoutFrameSyncBackend, which is why these
// exist at all: a successful lock, a lock that times out because a peer holds it, a teardown while
// contended, a restart and a resize are otherwise only reachable with a second process and a real
// named mutex.
// =================================================================================================

namespace SpoutPluginTests
{
	/**
	 * Scriptable stand-in for spoutFrameCount.
	 *
	 * Tracks whether the fake believes this process owns the mutex, so an unbalanced release --
	 * the SDK's own bug, where AllowAccess() calls ReleaseMutex() even after a failed
	 * CheckAccess() -- is detectable rather than invisible.
	 */
	class FFakeFrameSyncBackend final : public ISpoutFrameSyncBackend
	{
	public:
		// Failure and behaviour switches.
		bool bFailCreateMutex = false;
		bool bRefuseAccess = false;
		bool bReportNewFrame = false;

		// Call counters.
		int32 CreateMutexCalls = 0;
		int32 CloseMutexCalls = 0;
		int32 EnableCountCalls = 0;
		int32 CleanupCountCalls = 0;
		int32 CheckCalls = 0;
		int32 AllowCalls = 0;
		int32 SetNewFrameCalls = 0;
		int32 GetNewFrameCalls = 0;

		FString LastName;
		void* LastTexture = nullptr;
		/** The wait the last check was asked for; MAX_uint32 until the first check. */
		uint32 LastTimeoutMs = MAX_uint32;

		/** True while the fake believes this process owns the mutex. */
		bool bHeld = false;

		/** Set if a release ever arrived without a matching successful check. */
		bool bUnbalancedRelease = false;

		virtual bool CreateAccessMutex(const ANSICHAR* Utf8Name) override
		{
			++CreateMutexCalls;
			LastName = FString(UTF8_TO_TCHAR(Utf8Name));
			return !bFailCreateMutex;
		}

		virtual void CloseAccessMutex() override { ++CloseMutexCalls; }

		virtual void EnableFrameCount(const ANSICHAR* Utf8Name) override
		{
			++EnableCountCalls;
			LastName = FString(UTF8_TO_TCHAR(Utf8Name));
		}

		virtual void CleanupFrameCount() override { ++CleanupCountCalls; }

		virtual bool CheckTextureAccess(void* SharedTexture, uint32 TimeoutMs) override
		{
			++CheckCalls;
			LastTexture = SharedTexture;
			LastTimeoutMs = TimeoutMs;
			if (bRefuseAccess)
			{
				return false;
			}
			bHeld = true;
			return true;
		}

		virtual bool AllowTextureAccess(void* SharedTexture) override
		{
			++AllowCalls;
			if (!bHeld)
			{
				bUnbalancedRelease = true;
			}
			bHeld = false;
			return true;
		}

		virtual void SetNewFrame() override { ++SetNewFrameCalls; }

		virtual bool GetNewFrame() override { ++GetNewFrameCalls; return bReportNewFrame; }
	};

	/** A distinct non-null pointer standing in for a shared DX11 texture. */
	void* const FakeSharedTexture = reinterpret_cast<void*>(static_cast<UPTRINT>(0xA000));

	/** Utf8 name in the form the protocol consumes. */
	TArray<ANSICHAR> SyncName(const TCHAR* Name) { return Utf8(Name); }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSpoutFrameSyncSuccessfulLockTest,
	"SpoutPlugin.FrameSync.SuccessfulLockIsTakenAndGivenBackExactlyOnce",
	SPOUT_TEST_FLAGS)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutFrameSyncOpenFailureTest,
	"SpoutPlugin.P0.MutexCreationFailureClosesAccessAndCanRetry", SPOUT_TEST_FLAGS)
bool FSpoutFrameSyncOpenFailureTest::RunTest(const FString& Parameters)
{
	using namespace SpoutPluginTests;
	TUniquePtr<FFakeFrameSyncBackend> Owned = MakeUnique<FFakeFrameSyncBackend>();
	FFakeFrameSyncBackend* Fake = Owned.Get();
	Fake->bFailCreateMutex = true;
	FSpoutFrameSync Sync(MoveTemp(Owned));
	TestFalse(TEXT("failed mutex creation refuses open"), Sync.Open("SpoutP0_CreateFailure"));
	TestFalse(TEXT("failed open does not become ready"), Sync.IsOpen());
	{
		FSpoutTextureAccessScope Access(Sync, FakeSharedTexture);
		TestFalse(TEXT("failed open cannot allow an unsynchronized copy"), Access.IsAcquired());
	}
	TestEqual(TEXT("closed access does not consult permissive backend"), Fake->CheckCalls, 0);
	TestEqual(TEXT("frame counting not enabled after failed mutex creation"), Fake->EnableCountCalls, 0);
	Fake->bFailCreateMutex = false;
	TestTrue(TEXT("later open retries successfully"), Sync.Open("SpoutP0_CreateFailure"));
	{
		FSpoutTextureAccessScope Access(Sync, FakeSharedTexture);
		TestTrue(TEXT("successful retry permits access"), Access.IsAcquired());
	}
	TestEqual(TEXT("only acquired access is released"), Fake->AllowCalls, 1);
	return true;
}

bool FSpoutFrameSyncSuccessfulLockTest::RunTest(const FString& Parameters)
{
	using namespace SpoutPluginTests;

	TUniquePtr<FFakeFrameSyncBackend> Owned = MakeUnique<FFakeFrameSyncBackend>();
	FFakeFrameSyncBackend* Fake = Owned.Get();
	FSpoutFrameSync Sync(MoveTemp(Owned));

	const TArray<ANSICHAR> Name = SyncName(TEXT("LockOk"));
	TestTrue(TEXT("opening the protocol reports the access mutex is available"), Sync.Open(Name.GetData()));
	TestTrue(TEXT("the protocol is open"), Sync.IsOpen());
	TestEqual(TEXT("the access mutex was created once"), Fake->CreateMutexCalls, 1);
	TestEqual(TEXT("frame counting was enabled once"), Fake->EnableCountCalls, 1);
	TestEqual(TEXT("under the requested name"), Fake->LastName, FString(TEXT("LockOk")));

	{
		FSpoutTextureAccessScope Access(Sync, FakeSharedTexture);
		TestTrue(TEXT("the lock was granted"), Access.IsAcquired());
		TestTrue(TEXT("and is held for the scope"), Sync.IsAccessHeld());
		TestEqual(TEXT("exactly one check was issued"), Fake->CheckCalls, 1);
		TestEqual(TEXT("and nothing was released while still inside the scope"), Fake->AllowCalls, 0);
		TestTrue(TEXT("the shared texture was passed through unchanged"), Fake->LastTexture == FakeSharedTexture);
	}

	TestFalse(TEXT("the lock is no longer held after the scope"), Sync.IsAccessHeld());
	TestEqual(TEXT("it was released exactly once"), Fake->AllowCalls, 1);
	TestFalse(TEXT("and the release was balanced"), Fake->bUnbalancedRelease);
	TestEqual(TEXT("the counters agree"), Sync.GetAcquireCount(), 1);
	TestEqual(TEXT("acquires and releases match"), Sync.GetReleaseCount(), 1);
	TestEqual(TEXT("no acquire failed"), Sync.GetFailedAcquireCount(), 0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSpoutFrameSyncFailedLockTest,
	"SpoutPlugin.FrameSync.FailedLockIsNeverReleased",
	SPOUT_TEST_FLAGS)

bool FSpoutFrameSyncFailedLockTest::RunTest(const FString& Parameters)
{
	using namespace SpoutPluginTests;

	TUniquePtr<FFakeFrameSyncBackend> Owned = MakeUnique<FFakeFrameSyncBackend>();
	FFakeFrameSyncBackend* Fake = Owned.Get();
	FSpoutFrameSync Sync(MoveTemp(Owned));

	const TArray<ANSICHAR> Name = SyncName(TEXT("LockBusy"));
	Sync.Open(Name.GetData());

	// A peer is holding the texture, so the wait times out.
	Fake->bRefuseAccess = true;

	{
		FSpoutTextureAccessScope Access(Sync, FakeSharedTexture);
		TestFalse(TEXT("the lock was refused"), Access.IsAcquired());
		TestFalse(TEXT("and nothing is held"), Sync.IsAccessHeld());
	}

	// This is the whole point of the scope type. Releasing here would hand away a mutex this
	// process never took, which is exactly what the SDK's own AllowAccess() does.
	TestEqual(TEXT("a failed lock issues no release"), Fake->AllowCalls, 0);
	TestFalse(TEXT("so nothing unbalanced ever reached the backend"), Fake->bUnbalancedRelease);
	TestEqual(TEXT("the failure is counted"), Sync.GetFailedAcquireCount(), 1);
	TestEqual(TEXT("and no successful acquire was recorded"), Sync.GetAcquireCount(), 0);
	TestEqual(TEXT("nor any release"), Sync.GetReleaseCount(), 0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSpoutFrameSyncRepeatedLockTest,
	"SpoutPlugin.FrameSync.RepeatedLockAndUnlockStayBalanced",
	SPOUT_TEST_FLAGS)

bool FSpoutFrameSyncRepeatedLockTest::RunTest(const FString& Parameters)
{
	using namespace SpoutPluginTests;

	TUniquePtr<FFakeFrameSyncBackend> Owned = MakeUnique<FFakeFrameSyncBackend>();
	FFakeFrameSyncBackend* Fake = Owned.Get();
	FSpoutFrameSync Sync(MoveTemp(Owned));

	const TArray<ANSICHAR> Name = SyncName(TEXT("LockRepeat"));
	Sync.Open(Name.GetData());

	// Ten frames, with the fourth and eighth contended, as a real session would be.
	for (int32 Frame = 0; Frame < 10; ++Frame)
	{
		Fake->bRefuseAccess = (Frame == 3 || Frame == 7);
		FSpoutTextureAccessScope Access(Sync, FakeSharedTexture);
		TestTrue(TEXT("the scope's answer matches what the backend decided"),
			Access.IsAcquired() == !Fake->bRefuseAccess);
	}

	TestEqual(TEXT("every frame issued one check"), Fake->CheckCalls, 10);
	TestEqual(TEXT("the eight granted frames released once each"), Fake->AllowCalls, 8);
	TestEqual(TEXT("the two contended frames were counted as failures"), Sync.GetFailedAcquireCount(), 2);
	TestEqual(TEXT("acquires match releases"), Sync.GetAcquireCount(), Sync.GetReleaseCount());
	TestEqual(TEXT("and both equal the granted frame count"), Sync.GetAcquireCount(), 8);
	TestFalse(TEXT("no release was ever unbalanced"), Fake->bUnbalancedRelease);
	TestFalse(TEXT("nothing is left held"), Sync.IsAccessHeld());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSpoutFrameSyncNestedLockTest,
	"SpoutPlugin.FrameSync.NestedLockIsRefused",
	SPOUT_TEST_FLAGS)

bool FSpoutFrameSyncNestedLockTest::RunTest(const FString& Parameters)
{
	using namespace SpoutPluginTests;

	TUniquePtr<FFakeFrameSyncBackend> Owned = MakeUnique<FFakeFrameSyncBackend>();
	FFakeFrameSyncBackend* Fake = Owned.Get();
	FSpoutFrameSync Sync(MoveTemp(Owned));

	const TArray<ANSICHAR> Name = SyncName(TEXT("LockNested"));
	Sync.Open(Name.GetData());

	// The refusal logs a warning, which the automation framework records against this test. That is
	// deliberate: the condition is a programming error worth seeing in a real session, and it is
	// left at Warning rather than being silenced for the test's convenience.
	{
		FSpoutTextureAccessScope Outer(Sync, FakeSharedTexture);
		TestTrue(TEXT("the outer scope holds the lock"), Outer.IsAcquired());

		{
			// A Windows mutex is recursive for its owner, so the SDK would grant this and then need
			// two releases. One scope exit would leave it locked for every other process.
			FSpoutTextureAccessScope Inner(Sync, FakeSharedTexture);
			TestFalse(TEXT("the nested request is refused"), Inner.IsAcquired());
		}

		TestTrue(TEXT("the outer scope still holds it"), Sync.IsAccessHeld());
		TestEqual(TEXT("the nested request never reached the backend"), Fake->CheckCalls, 1);
		TestEqual(TEXT("and released nothing on its way out"), Fake->AllowCalls, 0);
	}

	TestEqual(TEXT("only the outer scope released"), Fake->AllowCalls, 1);
	TestFalse(TEXT("balanced"), Fake->bUnbalancedRelease);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSpoutFrameSyncShutdownWhileContendedTest,
	"SpoutPlugin.FrameSync.ShutdownWhileContendedClosesCleanly",
	SPOUT_TEST_FLAGS)

bool FSpoutFrameSyncShutdownWhileContendedTest::RunTest(const FString& Parameters)
{
	using namespace SpoutPluginTests;

	TUniquePtr<FFakeFrameSyncBackend> Owned = MakeUnique<FFakeFrameSyncBackend>();
	FFakeFrameSyncBackend* Fake = Owned.Get();
	FSpoutFrameSync Sync(MoveTemp(Owned));

	const TArray<ANSICHAR> Name = SyncName(TEXT("LockShutdown"));
	Sync.Open(Name.GetData());

	// A peer holds the texture and never gives it back; the session is torn down anyway.
	Fake->bRefuseAccess = true;
	Fake->bReportNewFrame = true;

	{
		FSpoutTextureAccessScope Access(Sync, FakeSharedTexture);
		TestFalse(TEXT("the wait timed out"), Access.IsAcquired());
	}

	// The frame reported new was never copied, so it is still pending when teardown begins.
	TestTrue(TEXT("a frame is pending at teardown"), Sync.ConsumeNewFrame());

	Sync.Close();

	TestFalse(TEXT("the protocol is closed"), Sync.IsOpen());
	TestFalse(TEXT("nothing is held"), Sync.IsAccessHeld());
	TestFalse(TEXT("the pending frame was dropped with the session"), Sync.HasPendingFrame());
	TestEqual(TEXT("the access mutex was closed once"), Fake->CloseMutexCalls, 1);
	TestEqual(TEXT("frame counting was cleaned up once"), Fake->CleanupCountCalls, 1);
	TestFalse(TEXT("teardown released nothing it did not own"), Fake->bUnbalancedRelease);

	// Teardown runs twice in some paths (explicit close, then destruction).
	Sync.Close();
	TestEqual(TEXT("a second close is a no-op"), Fake->CloseMutexCalls, 1);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSpoutFrameSyncRestartTest,
	"SpoutPlugin.FrameSync.RestartReopensTheProtocolFromScratch",
	SPOUT_TEST_FLAGS)

bool FSpoutFrameSyncRestartTest::RunTest(const FString& Parameters)
{
	using namespace SpoutPluginTests;

	TUniquePtr<FFakeFrameSyncBackend> Owned = MakeUnique<FFakeFrameSyncBackend>();
	FFakeFrameSyncBackend* Fake = Owned.Get();
	FSpoutFrameSync Sync(MoveTemp(Owned));

	const TArray<ANSICHAR> Name = SyncName(TEXT("LockRestart"));

	Sync.Open(Name.GetData());
	Fake->bReportNewFrame = true;
	TestTrue(TEXT("a frame arrives before the restart"), Sync.ConsumeNewFrame());

	// Close/reopen is what a Close Receiver followed by another Spout Receiver call does.
	Sync.Close();
	TestFalse(TEXT("the pending frame did not survive the close"), Sync.HasPendingFrame());

	TestTrue(TEXT("reopening succeeds"), Sync.Open(Name.GetData()));
	TestTrue(TEXT("the protocol is open again"), Sync.IsOpen());
	TestEqual(TEXT("the access mutex was created a second time"), Fake->CreateMutexCalls, 2);
	TestEqual(TEXT("and frame counting enabled a second time"), Fake->EnableCountCalls, 2);

	// A restarted stream must be able to lock and publish immediately.
	{
		FSpoutTextureAccessScope Access(Sync, FakeSharedTexture);
		TestTrue(TEXT("the restarted stream can lock"), Access.IsAcquired());
	}
	TestFalse(TEXT("and the restart left nothing unbalanced"), Fake->bUnbalancedRelease);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSpoutFrameSyncResizeTest,
	"SpoutPlugin.FrameSync.ResizeDoesNotDisturbTheProtocol",
	SPOUT_TEST_FLAGS)

bool FSpoutFrameSyncResizeTest::RunTest(const FString& Parameters)
{
	using namespace SpoutPluginTests;

	TUniquePtr<FFakeFrameSyncBackend> Owned = MakeUnique<FFakeFrameSyncBackend>();
	FFakeFrameSyncBackend* Fake = Owned.Get();
	FSpoutFrameSync Sync(MoveTemp(Owned));

	const TArray<ANSICHAR> Name = SyncName(TEXT("LockResize"));
	Sync.Open(Name.GetData());

	// A resize recreates the shared texture and republishes the registration, which calls Open
	// again. The mutex and the semaphore belong to the NAME, so they must survive untouched --
	// recreating them would drop a lock a peer is waiting on.
	for (int32 Resize = 0; Resize < 3; ++Resize)
	{
		Sync.Open(Name.GetData());
	}

	TestEqual(TEXT("the access mutex was created exactly once across three resizes"), Fake->CreateMutexCalls, 1);
	TestEqual(TEXT("frame counting was enabled exactly once"), Fake->EnableCountCalls, 1);
	TestEqual(TEXT("and nothing was closed"), Fake->CloseMutexCalls, 0);
	TestTrue(TEXT("the protocol is still open"), Sync.IsOpen());

	// Opening under a different name is a different stream and must not silently keep the old pair.
	const TArray<ANSICHAR> OtherName = SyncName(TEXT("LockResizeOther"));
	Sync.Open(OtherName.GetData());
	TestEqual(TEXT("changing the name closes the previous mutex"), Fake->CloseMutexCalls, 1);
	TestEqual(TEXT("and creates the new one"), Fake->CreateMutexCalls, 2);
	TestEqual(TEXT("under the new name"), Fake->LastName, FString(TEXT("LockResizeOther")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSpoutFrameSyncNoBackendTest,
	"SpoutPlugin.FrameSync.WithoutABackendCopiesAreRefused",
	SPOUT_TEST_FLAGS)

bool FSpoutFrameSyncNoBackendTest::RunTest(const FString& Parameters)
{
	using namespace SpoutPluginTests;

	// This is the Spout-unavailable case: MakeSpoutSdkFrameSyncBackend returns null when the DLL
	// never loaded. A missing synchronization mechanism must not allow unsafe copies.
	FSpoutFrameSync Sync(nullptr);

	const TArray<ANSICHAR> Name = SyncName(TEXT("LockNoBackend"));
	TestFalse(TEXT("opening reports no access mutex"), Sync.Open(Name.GetData()));
	TestFalse(TEXT("and the protocol is not open"), Sync.IsOpen());

	TestFalse(TEXT("closed protocol offers no frame"), Sync.ConsumeNewFrame());
	TestFalse(TEXT("and remains closed on the next call"), Sync.ConsumeNewFrame());

	{
		FSpoutTextureAccessScope Access(Sync, FakeSharedTexture);
		TestFalse(TEXT("access is refused"), Access.IsAcquired());
	}

	TestEqual(TEXT("and the counters stay balanced"), Sync.GetAcquireCount(), Sync.GetReleaseCount());
	TestEqual(TEXT("with no acquire recorded"), Sync.GetAcquireCount(), 0);
	TestFalse(TEXT("nothing is left held"), Sync.IsAccessHeld());

	// Nothing can be published through a closed protocol.
	Sync.PublishFrame();
	TestEqual(TEXT("no publication recorded without a backend"), Sync.GetPublishedFrameCount(), 0);

	return true;
}

// =================================================================================================
// Frame publication and new-frame detection
// =================================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSpoutFramePublishTest,
	"SpoutPlugin.FrameProtocol.PublishingAFrameSignalsThePeer",
	SPOUT_TEST_FLAGS)

bool FSpoutFramePublishTest::RunTest(const FString& Parameters)
{
	using namespace SpoutPluginTests;

	TUniquePtr<FFakeFrameSyncBackend> Owned = MakeUnique<FFakeFrameSyncBackend>();
	FFakeFrameSyncBackend* Fake = Owned.Get();
	FSpoutFrameSync Sync(MoveTemp(Owned));

	const TArray<ANSICHAR> Name = SyncName(TEXT("PublishOne"));
	Sync.Open(Name.GetData());

	TestEqual(TEXT("nothing is published before the first frame"), Fake->SetNewFrameCalls, 0);

	// The send path publishes inside the access scope, after the copy is submitted.
	for (int32 Frame = 0; Frame < 3; ++Frame)
	{
		FSpoutTextureAccessScope Access(Sync, FakeSharedTexture);
		TestTrue(TEXT("the sender holds the texture while publishing"), Access.IsAcquired());
		Sync.PublishFrame();
	}

	TestEqual(TEXT("three frames were announced"), Fake->SetNewFrameCalls, 3);
	TestEqual(TEXT("and counted"), Sync.GetPublishedFrameCount(), 3);
	TestEqual(TEXT("each under its own lock"), Fake->AllowCalls, 3);
	TestFalse(TEXT("balanced"), Fake->bUnbalancedRelease);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSpoutFrameNewDetectionTest,
	"SpoutPlugin.FrameProtocol.NewFrameIsDetectedAndConsumedOnce",
	SPOUT_TEST_FLAGS)

bool FSpoutFrameNewDetectionTest::RunTest(const FString& Parameters)
{
	using namespace SpoutPluginTests;

	TUniquePtr<FFakeFrameSyncBackend> Owned = MakeUnique<FFakeFrameSyncBackend>();
	FFakeFrameSyncBackend* Fake = Owned.Get();
	FSpoutFrameSync Sync(MoveTemp(Owned));

	const TArray<ANSICHAR> Name = SyncName(TEXT("NewFrame"));
	Sync.Open(Name.GetData());

	Fake->bReportNewFrame = true;
	TestTrue(TEXT("a published frame is reported new"), Sync.ConsumeNewFrame());
	TestTrue(TEXT("and is pending until copied"), Sync.HasPendingFrame());
	TestEqual(TEXT("the peer was asked once"), Fake->GetNewFrameCalls, 1);

	// The copy landed.
	Sync.MarkFrameConsumed();
	TestFalse(TEXT("the frame is no longer pending"), Sync.HasPendingFrame());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSpoutFrameRepeatedTest,
	"SpoutPlugin.FrameProtocol.RepeatedFrameIsNotReportedNew",
	SPOUT_TEST_FLAGS)

bool FSpoutFrameRepeatedTest::RunTest(const FString& Parameters)
{
	using namespace SpoutPluginTests;

	TUniquePtr<FFakeFrameSyncBackend> Owned = MakeUnique<FFakeFrameSyncBackend>();
	FFakeFrameSyncBackend* Fake = Owned.Get();
	FSpoutFrameSync Sync(MoveTemp(Owned));

	const TArray<ANSICHAR> Name = SyncName(TEXT("RepeatFrame"));
	Sync.Open(Name.GetData());

	// One new frame, copied.
	Fake->bReportNewFrame = true;
	TestTrue(TEXT("the first frame is new"), Sync.ConsumeNewFrame());
	Sync.MarkFrameConsumed();

	// The sender is now idle: the shared texture still holds the same frame. This is what makes the
	// receiver skip its transitions, its RHI flush and its GPU copy.
	Fake->bReportNewFrame = false;
	for (int32 Tick = 0; Tick < 5; ++Tick)
	{
		TestFalse(TEXT("an unchanged frame is not reported new"), Sync.ConsumeNewFrame());
	}
	TestFalse(TEXT("and nothing is pending"), Sync.HasPendingFrame());
	TestEqual(TEXT("the peer was asked on every tick"), Fake->GetNewFrameCalls, 6);

	// The sender publishes again.
	Fake->bReportNewFrame = true;
	TestTrue(TEXT("the next published frame is new again"), Sync.ConsumeNewFrame());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSpoutFrameSkippedTest,
	"SpoutPlugin.FrameProtocol.SkippedFrameStaysPendingUntilItIsCopied",
	SPOUT_TEST_FLAGS)

bool FSpoutFrameSkippedTest::RunTest(const FString& Parameters)
{
	using namespace SpoutPluginTests;

	TUniquePtr<FFakeFrameSyncBackend> Owned = MakeUnique<FFakeFrameSyncBackend>();
	FFakeFrameSyncBackend* Fake = Owned.Get();
	FSpoutFrameSync Sync(MoveTemp(Owned));

	const TArray<ANSICHAR> Name = SyncName(TEXT("SkipFrame"));
	Sync.Open(Name.GetData());

	// A frame arrives, but the access wait times out so no copy happens.
	Fake->bReportNewFrame = true;
	TestTrue(TEXT("the frame is reported new"), Sync.ConsumeNewFrame());

	Fake->bRefuseAccess = true;
	{
		FSpoutTextureAccessScope Access(Sync, FakeSharedTexture);
		TestFalse(TEXT("the copy could not take the lock"), Access.IsAcquired());
	}

	// Asking the peer again would answer "not new" -- the semaphore was already decremented. If the
	// answer were re-asked rather than latched, this frame would be lost outright.
	Fake->bReportNewFrame = false;
	TestTrue(TEXT("the uncopied frame is still pending on the next call"), Sync.ConsumeNewFrame());
	TestTrue(TEXT("and again on the one after that"), Sync.ConsumeNewFrame());

	// The lock frees up and the copy lands.
	Fake->bRefuseAccess = false;
	{
		FSpoutTextureAccessScope Access(Sync, FakeSharedTexture);
		TestTrue(TEXT("the lock is granted once the peer lets go"), Access.IsAcquired());
		Sync.MarkFrameConsumed();
	}

	TestFalse(TEXT("the frame is finally consumed"), Sync.ConsumeNewFrame());
	TestFalse(TEXT("and nothing stays pending"), Sync.HasPendingFrame());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSpoutFrameReceiverRestartTest,
	"SpoutPlugin.FrameProtocol.ReceiverRestartStartsFromNoPendingFrame",
	SPOUT_TEST_FLAGS)

bool FSpoutFrameReceiverRestartTest::RunTest(const FString& Parameters)
{
	using namespace SpoutPluginTests;

	TUniquePtr<FFakeFrameSyncBackend> Owned = MakeUnique<FFakeFrameSyncBackend>();
	FFakeFrameSyncBackend* Fake = Owned.Get();
	FSpoutFrameSync Sync(MoveTemp(Owned));

	const TArray<ANSICHAR> Name = SyncName(TEXT("RestartFrames"));
	Sync.Open(Name.GetData());

	// A frame is outstanding when the receiver is closed.
	Fake->bReportNewFrame = true;
	TestTrue(TEXT("a frame is pending before the restart"), Sync.ConsumeNewFrame());
	TestTrue(TEXT("confirmed pending"), Sync.HasPendingFrame());

	Sync.Close();
	Sync.Open(Name.GetData());

	// A restarted receiver must not believe it already has a frame from the previous session: the
	// destination texture it would copy into is a different object.
	TestFalse(TEXT("the restarted receiver has no carried-over pending frame"), Sync.HasPendingFrame());

	Fake->bReportNewFrame = false;
	TestFalse(TEXT("and does not copy until the sender publishes again"), Sync.ConsumeNewFrame());

	Fake->bReportNewFrame = true;
	TestTrue(TEXT("then it does"), Sync.ConsumeNewFrame());

	return true;
}

// =================================================================================================
// RHI resource-state contract
// =================================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSpoutRHIStateContractTest,
	"SpoutPlugin.RHIState.CopyBracketsMatchTheDocumentedContract",
	SPOUT_TEST_FLAGS)

bool FSpoutRHIStateContractTest::RunTest(const FString& Parameters)
{
	using namespace SpoutRHIState;

	const FCopyBracket BackBuffer = GetCopyBracket(EResourceRole::ViewportBackBuffer);
	TestEqual(TEXT("the back buffer is copied from"), static_cast<int32>(BackBuffer.CopyAccess), static_cast<int32>(ERHIAccess::CopySrc));
	// SRVGraphics, not Present. UE 5.3/5.4 put the back buffer in SRVGraphics immediately before
	// broadcasting and transition it back OUT of SRVGraphics afterwards, so restoring anything else
	// invalidates the engine's own next transition.
	TestEqual(TEXT("and restored to SRVGraphics, exactly as the engine left it"),
		static_cast<int32>(BackBuffer.ExitAfter), static_cast<int32>(ERHIAccess::SRVGraphics));
	TestTrue(TEXT("it is a copy source"), BackBuffer.bIsCopySource);

	const FCopyBracket SenderRT = GetCopyBracket(EResourceRole::SenderRenderTarget);
	TestEqual(TEXT("a sender render target is copied from"), static_cast<int32>(SenderRT.CopyAccess), static_cast<int32>(ERHIAccess::CopySrc));
	TestEqual(TEXT("and restored to SRVMask so a later compute read is covered"),
		static_cast<int32>(SenderRT.ExitAfter), static_cast<int32>(ERHIAccess::SRVMask));
	TestTrue(TEXT("it is a copy source"), SenderRT.bIsCopySource);

	const FCopyBracket ReceiverTexture = GetCopyBracket(EResourceRole::ReceiverTexture);
	TestEqual(TEXT("the received texture is copied into"), static_cast<int32>(ReceiverTexture.CopyAccess), static_cast<int32>(ERHIAccess::CopyDest));
	TestEqual(TEXT("and restored to SRVMask for sampling"), static_cast<int32>(ReceiverTexture.ExitAfter), static_cast<int32>(ERHIAccess::SRVMask));
	TestFalse(TEXT("it is a copy destination"), ReceiverTexture.bIsCopySource);

	const FCopyBracket ReceiverRT = GetCopyBracket(EResourceRole::ReceiverRenderTarget);
	TestEqual(TEXT("a receiver render target is copied into"), static_cast<int32>(ReceiverRT.CopyAccess), static_cast<int32>(ERHIAccess::CopyDest));
	TestEqual(TEXT("and restored to SRVMask"), static_cast<int32>(ReceiverRT.ExitAfter), static_cast<int32>(ERHIAccess::SRVMask));
	TestFalse(TEXT("it is a copy destination"), ReceiverRT.bIsCopySource);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSpoutRHIStateUnknownPolicyTest,
	"SpoutPlugin.RHIState.EntryDeclaresUnknownAndExitNeverDoes",
	SPOUT_TEST_FLAGS)

bool FSpoutRHIStateUnknownPolicyTest::RunTest(const FString& Parameters)
{
	using namespace SpoutRHIState;

	const EResourceRole Roles[] =
	{
		EResourceRole::ViewportBackBuffer,
		EResourceRole::SenderRenderTarget,
		EResourceRole::ReceiverTexture,
		EResourceRole::ReceiverRenderTarget
	};

	for (EResourceRole Role : Roles)
	{
		const FCopyBracket Bracket = GetCopyBracket(Role);
		const FString Name(ToString(Role));

		// Entering: this plugin owns none of these resources' prior access, so it declares none.
		TestEqual(*(Name + TEXT(": the entry transition declares Unknown")),
			static_cast<int32>(Bracket.EntryBefore), static_cast<int32>(ERHIAccess::Unknown));

		// Leaving: the plugin DOES own the state it just set, so the restore states it. An exit
		// that also declared Unknown would be the blanket use this contract exists to avoid.
		TestTrue(*(Name + TEXT(": the exit transition never declares Unknown")),
			Bracket.ExitAfter != ERHIAccess::Unknown);
		TestTrue(*(Name + TEXT(": the copy access is a real state")),
			Bracket.CopyAccess != ERHIAccess::Unknown);

		// And the two ends of the bracket differ, so the restore is a real transition rather than a
		// redundant barrier back to the state the resource is already in.
		TestTrue(*(Name + TEXT(": the copy access and the exit access differ")),
			Bracket.CopyAccess != Bracket.ExitAfter);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSpoutRHIStateWrapPairingTest,
	"SpoutPlugin.RHIState.WrapStateMatchesTheCopyAccess",
	SPOUT_TEST_FLAGS)

bool FSpoutRHIStateWrapPairingTest::RunTest(const FString& Parameters)
{
	using namespace SpoutRHIState;

	const EResourceRole Roles[] =
	{
		EResourceRole::ViewportBackBuffer,
		EResourceRole::SenderRenderTarget,
		EResourceRole::ReceiverTexture,
		EResourceRole::ReceiverRenderTarget
	};

	// AcquireWrappedResources requires the resource to ALREADY be in the state declared at
	// CreateWrappedResource time. That state and the ERHIAccess the resource was transitioned into
	// therefore have to describe the same thing; if they ever disagree, the acquire is invalid.
	for (EResourceRole Role : Roles)
	{
		const FCopyBracket Bracket = GetCopyBracket(Role);
		const FString Name(ToString(Role));

		if (Bracket.CopyAccess == ERHIAccess::CopySrc)
		{
			TestTrue(*(Name + TEXT(": a CopySrc bracket wraps as CopySource")),
				Bracket.WrapState == EWrapState::CopySource);
		}
		else
		{
			TestTrue(*(Name + TEXT(": a CopyDest bracket wraps as CopyDest")),
				Bracket.WrapState == EWrapState::CopyDest);
		}

		// The direction flag and the copy access must agree too, since the send and receive paths
		// pick their role from it.
		TestTrue(*(Name + TEXT(": the direction flag agrees with the copy access")),
			Bracket.bIsCopySource == (Bracket.CopyAccess == ERHIAccess::CopySrc));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSpoutDestinationIdentityTest,
	"SpoutPlugin.RHIState.DestinationIsRecreatedOnResizeFormatOrColorSpaceChange",
	SPOUT_TEST_FLAGS)

bool FSpoutDestinationIdentityTest::RunTest(const FString& Parameters)
{
	// The receive path reuses its destination texture between frames and only recreates it when the
	// incoming frame stops matching. Getting that identity wrong means copying a frame into a
	// destination with a different extent or layout, which is a dropped or reinterpreted frame.
	UTexture2D* Texture = nullptr;

	TestTrue(TEXT("the first call creates the destination"),
		SpoutTextureUtils::EnsureTransientTexture(Texture, 8, 8, PF_B8G8R8A8, /*bSRGB*/ true));
	if (!Texture)
	{
		AddError(TEXT("the destination texture could not be created"));
		return false;
	}

	UTexture2D* const First = Texture;
	TestFalse(TEXT("an unchanged frame reuses it"),
		SpoutTextureUtils::EnsureTransientTexture(Texture, 8, 8, PF_B8G8R8A8, true));
	TestTrue(TEXT("and it is the same object"), Texture == First);

	// Resize.
	TestTrue(TEXT("a resized frame recreates it"),
		SpoutTextureUtils::EnsureTransientTexture(Texture, 16, 8, PF_B8G8R8A8, true));
	TestTrue(TEXT("giving a different object"), Texture != First);
	TestEqual(TEXT("with the new width"), Texture ? Texture->GetSizeX() : 0, 16);

	// Format change at the same extent. Reusing here would copy one layout into another.
	UTexture2D* const Resized = Texture;
	TestTrue(TEXT("a format change recreates it"),
		SpoutTextureUtils::EnsureTransientTexture(Texture, 16, 8, PF_FloatRGBA, false));
	TestTrue(TEXT("giving a different object again"), Texture != Resized);

	// Colour-space change at the same extent and format. This is the colour-space half of the
	// identity: switching a receiver between Auto and Linear has to rebuild the SRV, not
	// reinterpret it.
	UTexture2D* const Reformatted = Texture;
	TestTrue(TEXT("a colour-space change recreates it"),
		SpoutTextureUtils::EnsureTransientTexture(Texture, 16, 8, PF_FloatRGBA, true));
	TestTrue(TEXT("giving a different object"), Texture != Reformatted);
	TestTrue(TEXT("with the requested sRGB setting"), Texture && Texture->SRGB != 0);

	return true;
}

// =================================================================================================
// Colour-space contract
// =================================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSpoutColorSpaceAutoTest,
	"SpoutPlugin.ColorSpace.AutoFollowsTheFormatConvention",
	SPOUT_TEST_FLAGS)

bool FSpoutColorSpaceAutoTest::RunTest(const FString& Parameters)
{
	using namespace SpoutFormatPolicy;

	struct FCase
	{
		DXGI_FORMAT Format;
		EPixelFormat Expected;
		bool bExpectSRGB;
		const TCHAR* Label;
	};

	// The four supported families, in both directions.
	const FCase Cases[] =
	{
		{ DXGI_FORMAT_B8G8R8A8_UNORM,      PF_B8G8R8A8,    true,  TEXT("BGRA8") },
		{ DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, PF_B8G8R8A8,    true,  TEXT("BGRA8 sRGB") },
		{ DXGI_FORMAT_R8G8B8A8_UNORM,      PF_R8G8B8A8,    true,  TEXT("RGBA8") },
		{ DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, PF_R8G8B8A8,    true,  TEXT("RGBA8 sRGB") },
		{ DXGI_FORMAT_R10G10B10A2_UNORM,   PF_A2B10G10R10, false, TEXT("RGB10A2") },
		{ DXGI_FORMAT_R16G16B16A16_FLOAT,  PF_FloatRGBA,   false, TEXT("RGBA16F") },
	};

	for (const FCase& Case : Cases)
	{
		const FReceiverFormat Resolved = ResolveReceiverFormat(static_cast<uint32>(Case.Format));
		const FString Label(Case.Label);

		TestTrue(*(Label + TEXT(" is usable")), Resolved.IsUsable());
		TestEqual(*(Label + TEXT(" resolves to the expected pixel format")),
			static_cast<int32>(Resolved.PixelFormat), static_cast<int32>(Case.Expected));

		bool bSRGB = !Case.bExpectSRGB;
		const EColorSpaceResult Result = ResolveReceiverColorSpace(Resolved, ESpoutColorSpace::Auto, bSRGB);
		TestTrue(*(Label + TEXT(": Auto is always representable")), Result == EColorSpaceResult::Ok);
		TestTrue(*(Label + TEXT(": Auto gives the documented decode setting")), bSRGB == Case.bExpectSRGB);
	}

	// The sender never publishes _UNORM_SRGB: every 8-bit source is shared as _UNORM with its
	// bytes unchanged. What the source states about its encoding is kept for diagnostics only and
	// is not published, which is the whole reason the receiver has an override for 8-bit _UNORM.
	TestTrue(TEXT("BGRA8 sRGB source is shared as plain BGRA8"),
		ResolveSenderFormat(DXGI_FORMAT_B8G8R8A8_UNORM_SRGB).SharedFormat == DXGI_FORMAT_B8G8R8A8_UNORM);
	TestTrue(TEXT("and still records that its bytes are sRGB-encoded"),
		ResolveSenderFormat(DXGI_FORMAT_B8G8R8A8_UNORM_SRGB).bSourceSRGB);
	TestFalse(TEXT("plain BGRA8 states no encoding even though Auto decodes it as sRGB"),
		ResolveSenderFormat(DXGI_FORMAT_B8G8R8A8_UNORM).bSourceSRGB);
	TestFalse(TEXT("RGBA16F states no sRGB encoding"),
		ResolveSenderFormat(DXGI_FORMAT_R16G16B16A16_FLOAT).bSourceSRGB);

	// A typeless source states nothing, so it maps to the _UNORM view and records no encoding.
	TestFalse(TEXT("typeless BGRA8 states no encoding"),
		ResolveSenderFormat(DXGI_FORMAT_B8G8R8A8_TYPELESS).bSourceSRGB);
	TestFalse(TEXT("and the receiver does not treat it as a declared sRGB format"),
		ResolveReceiverFormat(static_cast<uint32>(DXGI_FORMAT_B8G8R8A8_TYPELESS)).bFormatDeclaresSRGB);
	TestTrue(TEXT("while Auto still applies the 8-bit convention to it"),
		ResolveReceiverFormat(static_cast<uint32>(DXGI_FORMAT_B8G8R8A8_TYPELESS)).bPrefersSRGB);

	// The send path passes the render target's sRGB flag. It must never change the shared format:
	// UE allocates sRGB targets as typeless, and every 8-bit source is shared as _UNORM.
	struct FSendCase { DXGI_FORMAT Native; DXGI_FORMAT Shared; bool bNativeSRGB; };
	const FSendCase SendCases[] =
	{
		{ DXGI_FORMAT_B8G8R8A8_TYPELESS,    DXGI_FORMAT_B8G8R8A8_UNORM,     false },
		{ DXGI_FORMAT_B8G8R8A8_UNORM,       DXGI_FORMAT_B8G8R8A8_UNORM,     false },
		{ DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,  DXGI_FORMAT_B8G8R8A8_UNORM,     true  },
		{ DXGI_FORMAT_R8G8B8A8_TYPELESS,    DXGI_FORMAT_R8G8B8A8_UNORM,     false },
		{ DXGI_FORMAT_R8G8B8A8_UNORM,       DXGI_FORMAT_R8G8B8A8_UNORM,     false },
		{ DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,  DXGI_FORMAT_R8G8B8A8_UNORM,     true  },
		{ DXGI_FORMAT_R10G10B10A2_TYPELESS, DXGI_FORMAT_R10G10B10A2_UNORM,  false },
		{ DXGI_FORMAT_R16G16B16A16_FLOAT,   DXGI_FORMAT_R16G16B16A16_FLOAT, false },
	};
	for (const FSendCase& Case : SendCases)
	{
		const bool bEightBit = Case.Shared == DXGI_FORMAT_B8G8R8A8_UNORM || Case.Shared == DXGI_FORMAT_R8G8B8A8_UNORM;
		for (bool bSourceSRGB : {false, true})
		{
			const FString Label = FString::Printf(TEXT("%s with source sRGB flag %d"), *DescribeDxgiFormat(static_cast<uint32>(Case.Native)), bSourceSRGB ? 1 : 0);
			const FSenderFormat Result = ResolveSenderFormat(Case.Native, bSourceSRGB);
			TestTrue(*(Label + TEXT(" is usable")), Result.IsUsable());
			TestEqual(*(Label + TEXT(" shared format")), static_cast<int32>(Result.SharedFormat), static_cast<int32>(Case.Shared));
			TestEqual(*(Label + TEXT(" records the source encoding")), Result.bSourceSRGB, Case.bNativeSRGB || (bEightBit && bSourceSRGB));
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSpoutColorSpaceOverrideTest,
	"SpoutPlugin.ColorSpace.ExplicitOverridesAreHonouredOrRejected",
	SPOUT_TEST_FLAGS)

bool FSpoutColorSpaceOverrideTest::RunTest(const FString& Parameters)
{
	using namespace SpoutFormatPolicy;

	const FReceiverFormat Bgra8 = ResolveReceiverFormat(static_cast<uint32>(DXGI_FORMAT_B8G8R8A8_UNORM));
	const FReceiverFormat Rgba8 = ResolveReceiverFormat(static_cast<uint32>(DXGI_FORMAT_R8G8B8A8_UNORM));
	const FReceiverFormat Rgb10a2 = ResolveReceiverFormat(static_cast<uint32>(DXGI_FORMAT_R10G10B10A2_UNORM));
	const FReceiverFormat Rgba16f = ResolveReceiverFormat(static_cast<uint32>(DXGI_FORMAT_R16G16B16A16_FLOAT));

	bool bSRGB = true;

	// Linear is always representable: it is what every pixel format does by default.
	TestTrue(TEXT("BGRA8 accepts Linear"), ResolveReceiverColorSpace(Bgra8, ESpoutColorSpace::Linear, bSRGB) == EColorSpaceResult::Ok);
	TestFalse(TEXT("and decodes nothing"), bSRGB);

	bSRGB = true;
	TestTrue(TEXT("RGBA16F accepts Linear"), ResolveReceiverColorSpace(Rgba16f, ESpoutColorSpace::Linear, bSRGB) == EColorSpaceResult::Ok);
	TestFalse(TEXT("and stays linear"), bSRGB);

	// sRGB is representable only where an sRGB view exists.
	bSRGB = false;
	TestTrue(TEXT("BGRA8 accepts sRGB"), ResolveReceiverColorSpace(Bgra8, ESpoutColorSpace::SRGB, bSRGB) == EColorSpaceResult::Ok);
	TestTrue(TEXT("and decodes"), bSRGB);

	bSRGB = false;
	TestTrue(TEXT("RGBA8 accepts sRGB"), ResolveReceiverColorSpace(Rgba8, ESpoutColorSpace::SRGB, bSRGB) == EColorSpaceResult::Ok);
	TestTrue(TEXT("and decodes"), bSRGB);

	// UE has no sRGB variant of these, so setting the flag would be silently ignored and the
	// caller would never learn the request did nothing. Rejected instead.
	bSRGB = true;
	TestTrue(TEXT("RGB10A2 rejects an sRGB override"),
		ResolveReceiverColorSpace(Rgb10a2, ESpoutColorSpace::SRGB, bSRGB) == EColorSpaceResult::UnsupportedOverride);
	TestFalse(TEXT("and writes a defined value even on rejection"), bSRGB);

	bSRGB = true;
	TestTrue(TEXT("RGBA16F rejects an sRGB override"),
		ResolveReceiverColorSpace(Rgba16f, ESpoutColorSpace::SRGB, bSRGB) == EColorSpaceResult::UnsupportedOverride);
	TestFalse(TEXT("and writes a defined value even on rejection"), bSRGB);

	// The capability flag the rejection is derived from.
	TestTrue(TEXT("8-bit formats advertise an sRGB view"), Bgra8.bSupportsSRGBDecode);
	TestFalse(TEXT("10-bit does not"), Rgb10a2.bSupportsSRGBDecode);
	TestFalse(TEXT("float does not"), Rgba16f.bSupportsSRGBDecode);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSpoutColorSpaceUnsupportedFormatTest,
	"SpoutPlugin.ColorSpace.UnsupportedFormatsHaveNoColorSpace",
	SPOUT_TEST_FLAGS)

bool FSpoutColorSpaceUnsupportedFormatTest::RunTest(const FString& Parameters)
{
	using namespace SpoutFormatPolicy;

	struct FCase
	{
		DXGI_FORMAT Format;
		const TCHAR* Label;
	};

	const FCase Cases[] =
	{
		{ DXGI_FORMAT_UNKNOWN,  TEXT("UNKNOWN") },
		{ DXGI_FORMAT_BC1_UNORM, TEXT("BC1 (compressed)") },
		{ DXGI_FORMAT_BC3_UNORM, TEXT("BC3 (compressed)") },
		{ DXGI_FORMAT_BC7_UNORM, TEXT("BC7 (compressed)") },
		{ DXGI_FORMAT_NV12,      TEXT("NV12 (planar)") },
		{ DXGI_FORMAT_P010,      TEXT("P010 (planar)") },
	};

	const ESpoutColorSpace Requests[] = { ESpoutColorSpace::Auto, ESpoutColorSpace::SRGB, ESpoutColorSpace::Linear };

	for (const FCase& Case : Cases)
	{
		const FReceiverFormat Resolved = ResolveReceiverFormat(static_cast<uint32>(Case.Format));
		const FString Label(Case.Label);

		TestFalse(*(Label + TEXT(" is not usable")), Resolved.IsUsable());
		TestEqual(*(Label + TEXT(" resolves to no pixel format")),
			static_cast<int32>(Resolved.PixelFormat), static_cast<int32>(PF_Unknown));

		// No colour space can be resolved for a format the plugin will not share, whatever is
		// requested. The receive path refuses the frame before it ever allocates a destination.
		for (ESpoutColorSpace Request : Requests)
		{
			bool bSRGB = true;
			TestTrue(*(Label + TEXT(": every colour-space request is refused")),
				ResolveReceiverColorSpace(Resolved, Request, bSRGB) == EColorSpaceResult::UnusableFormat);
			TestFalse(*(Label + TEXT(": with a defined output value")), bSRGB);
		}

		// And the sender refuses to publish it, so neither direction reinterprets it.
		TestFalse(*(Label + TEXT(" cannot be published either")), ResolveSenderFormat(Case.Format).IsUsable());
	}

	return true;
}


#undef SPOUT_TEST_FLAGS

#endif // WITH_DEV_AUTOMATION_TESTS
