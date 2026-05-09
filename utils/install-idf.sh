#!/bin/bash

git submodule update --init --recursive

./esp-idf/install.sh esp32,esp32s3

. ./esp-idf/export.sh

cd esp-idf/examples/openthread/ot_rcp
idf.py set-target esp32h2
idf.py build
cd -
