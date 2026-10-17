# Copyright (c) Sensor Sim Sandbox. Private portfolio project.
#
# Headless editor-Python script that creates the minimal /Game/Maps/RoverLevel
# package WITHOUT the editor UI (design "Level content", Requirement 2.2). The
# saved map is intentionally empty: ARoverLevelBuilder (the global default game
# mode set in Config/DefaultEngine.ini) spawns the ground, lights, obstacles,
# landmarks, and the rover from config/level.yaml at BeginPlay. This script only
# needs to materialize the package so it can be loaded by name on a -game launch.
#
# Run headless (one Unreal process at a time), e.g. via scripts/run_rover.ps1:
#   UnrealEditor-Cmd.exe <uproject> -run=pythonscript \
#       -script="<repo>/scripts/build_rover_level.py" -unattended -nopause -nosplash
import unreal

MAP_PACKAGE = "/Game/Maps/RoverLevel"

les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)

# Create a brand-new empty level and save it to the target package path. An
# empty level is sufficient because the game mode builds the scene at runtime.
created = les.new_level(MAP_PACKAGE)
if not created:
    unreal.log_error("Failed to create new level at %s" % MAP_PACKAGE)
    raise SystemExit(1)

# Ensure World Settings use our game mode even when opened directly in-editor.
world = unreal.EditorLevelLibrary.get_editor_world()
if world is not None:
    ws = world.get_world_settings()
    gm = unreal.load_class(None, "/Script/SensorSimSandbox.RoverLevelBuilder")
    if gm is not None:
        ws.set_editor_property("default_game_mode", gm)

saved = les.save_current_level()
if not saved:
    unreal.log_error("Failed to save level %s" % MAP_PACKAGE)
    raise SystemExit(1)

unreal.log("RoverLevel package created and saved at %s" % MAP_PACKAGE)
