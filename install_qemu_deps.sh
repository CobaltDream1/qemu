#!/usr/bin/env bash

set -e

echo "==> Updating package index..."
sudo apt update

echo "==> Installing pkg-config..."
sudo apt install -y pkg-config

echo "==> Installing glib2.0 development package..."
sudo apt install -y libglib2.0-dev

echo "==> Installing flex and bison..."
sudo apt install -y flex bison

echo "==> All essential QEMU dependencies installed successfully!"
