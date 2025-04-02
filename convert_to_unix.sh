#!/bin/bash

find tests/ -type f -name "*.txt" -exec dos2unix {} \;

echo "Converted all test outputs unix-style line endings."