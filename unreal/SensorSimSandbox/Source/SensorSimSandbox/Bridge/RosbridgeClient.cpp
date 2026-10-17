// Copyright (c) Sensor Sim Sandbox. Private portfolio project.

#include "Bridge/RosbridgeClient.h"

#include "IWebSocket.h"
#include "WebSocketsModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "HAL/IConsoleManager.h"
#include "Containers/Ticker.h"

DEFINE_LOG_CATEGORY_STATIC(LogRosbridgeClient, Log, All);

namespace
{
	/** Serialize a JSON object to a COMPACT (single-line) string, matching
	 *  FTfPublisher so framing is deterministic and whitespace-free. */
	FString JsonToString(const TSharedRef<FJsonObject>& Obj)
	{
		FString Out;
		using FCondensedWriter = TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>;
		using FCondensedWriterFactory = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>;
		TSharedRef<FCondensedWriter> Writer = FCondensedWriterFactory::Create(&Out);
		FJsonSerializer::Serialize(Obj, Writer);
		return Out;
	}

	FString StripComment(const FString& InLine)
	{
		FString Line = InLine;
		int32 HashPos = INDEX_NONE;
		if (Line.FindChar(TEXT('#'), HashPos))
		{
			Line = Line.Left(HashPos);
		}
		return Line.TrimStartAndEnd();
	}

	bool SplitKeyValue(const FString& Line, FString& OutKey, FString& OutValue)
	{
		int32 ColonPos = INDEX_NONE;
		if (!Line.FindChar(TEXT(':'), ColonPos))
		{
			return false;
		}
		OutKey = Line.Left(ColonPos).TrimStartAndEnd();
		OutValue = Line.Mid(ColonPos + 1).TrimStartAndEnd();
		// Strip surrounding quotes on the value if present.
		OutValue = OutValue.TrimQuotes();
		return true;
	}
}

// ===========================================================================
// FRosbridgeConfig
// ===========================================================================

FString FRosbridgeConfig::ToUrl() const
{
	// ws://host:port/<path>. Path defaults empty -> trailing slash, which UE's
	// libwebsockets emits as `GET //`, matching the rosbridge url_path:=//
	// launch (M0 finding).
	return FString::Printf(TEXT("ws://%s:%d/%s"), *Host, Port, *Path);
}

bool FRosbridgeConfig::ParseFromString(const FString& YamlText, FRosbridgeConfig& OutConfig)
{
	TArray<FString> Lines;
	YamlText.ParseIntoArrayLines(Lines, /*bCullEmpty*/ false);
	for (const FString& Raw : Lines)
	{
		const FString Line = StripComment(Raw);
		if (Line.IsEmpty())
		{
			continue;
		}
		FString Key, Value;
		if (!SplitKeyValue(Line, Key, Value) || Value.IsEmpty())
		{
			continue;
		}
		if (Key == TEXT("host"))      { OutConfig.Host = Value; }
		else if (Key == TEXT("port")) { if (Value.IsNumeric()) { OutConfig.Port = FCString::Atoi(*Value); } }
		else if (Key == TEXT("path")) { OutConfig.Path = Value; }
	}
	return true;
}

FString FRosbridgeConfig::ResolveConfigPath()
{
	FString ConfigPath;
	if (FParse::Value(FCommandLine::Get(), TEXT("BridgeConfig="), ConfigPath) && !ConfigPath.IsEmpty())
	{
		return ConfigPath;
	}
	const FString ProjectDir = FPaths::ProjectDir();
	const FString RepoRoot = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(ProjectDir, TEXT("../../")));
	return FPaths::Combine(RepoRoot, TEXT("config"), TEXT("bridge.yaml"));
}

