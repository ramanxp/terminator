set -e

if command -v apt-get >/dev/null 2>&1; then
    apt-get update
    apt-get install -y build-essential libx11-dev
elif command -v dnf >/dev/null 2>&1; then
    dnf install -y gcc make libx11-devel
elif command -v yum >/dev/null 2>&1; then
    yum install -y gcc make libx11-devel
elif command -v pacman >/dev/null 2>&1; then
    pacman -Sy --noconfirm gcc make libx11
elif command -v zypper >/dev/null 2>&1; then
    zypper --non-interactive install gcc make libX11-devel
elif command -v apk >/dev/null 2>&1; then
    apk add --no-cache gcc make musl-dev libx11-dev
else
    echo "Unknown package manager; install gcc and libX11-devel manually."
    exit 1
fi

echo "Dependencies installed."
