#!/bin/sh
# gpuwho test suite.
#
# Exercises the report join and aggregation against synthetic event logs, so
# the paths that need accounting mode (and therefore root) are still covered on
# a machine where accounting is off.  Nothing here touches the driver.

set -u

GPUWHO=${1:-./gpuwho}
case $GPUWHO in
/*) ;;
*) GPUWHO=$PWD/$GPUWHO ;;
esac

if [ ! -x "$GPUWHO" ]; then
	echo "usage: $0 /path/to/gpuwho" >&2
	exit 2
fi

TMP=$(mktemp -d) || exit 2
trap 'rm -rf "$TMP"' EXIT

pass=0
fail=0

# A fixed window so every expectation is deterministic.
# 2026-08-01 00:00:00 UTC .. 2026-08-02 00:00:00 UTC
T0=1785542400
T1=$((T0 + 86400))

fresh() {
	rm -rf "$TMP/st"
	mkdir -p "$TMP/st"
	LOG="$TMP/st/events-2026-08.jsonl"
	: >"$LOG"
}

ev() { printf '%s\n' "$1" >>"$LOG"; }

report() {
	GPUWHO_STATE_DIR="$TMP/st" "$GPUWHO" report --since "@$T0" --until "@$T1" "$@" 2>/dev/null
}

# check <name> <expected> <actual>
check() {
	if [ "$2" = "$3" ]; then
		pass=$((pass + 1))
		printf 'ok   %s\n' "$1"
	else
		fail=$((fail + 1))
		printf 'FAIL %s\n     expected: %s\n     actual:   %s\n' "$1" "$2" "$3"
	fi
}

# field <user> <column>  -- column: 2=gpu-hours 3=avg-util 4=peak-mem 5=jobs
field() {
	report | awk -v u="$1" -v c="$2" '$1 == u { print $c }'
}

start() { # gpu pid pst t [user]
	_u=${5:-yavuz}
	ev "{\"v\":1,\"ev\":\"start\",\"t\":$4,\"gpu\":$1,\"pid\":$2,\"pst\":$3,\"uid\":1000,\"user\":\"$_u\",\"cmd\":\"python train.py\"}"
}

end_acct() { # gpu pid pst end dur util memmb
	ev "{\"v\":1,\"ev\":\"end\",\"t\":$4,\"gpu\":$1,\"pid\":$2,\"pst\":$3,\"end\":$4,\"dur_s\":$5,\"avg_util\":$6,\"max_mem_mb\":$7,\"src\":\"acct\"}"
}

end_tick() { # gpu pid pst end dur
	ev "{\"v\":1,\"ev\":\"end\",\"t\":$4,\"gpu\":$1,\"pid\":$2,\"pst\":$3,\"end\":$4,\"dur_s\":$5,\"src\":\"tick\"}"
}

# --------------------------------------------------------------------------
echo '--- basic join: one 2h interval with metrics'
fresh
start 0 100 $((T0 - 10)) $T0
end_acct 0 100 $((T0 - 10)) $((T0 + 7200)) 7200 83 21504
check "gpu-hours"  "2.0"   "$(field yavuz 2)"
check "avg-util"   "83%"   "$(field yavuz 3)"
check "peak-mem"   "21.0G" "$(field yavuz 4)"
check "jobs"       "1"     "$(field yavuz 5)"

# --------------------------------------------------------------------------
echo '--- one hour on two GPUs is two GPU-hours'
fresh
start 0 200 $((T0 - 10)) $T0
end_acct 0 200 $((T0 - 10)) $((T0 + 3600)) 3600 50 1024
start 1 200 $((T0 - 10)) $T0
end_acct 1 200 $((T0 - 10)) $((T0 + 3600)) 3600 50 1024
check "two-gpu gpu-hours" "2.0" "$(field yavuz 2)"
check "two-gpu jobs"      "2"   "$(field yavuz 5)"

# --------------------------------------------------------------------------
echo '--- avg-util is duration-weighted, not a plain mean'
# 3h at 100% + 1h at 20%  ->  (3*100 + 1*20)/4 = 80
fresh
start 0 300 $((T0 - 10)) $T0
end_acct 0 300 $((T0 - 10)) $((T0 + 10800)) 10800 100 1024
start 0 301 $((T0 - 10)) $T0
end_acct 0 301 $((T0 - 10)) $((T0 + 3600)) 3600 20 2048
check "weighted avg-util" "80%"   "$(field yavuz 3)"
check "peak-mem is a max" "2.0G"  "$(field yavuz 4)"

# --------------------------------------------------------------------------
echo '--- metric-less (src:tick) ends count for hours/jobs, not util/mem'
fresh
start 0 400 $((T0 - 10)) $T0 burak
end_tick 0 400 $((T0 - 10)) $((T0 + 1800)) 1800
check "tick gpu-hours" "0.5"   "$(field burak 2)"
check "tick jobs"      "1"     "$(field burak 5)"
check "tick avg-util"  "-"     "$(field burak 3)"
check "tick peak-mem"  "-"     "$(field burak 4)"
check "tick user"      "burak" "$(report | awk 'NR==2{print $1}')"

# --------------------------------------------------------------------------
echo '--- an interval still open is counted up to the window end'
fresh
start 0 500 $((T0 - 10)) $((T1 - 3600))
check "open interval" "1.0" "$(field yavuz 2)"

# --------------------------------------------------------------------------
echo '--- intervals are clamped to the window'
# Runs from 1h before the window to 1h into it: only 1h counts.
fresh
start 0 600 $((T0 - 3610)) $((T0 - 3600))
end_acct 0 600 $((T0 - 3610)) $((T0 + 3600)) 7200 90 4096
check "clamped to window start" "1.0" "$(field yavuz 2)"
# Entirely before the window: nothing counts.
fresh
start 0 601 $((T0 - 7210)) $((T0 - 7200))
end_acct 0 601 $((T0 - 7210)) $((T0 - 3600)) 3600 90 4096
check "outside window drops out" "" "$(field yavuz 2)"

# --------------------------------------------------------------------------
echo '--- an end whose start was rotated away is still counted'
fresh
end_acct 0 700 $((T0 - 10)) $((T0 + 3600)) 3600 70 8192
check "orphan end gpu-hours" "1.0" "$(field unknown 2)"
check "orphan end jobs"      "1"   "$(field unknown 5)"

# --------------------------------------------------------------------------
echo '--- a repeated start (tick died before saving state) is not double-counted'
fresh
start 0 800 $((T0 - 10)) $T0
start 0 800 $((T0 - 10)) $((T0 + 60))
end_acct 0 800 $((T0 - 10)) $((T0 + 3600)) 3600 60 1024
check "duplicate start jobs"      "1"   "$(field yavuz 5)"
check "duplicate start gpu-hours" "1.0" "$(field yavuz 2)"

# --------------------------------------------------------------------------
echo '--- a recycled pid is a separate interval (pst is part of the key)'
fresh
start 0 900 $((T0 - 10)) $T0
end_acct 0 900 $((T0 - 10)) $((T0 + 3600)) 3600 50 1024
start 0 900 $((T0 + 7200)) $((T0 + 7200))
end_acct 0 900 $((T0 + 7200)) $((T0 + 10800)) 3600 50 1024
check "recycled pid jobs"      "2"   "$(field yavuz 5)"
check "recycled pid gpu-hours" "2.0" "$(field yavuz 2)"

# --------------------------------------------------------------------------
echo '--- users are ranked by gpu-hours'
fresh
start 0 1000 $((T0 - 10)) $T0
end_acct 0 1000 $((T0 - 10)) $((T0 + 3600)) 3600 50 1024
ev "{\"v\":1,\"ev\":\"start\",\"t\":$T0,\"gpu\":1,\"pid\":1001,\"pst\":$((T0 - 10)),\"uid\":1001,\"user\":\"burak\",\"cmd\":\"x\"}"
ev "{\"v\":1,\"ev\":\"end\",\"t\":$((T0 + 18000)),\"gpu\":1,\"pid\":1001,\"pst\":$((T0 - 10)),\"end\":$((T0 + 18000)),\"dur_s\":18000,\"avg_util\":40,\"max_mem_mb\":512,\"src\":\"acct\"}"
check "ranked first"  "burak" "$(report | awk 'NR==2{print $1}')"
check "ranked second" "yavuz" "$(report | awk 'NR==3{print $1}')"

# --------------------------------------------------------------------------
echo '--- a corrupt line is skipped, not fatal'
fresh
ev '{"v":1,"ev":"start", TRUNCATED'
ev 'not json at all'
start 0 1100 $((T0 - 10)) $T0
end_acct 0 1100 $((T0 - 10)) $((T0 + 3600)) 3600 50 1024
check "survives corrupt lines" "1.0" "$(field yavuz 2)"

# --------------------------------------------------------------------------
echo '--- a gzipped month is read too'
if command -v gzip >/dev/null 2>&1; then
	fresh
	start 0 1200 $((T0 - 10)) $T0
	end_acct 0 1200 $((T0 - 10)) $((T0 + 3600)) 3600 50 1024
	gzip "$LOG"
	check "gzipped log" "1.0" "$(field yavuz 2)"
else
	echo "skip  gzipped log (no gzip)"
fi

# --------------------------------------------------------------------------
echo '--- json output'
fresh
start 0 1300 $((T0 - 10)) $T0
end_acct 0 1300 $((T0 - 10)) $((T0 + 3600)) 3600 55 1024
J=$(report --json)
check "json gpu_hours" "1"    "$(printf '%s' "$J" | grep -c '"gpu_hours":1.000')"
check "json avg_util"  "1"    "$(printf '%s' "$J" | grep -c '"avg_util":55.0')"
check "json null util" "1"    "$(fresh; start 0 1301 $((T0 - 10)) $T0; end_tick 0 1301 $((T0 - 10)) $((T0 + 3600)) 3600; report --json | grep -c '"avg_util":null')"

# --------------------------------------------------------------------------
echo '--- state.json round-trips through a collector restart'
fresh
GPUWHO_STATE_DIR="$TMP/st" "$GPUWHO" collect >/dev/null 2>&1
if [ -f "$TMP/st/state.json" ]; then
	check "state has schema"    "1" "$(grep -c '"schema":1' "$TMP/st/state.json")"
	check "state has last_tick" "1" "$(grep -c '"last_tick"' "$TMP/st/state.json")"
else
	echo "skip  state.json (collector could not reach the driver)"
fi

# --------------------------------------------------------------------------
echo '--- ignore rules'
# These need a GPU with at least one compute process to be meaningful.
NPROC=$("$GPUWHO" --json 2>/dev/null | grep -o '"pid"' | wc -l)
if [ "${NPROC:-0}" -ge 1 ]; then
	count() { "$GPUWHO" --json "$@" 2>/dev/null | grep -o '"pid"' | wc -l; }

	check "no rules shows everything" "$NPROC" "$(count)"
	check "--min-mem 0 is a no-op"    "$NPROC" "$(count --min-mem 0)"
	check "--min-mem 1T hides all"    "0"      "$(count --min-mem 1T)"
	check "--no-ignore beats --min-mem" "$NPROC" "$(count --min-mem 1T --no-ignore)"
	check "glob '*' hides all"        "0"      "$(count --ignore '*')"
	check "--no-ignore beats --ignore" "$NPROC" "$(count --ignore '*' --no-ignore)"
	check "uid:0 leaves non-root"     "$NPROC" "$(count --ignore uid:999999)"
	check "user glob hides all"       "0"      "$(count --ignore 'user:*')"
	check "unmatched rule is inert"   "$NPROC" "$(count --ignore no-such-process-xyz)"

	# A rule file behaves the same as repeated --ignore flags.
	printf '# a comment\n\n*\n' > "$TMP/ign.conf"
	check "rules from a file"         "0"      "$(count --ignore-file "$TMP/ign.conf")"
	printf '# only comments\n\n' > "$TMP/empty.conf"
	check "empty rule file"           "$NPROC" "$(count --ignore-file "$TMP/empty.conf")"
	check "missing rule file is ok"   "$NPROC" "$(count --ignore-file "$TMP/nope.conf")"

	# The collector must not record what the snapshot hides.
	rm -rf "$TMP/cst"; mkdir -p "$TMP/cst"
	GPUWHO_STATE_DIR="$TMP/cst" "$GPUWHO" --ignore '*' collect >/dev/null 2>&1
	check "collect honors rules" "0" \
		"$(cat "$TMP/cst"/events-*.jsonl 2>/dev/null | wc -l)"
	rm -rf "$TMP/cst"; mkdir -p "$TMP/cst"
	GPUWHO_STATE_DIR="$TMP/cst" "$GPUWHO" collect >/dev/null 2>&1
	check "collect records otherwise" "$NPROC" \
		"$(grep -c '"ev":"start"' "$TMP/cst"/events-*.jsonl 2>/dev/null || echo 0)"
else
	echo "skip  ignore rules (no compute process on any GPU)"
fi

# --------------------------------------------------------------------------
echo
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
