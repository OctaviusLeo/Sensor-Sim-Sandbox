# Task 11 — Rosbridge_Client live round-trip + reconnect evidence

- Generated: 2026-10-17T10:17:18.3052577-04:00
- Command: `pwsh -NoProfile -ExecutionPolicy Bypass -File scripts\run_bridge_roundtrip.ps1`
- Commit: `bb2d8ee465fb8277480f1dbc1ebd66581a1bb9d5`
- Engine: `C:\Program Files (x86)\UE_5.8`

## Scenario A — stub WebSocket round-trip (Requirements 6.2, 6.3)
Result: PASS — advertise=True publish=True out=C:\dev\Projects\runs\logs\bridge\stub_received.jsonl
```
{"recv_t": 1792246570.9221985, "op": "advertise", "topic": "/bridge_test", "raw": "{\"op\":\"advertise\",\"topic\":\"/bridge_test\",\"type\":\"std_msgs/msg/String\"}"}
{"recv_t": 1792246571.3136516, "op": "publish", "topic": "/bridge_test", "raw": "{\"op\":\"publish\",\"topic\":\"/bridge_test\",\"msg\":{\"data\":\"stub_round-trip_2026-10-17T10:15:58.7678462-04:00\"}}"}
```

## Scenario C — reconnect within 5 s (Requirement 6.5)
Result: PASS — scheduleLines=2 maxInterval=1s reconnects=2 opsAfterRestart=7 editorAlive=True downAt=2026-10-17T10:16:33.4723016-04:00 upAt=2026-10-17T10:16:37.4904437-04:00
```
(x86)/UE_5.8/Engine/Plugins/Animation/ControlRig/Content/Python/init_unreal.py... started...
[2026.10.17-14.16.32:908][  0]LogPython: registering <class 'ControlRigWorkflows.workflow_deformation_rig_preset.provider'>

[2026.10.17-14.16.32:912][  0]LogPython: Display: Running start-up script C:/Program Files (x86)/UE_5.8/Engine/Plugins/Animation/ControlRig/Content/Python/init_unreal.py... took 19.313 ms (total: 30.940 ms)
[2026.10.17-14.16.32:912][  0]Cmd: RosbridgeLoop reconnect_probe
[2026.10.17-14.16.32:912][  0]LogRosbridgeClient: Loaded bridge config from C:/dev/Projects/config/bridge.yaml
[2026.10.17-14.16.32:913][  0]LogRosbridgeClient: Rosbridge config: ws://127.0.0.1:9091/
[2026.10.17-14.16.32:913][  0]LogRosbridgeClient: Connecting to ws://127.0.0.1:9091/ ...
[2026.10.17-14.16.33:288][  1]LogAssetRegistry: AssetRegistryGather time 0.1699s: AssetDataDiscovery 0.0216s, AssetDataGather 0.0590s, StoreResults 0.0893s. Wall time 5.7080s.
	NumCachedDirectories 1726. NumUncachedDirectories 27. NumCachedFiles 8751. NumUncachedFiles 0.
	BackgroundTickInterruptions 2.
[2026.10.17-14.16.33:318][  1]LogAssetRegistry: Display: Starting OnFilesLoaded.Broadcast
[2026.10.17-14.16.33:346][  1]LogPlacementMode: Display: The Asset Registry is done with its initial scan, the list of placeable classes has been updated.
[2026.10.17-14.16.33:363][  1]LogCollectionManager: Fixed up redirectors for 0 collections in 0.000001 seconds (updated 0 objects)
[2026.10.17-14.16.33:382][  1]LogAssetRegistry: Display: Completed OnFilesLoaded.Broadcast: 0.064s
[2026.10.17-14.16.33:383][  1]LogSourceControl: Uncontrolled asset discovery started...
[2026.10.17-14.16.33:401][  1]LogRosbridgeClient: Connected to ws://127.0.0.1:9091/
[2026.10.17-14.16.33:492][  2]LogRosbridgeClient: WebSocket closed (1006):  (clean=0)
[2026.10.17-14.16.33:492][  2]LogRosbridgeClient: Scheduling reconnect attempt 1 in 0.50s (<= 5s).
[2026.10.17-14.16.33:746][ 29]LogSourceControl: Uncontrolled asset discovery finished in 0.363508 seconds (Found 8727 uncontrolled assets)
[2026.10.17-14.16.33:915][ 50]LogRosbridgeClient: Connecting to ws://127.0.0.1:9091/ ...
[2026.10.17-14.16.34:357][103]LogRosbridgeClient: RosbridgeLoop publish #0 (connected=0)
[2026.10.17-14.16.34:358][103]LogAssetRegistry: Display: Asset registry cache written as 118.4 MiB to ../../../../../dev/Projects/unreal/SensorSimSandbox/Intermediate/CachedAssetRegistry/CachedAssetRegistry_*.ref
[2026.10.17-14.16.34:365][103]LogAssetRegistry: Display: CleanupOrphanedCacheFiles (PostWrite): 1 .ref files found, 1 referenced binaries
[2026.10.17-14.16.34:365][104]LogAssetRegistry: Display:   CachedAssetRegistry_0_8284E1454748819C1CABC494565AEE45.bin: deleted (orphaned, unreferenced by any .ref)
[2026.10.17-14.16.34:365][104]LogAssetRegistry: Display: CleanupOrphanedCacheFiles: 2 total binaries, 1 referenced (kept), 0 old-style pre-migration (kept), 1 orphans deleted, 0 orphans locked (kept)
[2026.10.17-14.16.35:974][296]LogRosbridgeClient: Warning: Connection error to ws://127.0.0.1:9091/: socket connect failed
[2026.10.17-14.16.35:974][297]LogRosbridgeClient: Scheduling reconnect attempt 2 in 1.00s (<= 5s).
[2026.10.17-14.16.36:357][343]LogRosbridgeClient: RosbridgeLoop publish #1 (connected=0)
[2026.10.17-14.16.36:974][416]LogRosbridgeClient: Connecting to ws://127.0.0.1:9091/ ...
[2026.10.17-14.16.38:007][540]LogRosbridgeClient: Connected to ws://127.0.0.1:9091/
[2026.10.17-14.16.38:358][583]LogRosbridgeClient: RosbridgeLoop publish #2 (connected=1)
[2026.10.17-14.16.40:357][823]LogRosbridgeClient: RosbridgeLoop publish #3 (connected=1)
[2026.10.17-14.16.42:358][ 63]LogRosbridgeClient: RosbridgeLoop publish #4 (connected=1)
[2026.10.17-14.16.44:358][303]LogRosbridgeClient: RosbridgeLoop publish #5 (connected=1)
[2026.10.17-14.16.46:358][543]LogRosbridgeClient: RosbridgeLoop publish #6 (connected=1)
[2026.10.17-14.16.48:358][783]LogRosbridgeClient: RosbridgeLoop publish #7 (connected=1)
```

## Scenario B — real rosbridge_server round-trip in WSL (Requirements 6.2, 6.4)
Result: PASS — echoFile=C:\dev\Projects\runs\logs\bridge\ros2_echo_bridge.txt
```
data: real_round-trip_2026-10-17T10:16:58.0329221-04:00
---
```
