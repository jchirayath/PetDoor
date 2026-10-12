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
