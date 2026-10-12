#!/bin/bash
# WoWBridge, by hand: play the current build, run the in-game test suite, rebuild, or stop everything.
#
#   tools/play.sh            start the gameplay service and launch Skyrim (SKSE) to play
#   tools/play.sh test       run the unattended in-game test suite and print its results (hands off the PC: ~20 min)
#   tools/play.sh build      rebuild the plugin and deploy it into Skyrim (the game must not be running)
#   tools/play.sh stop       close Skyrim and the gameplay service
#   tools/play.sh status     what is built, deployed, running, and the last test run
#   tools/play.sh wow        start the reference WoW 1.12 client and log in (needs the realm containers up)
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PROJECT="${WOWBRIDGE_PROJECT_ROOT:-$(dirname "$ROOT")}"
PY="${WOWBRIDGE_PYTHON:-$PROJECT/toolchain/venv/bin/python}"
GAME_DIR="$PROJECT/steamapps/common/Skyrim Special Edition"
SKSE_LOGS="$PROJECT/steamapps/compatdata/489830/pfx/drive_c/users/steamuser/Documents/My Games/Skyrim Special Edition/SKSE"
cd "$ROOT" || exit 1

service_up() { (exec 3<>/dev/tcp/127.0.0.1/7878) 2>/dev/null; }
game_up() { pgrep -f '^S:.*SkyrimSE\.exe' > /dev/null; }

start_service() {
    if service_up; then echo "service: already running on 127.0.0.1:7878"; return; fi
    nohup "$PY" -m service.server --catalog build/catalog.json --trace build/service_trace.jsonl > build/service.log 2>&1 &
    for _ in $(seq 1 20); do service_up && break; sleep 0.25; done
    service_up && echo "service: started (log build/service.log)" || { echo "service: failed to start, see build/service.log"; exit 1; }
}

# The reference WoW client full screen keeps the focus from Skyrim, which then stops at its first frame.
minimize_wow() {
    "$PY" -c "import sys; sys.path.insert(0, 'tools'); from inputdev import kwin
kwin('for (const w of workspace.windowList()) if (w.caption === \"World of Warcraft\") w.minimized = true; print(\"TAG|\");')" > /dev/null 2>&1 || true
}

case "${1:-play}" in
play)
    game_up && { echo "Skyrim is already running."; exit 0; }
    sed -i 's/^autorun=1/autorun=0/' "$GAME_DIR/Data/SKSE/Plugins/WoWBridge.ini"   # play, not the test suite
    start_service
    minimize_wow
    steam -applaunch 489830 > /dev/null 2>&1 &
    cat <<'EOF'
Skyrim is launching through Steam (SKSE). For a new game, play the opening normally and choose Play WoWBridge
at character creation. Pick a night elf druid preset and enter a name. Dragonborn is the default.
The WoW interface, controls and equipment start when Skyrim returns movement and combat control.
Existing saves keep WoWBridge enabled; wb start opens preset selection or enables it on a native character.
For a disposable quick test, open the console (~) at the main menu and enter: coc Riverwood
WoW uses your Skyrim character's name. Rebind controls in WoW's Key Bindings, from its micro-menu:

  mouse moves the WoW cursor - left click targets, left drag turns the camera, hold right button to steer, hold both
  to run forward; right click is Skyrim's E (talk, open, loot, take) in reach, and attacks a hostile unit;
  Alt+right click attacks anyone (the cursor turns to a sword with Alt held); Alt+left click picks up a loose thing
  (any left click drops it); W/S move, A/D turn, Q/E strafe, Space jumps, Tab next enemy, F1 yourself,
  numpad / walk or run; tap Ctrl alone to sneak; Z draws or sheathes her staff (attacking draws it);
  Shift+Tab Skyrim's Tab menu; Esc Skyrim's pause menu; Shift+1..6 change the action bar page (Shift+1 back)
  1 Attack  2 Wrath  3 Moonfire  4 Rejuvenation  5 Healing Touch  6 Entangling Roots  7 Hurricane (then click the ground)
  the micro-menu opens Skyrim's inventory/skills/journal/map; the console key and keys WoW leaves unbound stay Skyrim's
Stop with: tools/play.sh stop
EOF
    ;;
test)
    game_up && { echo "Close Skyrim first (tools/play.sh stop)."; exit 1; }
    echo "Running the in-game suite (about 20 minutes; don't use the mouse or keyboard meanwhile)..."
    RUN_STATUS=0
    timeout 2500 "$PY" tools/run_skyrim.py --autorun --timeout 2400 "${@:2}" > build/skyrim_run.log 2>&1 || RUN_STATUS=$?
    head -1 build/skyrim_run.log
    "$PY" - "$RUN_STATUS" <<'EOF'
import json, re, sys
from pathlib import Path
sys.path.insert(0, 'tools')
from run_skyrim import suite_failures
match = re.search(r'run_dir=(.+)$', Path('build/skyrim_run.log').read_text(), re.M)
if not match:
    print('No run evidence directory was reported; see build/skyrim_run.log')
    sys.exit(1)
run = Path(match.group(1))
issues = suite_failures(run)
rows = [json.loads(line) for line in (run / 'WoWBridge_test.jsonl').read_text().splitlines() if line.strip()] if not any('unreadable' in issue for issue in issues) else []
steps = [r for r in rows if isinstance(r, dict) and r.get('test')]
talents = [r for r in rows if isinstance(r, dict) and r.get('event') == 'talent_result']
print(f'{sum(r.get("pass") is True for r in steps)}/{len(steps)} steps passed')
if talents:
    print(f'{sum(isinstance(r.get("result"), dict) and r["result"].get("pass") is True for r in talents)}/{len(talents)} talent checks passed')
for issue in issues:
    print('  FAIL', issue)
if int(sys.argv[1]):
    print('  Runner exited with status', sys.argv[1])
print(f'screenshots and logs: {run}')
sys.exit(1 if issues or int(sys.argv[1]) else 0)
EOF
    ;;
build)
    game_up && { echo "Close Skyrim first (tools/play.sh stop): the plugin cannot be replaced while it runs."; exit 1; }
    bash tools/ship.sh && echo "built and deployed"
    ;;
stop)
    "$PY" -c "import sys; sys.path.insert(0, 'tools'); import run_skyrim; run_skyrim.stop_game()"
    pkill -f 'm service\.serve[r]' && echo "service: stopped" || true
    echo "Skyrim: stopped"
    ;;
status)
    echo "commit:   $(git log -1 --format='%h %s')"
    echo "packaged: $(date -r dist/WoWBridge/SKSE/Plugins/WoWBridge.dll '+%F %T' 2>/dev/null || echo none)"
    echo "deployed: $(date -r "$GAME_DIR/Data/SKSE/Plugins/WoWBridge.dll" '+%F %T' 2>/dev/null || echo none)"
    echo "service:  $(service_up && echo running || echo stopped)    Skyrim: $(game_up && echo running || echo stopped)"
    RUN=$(ls -td build/runs/*/ 2>/dev/null | head -1)
    [ -n "$RUN" ] && echo "last run: $RUN $(grep -o 'suite [a-z]* ([0-9]* failed)' "$RUN/WoWBridge.log" 2>/dev/null)"
    echo "log:      $SKSE_LOGS/WoWBridge.log"
    ;;
wow)
    "$PY" tools/wowref.py launch login
    ;;
*)
    sed -n '2,10p' "$0"; exit 1
    ;;
esac
