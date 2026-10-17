// Copyright (c) Sensor Sim Sandbox. Private portfolio project.

#pragma once

#include "CoreMinimal.h"
#include "Coords/TfPublisher.h"   // ITfTransport — the client drops in as a TF transport.

class IWebSocket;
class FJsonObject;

/**
 * Rosbridge_Client (design "Rosbridge_Client" / "Fallback Rosbridge_Client
 * specification", Requirements 6.2, 6.3, 6.4, 6.5).
 *
 * The fallback bridge, chosen in task 10 because no evaluated Unreal->ROS 2
 * plugin satisfied all three criteria. It speaks the rosbridge v2 protocol over
 * Unreal's WebSockets module with two JSON op types:
 *
 *   advertise : {"op":"advertise","topic":"<topic>","type":"<ros_type>"}
 *   publish   : {"op":"publish","topic":"<topic>","msg":{...}}
 *
 * Binary message fields (sensor_msgs/PointCloud2.data, the
 * sensor_msgs/CompressedImage.data JPEG bytes) are base64-encoded strings
 * inside `msg` (Requirement 6.2, design "Fallback Rosbridge_Client").
 *
 * host/port are read from config and changeable WITHOUT recompilation
 * (Requirement 6.3) — see FRosbridgeConfig.
 *
 * On connection loss the client schedules reconnection attempts no more than
 * 5 seconds apart until the socket reopens, then re-sends every advertise and
 * resumes publishing; the Unreal editor keeps running throughout (Requirement
 * 6.5). It never relies on Windows<->WSL DDS discovery — the only link is the
 * WebSocket (Requirement 6.4).
 *
 * ===== Engine-light separation (so the property tests run headlessly) =====
 *
 * As with FSmokePublisher / FTfPublisher, the PURE framing and the reconnect
 * timing policy are isolated from any live socket:
 *
 *   - FRosbridgeFraming  : static advertise/publish JSON builders and the
 *                          base64 of binary-in-msg. Property 10 drives these
 *                          directly: base64-decode of a built publish payload
 *                          must reproduce the original bytes, over >= 100
 *                          seeded iterations, with NO socket.
 *
 *   - FReconnectPolicy   : the pure reconnect-interval scheduler. Given a loss
 *                          sequence it yields the next-attempt delay, which is
 *                          ALWAYS <= 5 s. Property 11 drives this over >= 100
 *                          seeded loss sequences: every attempt is <= 5 s apart
 *                          and the policy (standing in for the client's loop)
 *                          stays alive, with NO socket.
 *
 * The live FRosbridgeClient composes both: it builds ops with FRosbridgeFraming
 * and times its reconnect attempts with FReconnectPolicy, and implements
 * ITfTransport so FTfPublisher (task 9.1) can publish /tf straight through it.
 */

// ===========================================================================
// FRosbridgeConfig — host/port, runtime-configurable (Requirement 6.3).
// ===========================================================================

/**
 * Bridge connection config. host/port come from (in priority order) explicit
 * command-line overrides, then a `config/bridge.yaml` file, then the built-in
 * defaults — none of which require recompiling the module (Requirement 6.3).
 */
struct FRosbridgeConfig
{
	/** rosbridge host. Default 127.0.0.1 (rosbridge in WSL, reached from Windows). */
	FString Host = TEXT("127.0.0.1");

	/** rosbridge WebSocket port. Default 9090. */
	int32 Port = 9090;

	/**
	 * URL path. UE's bundled libwebsockets emits `GET //` for a root ws:// URL,
	 * so the real rosbridge_server is launched with url_path:=// (M0 finding).
	 * Kept configurable; the default empty path yields a ws://host:port/ URL
	 * which libwebsockets turns into the `GET //` the server is launched for.
	 */
	FString Path = TEXT("");

	/** Build the ws:// URL FWebSocketsModule connects to. */
	FString ToUrl() const;

