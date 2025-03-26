#!/bin/bash
set -e

mkdir -p debs

echo "Building Docker image..."
docker build -t s3fs-fuse-ubuntu20:latest -f Dockerfile.ubuntu20 .

echo "Building .deb package..."
docker run --rm -v "$(pwd):/build/s3fs-fuse" -v "$(pwd)/debs:/build/debs" s3fs-fuse-ubuntu20:latest

echo "Build completed. Check the ./debs directory for the .deb package." 
