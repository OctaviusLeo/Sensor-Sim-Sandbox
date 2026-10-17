# Task 11 — Rosbridge_Client live round-trip + reconnect evidence

- Generated: 2026-10-17T11:15:07.5918440-04:00
- Command: `pwsh -NoProfile -ExecutionPolicy Bypass -File scripts\run_bridge_roundtrip.ps1`
- Commit: `556f171efa7c17bf68eebc801866663e1335d663`
- Engine: `C:\Program Files (x86)\UE_5.8`

## Scenario A — stub WebSocket round-trip (Requirements 6.2, 6.3)
Result: PASS — advertise=True publish=True out=C:\dev\Projects\runs\logs\bridge\stub_received.jsonl
```
{"recv_t": 1792250042.4180522, "op": "advertise", "topic": "/bridge_test", "raw": "{\"op\":\"advertise\",\"topic\":\"/bridge_test\",\"type\":\"std_msgs/msg/String\"}"}
{"recv_t": 1792250042.8275971, "op": "publish", "topic": "/bridge_test", "raw": "{\"op\":\"publish\",\"topic\":\"/bridge_test\",\"msg\":{\"data\":\"stub_round-trip_2026-10-17T11:13:49.9622629-04:00\"}}"}
```

## Scenario C — reconnect within 5 s (Requirement 6.5)
Result: PASS — scheduleLines=2 maxInterval=1s reconnects=2 opsAfterRestart=6 editorAlive=True downAt=2026-10-17T11:14:23.8926675-04:00 upAt=2026-10-17T11:14:27.9255967-04:00
```
ipt C:/Program Files (x86)/UE_5.8/Engine/Plugins/Animation/ControlRig/Content/Python/init_unreal.py... started...
[2026.10.17-15.14.22:522][  0]LogPython: registering <class 'ControlRigWorkflows.workflow_deformation_rig_preset.provider'>

[2026.10.17-15.14.22:531][  0]LogPython: Display: Running start-up script C:/Program Files (x86)/UE_5.8/Engine/Plugins/Animation/ControlRig/Content/Python/init_unreal.py... took 22.098 ms (total: 33.506 ms)
[2026.10.17-15.14.22:531][  0]Cmd: RosbridgeLoop reconnect_probe
[2026.10.17-15.14.22:532][  0]LogRosbridgeClient: Loaded bridge config from C:/dev/Projects/config/bridge.yaml
[2026.10.17-15.14.22:532][  0]LogRosbridgeClient: Rosbridge config: ws://127.0.0.1:9091/
[2026.10.17-15.14.22:532][  1]LogRosbridgeClient: Connecting to ws://127.0.0.1:9091/ ...
[2026.10.17-15.14.22:856][  1]LogAssetRegistry: AssetRegistryGather time 0.1540s: AssetDataDiscovery 0.0210s, AssetDataGather 0.0589s, StoreResults 0.0741s. Wall time 5.0810s.
	NumCachedDirectories 1726. NumUncachedDirectories 27. NumCachedFiles 8751. NumUncachedFiles 0.
	BackgroundTickInterruptions 2.
[2026.10.17-15.14.22:885][  1]LogAssetRegistry: Display: Starting OnFilesLoaded.Broadcast
[2026.10.17-15.14.22:908][  1]LogPlacementMode: Display: The Asset Registry is done with its initial scan, the list of placeable classes has been updated.
[2026.10.17-15.14.22:924][  1]LogCollectionManager: Fixed up redirectors for 0 collections in 0.000001 seconds (updated 0 objects)
[2026.10.17-15.14.22:940][  1]LogAssetRegistry: Display: Completed OnFilesLoaded.Broadcast: 0.056s
[2026.10.17-15.14.22:940][  1]LogSourceControl: Uncontrolled asset discovery started...
[2026.10.17-15.14.22:963][  1]LogRosbridgeClient: Connected to ws://127.0.0.1:9091/
[2026.10.17-15.14.23:369][ 38]LogSourceControl: Uncontrolled asset discovery finished in 0.428438 seconds (Found 8727 uncontrolled assets)
[2026.10.17-15.14.23:902][101]LogRosbridgeClient: WebSocket closed (1006):  (clean=0)
[2026.10.17-15.14.23:903][102]LogRosbridgeClient: Scheduling reconnect attempt 1 in 0.50s (<= 5s).
[2026.10.17-15.14.23:920][103]LogAssetRegistry: Display: Asset registry cache written as 118.4 MiB to ../../../../../dev/Projects/unreal/SensorSimSandbox/Intermediate/CachedAssetRegistry/CachedAssetRegistry_*.ref
[2026.10.17-15.14.23:926][104]LogAssetRegistry: Display: CleanupOrphanedCacheFiles (PostWrite): 1 .ref files found, 1 referenced binaries
[2026.10.17-15.14.23:927][104]LogAssetRegistry: Display:   CachedAssetRegistry_0_72564A874B85BBAF73716C9420CC08C0.bin: deleted (orphaned, unreferenced by any .ref)
[2026.10.17-15.14.23:927][104]LogAssetRegistry: Display: CleanupOrphanedCacheFiles: 2 total binaries, 1 referenced (kept), 0 old-style pre-migration (kept), 1 orphans deleted, 0 orphans locked (kept)
[2026.10.17-15.14.24:011][115]LogRosbridgeClient: RosbridgeLoop publish #0 (connected=0)
[2026.10.17-15.14.24:402][161]LogRosbridgeClient: Connecting to ws://127.0.0.1:9091/ ...
[2026.10.17-15.14.26:011][355]LogRosbridgeClient: RosbridgeLoop publish #1 (connected=0)
[2026.10.17-15.14.26:452][407]LogRosbridgeClient: Warning: Connection error to ws://127.0.0.1:9091/: 
[2026.10.17-15.14.26:452][408]LogRosbridgeClient: Scheduling reconnect attempt 2 in 1.00s (<= 5s).
[2026.10.17-15.14.27:452][528]LogRosbridgeClient: Connecting to ws://127.0.0.1:9091/ ...
[2026.10.17-15.14.28:012][595]LogRosbridgeClient: RosbridgeLoop publish #2 (connected=0)
[2026.10.17-15.14.28:485][651]LogRosbridgeClient: Connected to ws://127.0.0.1:9091/
[2026.10.17-15.14.30:011][835]LogRosbridgeClient: RosbridgeLoop publish #3 (connected=1)
[2026.10.17-15.14.32:012][ 75]LogRosbridgeClient: RosbridgeLoop publish #4 (connected=1)
[2026.10.17-15.14.34:012][315]LogRosbridgeClient: RosbridgeLoop publish #5 (connected=1)
[2026.10.17-15.14.36:013][555]LogRosbridgeClient: RosbridgeLoop publish #6 (connected=1)
[2026.10.17-15.14.38:013][795]LogRosbridgeClient: RosbridgeLoop publish #7 (connected=1)
```

## Scenario B — real rosbridge_server round-trip in WSL (Requirements 6.2, 6.4)
Result: PASS — echoFile=C:\dev\Projects\runs\logs\bridge\ros2_echo_bridge.txt
```
data: real_round-trip_2026-10-17T11:14:48.4190350-04:00
---
```