	/**
	 * Resolve the effective config at runtime (Requirement 6.3):
	 *   1. -BridgeHost=<h> / -BridgePort=<n> command-line overrides, else
	 *   2. config/bridge.yaml (host:/port:), else
	 *   3. the defaults above.
	 * Never reads a value baked in at compile time.
	 */
	static FRosbridgeConfig Resolve();

	/** Parse `host:`/`port:`/`path:` from a minimal YAML string (best effort). */
	static bool ParseFromString(const FString& YamlText, FRosbridgeConfig& OutConfig);

	/** Default config/bridge.yaml path at the repo root. */
	static FString ResolveConfigPath();
};

// ===========================================================================
// FRosbridgeFraming — PURE rosbridge v2 op framing (Property 10).
// ===========================================================================

/**
 * Pure, static rosbridge v2 framing. No socket, no engine state. These are the
 * exact bytes the live client sends, so Property 10 can assert the base64
 * round-trip on the built payload directly.
 */
class FRosbridgeFraming
{
public:
	/** advertise op: {"op":"advertise","topic":...,"type":...}. */
	static FString BuildAdvertiseJson(const FString& Topic, const FString& Type);

	/**
	 * publish op from an already-built msg object:
	 * {"op":"publish","topic":...,"msg":{...}}.
	 */
	static FString BuildPublishJson(const FString& Topic, const TSharedRef<FJsonObject>& Msg);

	/**
	 * Convenience: a publish whose `msg` has a single base64-encoded binary
	 * field (the shape PointCloud2.data / CompressedImage.data use). `FieldName`
	 * is the msg key (e.g. "data"); `Bytes` are base64-encoded into it.
	 * This is the exact framing Property 10 round-trips.
	 */
	static FString BuildBinaryPublishJson(const FString& Topic, const FString& FieldName, const TArray<uint8>& Bytes);

	/** Base64-encode raw bytes (binary-in-msg, Requirement 6.2). */
	static FString EncodeBase64(const TArray<uint8>& Bytes);

	/** Base64-decode back to bytes. Returns false on malformed input. */
	static bool DecodeBase64(const FString& Encoded, TArray<uint8>& OutBytes);

	/**
	 * Extract the base64 string at `msg.<FieldName>` from a publish op JSON and
	 * decode it to bytes. Used by Property 10 to prove the round-trip on the
	 * actual serialized op. Returns false if the op/field is missing or the
	 * base64 is malformed.
	 */
	static bool ExtractBinaryField(const FString& PublishOpJson, const FString& FieldName, TArray<uint8>& OutBytes);
};

// ===========================================================================
// FReconnectPolicy — PURE reconnect-interval scheduler (Property 11).
// ===========================================================================

/**
 * The reconnect-timing policy, free of any socket. On a connection loss the
 * client asks this policy when to attempt the next reconnect; the answer is
 * ALWAYS an interval <= MaxIntervalSeconds (5 s, Requirement 6.5).
 *
 * The policy is a simple capped backoff: it starts at an initial interval and
 * may grow, but is HARD-CAPPED at 5 s so the Requirement 6.5 bound holds for
 * every attempt no matter how long the outage lasts. It carries a liveness flag
 * that stays true across arbitrarily long loss sequences — the live client's
 * loop likewise keeps running (and the editor with it) throughout.
 *
 * Property 11 drives this over >= 100 seeded loss sequences: every scheduled
 * attempt is <= 5 s apart and IsAlive() never becomes false.
 */
class FReconnectPolicy
{
public:
	/** The hard cap on any reconnect interval (Requirement 6.5). Never raised. */
	static constexpr double MaxIntervalSeconds = 5.0;

	/**
	 * @param InInitialSeconds first-attempt delay (clamped to (0, Max]).
	 * @param InMultiplier     growth factor per failed attempt (>= 1).
	 */
	explicit FReconnectPolicy(double InInitialSeconds = 0.5, double InMultiplier = 2.0);

	/** Reset to the initial interval (called on a successful reconnect). */
	void Reset();

