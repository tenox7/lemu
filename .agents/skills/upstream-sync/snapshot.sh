#!/bin/sh
# Save what a LEMU build has, and how its key qtests do, so that the build
# from before a merge can be compared with the build from after it.
# Run it in the build folder:
#   snapshot.sh old    before the merge
#   snapshot.sh new    after the merge; it also compares with old
# -q skips the qtests and only lists features.
# It writes feat-LABEL/, tests-LABEL.txt and tests-LABEL.log.
# exit: 0 fine, 1 a feature was lost or a qtest fails or stops running,
#       2 error

quick=no
if [ "${1-}" = -q ]; then
    quick=yes
    shift
fi
label=${1-}
case $label in
old|new) ;;
*) echo "usage: snapshot.sh [-q] old|new" >&2; exit 2 ;;
esac

die() {
    echo "snapshot.sh: $*" >&2
    exit 2
}

[ -f build.ninja ] && [ -f config-host.mak ] || die "run it in a configured build folder"
src=$(sed -n 's/^SRC_PATH=//p' config-host.mak)
features=$src/machines/release/features.sh
[ -f "$features" ] || die "no $features"
py=./pyvenv/bin/python3
pass='^(OK|SKIP|EXPECTEDFAIL)$'

# 1. one feature list per system emulator
rm -rf "feat-$label"
mkdir "feat-$label" || die "cannot make feat-$label"
for q in qemu-system-*; do
    case $q in *-unsigned|*.p) continue ;; esac
    [ -f "$q" ] && [ -x "$q" ] || continue
    sh "$features" "./$q" >"feat-$label/$q.txt" || die "features.sh failed on $q"
done
n=$(ls "feat-$label" | wc -l | tr -d ' ')
[ "$n" -gt 0 ] || die "no qemu-system-* binaries here: build first"
echo "features: $n binaries listed in feat-$label/"

# 2. the qtests that start every machine and every device
if [ $quick = no ]; then
    tests=$("$py" - <<'EOF'
import json, re
want = re.compile(r'qtest-[^/]+/(qmp-cmd-test|device-introspect-test|qom-test|test-hmp|mcp-test)')
with open('meson-info/intro-tests.json') as f:
    names = {t['name'] for t in json.load(f) if want.fullmatch(t['name'])}
print(' '.join(sorted(names)))
EOF
) || die "cannot read meson-info/intro-tests.json"
    [ -n "$tests" ] || die "none of the qtests are configured"
    # shellcheck disable=SC2086
    echo "qtests: running $(echo $tests | wc -w | tr -d ' ') tests, this takes a while"
    # a stale log from the last run must not count as this run
    rm -f meson-logs/testlog-slow.json
    # QEMU puts these in its "slow" suite, which the default setup skips
    # shellcheck disable=SC2086
    ./run meson test --setup slow --print-errorlogs $tests >"tests-$label.log" 2>&1
    "$py" - <<'EOF' >"tests-$label.unsorted" || die "no test results: see tests-$label.log"
import json
with open('meson-logs/testlog-slow.json') as f:
    for line in f:
        t = json.loads(line)
        print(t['name'].split(' / ')[-1], t['result'])
EOF
    LC_ALL=C sort "tests-$label.unsorted" >"tests-$label.txt"
    rm -f "tests-$label.unsorted"
    [ -s "tests-$label.txt" ] || die "no test results: see tests-$label.log"
    ok=$(awk -v p="$pass" '$2 ~ p' "tests-$label.txt" | wc -l | tr -d ' ')
    bad=$(awk -v p="$pass" '$2 !~ p' "tests-$label.txt" | wc -l | tr -d ' ')
    echo "qtests: $ok passed, $bad failed (tests-$label.txt, output in tests-$label.log)"
fi

[ "$label" = new ] || exit 0

# 3. compare with the build from before the merge
[ -d feat-old ] || die "no feat-old/: run 'snapshot.sh old' before the merge"
fail=0
for f in feat-old/*.txt; do
    b=$(basename "$f" .txt)
    if [ ! -f "feat-new/$b.txt" ]; then
        echo "LOST: $b, the whole binary"
        fail=1
        continue
    fi
    lost=$(LC_ALL=C comm -23 "$f" "feat-new/$b.txt")
    if [ -n "$lost" ]; then
        echo "$lost" | sed "s/^/LOST $b: /"
        fail=1
    fi
    added=$(LC_ALL=C comm -13 "$f" "feat-new/$b.txt" | wc -l | tr -d ' ')
    [ "$added" -eq 0 ] || echo "$b: $added new (comm -13 feat-old/$b.txt feat-new/$b.txt)"
done
if [ $quick = no ]; then
    if [ -f tests-old.txt ]; then
        # GONE: a test that passed before and no longer runs, for example
        # because a conflict fix dropped LEMU's line from a meson.build
        out=$(awk -v p="$pass" '
            NR == FNR { old[$1] = $2; next }
            { seen[$1] = 1 }
            $2 ~ p { next }
            ($1 in old) && old[$1] !~ p { print "still failing: " $1 " " $2; next }
            { print "NEW FAIL: " $1 " " $2 }
            END { for (t in old) if (!(t in seen) && old[t] ~ p) print "GONE: " t " (passed before)" }
        ' tests-old.txt tests-new.txt)
        [ -z "$out" ] || echo "$out"
        case $out in *"NEW FAIL"*|*"GONE"*) fail=1 ;; esac
    else
        echo "no tests-old.txt, so the qtests were not compared"
    fi
fi
if [ $fail = 0 ]; then
    echo "compare: OK"
else
    echo "compare: FAILED. Explain or fix every LOST, NEW FAIL and GONE line."
fi
exit $fail
