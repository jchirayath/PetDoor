#!/usr/bin/env bash
# Alerting tests for the log server, and the commissioning script's parsers.
# No network, no real database, no door.
set -euo pipefail
cd "$(dirname "$0")"
python3 test_notify.py

# The commissioning script reads the door's status block with regexes, and the
# one time it is most needed is when the door is misbehaving — which is exactly
# when a parser bug would be mistaken for a hardware fault. Its self-test runs
# against captured blocks and needs nothing attached, so it belongs in CI.
echo
echo "▸ commissioning script: parsers (no hardware)"
python3 ../../tools/commission.py --selftest

# The public sample dashboard is GENERATED from the real one, and the generator
# is run by a person or not at all. That is how it came to be broken for months
# after the page's CSS and JS were extracted for the CSP: every anchor it
# replaced had moved into the extracted files, so it refused to run and the
# sample page quietly aged — eighteen functions behind the dashboard, no tab
# bar, and a heading naming a page that had since been renamed. Nothing noticed,
# because a stale sample page looks exactly like a current one.
echo
echo "▸ sample dashboard: still in step with the real one"
python3 ../../tools/logserver/make-demo.py --check
