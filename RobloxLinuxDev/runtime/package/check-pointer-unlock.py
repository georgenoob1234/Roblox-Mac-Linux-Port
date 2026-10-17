#!/usr/bin/env python3
"""Offline regression checks for the Wayland pointer-capture unlock path.

What happened (2026-10-17): KWin applies the destroy of a persistent
zwp_locked_pointer_v1 at the constrained surface's NEXT COMMIT (or at
focus-loss deactivation). apply_capture's unlock branch destroyed the
constraint proxies but never committed the GTK toplevel — and during
gameplay only the SDL subsurface commits — so the compositor lock
outlived the client-side destroy. Result: right-click camera release and
Shift Lock exit trapped the cursor until the next focus change (Super).

The runtime capture state machine (request coalescing, FIFO ordering,
focus/blur release, cursor visibility) is exercised by `wayland-check`,
built and run by `RobloxLinuxDev/runtime/optimized/ui-build.sh`. No hosted
CI job can reproduce the compositor interaction itself, so this check
asserts the exact unlock sequence that provably releases the constraint,
and then SELF-VERIFIES by re-running the assertions against mutated
sources that reintroduce the historical bug — every mutation must be
detected, so the check cannot silently degrade into a presence-only test.

Asserted invariants on `runtime/browser/wayland.cpp` apply_capture:
  1. Idempotence guard: the function returns early when the requested
     state already matches the proxy state, so a redundant unlock can
     never destroy a null proxy.
  2. Unlock branch sequence: destroy zwp_locked_pointer_v1 and
     zwp_relative_pointer_v1, queue inactive capture state, update the
     capture cursor, release the SDL capture, and THEN commit the
     constrained GTK toplevel — the compositor-side release.
  3. The unlock commit sits before the trailing wl_display_flush that
     publishes it.
  4. Lock branch: install the constraint, then commit (activation is
     latched at that commit — the part that always worked).
  5. native/wayland.cpp (the optimized build the game actually loads)
     keeps #including the shared browser backend, so capture-path fixes
     reach the running game.
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BROWSER = ROOT / "runtime" / "browser" / "wayland.cpp"
NATIVE = ROOT / "native" / "wayland.cpp"


def apply_capture_body(source: str) -> str:
    start = source.index("static void apply_capture(")
    end = source.index("static void refresh_pointer_focus", start)
    return source[start:end]


def position(text: str, needle: str):
    try:
        return text.index(needle)
    except ValueError:
        return None


def problems_for(browser: str, native: str) -> list:
    problems = []

    def require(condition, message):
        if not condition:
            problems.append(message)

    body = apply_capture_body(browser)
    split = body.index("} else {")
    lock_branch, unlock_branch = body[:split], body[split:]

    # 1. Idempotence guard: no-op when proxy state already matches request.
    require(
        "pointer_capture.requested" in body
        and position(body, "(locked_pointer!=nullptr)==pointer_capture.requested")
        is not None,
        "apply_capture must keep its idempotence guard "
        "((locked_pointer!=nullptr)==pointer_capture.requested) so repeated "
        "unlock requests cannot destroy a null proxy",
    )

    # 2. Unlock branch: destroy -> state/cursor/SDL release -> toplevel commit.
    unlock_steps = [
        "zwp_locked_pointer_v1_destroy",
        "zwp_relative_pointer_v1_destroy",
        "queue_capture_state(false)",
        "update_capture_cursor()",
        "SDL_CaptureMouse(false)",
        "wl_surface_commit",
    ]
    seen = {}
    for step in unlock_steps:
        pos = position(unlock_branch, step)
        if pos is None:
            problems.append(
                f"apply_capture unlock branch lost a required step: {step}"
            )
        else:
            seen[step] = pos
    ordered = [
        seen[step]
        for step in unlock_steps
        if step in seen
    ]
    require(
        ordered == sorted(ordered),
        "apply_capture unlock branch must run, in order: destroy "
        "zwp_locked_pointer_v1/zwp_relative_pointer_v1, queue inactive "
        "capture state, update the capture cursor, release the SDL capture, "
        "then wl_surface_commit the constrained toplevel — KWin applies the "
        "constraint release at that commit, and during gameplay only the SDL "
        "subsurface commits (missing/early commit reintroduces the "
        "stuck-cursor bug: the lock survives a client-side destroy until "
        "focus loss, trapping the mouse after right-click release and Shift "
        "Lock exit)",
    )

    # 3. The release commit must precede the flush that publishes it.
    commit_pos = seen.get("wl_surface_commit")
    flush_pos = position(unlock_branch, "wl_display_flush")
    require(
        commit_pos is not None and flush_pos is not None and commit_pos < flush_pos,
        "the unlock wl_surface_commit must occur before the trailing "
        "wl_display_flush that publishes the release to the compositor",
    )

    # 4. Lock branch: install the constraint, then commit.
    install = position(lock_branch, "zwp_pointer_constraints_v1_lock_pointer")
    lock_commit = position(lock_branch, "wl_surface_commit")
    require(
        install is not None
        and lock_commit is not None
        and lock_commit > install,
        "apply_capture lock branch must wl_surface_commit the toplevel after "
        "lock_pointer — constraint activation is latched at that commit",
    )

    # 5. The optimized build the game loads must compile the shared backend.
    require(
        '#include "../runtime/browser/wayland.cpp"' in native,
        'native/wayland.cpp must keep #include "../runtime/browser/wayland.cpp" '
        "so the optimized build the game actually loads picks up capture-path "
        "fixes (a stale optimized/ui copy once made a correct fix appear "
        "ineffective)",
    )
    return problems


# Mutations that reintroduce the historical bug; each MUST be detected.
UNLOCK_COMMIT = "if(release_surface)wl_surface_commit(release_surface);"
LOCK_COMMIT_BLOCK = (
    "// Commit the parent surface after installing its pointer constraint.\n"
    "        wl_surface_commit(surface);"
)
MUTATIONS = [
    (
        "drop unlock commit (the original bug)",
        lambda b, n: (b.replace(UNLOCK_COMMIT, "(void)release_surface;"), n),
    ),
    (
        "commit before the constraint destroy",
        lambda b, n: (
            b.replace(
                "zwp_locked_pointer_v1_destroy(locked_pointer);locked_pointer=nullptr;",
                "auto early_surface=gdk_wayland_window_get_wl_surface("
                "gtk_widget_get_window(window));"
                "if(early_surface)wl_surface_commit(early_surface);"
                "zwp_locked_pointer_v1_destroy(locked_pointer);"
                "locked_pointer=nullptr;",
            ),
            n,
        ),
    ),
    (
        "drop lock-branch commit",
        lambda b, n: (b.replace(LOCK_COMMIT_BLOCK, ""), n),
    ),
    (
        "drop shared backend include",
        lambda b, n: (
            b,
            n.replace('#include "../runtime/browser/wayland.cpp"', ""),
        ),
    ),
]


def main() -> int:
    browser = BROWSER.read_text()
    native = NATIVE.read_text()

    problems = problems_for(browser, native)
    if problems:
        for problem in problems:
            print(f"FAIL: {problem}", file=sys.stderr)
        return 1

    # Self-verification: every bug-reintroducing mutation must be caught.
    undetected = []
    for name, mutate in MUTATIONS:
        mutated_browser, mutated_native = mutate(browser, native)
        if mutated_browser == browser and mutated_native == native:
            undetected.append(f"{name}: mutation did not apply (stale pattern?)")
            continue
        if not problems_for(mutated_browser, mutated_native):
            undetected.append(f"{name}: NOT DETECTED")
    if undetected:
        for problem in undetected:
            print(f"FAIL self-test: {problem}", file=sys.stderr)
        return 1

    print(
        "PASS pointer unlock sequence, idempotence guard, lock commit, "
        f"shared optimized backend; {len(MUTATIONS)} bug-reintroducing "
        "mutations all detected"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
