#!/usr/bin/env bash
set -Eeuo pipefail

NODE_HOST="${1:-sorting-node-01.local}"
SERVER_IP="${2:-172.20.33.72}"
SERVER_USER="${SERVER_USER:-server}"
SSH_KEY="${SSH_KEY:-$HOME/.ssh/logistics_yocto_admin}"
DEVICE_ID="${DEVICE_ID:-PI-SORTING-01}"
NODE_NAME="${NODE_NAME:-sorting-node-01}"
MQTT_PORT="${MQTT_PORT:-8883}"
DEFAULT_SPEED="${DEFAULT_SPEED:-50}"
SERVICE="logistics-sorting-node.service"

for command in ssh scp openssl mktemp; do
    if ! command -v "$command" >/dev/null 2>&1; then
        echo "Required command is missing: $command" >&2
        exit 1
    fi
done

if [ ! -r "$SSH_KEY" ]; then
    echo "SSH private key is missing or unreadable: $SSH_KEY" >&2
    exit 1
fi

work_dir=$(mktemp -d)
cleanup() {
    rm -rf "$work_dir"
}
trap cleanup EXIT INT TERM

ca_file="$work_dir/ca.crt"
config_file="$work_dir/sorting-node.ini"

echo "[1/7] Checking SSH access to $NODE_HOST"
ssh -o IdentitiesOnly=yes -i "$SSH_KEY" "root@$NODE_HOST" \
    'hostname; test -c /dev/vedauart'

echo "[2/7] Detecting the node IPv4 address"
node_ip=$(ssh -o IdentitiesOnly=yes -i "$SSH_KEY" "root@$NODE_HOST" \
    "ip -4 address show wlan0 2>/dev/null | awk '/inet / { split(\$2, address, \"/\"); print address[1]; exit }'")

if [ -z "$node_ip" ]; then
    node_ip=$(ssh -o IdentitiesOnly=yes -i "$SSH_KEY" "root@$NODE_HOST" \
        "ip -4 address show eth0 2>/dev/null | awk '/inet / { split(\$2, address, \"/\"); print address[1]; exit }'")
fi

if [ -z "$node_ip" ]; then
    echo "Unable to detect an IPv4 address on wlan0 or eth0" >&2
    exit 1
fi

echo "Detected node IPv4 address: $node_ip"

echo "[3/7] Downloading the MQTT CA certificate from $SERVER_USER@$SERVER_IP"
scp "$SERVER_USER@$SERVER_IP:/etc/logistics/tls/ca.crt" "$ca_file"
openssl x509 -in "$ca_file" -noout -subject -fingerprint -sha256

printf 'MQTT password for %s: ' "$DEVICE_ID"
IFS= read -r -s mqtt_password
printf '\n'

if [ -z "$mqtt_password" ]; then
    echo "MQTT password must not be empty" >&2
    exit 1
fi

cat > "$config_file" <<EOF
[device]
device_id=$DEVICE_ID
node_name=$NODE_NAME
ip_address=$node_ip

[mqtt]
host=$SERVER_IP
port=$MQTT_PORT
client_id=$DEVICE_ID
username=$DEVICE_ID
password=$mqtt_password
tls_enabled=true
ca_certificate=/etc/logistics/tls/ca.crt
keep_alive_seconds=30
reconnect_min_delay_seconds=1
reconnect_max_delay_seconds=30
clean_session=false

[sorting]
default_speed=$DEFAULT_SPEED
EOF

unset mqtt_password
chmod 0600 "$config_file"

echo "[4/7] Copying the CA certificate and node configuration"
scp -o IdentitiesOnly=yes -i "$SSH_KEY" \
    "$ca_file" "$config_file" "root@$NODE_HOST:/tmp/"

echo "[5/7] Installing files with production permissions"
ssh -o IdentitiesOnly=yes -i "$SSH_KEY" "root@$NODE_HOST" <<'REMOTE_INSTALL'
set -eu

mkdir -p /etc/logistics/tls
cp /tmp/ca.crt /etc/logistics/tls/ca.crt
cp /tmp/sorting-node.ini /etc/logistics/sorting-node.ini
chown root:root /etc/logistics/tls/ca.crt
chmod 0644 /etc/logistics/tls/ca.crt
chown root:logistics /etc/logistics/sorting-node.ini
chmod 0640 /etc/logistics/sorting-node.ini
rm -f /tmp/ca.crt /tmp/sorting-node.ini

openssl x509 -in /etc/logistics/tls/ca.crt -noout -fingerprint -sha256
stat -c '%a %U:%G %n' \
    /etc/logistics/tls/ca.crt \
    /etc/logistics/sorting-node.ini
REMOTE_INSTALL

echo "[6/7] Verifying the MQTT broker certificate"
ssh -o IdentitiesOnly=yes -i "$SSH_KEY" "root@$NODE_HOST" \
    "openssl s_client -connect '$SERVER_IP:$MQTT_PORT' -CAfile /etc/logistics/tls/ca.crt -verify_ip '$SERVER_IP' -brief </dev/null"

echo "[7/7] Enabling and starting $SERVICE"
ssh -o IdentitiesOnly=yes -i "$SSH_KEY" "root@$NODE_HOST" <<REMOTE_START
set -eu

systemctl daemon-reload
systemctl enable "$SERVICE"
systemctl restart "$SERVICE"
sleep 3
systemctl is-enabled "$SERVICE"
systemctl is-active "$SERVICE"
journalctl -u "$SERVICE" -n 30 --no-pager -l
REMOTE_START

echo "Sorting node provisioning completed successfully."
