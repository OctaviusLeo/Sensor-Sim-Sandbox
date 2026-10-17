// Copyright (c) Sensor Sim Sandbox. Private portfolio project.

#include "Bridge/SmokePublisher.h"

#include "IWebSocket.h"
#include "WebSocketsModule.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "HAL/IConsoleManager.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

DEFINE_LOG_CATEGORY_STATIC(LogSmokePublisher, Log, All);

namespace
{
	/** Serialize a JSON object to a compact string. */
	FString JsonToString(const TSharedRef<FJsonObject>& Obj)
	{
		FString Out;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Obj, Writer);
		return Out;
	}
}

FString FSmokePublisher::BuildAdvertiseJson()
{
	TSharedRef<FJsonObject> Op = MakeShared<FJsonObject>();
	Op->SetStringField(TEXT("op"), TEXT("advertise"));
	Op->SetStringField(TEXT("topic"), GetTopic());
	Op->SetStringField(TEXT("type"), GetMessageType());
	return JsonToString(Op);
}

FString FSmokePublisher::BuildPublishJson(const FString& Payload)
{
	TSharedRef<FJsonObject> Msg = MakeShared<FJsonObject>();
	Msg->SetStringField(TEXT("data"), Payload);

	TSharedRef<FJsonObject> Op = MakeShared<FJsonObject>();
	Op->SetStringField(TEXT("op"), TEXT("publish"));
	Op->SetStringField(TEXT("topic"), GetTopic());
	Op->SetObjectField(TEXT("msg"), Msg);
	return JsonToString(Op);
}

void FSmokePublisher::Finish(bool bSuccess, const FString& Detail)
{
	if (bCompleted)
	{
		return;
	}
	bCompleted = true;

	if (Socket.IsValid() && Socket->IsConnected())
	{
		Socket->Close();
	}

	UE_LOG(LogSmokePublisher, Log, TEXT("SmokePublish %s: %s"),
		bSuccess ? TEXT("succeeded") : TEXT("failed"), *Detail);

	if (CompletionCallback)
	{
		CompletionCallback(bSuccess, Detail);
	}
}

void FSmokePublisher::Run(const FString& Url, const FString& Payload, TFunction<void(bool, const FString&)> OnComplete)
{
	CompletionCallback = MoveTemp(OnComplete);
	bCompleted = false;

	// Ensure the WebSockets module is loaded before creating a socket.
	if (!FModuleManager::Get().IsModuleLoaded("WebSockets"))
	{
		FModuleManager::Get().LoadModule("WebSockets");
	}

	Socket = FWebSocketsModule::Get().CreateWebSocket(Url, TEXT("ws"));
	if (!Socket.IsValid())
	{
		Finish(false, FString::Printf(TEXT("failed to create WebSocket for %s"), *Url));
		return;
	}

	TSharedRef<FSmokePublisher> Self = AsShared();

	Socket->OnConnected().AddLambda([Self, Payload, Url]()
	{
		UE_LOG(LogSmokePublisher, Log, TEXT("Connected to %s"), *Url);

		const FString Advertise = FSmokePublisher::BuildAdvertiseJson();
		const FString Publish = FSmokePublisher::BuildPublishJson(Payload);

		Self->Socket->Send(Advertise);
		Self->Socket->Send(Publish);

		UE_LOG(LogSmokePublisher, Log, TEXT("Sent advertise: %s"), *Advertise);
		UE_LOG(LogSmokePublisher, Log, TEXT("Sent publish: %s"), *Publish);

		Self->Finish(true, FString::Printf(TEXT("advertised and published to %s on %s"),
			*FSmokePublisher::GetTopic(), *Url));
	});

	Socket->OnConnectionError().AddLambda([Self](const FString& Error)
	{
		Self->Finish(false, FString::Printf(TEXT("connection error: %s"), *Error));
	});

	Socket->OnClosed().AddLambda([](int32 StatusCode, const FString& Reason, bool bWasClean)
	{
		UE_LOG(LogSmokePublisher, Log, TEXT("WebSocket closed (%d): %s (clean=%d)"),
			StatusCode, *Reason, bWasClean ? 1 : 0);
	});

	UE_LOG(LogSmokePublisher, Log, TEXT("Connecting to %s ..."), *Url);
	Socket->Connect();
}

// ---------------------------------------------------------------------------
// Console command: `SmokePublish [url] [payload]`
//
// Invoked non-interactively by scripts/smoke_test.ps1 via -ExecCmds. Keeps a
// strong reference to the publisher alive until the async connection resolves.
// ---------------------------------------------------------------------------
static TSharedPtr<FSmokePublisher> GActiveSmokePublisher;

static void ExecuteSmokePublish(const TArray<FString>& Args)
{
	FString Url = FSmokePublisher::GetDefaultUrl();
	FString Payload = TEXT("sensor-sim-sandbox smoke test");

	if (Args.Num() >= 1 && !Args[0].IsEmpty())
	{
		Url = Args[0];
	}
	if (Args.Num() >= 2)
	{
		// Join remaining args so a payload with spaces still works.
		TArray<FString> PayloadParts;
		for (int32 i = 1; i < Args.Num(); ++i)
		{
			PayloadParts.Add(Args[i]);
		}
		Payload = FString::Join(PayloadParts, TEXT(" "));
	}

	GActiveSmokePublisher = MakeShared<FSmokePublisher>();
	GActiveSmokePublisher->Run(Url, Payload, [](bool bSuccess, const FString& Detail)
	{
		UE_LOG(LogSmokePublisher, Log, TEXT("SmokePublish result: success=%d detail=%s"),
			bSuccess ? 1 : 0, *Detail);
		// Release on the game thread after completion.
		GActiveSmokePublisher.Reset();
	});
}

static FAutoConsoleCommand GSmokePublishCommand(
	TEXT("SmokePublish"),
	TEXT("Smoke test: send a rosbridge advertise+publish for /smoke_test (std_msgs/msg/String) to ws://localhost:9090. Usage: SmokePublish [url] [payload]"),
	FConsoleCommandWithArgsDelegate::CreateStatic(&ExecuteSmokePublish));
