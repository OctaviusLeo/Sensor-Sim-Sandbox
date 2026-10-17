// Copyright (c) Sensor Sim Sandbox. Private portfolio project.

#pragma once

#include "CoreMinimal.h"

class IWebSocket;

/**
 * Minimal smoke publisher used by the M0 Smoke_Test (Requirement 1.2).
 *
 * Opens a rosbridge-style WebSocket to ws://localhost:9090 and sends a
 * rosbridge v2 `advertise` followed by a `publish` for /smoke_test
 * (std_msgs/msg/String). This is the smallest possible exercise of the
 * Unreal WebSockets module + rosbridge framing that the full
 * Rosbridge_Client (M2, task 11) later generalizes.
 *
 * Driven entirely from the command line / console command `SmokePublish`
 * so the smoke test needs no interactive input.
 */
class FSmokePublisher : public TSharedFromThis<FSmokePublisher>
{
public:
	/** rosbridge WebSocket URL. Overridable so a test can point at a stub server. */
	static FString GetDefaultUrl() { return TEXT("ws://localhost:9090"); }

	/** Topic and message type sent by the smoke test. */
	static FString GetTopic() { return TEXT("/smoke_test"); }
	static FString GetMessageType() { return TEXT("std_msgs/msg/String"); }

	/**
	 * Connect to `Url`, advertise and publish a single String message whose
	 * `data` field is `Payload`, then disconnect. Non-blocking: the connection
	 * runs on the WebSockets module's own threads and `OnComplete` is invoked
	 * with success/failure plus a human-readable detail string.
	 */
	void Run(const FString& Url, const FString& Payload, TFunction<void(bool, const FString&)> OnComplete);

	/** Build the rosbridge v2 `advertise` op for /smoke_test. */
	static FString BuildAdvertiseJson();

	/** Build the rosbridge v2 `publish` op carrying `Payload` in msg.data. */
	static FString BuildPublishJson(const FString& Payload);

private:
	TSharedPtr<IWebSocket> Socket;
	TFunction<void(bool, const FString&)> CompletionCallback;
	bool bCompleted = false;

	void Finish(bool bSuccess, const FString& Detail);
};
