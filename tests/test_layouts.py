"""Scenario tests: native Windows Alt+Shift vs LayoutToggle.

Every scenario runs twice with real (synthetic) key input in a focused window:
first without the app (native Windows = oracle), then with the app running.

Rule: the app must produce exactly what native Windows produces, except for the
chord's *first* selection, which must be the most recently used layout (see
`model_override`). Scenarios that do not trigger a layout chord in native
Windows must stay untouched by the app.

Run:  python tests\\test_layouts.py        (needs an unlocked interactive desktop,
      >= 3 installed layouts; do not touch keyboard/mouse while it runs)
"""
import itertools
import sys
import unittest

import harness as h

L, R, ALT = "LSHIFT", "RSHIFT", "LALT"


def tap(k):
    return [("down", k), ("up", k)]


def chord(shifts, flyout_after_first=False, held_wait=0.0):
    """Alt held, Shifts tapped in order, Alt released."""
    s = [("down", ALT)]
    for i, k in enumerate(shifts):
        s += tap(k)
        if i == 0 and flyout_after_first:
            s.append(("wait", 0.9))   # > 400 ms: the flyout opens
        else:
            s.append(("wait", 0.05))
    if held_wait:
        s.append(("wait", held_wait))
    s.append(("up", ALT))
    return s


# name -> (steps, presses or None, histories)   presses: list of "L"/"R" the chord
# should count as; None = the scenario is not a chord (or decided from native).
ALL = list(itertools.permutations(range(3)))      # (older, prev, current)
ONE = [ALL[0], ALL[3]]
SCENARIOS = {
    "tap_left": (chord([L]), "L", ALL),
    "tap_right": (chord([R]), "R", ALL),
    "double_left": (chord([L, L]), "LL", ALL),
    "double_right": (chord([R, R]), "RR", ALL),
    "left_then_right": (chord([L, R]), "LR", ALL),
    "right_then_left": (chord([R, L]), "RL", ALL),
    "triple_left": (chord([L, L, L]), "LLL", ONE),
    "four_left": (chord([L] * 4), "LLLL", ONE),
    "flyout_left": (chord([L], True), "L", ALL),
    "flyout_right": (chord([R], True), "R", ALL),
    "flyout_left_left": (chord([L, L], True), "LL", ALL),
    "flyout_left_right": (chord([L, R], True), "LR", ALL),
    "flyout_right_right": (chord([R, R], True), "RR", ALL),
    "flyout_three": (chord([L, L, L], True), "LLL", ONE),
    "flyout_four_mixed": (chord([L, R, R, L], True), "LRRL", ONE),
    "flyout_hold_extra": (chord([L], True, 0.6), "L", ONE),
    "flyout_release_right_after_open": ([("down", ALT)] + tap(L) + [("wait", 0.47), ("up", ALT)], "L", ONE),
    "flyout_shift_right_after_open": ([("down", ALT)] + tap(L) + [("wait", 0.47)] + tap(L) + [("wait", 0.9), ("up", ALT)], "LL", ONE),
    "alt_auto_repeat": ([("down", ALT), ("down", ALT), ("down", ALT)] + tap(L) + [("up", ALT)], "L", ONE),
    "shift_auto_repeat": ([("down", ALT), ("down", L), ("down", L), ("down", L), ("up", L), ("up", ALT)], "L", ONE),
    "shift_first_then_alt": ([("down", L), ("down", ALT), ("up", L), ("up", ALT)], "auto", ONE),
    "shift_released_after_alt": ([("down", ALT), ("down", L), ("up", ALT), ("up", L)], "auto", ONE),
    "two_chords_in_a_row": (chord([L]) + [("wait", 0.2)] + chord([L]), "LL-separate", ALL),
    "right_alt_left_shift": ([("down", "RALT"), ("down", L), ("up", L), ("up", "RALT")], "auto", ONE),
    # Never a layout chord: must match native exactly (no change).
    "alt_only": ([("down", ALT), ("up", ALT)], None, ONE),
    "alt_held_long": ([("down", ALT), ("wait", 0.9), ("up", ALT)], None, ONE),
    "shift_only": (tap(L), None, ONE),
    "shift_held_long": ([("down", L), ("wait", 0.9), ("up", L)], None, ONE),
    "ctrl_shift": ([("down", "LCTRL")] + tap(L) + [("up", "LCTRL")], None, ONE),
    "alt_key_then_shift": ([("down", ALT)] + tap("A") + tap(L) + [("up", ALT)], "auto", ONE),
    "shift_key_no_alt": ([("down", L)] + tap("A") + [("up", L)], None, ONE),
    "alt_esc_like_cancel": ([("down", ALT)] + tap("ESC") + tap(L) + [("up", ALT)], "auto", ONE),
}

