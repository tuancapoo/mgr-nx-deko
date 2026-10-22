#!/usr/bin/env python3
"""The post-codegen steps of the codegen flavour, in one place (docs/enhancements.md, "Build-time").

Run after EVERY codegen, by tools/codegen.ps1 and by the build itself: app/CMakeLists.txt appends this script to the
generated sa2_codegen command, because the PC build re-runs the generator on its own whenever an input changes (the
manifest, a toml, the xex, the generator binary) and that would otherwise drop the steps silently.

  - the flavour is read from app/codegen_flavor.toml (written by tools/codegen.ps1 -Flavor): "optimised" when it sets
    any codegen key, else "classic". An explicit `# codegen-stage:` marker instead selects a validated,
    generator-only partial experiment: none of the optimised post-codegen passes below run;
  - optimised: tools/pch_no_volatile.py + tools/loop_barriers.py (required together) and tools/direct_calls.py on both
    generated trees; then sasrt-nx's steps of 2026-10-12, each a rename of calls into macros the generated pch
    defines: tools/guest_cas.py (compare-and-swap without the legacy builtin's trailing `dmb ish` on AArch64),
    tools/flush_mode_flow.py (the flush-mode checks the control flow and the callees' summaries fix: nothing on the
    Switch, still tested in the PC build), tools/vec_byteswap.py (the 16-byte reversal of guest vector loads and
    stores without a table lookup on AArch64) and tools/pch_neon_shuffles.py (constant word shuffles as one
    permutation on AArch64);
  - optimised, the second CPU round of 2026-10-12 (each step has its own compile-time switch back, see the tool):
      tools/indirect_calls.py   every `bctrl` tagged with its site (PC census), and the sites of
                                app/indirect_promotions.txt as a guarded direct call. Before direct_calls.py.
      tools/ro_constants.py     loads from the image's read-only data as constants. Before float_loads.py.
      tools/float_loads.py      a single-precision load straight into a float register
      tools/frame_traffic.py    the link register and the non-volatile saves of the function prologues
      tools/leaf_locals.py      the ctx registers of leaf functions with loops as locals. Last: it renames registers.
      tools/pch_cold_traps.py   the pch's trap helper (before every division) out of line and cold
  - optimised, the third CPU round of 2026-10-12:
      tools/loop_locals.py      the ctx registers a call-free loop uses as locals for the length of the loop (the
                                barrier stays). After loop_barriers.py, before the localized copies and
                                leaf_locals.py (which takes the leaf functions).
    ro_constants.py and the entry modes of flush_mode_flow.py need the generator's image dump (out/default_image.bin,
    written by tools/codegen.ps1 through REXGLUE_DUMP_IMAGE); the steps whose PC form calls into the app
    (indirect_calls, ro_constants) run on the game's tree only, not on the stage module (a DLL of its own on the PC);
  - both: the localized native copies (tools/localize_native.py --list app/src/cpu/hot/localized.txt) and
    app/generated/codegen_flavor.txt (the build's name in the first "[enh]" log line).

Every step is idempotent and the whole sequence is too: a second run of this script on a processed tree changes no
file (checked on a copy: tools/scratch_ab.py copy, then this script twice with --gen out/gen_scratch and a hash of the
tree after each run; the profile of a PGO build fits only the exact
text it was made from).

  python tools/post_codegen.py [--gen <generated folder>]
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
APP = os.path.join(ROOT, 'app')
GEN = os.path.join(APP, 'generated')
IMAGE = os.path.join(ROOT, 'out', 'default_image.bin')
PROMOTIONS = os.path.join(APP, 'indirect_promotions.txt')


def run(*args):
    result = subprocess.run([sys.executable, *args], cwd=ROOT)
    if result.returncode != 0:
        sys.exit(f'post_codegen: {" ".join(os.path.basename(a) for a in args[:1])} failed ({result.returncode})')


def tool(name):
    return os.path.join(ROOT, 'tools', name)


def select_pipeline(flavor_toml):
    """Partial experiments must never implicitly enable all post-codegen passes."""
    enabled = set(re.findall(r'^\s*([a-z_]+)\s*=\s*true\s*(?:#.*)?$', flavor_toml, re.M))
    stage = re.search(r'^# codegen-stage: ([a-z0-9-]+)\s*$', flavor_toml, re.M)
    if stage:
        expected = {
            's1-fp-moves': {'fp_moves_modeless'},
            's2-control-locals': {'fp_moves_modeless', 'cr_as_local', 'ctr_as_local',
                                  'xer_as_local', 'reserved_as_local'},
        }
        name = stage.group(1)
        if name not in expected or enabled != expected[name]:
            raise ValueError(f'invalid generator-only stage {name}: {sorted(enabled)}')
        return name, False
    return ('optimised', True) if enabled else ('classic', False)


def main():
    gen = sys.argv[sys.argv.index('--gen') + 1] if '--gen' in sys.argv else GEN
    scratch = gen != GEN  # a copy for measurements (tools/scratch_ab.py): no localized copies, no flavour file
    flavor_toml = open(os.path.join(APP, 'codegen_flavor.toml'), encoding='utf-8').read()
    flavor, optimised = select_pipeline(flavor_toml)
    # MGR's inline jump-table data overlaps gap-fill nodes in existing generated
    # trees. Migrate only image-verified branches, then recompute flow proofs.
    run(tool('mgr_repair_branches.py'), '--gen', os.path.join(gen, 'default'))
    trees = [t for t in ('default', 'data_dll') if os.path.isdir(os.path.join(gen, t))]
    if optimised:
        if not os.path.isfile(IMAGE):
            sys.exit('post_codegen: out/default_image.bin is missing (the image dump of the code generator): run '
                     'tools\\codegen.ps1 -Flavor optimised once, it writes it (REXGLUE_DUMP_IMAGE)')
        xex = os.path.join(ROOT, 'assets', 'game_root', 'default.xex')
        if os.path.isfile(xex) and os.path.getmtime(xex) > os.path.getmtime(IMAGE):
            sys.exit('post_codegen: out/default_image.bin is older than assets/game_root/default.xex: run '
                     'tools\\codegen.ps1 -Flavor optimised (it writes the dump again)')
        run(tool('pch_no_volatile.py'), *[os.path.join(gen, t, 'sa2_pch.h') for t in trees])
        os.environ.setdefault('REXSDK_DIR', os.path.join(ROOT, 'sdk'))
        # Second round: the tagged and promoted indirect calls. Before direct_calls.py (a promoted call is a direct
        # call like any other: __imp__ where its target has no hook) and BEFORE loop_barriers.py: a loop needs no
        # barrier when every way round it passes an indirect call, and a promoted call is no longer one (the
        # compiler can inline its target and see that it leaves the polled memory alone).
        run(tool('indirect_calls.py'), '--gen', os.path.join(gen, 'default'), '--promote', PROMOTIONS)
        # REQUIRED with pch_no_volatile.py: without barriers GCC turns guest polling loops into infinite loops.
        run(tool('loop_barriers.py'), *[os.path.join(gen, t) for t in trees])
        run(tool('pch_cold_traps.py'), *[os.path.join(gen, t, 'sa2_pch.h') for t in trees])
        for tree in trees:
            # Undo and redo in one pass: a function hooked since the last run (a new native) must be called through
            # its hook again. --redo writes only the files that change, so the build does not recompile the others.
            run(tool('direct_calls.py'), '--redo', '--gen', os.path.join(gen, tree))
        for tree in trees:
            # sasrt-nx's steps (2026-10-12). After direct_calls.py: the flush-mode proofs depend on which callees are
            # called directly, and --redo drops proofs from before a function was hooked. They only rename calls to
            # macros of the generated pch, so the localized copies below are made from the same statements.
            run(tool('guest_cas.py'), '--gen', os.path.join(gen, tree))
            # The entry modes (second round) need the image: the game's tree only.
            run(tool('flush_mode_flow.py'), '--redo', '--gen', os.path.join(gen, tree),
                *(['--image', IMAGE] if tree == 'default' else []))
            run(tool('vec_byteswap.py'), '--gen', os.path.join(gen, tree))
            run(tool('pch_neon_shuffles.py'), os.path.join(gen, tree, 'sa2_pch.h'))
        # Second round, the rest. ro_constants before float_loads (it folds the integer form of a float load).
        run(tool('ro_constants.py'), '--gen', os.path.join(gen, 'default'), '--image', IMAGE)
        for tree in trees:
            run(tool('float_loads.py'), '--gen', os.path.join(gen, tree))
            run(tool('frame_traffic.py'), '--gen', os.path.join(gen, tree))
    if optimised:
        for tree in trees:
            # Third round: the ctx registers of call-free loops as locals for the length of the loop. Before the
            # localized copies, which are made from this text and rename its ctx registers once more.
            run(tool('loop_locals.py'), '--gen', os.path.join(gen, tree))
    if not scratch:
        # Before leaf_locals.py in a fresh tree; after it when this script runs again (the tool reads both forms).
        run(tool('localize_native.py'), '--list', os.path.join(APP, 'src', 'cpu', 'hot', 'localized.txt'))
    if optimised:
        for tree in trees:
            run(tool('leaf_locals.py'), '--gen', os.path.join(gen, tree))
    if scratch:
        print(f'post_codegen: {flavor} codegen steps done on {gen}')
        return
    # Round 7: the options added since round 6 are named too, so a console log tells the builds apart.
    extras = [name for key, name in (('non_volatile_as_local', 'non-volatile locals'), ('elide_lr_stores', 'no lr stores'),
                                     ('fp_moves_modeless', 'modeless fp moves'),
                                     ('non_argument_as_local', 'non-argument locals'),
                                     ('fp_mode_lookahead', 'fp mode lookahead'),
                                     ('fcti_inline', 'second round'))
              if re.search(r'^\s*' + key + r'\s*=\s*true', flavor_toml, re.M)]
    # Written only when it changes: the file is a CMake configure dependency (app/CMakeLists.txt), and rewriting it
    # made every build re-run CMake, rebuild the generator and run the codegen again (2026-10-07).
    text = f'{flavor} codegen' + (f' ({", ".join(extras)})' if extras else '')
    path = os.path.join(GEN, 'codegen_flavor.txt')
    try:
        old_text = open(path, encoding='utf-8').read()
    except OSError:
        old_text = None
    if old_text != text:
        with open(path, 'w', encoding='utf-8', newline='\n') as f:
            f.write(text)
    print(f'post_codegen: {flavor} codegen steps done')


if __name__ == '__main__':
    main()