FRosbridgeConfig FRosbridgeConfig::Resolve()
{
	FRosbridgeConfig Config;

	// 2. config/bridge.yaml (if present) overrides the built-in defaults.
	const FString ConfigPath = ResolveConfigPath();
	FString Text;
	if (FFileHelper::LoadFileToString(Text, *ConfigPath))
	{
		ParseFromString(Text, Config);
		UE_LOG(LogRosbridgeClient, Log, TEXT("Loaded bridge config from %s"), *ConfigPath);
	}

	// 1. Command-line overrides win over the file (both runtime, no recompile).
	const TCHAR* CmdLine = FCommandLine::Get();
	FString HostOverride;
	if (FParse::Value(CmdLine, TEXT("BridgeHost="), HostOverride) && !HostOverride.IsEmpty())
	{
		Config.Host = HostOverride;
	}
	int32 PortOverride = 0;
	if (FParse::Value(CmdLine, TEXT("BridgePort="), PortOverride) && PortOverride > 0)
	{
		Config.Port = PortOverride;
	}
	FString PathOverride;
	if (FParse::Value(CmdLine, TEXT("BridgePath="), PathOverride))
	{
		Config.Path = PathOverride;
	}

	UE_LOG(LogRosbridgeClient, Log, TEXT("Rosbridge config: %s"), *Config.ToUrl());
	return Config;
}

// ===========================================================================
// FRosbridgeFraming
// ===========================================================================

FString FRosbridgeFraming::BuildAdvertiseJson(const FString& Topic, const FString& Type)
{
	TSharedRef<FJsonObject> Op = MakeShared<FJsonObject>();
	Op->SetStringField(TEXT("op"), TEXT("advertise"));
	Op->SetStringField(TEXT("topic"), Topic);
	Op->SetStringField(TEXT("type"), Type);
	return JsonToString(Op);
}

FString FRosbridgeFraming::BuildPublishJson(const FString& Topic, const TSharedRef<FJsonObject>& Msg)
{
	TSharedRef<FJsonObject> Op = MakeShared<FJsonObject>();
	Op->SetStringField(TEXT("op"), TEXT("publish"));
	Op->SetStringField(TEXT("topic"), Topic);
	Op->SetObjectField(TEXT("msg"), Msg);
	return JsonToString(Op);
}

FString FRosbridgeFraming::EncodeBase64(const TArray<uint8>& Bytes)
{
	if (Bytes.Num() == 0)
	{
		return FString();
	}
	return FBase64::Encode(Bytes);
}

bool FRosbridgeFraming::DecodeBase64(const FString& Encoded, TArray<uint8>& OutBytes)
{
	OutBytes.Reset();
	if (Encoded.IsEmpty())
	{
		return true; // empty encodes to empty.
	}
	return FBase64::Decode(Encoded, OutBytes);
}

FString FRosbridgeFraming::BuildBinaryPublishJson(const FString& Topic, const FString& FieldName, const TArray<uint8>& Bytes)
{
	TSharedRef<FJsonObject> Msg = MakeShared<FJsonObject>();
	Msg->SetStringField(FieldName, EncodeBase64(Bytes));
	return BuildPublishJson(Topic, Msg);
}

bool FRosbridgeFraming::ExtractBinaryField(const FString& PublishOpJson, const FString& FieldName, TArray<uint8>& OutBytes)
{
	OutBytes.Reset();

	TSharedPtr<FJsonObject> Root;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(PublishOpJson);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		return false;
	}

	const TSharedPtr<FJsonObject>* Msg = nullptr;
	if (!Root->TryGetObjectField(TEXT("msg"), Msg) || !Msg || !Msg->IsValid())
	{
		return false;
	}

	FString Encoded;
	if (!(*Msg)->TryGetStringField(FieldName, Encoded))
	{
		return false;
	}

	return DecodeBase64(Encoded, OutBytes);
}

// ===========================================================================
// FReconnectPolicy
// ===========================================================================

FReconnectPolicy::FReconnectPolicy(double InInitialSeconds, double InMultiplier)
{
	// Initial interval clamped into (0, Max]; a non-positive request falls back
	// to a small positive value so an attempt is always scheduled.
	InitialSeconds = FMath::Clamp(InInitialSeconds, 1.0e-3, MaxIntervalSeconds);
	// Multiplier >= 1 so the interval never shrinks below the initial value.
	Multiplier = FMath::Max(1.0, InMultiplier);
	CurrentSeconds = InitialSeconds;
	AttemptCount = 0;
	bAlive = true;
}

void FReconnectPolicy::Reset()
{
	CurrentSeconds = InitialSeconds;
	AttemptCount = 0;
	// Liveness is preserved across resets; the loop never dies.
	bAlive = true;
}

