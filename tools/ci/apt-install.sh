#!/usr/bin/env bash
# apt-get install for CI runners, with a time limit per attempt. A runner's
# Azure mirror sometimes crawls (13 MB in 15 minutes once); a stuck attempt
# is cut off after two minutes and retried, from Ubuntu's main archive.
#   tools/ci/apt-install.sh <package>...
set -euo pipefail

apt_opts=(-q -o Acquire::Retries=2 -o Acquire::http::Timeout=20)
for attempt in 1 2 3; do
    if timeout 120 sudo apt-get "${apt_opts[@]}" update &&
        timeout 120 sudo apt-get "${apt_opts[@]}" install -y "$@"; then
        exit 0
    fi
    echo "::warning::apt attempt ${attempt} failed or took over 2 minutes"
    # Later attempts skip the runner's mirror. GitHub's Ubuntu images name it
    # in a mirror list (sources say mirror+file:/etc/apt/apt-mirrors.txt);
    # others name it in the sources themselves.
    if [ -f /etc/apt/apt-mirrors.txt ]; then
        echo "http://archive.ubuntu.com/ubuntu/" | sudo tee /etc/apt/apt-mirrors.txt >/dev/null
    fi
    sudo sed -i 's|http://azure.archive.ubuntu.com/ubuntu|http://archive.ubuntu.com/ubuntu|g' \
        /etc/apt/sources.list /etc/apt/sources.list.d/*.sources 2>/dev/null || true
done
exit 1
