#!/usr/bin/env bash
#
# Deploy the PetDoor log server to a machine you can reach over SSH.
#
#   ./deploy.sh user@host                      # plain HTTP on :8080
#   ./deploy.sh user@host petdoor.example.com  # + nginx vhost and TLS
#
# Safe to re-run: it upgrades in place and leaves the database and shared key
# alone. Nothing here is specific to one host — it is the same script whether
# you are deploying to a Raspberry Pi on your desk or a VM on the internet.
set -euo pipefail

# ── BEFORE YOU RUN THIS ──────────────────────────────────────────────────────
# This is the recipe for a BARE VM: it installs to /opt/petdoor, writes a
# systemd unit, and optionally configures **nginx** on ports 80 and 443.
#
# If the host already runs a web server — Caddy, Traefik, Apache, or anything
# in Docker publishing 80/443 — this will install a second one on ports the
# first already owns, and can take every other site on that machine down.
#
# On a host like that, deploy the two files and the HTML by hand and point the
# EXISTING proxy at 127.0.0.1:8080. docs/WEB-DASHBOARD.md has worked Caddy,
# nginx and Apache rules, including which paths must stay public and which must
# not.
TARGET="${1:-}"
DOMAIN="${2:-}"
REMOTE_DIR="/opt/petdoor"
SERVICE="petdoor-log"
PORT="${PETDOOR_PORT:-8080}"

if [[ -z "$TARGET" ]]; then
  awk 'NR>1 && /^#/ {sub(/^# ?/,""); print; next} NR>1 {exit}' "$0"
  exit 1
fi

say() { printf '\n\033[1m==> %s\033[0m\n' "$1"; }
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

say "Checking $TARGET"
ssh -o BatchMode=yes -o ConnectTimeout=10 "$TARGET" true 2>/dev/null || {
  echo "Cannot log in to $TARGET without a password."
  echo "Set up a key first:   ssh-copy-id $TARGET"
  exit 1
}
ssh "$TARGET" 'command -v python3 >/dev/null' || {
  echo "python3 is not installed on the remote host."; exit 1; }
echo "ok — $(ssh "$TARGET" 'python3 -V; echo "as $(whoami) on $(hostname)"' | tr '\n' ' ')"

say "Copying files to $REMOTE_DIR"
ssh "$TARGET" "sudo mkdir -p $REMOTE_DIR && sudo chown \$(whoami) $REMOTE_DIR"
scp -q "$here/petdoor-logserver.py" "$here/dashboard.html" "$here/public.html" \
       "$here/demo.html" "$TARGET:$REMOTE_DIR/"

# The public page shows photographs of the build. They live at the repo root,
# so copy them across if they are there; the page degrades to text without them.
if [ -d "$here/../../images" ]; then
  ssh "$TARGET" "mkdir -p $REMOTE_DIR/images"
  scp -q "$here"/../../images/*.jpeg "$TARGET:$REMOTE_DIR/images/" 2>/dev/null || true
fi
ssh "$TARGET" "chmod +x $REMOTE_DIR/petdoor-logserver.py"

say "Creating the service"
ssh "$TARGET" "sudo tee /etc/systemd/system/$SERVICE.service >/dev/null" <<UNIT
[Unit]
Description=PetDoor log server
Documentation=https://github.com/jchirayath/PetDoor
After=network-online.target

[Service]
ExecStart=/usr/bin/python3 $REMOTE_DIR/petdoor-logserver.py --port $PORT --host 127.0.0.1
Environment=PETDOOR_DB=$REMOTE_DIR/petdoor.sqlite3
Restart=always
RestartSec=5
# The service only needs its own directory.
NoNewPrivileges=true
PrivateTmp=true
ProtectSystem=strict
ProtectHome=true
ReadWritePaths=$REMOTE_DIR

[Install]
WantedBy=multi-user.target
UNIT

say "Starting it"
# --init is idempotent: it creates the database and a key only if absent, so
# re-deploying never rotates a key that doors are already using.
ssh "$TARGET" "cd $REMOTE_DIR && PETDOOR_DB=$REMOTE_DIR/petdoor.sqlite3 python3 petdoor-logserver.py --init >/dev/null"
ssh "$TARGET" "sudo systemctl daemon-reload && sudo systemctl enable --now $SERVICE && sleep 2 && sudo systemctl is-active $SERVICE"

if [[ -n "$DOMAIN" ]]; then
  say "Publishing as $DOMAIN"
  ssh "$TARGET" "command -v nginx >/dev/null" || {
    echo "nginx is not installed. Install it, or skip the domain argument"
    echo "and reach the server on port $PORT directly."; exit 1; }
  ssh "$TARGET" "sudo tee /etc/nginx/sites-available/$DOMAIN >/dev/null" <<VHOST
server {
    listen 80;
    server_name $DOMAIN;

    # The dashboard and the ingest endpoint share one origin, so the page can
    # call /api/events with no CORS setup.
    location / {
        proxy_pass http://127.0.0.1:$PORT;
        proxy_set_header Host \$host;
        proxy_set_header X-Real-IP \$remote_addr;
        proxy_set_header X-Forwarded-For \$proxy_add_x_forwarded_for;
        proxy_set_header X-Forwarded-Proto \$scheme;
    }

    # Doors speak plain HTTP deliberately — a TLS handshake costs them seconds
    # of radio time that belongs to Bluetooth. Uploads are signed with
    # HMAC-SHA256 instead, so they cannot be forged. Keep this location on :80
    # even after certbot adds TLS above.
    location /ingest {
        proxy_pass http://127.0.0.1:$PORT/ingest;
        client_max_body_size 2m;
    }
}
VHOST
  ssh "$TARGET" "sudo ln -sf /etc/nginx/sites-available/$DOMAIN /etc/nginx/sites-enabled/ \
                 && sudo nginx -t && sudo systemctl reload nginx"
  echo "ok — http://$DOMAIN"
  echo
  echo "For TLS on the dashboard (optional, and NOT needed for the doors):"
  echo "    ssh $TARGET sudo certbot --nginx -d $DOMAIN"
fi

say "Done"
echo "Shared key for the door's secrets.h:"
ssh "$TARGET" "cd $REMOTE_DIR && PETDOOR_DB=$REMOTE_DIR/petdoor.sqlite3 python3 petdoor-logserver.py --show-key" \
  | sed 's/^/    #define LOG_SHARED_KEY "/;s/$/"/'
echo
if [[ -n "$DOMAIN" ]]; then
  echo "    #define LOG_ENDPOINT_URL \"http://$DOMAIN/ingest\""
  echo "    dashboard: http://$DOMAIN/"
else
  echo "    #define LOG_ENDPOINT_URL \"http://<this-host>:$PORT/ingest\""
  echo "    (the service listens on 127.0.0.1 only — put nginx in front, or"
  echo "     change --host in the unit file to expose it on your LAN)"
fi