double FReconnectPolicy::PeekNextIntervalSeconds() const
{
	// HARD cap at 5 s (Requirement 6.5): no matter how far the backoff has
	// grown, the next attempt is never more than 5 s away.
	return FMath::Min(CurrentSeconds, MaxIntervalSeconds);
}

double FReconnectPolicy::NextIntervalSeconds()
{
	const double Interval = PeekNextIntervalSeconds();
	++AttemptCount;
	// Grow for the attempt after this one, but keep it capped so a long outage
	// cannot push any interval over 5 s.
	CurrentSeconds = FMath::Min(CurrentSeconds * Multiplier, MaxIntervalSeconds);
	// The loop stays alive regardless of how many attempts have failed.
	bAlive = true;
	return Interval;
}

// ===========================================================================
// FRosbridgeClient
// ===========================================================================

FRosbridgeClient::FRosbridgeClient(const FRosbridgeConfig& InConfig)
	: Config(InConfig)
	, ReconnectPolicy(/*InitialSeconds*/ 0.5, /*Multiplier*/ 2.0)
{
	if (!FModuleManager::Get().IsModuleLoaded("WebSockets"))
	{
		FModuleManager::Get().LoadModule("WebSockets");
	}
}

FRosbridgeClient::~FRosbridgeClient()
{
	Disconnect();
}

bool FRosbridgeClient::IsConnected() const
{
	return Socket.IsValid() && Socket->IsConnected();
}

void FRosbridgeClient::Connect()
{
	bWantConnected = true;
	ReconnectPolicy.Reset();
	OpenSocket();
}

void FRosbridgeClient::Disconnect()
{
	bWantConnected = false;

	if (ReconnectTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(ReconnectTickerHandle);
		ReconnectTickerHandle.Reset();
	}
	bReconnectPending = false;

	if (Socket.IsValid())
	{
		if (Socket->IsConnected())
		{
			Socket->Close();
		}
		Socket.Reset();
	}
}

void FRosbridgeClient::OpenSocket()
{
	if (!bWantConnected)
	{
		return;
	}

	Socket = FWebSocketsModule::Get().CreateWebSocket(Config.ToUrl(), TEXT("ws"));
	if (!Socket.IsValid())
	{
		UE_LOG(LogRosbridgeClient, Warning, TEXT("Failed to create WebSocket for %s; scheduling retry."), *Config.ToUrl());
		ScheduleReconnect();
		return;
	}

	TSharedRef<FRosbridgeClient> Self = AsShared();

	Socket->OnConnected().AddLambda([Self]() { Self->HandleConnected(); });
	Socket->OnConnectionError().AddLambda([Self](const FString& Error) { Self->HandleConnectionError(Error); });
	Socket->OnClosed().AddLambda([Self](int32 Code, const FString& Reason, bool bClean)
	{
		Self->HandleClosed(Code, Reason, bClean);
	});

	UE_LOG(LogRosbridgeClient, Log, TEXT("Connecting to %s ..."), *Config.ToUrl());
	Socket->Connect();
}

void FRosbridgeClient::HandleConnected()
{
	UE_LOG(LogRosbridgeClient, Log, TEXT("Connected to %s"), *Config.ToUrl());
	// A clean reconnect resets the backoff so the next outage starts fast again.
	ReconnectPolicy.Reset();
	bReconnectPending = false;
	// Re-send every advertise so rosbridge recreates the publishers (design:
	// "re-sends advertises") then publishing resumes.
	ResendAdvertises();
}

void FRosbridgeClient::HandleConnectionError(const FString& Error)
{
	UE_LOG(LogRosbridgeClient, Warning, TEXT("Connection error to %s: %s"), *Config.ToUrl(), *Error);
	// The editor keeps running (Requirement 6.5): we never crash on a failed
	// connect, we just schedule the next attempt within 5 s.
	ScheduleReconnect();
}

void FRosbridgeClient::HandleClosed(int32 StatusCode, const FString& Reason, bool bWasClean)
{
	UE_LOG(LogRosbridgeClient, Log, TEXT("WebSocket closed (%d): %s (clean=%d)"),
		StatusCode, *Reason, bWasClean ? 1 : 0);
	// On an unexpected loss while we still want a connection, reconnect.
	if (bWantConnected)
	{
		ScheduleReconnect();
	}
}

