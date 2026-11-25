import json
P = "esphome.piazzetta_stove_press_keys"
L1, L2 = "sensor.pellet_stove_screen_line_1", "sensor.pellet_stove_screen_line_2"
def press(keys):
    return {"action": P, "data": {"keys": keys, "gap_ms": 700, "door": True}}
in_menu = "{{ 'RPM' in states('%s') or states('%s') in ['SELECT','UNIT OF','SET','MODE','MULTI','ENERGY','PARAMETER','ENABLE','STOVE'] }}" % (L1, L1)
at_status = "{{ is_state('%s','STOVE') and is_state('%s','STATUS') }}" % (L1, L2)
cfg = {
  "alias": "Pellet stove: read status",
  "description": "Opens STOVE STATUS over the radio (door frames), lets the board decode exhaust temperature, exhaust fan rpm and auger feed, then closes the menu with ESC and MENU. Checks the screen before SET. Keys rotate their counter: the stove drops a repeat of the previous key+counter.",
  "icon": "mdi:fireplace",
  "mode": "single",
  "sequence": [
    {"condition": "state", "entity_id": "switch.pellet_stove_transmit", "state": "on"},
    {"repeat": {"sequence": [
        press("{{ [[164,128,128],[4,128,128],[164,128,128]][repeat.index - 1] }}"),
        {"wait_template": "{{ is_state('%s','SELECT') }}" % L1, "timeout": 8, "continue_on_timeout": True}],
      "until": [{"condition": "template", "value_template": "{{ is_state('%s','SELECT') or repeat.index >= 3 }}" % L1}]}},
    {"if": [{"condition": "template", "value_template": "{{ not is_state('%s','SELECT') }}" % L1}],
     "then": [{"stop": "Menu did not open"}]},
    {"repeat": {"sequence": [
        press("{{ [[168,128,128],[200,128,128],[232,128,128]][(repeat.index - 1) % 3] }}"),
        {"wait_template": at_status, "timeout": 3, "continue_on_timeout": True}],
      "until": [{"condition": "template", "value_template": "{{ (is_state('%s','STOVE') and is_state('%s','STATUS')) or repeat.index >= 14 }}" % (L1, L2)}]}},
    {"if": [{"condition": "template", "value_template": "{{ not (is_state('%s','STOVE') and is_state('%s','STATUS')) }}" % (L1, L2)}],
     "then": [press([228, 128]), {"stop": "STOVE STATUS not reached"}]},
    {"delay": {"seconds": 2}},
    press([184, 128]),
    {"wait_template": "{{ 'RPM' in states('%s') }}" % L1, "timeout": 6, "continue_on_timeout": True},
    {"if": [{"condition": "template", "value_template": "{{ 'RPM' not in states('%s') }}" % L1}],
     "then": [{"delay": {"seconds": 2}}, press([216, 128]),
              {"wait_template": "{{ 'RPM' in states('%s') }}" % L1, "timeout": 6, "continue_on_timeout": True}]},
    {"delay": {"seconds": 3}},
    # Close (tested 2026-11-20 09:49): MENU (E4) is ignored on the status screen. ESC (0x90 + counter) goes back to
    # the STOVE STATUS menu item, then E4 closes the menu. E4 is only sent while the menu is showing.
    {"repeat": {"sequence": [
        press("{{ [[176,128,128],[208,128,128],[240,128,128]][repeat.index - 1] }}"),
        {"wait_template": "{{ 'RPM' not in states('%s') }}" % L1, "timeout": 4, "continue_on_timeout": True}],
      "until": [{"condition": "template", "value_template": "{{ 'RPM' not in states('%s') or repeat.index >= 3 }}" % L1}]}},
    {"repeat": {"sequence": [
        {"if": [{"condition": "template", "value_template": in_menu}],
         "then": [press("{{ [[228,128,128],[196,128,128],[228,128,128]][repeat.index - 1] }}"),
                  {"wait_template": "{{ not (%s) }}" % in_menu[3:-3], "timeout": 4, "continue_on_timeout": True}]}],
      "until": [{"condition": "template", "value_template": "{{ not (%s) or repeat.index >= 3 }}" % in_menu[3:-3]}]}},
  ],
}
print(json.dumps({"script_id": "pellet_stove_read_status", "config": cfg}))