import os

_only = os.environ.get("SCEN")
if _only:   # e.g. SCEN=flyout,triple runs only matching scenarios
    SCENARIOS = {n: v for n, v in SCENARIOS.items() if any(p in n for p in _only.split(","))}

STEP = {"L": 1, "R": -1}   # Left Shift walks down the language list, Right Shift up


def calibrate(win):
    """Native cycle order as a list of system-list indexes, learned from Left taps."""
    sysl = h.system_layouts()
    nxt = {}
    for i in range(len(sysl)):
        nxt[i] = h.run_scenario(win, "native", [i], chord([L]))
    order, cur = [], 0
    for _ in sysl:
        order.append(cur)
        cur = nxt[cur]
    assert sorted(order) == list(range(len(sysl))), "native order is not a single cycle: %r" % nxt
    return order


def model_native(order, hist, presses):
    cur = hist[-1]
    for p in presses:
        cur = order[(order.index(cur) + STEP[p]) % len(order)]
    return cur


def model_override(order, hist, presses):
    """First press -> MRU previous. Then native direction; the layout the chord
    started from is skipped on the second press so it reaches the third layout."""
    orig, sel = hist[-1], hist[-2]
    for k, p in enumerate(presses[1:], start=2):
        step = STEP[p]
        sel = order[(order.index(sel) + step) % len(order)]
        if k == 2 and sel == orig:
            sel = order[(order.index(sel) + step) % len(order)]
    return sel


class Layouts(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if len(h.system_layouts()) < 3:
            raise unittest.SkipTest("needs at least 3 installed keyboard layouts")
        cls.win = h.TestWindow()
        cls.win.focus()
        cls.order = calibrate(cls.win)
        cls.native, cls.override = {}, {}
        cls._collect(cls.native, "native")
        with h.Override():
            cls._collect(cls.override, "override")

    @classmethod
    def _collect(cls, into, mode):
        for name, (steps, _, hists) in SCENARIOS.items():
            for hist in hists:
                into[name, hist] = h.run_scenario(cls.win, mode, hist, steps)

    @classmethod
    def tearDownClass(cls):
        cls.win.close()


def make_test(name, steps, presses, hist):
    def test(self):
        order = self.order
        nat = self.native[name, hist]
        ovr = self.override[name, hist]
        orig = hist[-1]
        if presses is None:
            self.assertEqual(nat, orig, "native Windows changed layout unexpectedly")
            self.assertEqual(ovr, nat, "app changed a non-chord scenario")
            return
        if presses == "auto":
            expected = orig if nat == orig else None
            if expected is None:
                self.assertEqual(ovr, model_override(order, hist, "L"),
                                 "native switches here, app must act as a chord")
            else:
                self.assertEqual(ovr, nat, "native does nothing here, app must not either")
            return
        if presses == "LL-separate":
            # two independent chords: the second one toggles back to where we were
            self.assertEqual(ovr, orig, "two consecutive Alt+Shift taps must toggle back")
            return
        self.assertEqual(nat, model_native(order, hist, presses), "oracle (native) disagrees with model")
        self.assertEqual(ovr, model_override(order, hist, presses))
    return test


for _name, (_steps, _presses, _hists) in SCENARIOS.items():
    for _hist in _hists:
        setattr(Layouts, "test_%s_%s" % (_name, "".join(map(str, _hist))),
                make_test(_name, _steps, _presses, _hist))

if __name__ == "__main__":
    unittest.main(verbosity=2)