void FRosbridgeClient::ScheduleReconnect()
{
	if (!bWantConnected || bReconnectPending)
	{
		return;
	}

	// Delay is guaranteed <= 5 s by FReconnectPolicy (Requirement 6.5).
	const double Delay = ReconnectPolicy.NextIntervalSeconds();
	bReconnectPending = true;

	UE_LOG(LogRosbridgeClient, Log, TEXT("Scheduling reconnect attempt %d in %.2fs (<= %.0fs)."),
		ReconnectPolicy.GetAttemptCount(), Delay, FReconnectPolicy::MaxIntervalSeconds);

	TSharedRef<FRosbridgeClient> Self = AsShared();
	ReconnectTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda([Self](float /*Dt*/) -> bool
		{
			Self->bReconnectPending = false;
			Self->ReconnectTickerHandle.Reset();
			if (Self->bWantConnected && !Self->IsConnected())
			{
				// Drop any stale socket before reopening.
				if (Self->Socket.IsValid())
				{
					Self->Socket.Reset();
				}
				Self->OpenSocket();
			}
			return false; // one-shot
		}),
		static_cast<float>(Delay));
}

void FRosbridgeClient::ResendAdvertises()
{
	for (const TPair<FString, FString>& Pair : AdvertisedTopics)
	{
		SendRaw(FRosbridgeFraming::BuildAdvertiseJson(Pair.Key, Pair.Value));
	}
}

void FRosbridgeClient::SendRaw(const FString& OpJson)
{
	if (IsConnected())
	{
		Socket->Send(OpJson);
	}
}

void FRosbridgeClient::Advertise(const FString& Topic, const FString& Type)
{
	AdvertisedTopics.Add(Topic, Type);
	if (IsConnected())
	{
		SendRaw(FRosbridgeFraming::BuildAdvertiseJson(Topic, Type));
	}
}

FString FRosbridgeClient::Publish(const FString& Topic, const TSharedRef<FJsonObject>& Msg)
{
	const FString Op = FRosbridgeFraming::BuildPublishJson(Topic, Msg);
	SendRaw(Op);
	return Op;
}

FString FRosbridgeClient::PublishBinary(const FString& Topic, const FString& FieldName, const TArray<uint8>& Bytes)
{
	const FString Op = FRosbridgeFraming::BuildBinaryPublishJson(Topic, FieldName, Bytes);
	SendRaw(Op);
	return Op;
}

void FRosbridgeClient::Send(const FString& OpJson)
{
	// ITfTransport entry point: FTfPublisher hands us pre-built /tf ops. If the
	// op is an advertise, remember it so it is replayed on reconnect.
	if (OpJson.Contains(TEXT("\"op\":\"advertise\"")))
	{
		TSharedPtr<FJsonObject> Root;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(OpJson);
		if (FJsonSerializer::Deserialize(Reader, Root) && Root.IsValid())
		{
			FString Topic, Type;
			if (Root->TryGetStringField(TEXT("topic"), Topic) && Root->TryGetStringField(TEXT("type"), Type))
			{
				AdvertisedTopics.Add(Topic, Type);
			}
		}
	}
	SendRaw(OpJson);
}

// ---------------------------------------------------------------------------
// Console command: `RosbridgePublish [payload]`
//
// A one-command advertise+publish round-trip (Requirement 6.2 demo). Resolves
// host/port from config at runtime, advertises /bridge_test (std_msgs/msg/String)
// and publishes `payload`. The client's reconnect loop keeps the editor alive
// if the server is down. Used by the live verification script, mirroring how
// SmokePublish is driven by scripts/smoke_test.ps1.
// ---------------------------------------------------------------------------
static TSharedPtr<FRosbridgeClient> GActiveRosbridgeClient;

