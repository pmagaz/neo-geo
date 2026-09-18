-- Drive the game from MAME and capture frames, with no window and nobody
-- watching it. This is how scrolling and animation get checked: holding a
-- button for a while and comparing the frames that come out.
--
--   mame ... -autoboot_script tools/playtest.lua -autoboot_delay 0 \
--            -snapshot_directory .shot -seconds_to_run 8
--
-- Always pass -seconds_to_run. The script asks MAME to quit once it has the
-- frames it wants, but that is a request from inside the emulated machine;
-- -seconds_to_run is enforced from outside and cannot be left hanging.
--
-- Configured through the environment, so the script itself needs no editing:
--
--   PT_HOLD    button to hold, as MAME names it   (default "P1 Right")
--   PT_START   frame to start holding it          (default 90)
--   PT_SHOTS   frames to snapshot on, comma separated (default "95,160")
--   PT_END     frame to quit on                   (default 340)

local function getenv(name, fallback)
    local v = os.getenv(name)
    if v == nil or v == "" then return fallback end
    return v
end

local hold_name = getenv("PT_HOLD", "P1 Right")
local start_frame = tonumber(getenv("PT_START", "90"))
local end_frame = tonumber(getenv("PT_END", "340"))

local shots = {}
for f in string.gmatch(getenv("PT_SHOTS", "95,160"), "([^,]+)") do
    shots[tonumber(f)] = true
end

local joy = manager.machine.ioport.ports[":ctrl1:joy:JOY"]
local button = joy and joy.fields[hold_name]
if button == nil then
    print("playtest: no such input: " .. hold_name)
end

local frame = 0
local function tick()
    frame = frame + 1
    if button and frame >= start_frame then
        button:set_value(1)
    end
    if shots[frame] then
        manager.machine.video:snapshot()
    end
    if frame >= end_frame then
        manager.machine:exit()
    end
end

emu.add_machine_frame_notifier(tick)