	/**
	 * Record that the connection was just lost (or a reconnect attempt failed)
	 * and return the delay until the NEXT attempt. Guaranteed in
	 * (0, MaxIntervalSeconds]. Repeated calls model a prolonged outage; the
	 * returned delay grows but never exceeds the cap.
	 */
	double NextIntervalSeconds();

	/** The interval the next NextIntervalSeconds() call would return (peek). */
	double PeekNextIntervalSeconds() const;

	/** Number of consecutive failed attempts since the last Reset(). */
	int32 GetAttemptCount() const { return AttemptCount; }

	/**
	 * The client's loop (and the editor) stay alive throughout a loss. This is
	 * true at construction and remains true for the life of the policy; it is
	 * exposed so Property 11 can assert the client never dies mid-outage.
	 */
	bool IsAlive() const { return bAlive; }

private:
	double InitialSeconds = 0.5;
	double Multiplier = 2.0;
	double CurrentSeconds = 0.5;
	int32 AttemptCount = 0;
	bool bAlive = true;
};

// ===========================================================================
// FRosbridgeClient — live WebSocket client (composes the two pure cores).
// ===========================================================================

/**
 * The live client. Owns the WebSocket, advertises topics, publishes ops built
 * by FRosbridgeFraming, and reconnects on the FReconnectPolicy schedule using
 * the engine ticker so the editor keeps running. Implements ITfTransport so
 * FTfPublisher (task 9.1) can send /tf straight through it.
 */
class FRosbridgeClient
	: public ITfTransport
	, public TSharedFromThis<FRosbridgeClient>
{
public:
	explicit FRosbridgeClient(const FRosbridgeConfig& InConfig);
	virtual ~FRosbridgeClient();

	/** The resolved connection config (host/port/url). */
	const FRosbridgeConfig& GetConfig() const { return Config; }

	/** Open the WebSocket and begin the connect/reconnect loop. */
	void Connect();

	/** Close the socket and stop the reconnect loop (no more attempts). */
	void Disconnect();

	/** True while the underlying socket is open. */
	bool IsConnected() const;

	/**
	 * Advertise a topic once. Remembered so every advertise is re-sent on
	 * reconnect (design: "re-sends advertises"). If already connected the
	 * advertise goes out immediately; otherwise it is sent on (re)connect.
	 */
	void Advertise(const FString& Topic, const FString& Type);

	/**
	 * Publish a msg object to a topic. If the socket is open the op goes out
	 * now; if not, the op is dropped (sensor data is time-sensitive) but the
	 * loop keeps trying to reconnect. Returns the op JSON that was built.
	 */
	FString Publish(const FString& Topic, const TSharedRef<FJsonObject>& Msg);

	/** Publish a single base64-encoded binary field (PointCloud2 / image data). */
	FString PublishBinary(const FString& Topic, const FString& FieldName, const TArray<uint8>& Bytes);

	// --- ITfTransport: let FTfPublisher send pre-built /tf ops through us. ---
	virtual void Send(const FString& OpJson) override;

private:
	void OpenSocket();
	void HandleConnected();
	void HandleConnectionError(const FString& Error);
	void HandleClosed(int32 StatusCode, const FString& Reason, bool bWasClean);
	void ScheduleReconnect();
	void SendRaw(const FString& OpJson);
	void ResendAdvertises();

	FRosbridgeConfig Config;
	TSharedPtr<IWebSocket> Socket;
	FReconnectPolicy ReconnectPolicy;

	/** topic -> type, so advertises can be replayed on reconnect. */
	TMap<FString, FString> AdvertisedTopics;

	/** True once Connect() ran and until Disconnect(); gates reconnect attempts. */
	bool bWantConnected = false;
	/** A reconnect timer is already pending (avoid stacking timers). */
	bool bReconnectPending = false;
	/** Handle to the pending reconnect ticker so it can be cancelled. */
	FTSTicker::FDelegateHandle ReconnectTickerHandle;
};
