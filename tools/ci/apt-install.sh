#!/usr/bin/env bash
# apt-get install for CI runners, with a time limit per attempt. A runner's
# Azure mirror sometimes crawls (13 MB in 15 minutes once); a stuck download
# is cut off after two minutes and retried, from Ubuntu's main archive over
# HTTPS. Only the downloads are limited: the install then unpacks from apt's
# cache, and cutting it off part way leaves dpkg interrupted, which fails
# every retry (#83: libgdal-dev's packages took over two minutes to unpack).
#   tools/ci/apt-install.sh <package>...
set -euo pipefail

apt_opts=(-q -o Acquire::Retries=2 -o Acquire::http::Timeout=20)
for attempt in 1 2 3; do
    if timeout 120 sudo apt-get "${apt_opts[@]}" update &&
        timeout 120 sudo apt-get "${apt_opts[@]}" install -y --download-only "$@" &&
        sudo apt-get "${apt_opts[@]}" install -y "$@"; then
        exit 0
    fi
    echo "::warning::apt attempt ${attempt} failed or took over 2 minutes"
    # Finish anything an earlier attempt left part way.
    sudo dpkg --configure -a || true
    # Later attempts skip the runner's mirror, and use HTTPS: on 2026-11-11
    # every plain HTTP request from the runners to Ubuntu's archives hung for
    # over half an hour, the Azure mirror's and archive.ubuntu.com's alike, while HTTPS
    # (packages.microsoft.com) answered. GitHub's Ubuntu images name the
    # mirror in a mirror list (sources say mirror+file:/etc/apt/apt-mirrors.txt);
    # others name it in the sources themselves.
    if [ -f /etc/apt/apt-mirrors.txt ]; then
        echo "https://archive.ubuntu.com/ubuntu/" | sudo tee /etc/apt/apt-mirrors.txt >/dev/null
    fi
    sudo sed -i -E 's#http://(azure\.)?(archive|security)\.ubuntu\.com/ubuntu#https://\2.ubuntu.com/ubuntu#g' \
        /etc/apt/sources.list /etc/apt/sources.list.d/*.sources 2>/dev/null || true
done
exit 1
