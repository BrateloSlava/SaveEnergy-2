#!/bin/bash
make -j4 zImage;
make -j4 modules;
make modules_install;
MOD_DIR=zmodules_ready;
if [ ! -f $MOD_DIR ]; then
mkdir zmodules_ready
fi
mkdir tempo;
cp -r /lib/modules/3.4.10/kernel tempo/;
rm -rf /lib/modules/3.4.10;
find tempo/kernel/ -name '*.ko' -exec cp --target-directory=zmodules_ready {} \;
rm -rf tempo;
cd zmodules_ready;
arm-eabi-strip --strip-unneeded *.ko;


