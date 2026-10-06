#!/usr/bin/env bash
# Alerting tests for the log server. No network, no real database.
set -euo pipefail
cd "$(dirname "$0")"
python3 test_notify.py