static void ExecuteRosbridgePublish(const TArray<FString>& Args)
{
	FString Payload = TEXT("sensor-sim-sandbox bridge round-trip");
	if (Args.Num() >= 1)
	{
		Payload = FString::Join(Args, TEXT(" "));
	}

	const FRosbridgeConfig Config = FRosbridgeConfig::Resolve();
	GActiveRosbridgeClient = MakeShared<FRosbridgeClient>(Config);
	GActiveRosbridgeClient->Connect();

	const FString Topic = TEXT("/bridge_test");
	GActiveRosbridgeClient->Advertise(Topic, TEXT("std_msgs/msg/String"));

	// Publish after a short grace so the socket has time to open; the client
	// re-advertises on connect so this is safe even on the first open.
	TSharedPtr<FRosbridgeClient> ClientRef = GActiveRosbridgeClient;
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
		[ClientRef, Topic, Payload](float /*Dt*/) -> bool
		{
			TSharedRef<FJsonObject> Msg = MakeShared<FJsonObject>();
			Msg->SetStringField(TEXT("data"), Payload);
			const FString Op = ClientRef->Publish(Topic, Msg);
			UE_LOG(LogRosbridgeClient, Log, TEXT("RosbridgePublish sent: %s (connected=%d)"),
				*Op, ClientRef->IsConnected() ? 1 : 0);

			// This command exists only for the one-command round-trip demo /
			// verification (it is not wired into the simulation), so after a
			// short grace to let the server deliver the message, request a clean
			// engine exit. This makes `RosbridgePublish` self-terminating so the
			// driving script does not depend on a separate `;Quit` command (which
			// the console arg parser folds into the payload token).
			FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
				[ClientRef](float /*Dt2*/) -> bool
				{
					ClientRef->Disconnect();
					UE_LOG(LogRosbridgeClient, Log, TEXT("RosbridgePublish requesting engine exit."));
					RequestEngineExit(TEXT("RosbridgePublish complete"));
					return false; // one-shot
				}), 2.5f);
			return false; // one-shot
		}), 1.5f);
}

static FAutoConsoleCommand GRosbridgePublishCommand(
	TEXT("RosbridgePublish"),
	TEXT("Rosbridge_Client round-trip: advertise+publish /bridge_test (std_msgs/msg/String) to the configured host/port. Usage: RosbridgePublish [payload]"),
	FConsoleCommandWithArgsDelegate::CreateStatic(&ExecuteRosbridgePublish));

// ---------------------------------------------------------------------------
// Console command: `RosbridgeLoop [payload]`
//
// Like RosbridgePublish but it NEVER quits: it connects once and republishes
// every ~2 s on a repeating ticker, so the reconnect behavior (Requirement 6.5)
// is observable across a server stop/restart while the editor keeps running.
// Used by scenario C of scripts/run_bridge_roundtrip.ps1. The driving script
// kills the editor when the reconnect window is over.
// ---------------------------------------------------------------------------
static FTSTicker::FDelegateHandle GRosbridgeLoopHandle;

static void ExecuteRosbridgeLoop(const TArray<FString>& Args)
{
	FString Payload = TEXT("rosbridge_loop");
	if (Args.Num() >= 1)
	{
		Payload = FString::Join(Args, TEXT("_"));
	}

	const FRosbridgeConfig Config = FRosbridgeConfig::Resolve();
	GActiveRosbridgeClient = MakeShared<FRosbridgeClient>(Config);
	GActiveRosbridgeClient->Connect();

	const FString Topic = TEXT("/bridge_test");
	GActiveRosbridgeClient->Advertise(Topic, TEXT("std_msgs/msg/String"));

	TSharedPtr<FRosbridgeClient> ClientRef = GActiveRosbridgeClient;
	int32* CounterPtr = new int32(0);
	GRosbridgeLoopHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
		[ClientRef, Topic, Payload, CounterPtr](float /*Dt*/) -> bool
		{
			const int32 Seq = (*CounterPtr)++;
			TSharedRef<FJsonObject> Msg = MakeShared<FJsonObject>();
			Msg->SetStringField(TEXT("data"), FString::Printf(TEXT("%s_%d"), *Payload, Seq));
			const FString Op = ClientRef->Publish(Topic, Msg);
			UE_LOG(LogRosbridgeClient, Log, TEXT("RosbridgeLoop publish #%d (connected=%d)"),
				Seq, ClientRef->IsConnected() ? 1 : 0);
			return true; // keep ticking forever (until the editor is killed)
		}), 2.0f);
}

static FAutoConsoleCommand GRosbridgeLoopCommand(
	TEXT("RosbridgeLoop"),
	TEXT("Rosbridge_Client reconnect probe: connect and republish /bridge_test every ~2s forever (never quits). Usage: RosbridgeLoop [payload]"),
	FConsoleCommandWithArgsDelegate::CreateStatic(&ExecuteRosbridgeLoop));
